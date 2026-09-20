/* cgcam: PAJ7025R2 over SPI. See cgcam.h for the wiring and for why the
 * register access is probed rather than assumed (D-012). */

#include "cgcam.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cgcam";

#define BURST_MAX 256

static struct {
    bool ready;
    spi_device_handle_t dev;
    cgcam_format_t format;

    /* Filled in by cgcam_probe. Until then the defaults are a guess and
     * cgcam_product_id will say so by failing. */
    bool located;
    uint8_t bank;
    uint8_t reg_low;
    bool big_endian;
} s;

esp_err_t cgcam_init(const cgcam_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s, 0, sizeof(s));
    s.format = CGCAM_FMT_READ_LOW;
    s.reg_low = 0x00;

    const spi_bus_config_t bus = {
        .sclk_io_num = cfg->sck_gpio,
        .miso_io_num = cfg->miso_gpio,
        .mosi_io_num = cfg->mosi_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BURST_MAX + 8,
    };
    esp_err_t err = spi_bus_initialize((spi_host_device_t)cfg->host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    const spi_device_interface_config_t dev = {
        .mode = cfg->spi_mode,
        .clock_speed_hz = cfg->clock_hz,
        .spics_io_num = cfg->cs_gpio,
        .queue_size = 2,
        /* The sensor is specified into 100 pF and drives 4 mA, so the bench
         * wiring gets the slow clock and generous chip select edges. */
        .cs_ena_pretrans = 2,
        .cs_ena_posttrans = 2,
    };
    err = spi_bus_add_device((spi_host_device_t)cfg->host, &dev, &s.dev);
    if (err != ESP_OK) {
        return err;
    }

    s.ready = true;
    ESP_LOGI(TAG, "SPI up: sck %d miso %d mosi %d cs %d, %d Hz, mode %d", cfg->sck_gpio,
             cfg->miso_gpio, cfg->mosi_gpio, cfg->cs_gpio, cfg->clock_hz, cfg->spi_mode);

    /* Let the module finish its own power up before it is asked anything. */
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t cgcam_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!s.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0 || len > BURST_MAX + 8) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s.dev, &t);
}

void cgcam_set_format(cgcam_format_t format)
{
    if (format < CGCAM_FMT_COUNT) {
        s.format = format;
    }
}

cgcam_format_t cgcam_format(void)
{
    return s.format;
}

const char *cgcam_format_name(cgcam_format_t format)
{
    switch (format) {
    case CGCAM_FMT_READ_LOW:
        return "read bit7=0";
    case CGCAM_FMT_READ_HIGH:
        return "read bit7=1";
    case CGCAM_FMT_READ_LOW_DUMMY:
        return "read bit7=0 + dummy";
    case CGCAM_FMT_READ_HIGH_DUMMY:
        return "read bit7=1 + dummy";
    default:
        return "unknown";
    }
}

/* One register read in the given encoding. */
static esp_err_t read_reg_as(cgcam_format_t format, uint8_t reg, uint8_t *value)
{
    uint8_t tx[3] = {0};
    uint8_t rx[3] = {0};
    size_t len;
    size_t value_index;

    switch (format) {
    case CGCAM_FMT_READ_LOW:
        tx[0] = (uint8_t)(reg & 0x7Fu);
        len = 2;
        value_index = 1;
        break;
    case CGCAM_FMT_READ_HIGH:
        tx[0] = (uint8_t)(reg | 0x80u);
        len = 2;
        value_index = 1;
        break;
    case CGCAM_FMT_READ_LOW_DUMMY:
        tx[0] = (uint8_t)(reg & 0x7Fu);
        len = 3;
        value_index = 2;
        break;
    case CGCAM_FMT_READ_HIGH_DUMMY:
        tx[0] = (uint8_t)(reg | 0x80u);
        len = 3;
        value_index = 2;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = cgcam_xfer(tx, rx, len);
    if (err == ESP_OK && value != NULL) {
        *value = rx[value_index];
    }
    return err;
}

esp_err_t cgcam_read_reg(uint8_t reg, uint8_t *value)
{
    return read_reg_as(s.format, reg, value);
}

esp_err_t cgcam_write_reg(uint8_t reg, uint8_t value)
{
    /* A write uses the opposite sense of bit 7 from the read of the same
     * encoding, which is how every PixArt part in this family behaves. */
    uint8_t tx[2];

    switch (s.format) {
    case CGCAM_FMT_READ_LOW:
    case CGCAM_FMT_READ_LOW_DUMMY:
        tx[0] = (uint8_t)(reg | 0x80u);
        break;
    default:
        tx[0] = (uint8_t)(reg & 0x7Fu);
        break;
    }
    tx[1] = value;
    return cgcam_xfer(tx, NULL, sizeof(tx));
}

esp_err_t cgcam_select_bank(uint8_t bank)
{
    return cgcam_write_reg(CGCAM_REG_BANK, bank);
}

esp_err_t cgcam_product_id(uint16_t *id)
{
    uint8_t a = 0;
    uint8_t b = 0;

    if (!s.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s.located) {
        cgcam_select_bank(s.bank);
    }

    esp_err_t err = cgcam_read_reg(s.reg_low, &a);
    if (err != ESP_OK) {
        return err;
    }
    err = cgcam_read_reg((uint8_t)(s.reg_low + 1u), &b);
    if (err != ESP_OK) {
        return err;
    }

    const uint16_t value = s.big_endian ? (uint16_t)(((uint16_t)a << 8) | b)
                                        : (uint16_t)(((uint16_t)b << 8) | a);
    if (id != NULL) {
        *id = value;
    }
    return (value == CGCAM_PRODUCT_ID) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

int cgcam_probe(cgcam_probe_hit_t *hits, int max_hits, int banks_to_try)
{
    if (!s.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (banks_to_try < 1) {
        banks_to_try = 1;
    }

    int found = 0;
    uint8_t page[256];

    for (int f = 0; f < CGCAM_FMT_COUNT; f++) {
        const cgcam_format_t format = (cgcam_format_t)f;

        for (int bank = 0; bank < banks_to_try; bank++) {
            if (banks_to_try > 1) {
                /* Selecting a bank needs a write, and a write needs the
                 * encoding to be right, so the bank sweep is only as good as
                 * the encoding being tried. That is the point: a wrong
                 * encoding simply finds nothing. */
                const cgcam_format_t saved = s.format;
                s.format = format;
                cgcam_select_bank((uint8_t)bank);
                s.format = saved;
            }

            /* Read the whole page once, then look for the signature in it. */
            for (int reg = 0; reg < 256; reg++) {
                if (read_reg_as(format, (uint8_t)reg, &page[reg]) != ESP_OK) {
                    page[reg] = 0xFF;
                }
            }

            /* An all identical page is a dead bus, not a register map. */
            bool varies = false;
            for (int reg = 1; reg < 256; reg++) {
                if (page[reg] != page[0]) {
                    varies = true;
                    break;
                }
            }
            if (!varies) {
                continue;
            }

            for (int reg = 0; reg < 255; reg++) {
                const bool little = (page[reg] == 0x25u && page[reg + 1] == 0x70u);
                const bool big = (page[reg] == 0x70u && page[reg + 1] == 0x25u);
                if (!little && !big) {
                    continue;
                }

                ESP_LOGI(TAG, "product id 0x7025 at bank %d reg 0x%02X, %s, %s", bank, reg,
                         cgcam_format_name(format), big ? "big endian" : "little endian");

                if (found == 0) {
                    s.format = format;
                    s.bank = (uint8_t)bank;
                    s.reg_low = (uint8_t)reg;
                    s.big_endian = big;
                    s.located = true;
                }
                if (hits != NULL && found < max_hits) {
                    hits[found].format = format;
                    hits[found].bank = (uint8_t)bank;
                    hits[found].reg_low = (uint8_t)reg;
                    hits[found].big_endian = big;
                }
                found++;
            }
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "no 0x7025 anywhere, in any of the %d encodings", CGCAM_FMT_COUNT);
    }
    return found;
}

esp_err_t cgcam_burst_read(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!s.ready || buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0 || len > BURST_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    static uint8_t tx[BURST_MAX + 2];
    static uint8_t rx[BURST_MAX + 2];
    size_t head;

    switch (s.format) {
    case CGCAM_FMT_READ_LOW:
        tx[0] = (uint8_t)(reg & 0x7Fu);
        head = 1;
        break;
    case CGCAM_FMT_READ_HIGH:
        tx[0] = (uint8_t)(reg | 0x80u);
        head = 1;
        break;
    case CGCAM_FMT_READ_LOW_DUMMY:
        tx[0] = (uint8_t)(reg & 0x7Fu);
        head = 2;
        break;
    default:
        tx[0] = (uint8_t)(reg | 0x80u);
        head = 2;
        break;
    }
    memset(&tx[head], 0, len);

    const esp_err_t err = cgcam_xfer(tx, rx, head + len);
    if (err == ESP_OK) {
        memcpy(buf, &rx[head], len);
    }
    return err;
}
