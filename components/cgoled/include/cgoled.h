/* cgoled: the SSD1306 on the Heltec WiFi LoRa 32 V2.
 *
 * 128 by 64 over I2C on SDA 4, SCL 15, with a reset line on 16. Those three
 * pins are the ones concept.md section 4 tells the beacon driver to leave
 * alone.
 *
 * Text only, a 5 by 7 font in a 6 by 8 cell, so 21 columns by 8 rows. The
 * font covers ASCII 0x20 to 0x5F and folds lowercase onto uppercase, which
 * is all a bench display needs and keeps the table at 320 bytes.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CGOLED_COLS 21
#define CGOLED_ROWS 8

typedef struct {
    int sda_gpio;
    int scl_gpio;
    int reset_gpio; /* -1 when the board has no reset line */
    uint8_t address;
    int clock_hz;
} cgoled_config_t;

#define CGOLED_HELTEC_V2_CONFIG()                                            \
    (cgoled_config_t)                                                        \
    {                                                                        \
        .sda_gpio = 4, .scl_gpio = 15, .reset_gpio = 16, .address = 0x3C,    \
        .clock_hz = 400000,                                                  \
    }

/* Returns ESP_ERR_NOT_FOUND when the panel does not answer on the bus. The
 * caller decides whether that is fatal; on the bench it is not, and every
 * target keeps running with the display dark. */
esp_err_t cgoled_init(const cgoled_config_t *cfg);

bool cgoled_present(void);

void cgoled_clear(void);

/* Writes text into the buffer. Clipped at the edges, never wraps. */
void cgoled_text(int col, int row, const char *text);

void cgoled_printf(int col, int row, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Fills one row with a solid line, for a separator. */
void cgoled_rule(int row);

/* Pushes the buffer to the panel. Nothing is visible until this runs. */
esp_err_t cgoled_flush(void);

#ifdef __cplusplus
}
#endif
