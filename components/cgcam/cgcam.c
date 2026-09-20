/* cgcam: PAJ7025R2 over SPI. See cgcam.h for the wiring and the three
 * things about this bus that have to be right together, and D-015 for where
 * the register map came from. */

#include "cgcam.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cgcam";

#define REPORT_MAX 256
#define OBJECT_STRIDE 16 /* bytes per object in format 1 */

/* Command bytes. They lead the transaction; the register address that
 * follows carries no direction bit of its own. */
#define CMD_WRITE 0x00u
#define CMD_READ 0x80u
#define CMD_BURST_READ 0x81u

/* An empty object slot reads back as no area at the far corner. */
#define CX_EMPTY 0x0FFFu

static struct {
    bool ready;
    spi_device_handle_t dev;
    int cs_gpio;
    uint8_t bank; /* the bank the sensor is currently switched to */
} s;

/* Chip select is ours, not the peripheral's, because a bank switch and the
 * operation that follows it are one transaction as far as the sensor is
 * concerned. */
static void cs_set(bool selected)
{
    gpio_set_level((gpio_num_t)s.cs_gpio, selected ? 0 : 1);
}

static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s.dev, &t);
}

/* One register write, with chip select already held by the caller. */
static esp_err_t write_raw(uint8_t reg, uint8_t value)
{
    const uint8_t tx[3] = {CMD_WRITE, reg, value};
    return xfer(tx, NULL, sizeof(tx));
}

/* One register read, with chip select already held by the caller. */
static esp_err_t read_raw(uint8_t reg, uint8_t *value)
{
    const uint8_t tx[3] = {CMD_READ, reg, 0x00};
    uint8_t rx[3] = {0};

    const esp_err_t err = xfer(tx, rx, sizeof(tx));
    if (err == ESP_OK && value != NULL) {
        *value = rx[2];
    }
    return err;
}

/* Switches bank, with chip select already held. The sensor remembers the
 * bank, so this is skipped when it is already the right one, except that
 * the report read always writes it because the format code goes to the
 * same register. */
static esp_err_t select_bank_raw(uint8_t bank)
{
    const esp_err_t err = write_raw(CGCAM_REG_BANK, bank);
    if (err == ESP_OK) {
        s.bank = bank;
    }
    return err;
}

esp_err_t cgcam_write_reg(uint8_t bank, uint8_t reg, uint8_t value)
{
    if (!s.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    cs_set(true);
    esp_err_t err = select_bank_raw(bank);
    if (err == ESP_OK) {
        err = write_raw(reg, value);
    }
    cs_set(false);
    return err;
}

esp_err_t cgcam_read_reg(uint8_t bank, uint8_t reg, uint8_t *value)
{
    if (!s.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    cs_set(true);
    esp_err_t err = select_bank_raw(bank);
    if (err == ESP_OK) {
        err = read_raw(reg, value);
    }
    cs_set(false);
    return err;
}

/* ------------------------------------------------------ initial settings -- */

/* The initial settings of datasheet section 7.1.2, as a flat list of writes
 * in the order they must happen. An entry whose register is CGCAM_REG_BANK
 * switches bank, which is why the list is flat rather than grouped: the
 * order is what matters and grouping it would invite someone to reorder it.
 *
 * These are hardware configuration values, not a program. D-015 records
 * where they were read and why they are not guessed. */
static const struct {
    uint8_t reg;
    uint8_t value;
} INITIAL_SETTINGS[] = {
    {CGCAM_REG_BANK, 0x00}, {0xDC, 0x00}, {0xFB, 0x04},
    {CGCAM_REG_BANK, 0x00}, {0x2F, 0x05}, {0x30, 0x00}, {0x30, 0x01},
    {0x1F, 0x00},
    {CGCAM_REG_BANK, 0x01}, {0x2D, 0x00},
    {CGCAM_REG_BANK, 0x0C}, {0x64, 0x00}, {0x65, 0x00}, {0x66, 0x00},
    {0x67, 0x00}, {0x68, 0x00}, {0x69, 0x00}, {0x6A, 0x00}, {0x6B, 0x00},
    {0x6C, 0x00}, {0x71, 0x00}, {0x72, 0x00}, {0x12, 0x00}, {0x13, 0x00},
    {CGCAM_REG_BANK, 0x00}, {0x01, 0x01},
};

esp_err_t cgcam_load_initial_settings(void)
{
    if (!s.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* One chip select for the whole sequence, as the sensor expects. */
    cs_set(true);
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(INITIAL_SETTINGS) / sizeof(INITIAL_SETTINGS[0]); i++) {
        err = write_raw(INITIAL_SETTINGS[i].reg, INITIAL_SETTINGS[i].value);
        if (err != ESP_OK) {
            break;
        }
        if (INITIAL_SETTINGS[i].reg == CGCAM_REG_BANK) {
            s.bank = INITIAL_SETTINGS[i].value;
        }
    }
    cs_set(false);
    return err;
}

/* ------------------------------------------------------------------ init -- */

esp_err_t cgcam_init(const cgcam_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s, 0, sizeof(s));
    s.cs_gpio = cfg->cs_gpio;

    const gpio_config_t cs_io = {
        .pin_bit_mask = 1ULL << cfg->cs_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cs_io);
    if (err != ESP_OK) {
        return err;
    }
    cs_set(false);

    const spi_bus_config_t bus = {
        .sclk_io_num = cfg->sck_gpio,
        .miso_io_num = cfg->miso_gpio,
        .mosi_io_num = cfg->mosi_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = REPORT_MAX + 8,
    };
    err = spi_bus_initialize((spi_host_device_t)cfg->host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    const spi_device_interface_config_t dev = {
        .mode = 3,
        .clock_speed_hz = cfg->clock_hz,
        .spics_io_num = -1, /* cgcam holds chip select across several transfers */
        .queue_size = 2,
        /* The one setting nobody guesses. The part clocks the least
         * significant bit first, in both directions. */
        .flags = SPI_DEVICE_BIT_LSBFIRST,
    };
    err = spi_bus_add_device((spi_host_device_t)cfg->host, &dev, &s.dev);
    if (err != ESP_OK) {
        return err;
    }

    s.ready = true;
    ESP_LOGI(TAG, "SPI up: sck %d miso %d mosi %d cs %d, %d Hz, mode 3, LSB first",
             cfg->sck_gpio, cfg->miso_gpio, cfg->mosi_gpio, cfg->cs_gpio, cfg->clock_hz);

    /* Let the module finish its own power up before it is asked anything. */
    vTaskDelay(pdMS_TO_TICKS(50));

    err = cgcam_load_initial_settings();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "initial settings failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t cgcam_product_id(uint16_t *id)
{
    uint8_t low = 0;
    uint8_t high = 0;

    if (!s.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    cs_set(true);
    esp_err_t err = select_bank_raw(CGCAM_BANK_ID);
    if (err == ESP_OK) {
        err = read_raw(CGCAM_REG_ID_LOW, &low);
    }
    if (err == ESP_OK) {
        err = read_raw(CGCAM_REG_ID_HIGH, &high);
    }
    cs_set(false);
    if (err != ESP_OK) {
        return err;
    }

    const uint16_t value = (uint16_t)(((uint16_t)high << 8) | low);
    if (id != NULL) {
        *id = value;
    }
    return (value == CGCAM_PRODUCT_ID) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t cgcam_frame_period_us(uint32_t *period_us)
{
    uint8_t b0 = 0;
    uint8_t b1 = 0;
    uint8_t b2 = 0;

    if (!s.ready || period_us == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    cs_set(true);
    esp_err_t err = select_bank_raw(CGCAM_BANK_SETTINGS);
    if (err == ESP_OK) {
        err = read_raw(CGCAM_REG_FRAME_PERIOD, &b0);
    }
    if (err == ESP_OK) {
        err = read_raw(CGCAM_REG_FRAME_PERIOD + 1u, &b1);
    }
    if (err == ESP_OK) {
        err = read_raw(CGCAM_REG_FRAME_PERIOD + 2u, &b2);
    }
    cs_set(false);
    if (err != ESP_OK) {
        return err;
    }

    /* Three bytes, little endian, in units of 100 ns. */
    const uint32_t ticks = (uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16);
    *period_us = (ticks / 10u) + (((ticks % 10u) >= 5u) ? 1u : 0u);
    return ESP_OK;
}

/* ---------------------------------------------------------------- report -- */

size_t cgcam_format_size(cgcam_format_t format)
{
    switch (format) {
    case CGCAM_FORMAT_1:
        return 256;
    case CGCAM_FORMAT_2:
        return 96;
    case CGCAM_FORMAT_3:
        return 144;
    case CGCAM_FORMAT_4:
        return 208;
    default:
        return 0;
    }
}

/* The value written to the bank register to expose each report. */
static uint8_t format_bank(cgcam_format_t format)
{
    switch (format) {
    case CGCAM_FORMAT_1:
        return 0x05;
    case CGCAM_FORMAT_2:
        return 0x09;
    case CGCAM_FORMAT_3:
        return 0x0A;
    case CGCAM_FORMAT_4:
        return 0x0B;
    default:
        return 0x05;
    }
}

esp_err_t cgcam_read_report(uint8_t *buf, cgcam_format_t format)
{
    const size_t len = cgcam_format_size(format);

    if (!s.ready || buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    static uint8_t tx[REPORT_MAX + 2];
    static uint8_t rx[REPORT_MAX + 2];

    tx[0] = CMD_BURST_READ;
    tx[1] = 0x00; /* the report always starts at register 0 */
    memset(&tx[2], 0, len);

    cs_set(true);
    esp_err_t err = select_bank_raw(format_bank(format));
    if (err == ESP_OK) {
        err = xfer(tx, rx, len + 2u);
    }
    cs_set(false);

    if (err == ESP_OK) {
        memcpy(buf, &rx[2], len);
    }
    return err;
}

/* Sixteen bytes of one object slot, format 1. The bit widths are the
 * sensor's, not a convention: area is 14 bits, the centre is 12 bits per
 * axis, range and radius share a byte, and the boundaries are 7 bits
 * because the array is 98 pixels across. */
static void parse_object(const uint8_t *d, cgcam_object_t *o)
{
    o->area = (uint16_t)(d[0] | ((uint16_t)(d[1] & 0x3Fu) << 8));
    o->cx = (uint16_t)(d[2] | ((uint16_t)(d[3] & 0x0Fu) << 8));
    o->cy = (uint16_t)(d[4] | ((uint16_t)(d[5] & 0x0Fu) << 8));
    o->average_brightness = d[6];
    o->max_brightness = d[7];
    o->range = (uint8_t)(d[8] >> 4);
    o->radius = (uint8_t)(d[8] & 0x0Fu);
    o->boundary_left = (uint8_t)(d[9] & 0x7Fu);
    o->boundary_right = (uint8_t)(d[10] & 0x7Fu);
    o->boundary_up = (uint8_t)(d[11] & 0x7Fu);
    o->boundary_down = (uint8_t)(d[12] & 0x7Fu);
    o->aspect_ratio = d[13];
    o->vx = d[14];
    o->vy = d[15];
}

esp_err_t cgcam_read_frame(cgcam_frame_t *frame, cgcam_format_t format)
{
    uint8_t report[REPORT_MAX];

    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (format != CGCAM_FORMAT_1) {
        /* The other formats drop fields and shift the ones that remain.
         * The bench has no reason to use them, so they are not parsed
         * rather than parsed wrongly. */
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_err_t err = cgcam_read_report(report, format);
    if (err != ESP_OK) {
        return err;
    }

    memset(frame, 0, sizeof(*frame));
    for (int i = 0; i < CGCAM_MAX_OBJECTS; i++) {
        cgcam_object_t object;
        parse_object(&report[i * OBJECT_STRIDE], &object);

        /* An unused slot reports no area and parks its centre at the far
         * corner. Either alone is enough to reject it. */
        if (object.area == 0 || object.cx == CX_EMPTY) {
            continue;
        }
        frame->object[frame->count++] = object;
    }
    return ESP_OK;
}

/* ----------------------------------------------------------------- probe -- */

int cgcam_probe(uint8_t *bank_out)
{
    if (!s.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    int found = 0;

    for (int bank = 0; bank < 16; bank++) {
        uint8_t low = 0;
        uint8_t high = 0;

        cs_set(true);
        esp_err_t err = select_bank_raw((uint8_t)bank);
        if (err == ESP_OK) {
            err = read_raw(CGCAM_REG_ID_LOW, &low);
        }
        if (err == ESP_OK) {
            err = read_raw(CGCAM_REG_ID_HIGH, &high);
        }
        cs_set(false);
        if (err != ESP_OK) {
            continue;
        }

        if ((uint16_t)(((uint16_t)high << 8) | low) == CGCAM_PRODUCT_ID) {
            ESP_LOGI(TAG, "product id 0x7025 in bank %d at 0x%02X and 0x%02X", bank,
                     CGCAM_REG_ID_LOW, CGCAM_REG_ID_HIGH);
            if (found == 0 && bank_out != NULL) {
                *bank_out = (uint8_t)bank;
            }
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "no bank answered 0x7025");
    }
    return found;
}
