/* cgcam: the PAJ7025R2 multiple object tracking sensor.
 *
 * Wiring on the Heltec V2 bench, concept.md section 5 as clarified on
 * 20 September 2026. The camera sits on free GPIOs rather than the VSPI
 * default pins, because LoRa holds 5, 18, 19 and 27 on this board. ESP-IDF
 * routes any pin through the GPIO matrix, which costs nothing at 1 MHz.
 *
 * The pin numbers on the right are the module's own, from table 1 of the
 * PAJ7025R2 datasheet version 1.3.
 *
 *   ESP32 GPIO 21  ->  pin 10  G9/CSB    chip select, active low
 *   ESP32 GPIO 22  ->  pin 11  G10/SCK   clock
 *   ESP32 GPIO 17  <-  pin 12  G11/MISO  data out of the sensor
 *   ESP32 GPIO 23  ->  pin 13  G12/MOSI  data into the sensor
 *   3V3            ->  pin 17  VDDMA
 *   GND            ->  pin 14  VSSD  and  pin 20  VSSD_LED, both required
 *   0.1 uF and 10 uF from VDDMA to GND, as close to the module pins as the
 *   flying wires allow (datasheet figure 13).
 *
 * Voltage, and this one bites: VDDMA is 2.0 to 3.6 V with an absolute
 * maximum of 3.96 V, and every signal pin is limited to VDDMA + 0.3 V. The
 * 5 V supply that feeds the beacon clusters must not reach this module or
 * any of its pins. At 3.3 V the levels line up with the ESP32 without a
 * shifter: the sensor needs 0.7 x VDDMA to read a high, which is 2.31 V,
 * and it drives 0.9 x VDDMA, which is 2.97 V.
 *
 * Clock: the part does up to 14 MHz on a board, but its pins are specified
 * into 100 pF and drive 4 mA, so flying wires get 1 to 2 MHz.
 *
 * The bus is unusual in three ways and all three have to be right together
 * or nothing answers (D-015):
 *
 *   1. SPI mode 3, and LSB first. Bit order is the one that is never
 *      guessed, because every other sensor on the bench is MSB first.
 *   2. A transaction is a command byte, then the register, then the data.
 *      The command is 0x00 to write, 0x80 to read one byte and 0x81 to
 *      burst read. The register address does not carry the direction bit.
 *   3. Chip select is held across a bank switch and the operation that
 *      follows it, so it cannot be the hardware chip select that ESP-IDF
 *      drives per transaction. cgcam drives it as an ordinary GPIO.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CGCAM_MAX_OBJECTS 16
#define CGCAM_PRODUCT_ID 0x7025u

/* The register that selects the bank, in every bank. */
#define CGCAM_REG_BANK 0xEFu

/* Product ID, bank 0, low byte then high byte. */
#define CGCAM_BANK_ID 0x00u
#define CGCAM_REG_ID_LOW 0x02u
#define CGCAM_REG_ID_HIGH 0x03u

/* Frame period, bank 0x0C, three bytes in units of 100 ns. */
#define CGCAM_BANK_SETTINGS 0x0Cu
#define CGCAM_REG_FRAME_PERIOD 0x07u

/* The four output formats. The value is what goes into the bank register to
 * expose that report, and the size is how many bytes the burst read then
 * returns from register 0. Format 1 is the only one that carries every
 * field, so it is what the bench uses. */
typedef enum {
    CGCAM_FORMAT_1 = 1, /* 256 bytes, 16 bytes per object, all fields */
    CGCAM_FORMAT_2 = 2, /* 96 bytes, area and centre only */
    CGCAM_FORMAT_3 = 3, /* 144 bytes */
    CGCAM_FORMAT_4 = 4, /* 208 bytes */
} cgcam_format_t;

typedef struct {
    int host; /* SPI host, 2 is SPI3_HOST on the ESP32 */
    int sck_gpio;
    int miso_gpio;
    int mosi_gpio;
    int cs_gpio; /* driven by cgcam, not by the SPI peripheral */
    int clock_hz;
} cgcam_config_t;

#define CGCAM_HELTEC_V2_CONFIG()                                      \
    (cgcam_config_t)                                                  \
    {                                                                 \
        .host = 2,                                                    \
        .sck_gpio = 22, .miso_gpio = 17, .mosi_gpio = 23,             \
        .cs_gpio = 21, .clock_hz = 1000000,                           \
    }

/* One tracked object. The centre is 12 bits per axis, which is the
 * 4095 by 4095 interpolated grid of concept.md section 5. The boundary and
 * radius fields are in raw sensor pixels, of which there are 98 by 98. */
typedef struct {
    uint16_t area; /* 14 bits */
    uint16_t cx;   /* 12 bits, 0 is left */
    uint16_t cy;   /* 12 bits, 0 is top */
    uint8_t average_brightness;
    uint8_t max_brightness;
    uint8_t range;  /* 4 bits */
    uint8_t radius; /* 4 bits */
    uint8_t boundary_left;
    uint8_t boundary_right;
    uint8_t boundary_up;
    uint8_t boundary_down;
    uint8_t aspect_ratio;
    uint8_t vx;
    uint8_t vy;
} cgcam_object_t;

typedef struct {
    int count; /* how many of the sixteen slots carry an object */
    cgcam_object_t object[CGCAM_MAX_OBJECTS];
} cgcam_frame_t;

esp_err_t cgcam_init(const cgcam_config_t *cfg);

/* Reads the product ID from bank 0. Returns ESP_ERR_NOT_FOUND when the
 * answer is not 0x7025, and still writes what it saw to id. */
esp_err_t cgcam_product_id(uint16_t *id);

/* Writes the initial settings the datasheet asks for before the sensor
 * reports anything useful. cgcam_init has already done this. */
esp_err_t cgcam_load_initial_settings(void);

esp_err_t cgcam_read_reg(uint8_t bank, uint8_t reg, uint8_t *value);
esp_err_t cgcam_write_reg(uint8_t bank, uint8_t reg, uint8_t value);

/* Frame period in microseconds, as the sensor is currently configured. */
esp_err_t cgcam_frame_period_us(uint32_t *period_us);

/* Reads one report and parses it. An object slot with zero area and a
 * centre of 0xFFF is empty, which is how count is arrived at. */
esp_err_t cgcam_read_frame(cgcam_frame_t *frame, cgcam_format_t format);

/* The raw report, for looking at bytes when the parse is in doubt. buf must
 * hold cgcam_format_size(format) bytes. */
esp_err_t cgcam_read_report(uint8_t *buf, cgcam_format_t format);
size_t cgcam_format_size(cgcam_format_t format);

/* Confirms on silicon what the reference says: sweeps the banks for the
 * product ID and reports where it answered. Kept because a register map
 * read out of somebody else's driver is a claim until the part agrees with
 * it (D-015). Returns the number of banks that answered 0x7025. */
int cgcam_probe(uint8_t *bank_out);

#ifdef __cplusplus
}
#endif
