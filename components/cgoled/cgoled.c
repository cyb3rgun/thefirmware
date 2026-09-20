/* cgoled: SSD1306 text display for the Heltec V2 bench boards. */

#include "cgoled.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cgoled";

#define OLED_WIDTH 128
#define OLED_PAGES 8
#define CELL_W 6

static struct {
    bool present;
    uint8_t address;
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t fb[OLED_WIDTH * OLED_PAGES];
} s;

/* 5 by 7 font, ASCII 0x20 to 0x5F. One byte per column, bit 0 is the top
 * row. Lowercase is folded onto uppercase by the drawing code. */
static const uint8_t FONT[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* space */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* ! */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* " */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* # */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* $ */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* % */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* & */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* ( */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ) */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* * */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* + */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* , */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* . */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ; */
    {0x00, 0x08, 0x14, 0x22, 0x41}, /* < */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* = */
    {0x41, 0x22, 0x14, 0x08, 0x00}, /* > */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* ? */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* @ */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x01, 0x01}, /* F */
    {0x3E, 0x41, 0x41, 0x51, 0x32}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x03, 0x04, 0x78, 0x04, 0x03}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    {0x00, 0x00, 0x7F, 0x41, 0x41}, /* [ */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* backslash */
    {0x41, 0x41, 0x7F, 0x00, 0x00}, /* ] */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* ^ */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* _ */
};

#define FONT_FIRST 0x20
#define FONT_LAST 0x5F

static esp_err_t send(const uint8_t *data, size_t len)
{
    return i2c_master_transmit(s.dev, data, len, 100);
}

static esp_err_t command(uint8_t cmd)
{
    const uint8_t buf[2] = {0x00, cmd};
    return send(buf, sizeof(buf));
}

esp_err_t cgoled_init(const cgoled_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s, 0, sizeof(s));
    s.address = cfg->address;

    if (cfg->reset_gpio >= 0) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << cfg->reset_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) {
            return err;
        }
        gpio_set_level((gpio_num_t)cfg->reset_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level((gpio_num_t)cfg->reset_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s.bus);
    if (err != ESP_OK) {
        return err;
    }

    if (i2c_master_probe(s.bus, cfg->address, 100) != ESP_OK) {
        ESP_LOGW(TAG, "no panel at 0x%02X on SDA %d SCL %d", cfg->address, cfg->sda_gpio,
                 cfg->scl_gpio);
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->address,
        .scl_speed_hz = cfg->clock_hz,
    };
    err = i2c_master_bus_add_device(s.bus, &dev_cfg, &s.dev);
    if (err != ESP_OK) {
        return err;
    }

    static const uint8_t INIT[] = {
        0xAE,             /* display off */
        0xD5, 0x80,       /* clock divide */
        0xA8, 0x3F,       /* multiplex, 64 rows */
        0xD3, 0x00,       /* no display offset */
        0x40,             /* start line 0 */
        0x8D, 0x14,       /* charge pump on */
        0x20, 0x00,       /* horizontal addressing */
        0xA1,             /* segment remap */
        0xC8,             /* scan from COM63 */
        0xDA, 0x12,       /* alternative COM pins */
        0x81, 0xCF,       /* contrast */
        0xD9, 0xF1,       /* precharge */
        0xDB, 0x40,       /* VCOMH */
        0xA4,             /* follow the buffer */
        0xA6,             /* not inverted */
        0x2E,             /* no scrolling */
        0xAF,             /* display on */
    };
    for (size_t i = 0; i < sizeof(INIT); i++) {
        err = command(INIT[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    s.present = true;
    cgoled_clear();
    return cgoled_flush();
}

bool cgoled_present(void)
{
    return s.present;
}

void cgoled_clear(void)
{
    memset(s.fb, 0, sizeof(s.fb));
}

void cgoled_rule(int row)
{
    if (row < 0 || row >= CGOLED_ROWS) {
        return;
    }
    memset(&s.fb[row * OLED_WIDTH], 0x08, OLED_WIDTH);
}

void cgoled_text(int col, int row, const char *text)
{
    if (text == NULL || row < 0 || row >= CGOLED_ROWS) {
        return;
    }

    for (; *text != '\0'; text++, col++) {
        if (col < 0 || col >= CGOLED_COLS) {
            continue;
        }
        unsigned char ch = (unsigned char)*text;
        if (ch >= 'a' && ch <= 'z') {
            ch = (unsigned char)(ch - 'a' + 'A');
        }
        if (ch < FONT_FIRST || ch > FONT_LAST) {
            ch = '?';
        }
        const uint8_t *glyph = FONT[ch - FONT_FIRST];
        uint8_t *cell = &s.fb[row * OLED_WIDTH + col * CELL_W];
        for (int i = 0; i < 5; i++) {
            cell[i] = glyph[i];
        }
        cell[5] = 0x00; /* the gap between characters */
    }
}

void cgoled_printf(int col, int row, const char *fmt, ...)
{
    char line[CGOLED_COLS + 1];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    cgoled_text(col, row, line);
}

esp_err_t cgoled_flush(void)
{
    if (!s.present) {
        return ESP_ERR_INVALID_STATE;
    }

    static const uint8_t WINDOW[] = {
        0x00,       /* command stream */
        0x21, 0, 127, /* column range */
        0x22, 0, 7,   /* page range */
    };
    esp_err_t err = send(WINDOW, sizeof(WINDOW));
    if (err != ESP_OK) {
        return err;
    }

    /* One page at a time keeps the transfer buffer small. */
    uint8_t chunk[1 + OLED_WIDTH];
    chunk[0] = 0x40; /* data stream */
    for (int page = 0; page < OLED_PAGES; page++) {
        memcpy(&chunk[1], &s.fb[page * OLED_WIDTH], OLED_WIDTH);
        err = send(chunk, sizeof(chunk));
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
