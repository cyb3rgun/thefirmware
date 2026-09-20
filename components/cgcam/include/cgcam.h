/* cgcam: the PAJ7025R2 multiple object tracking sensor.
 *
 * Wiring on the Heltec V2 bench, VSPI with LoRa disabled, concept.md
 * section 5. The pin numbers on the right are the module's own, from table 1
 * of the PAJ7025R2 datasheet version 1.3.
 *
 *   ESP32 GPIO 5   ->  pin 10  G9/CSB    chip select, active low
 *   ESP32 GPIO 18  ->  pin 11  G10/SCK   clock
 *   ESP32 GPIO 19  <-  pin 12  G11/MISO  data out of the sensor
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
 * What this component cannot do yet, and why: the copy of the datasheet in
 * THEHARDWARE stops at page 20. The SPI data format (section 6.1.2, page
 * 26), the initialisation flow (7.1.1, page 33), the product ID register
 * (7.1.3, page 34), the bank switching (7.3.2, page 40) and the output
 * access (7.4, page 42) are all past the end of the file. Rather than
 * inventing a register map, cgcam_probe sweeps the plausible transaction
 * encodings and the whole register space and reports where the sensor
 * actually answers with 0x7025. See D-012.
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

/* How a register access is laid out on the wire. The datasheet pages that
 * would say which of these is right are missing, so the bench finds out by
 * asking the silicon. D-012. */
typedef enum {
    /* read: send the address with bit 7 clear, then clock one byte out.
     * write: send the address with bit 7 set, then the value. */
    CGCAM_FMT_READ_LOW = 0,
    /* the same, with the sense of bit 7 swapped */
    CGCAM_FMT_READ_HIGH,
    /* read: address with bit 7 clear, one dummy byte, then the value */
    CGCAM_FMT_READ_LOW_DUMMY,
    /* read: address with bit 7 set, one dummy byte, then the value */
    CGCAM_FMT_READ_HIGH_DUMMY,
    CGCAM_FMT_COUNT,
} cgcam_format_t;

/* The register that selects the bank, on every PixArt part that has banks.
 * Confirm against section 7.3.2 when the full datasheet arrives. */
#define CGCAM_REG_BANK 0xEFu

typedef struct {
    int host;  /* SPI host, SPI3_HOST is VSPI on the ESP32 */
    int sck_gpio;
    int miso_gpio;
    int mosi_gpio;
    int cs_gpio;
    int clock_hz;
    int spi_mode; /* 0 to 3; the timing section of the datasheet is missing */
} cgcam_config_t;

#define CGCAM_HELTEC_V2_CONFIG()                                      \
    (cgcam_config_t)                                                  \
    {                                                                 \
        .host = 2, /* SPI3_HOST */                                    \
        .sck_gpio = 18, .miso_gpio = 19, .mosi_gpio = 23,             \
        .cs_gpio = 5, .clock_hz = 1000000, .spi_mode = 3,             \
    }

esp_err_t cgcam_init(const cgcam_config_t *cfg);

/* Raw full duplex transfer with chip select held over the whole of it. */
esp_err_t cgcam_xfer(const uint8_t *tx, uint8_t *rx, size_t len);

void cgcam_set_format(cgcam_format_t format);
cgcam_format_t cgcam_format(void);
const char *cgcam_format_name(cgcam_format_t format);

esp_err_t cgcam_read_reg(uint8_t reg, uint8_t *value);
esp_err_t cgcam_write_reg(uint8_t reg, uint8_t value);
esp_err_t cgcam_select_bank(uint8_t bank);

/* Reads the two bytes that hold the product ID, using the addresses and the
 * format that cgcam_probe found, or the defaults when nothing was probed.
 * Returns ESP_ERR_NOT_FOUND when the answer is not 0x7025. */
esp_err_t cgcam_product_id(uint16_t *id);

typedef struct {
    cgcam_format_t format;
    uint8_t bank;
    uint8_t reg_low;  /* the register holding 0x25 */
    bool big_endian;  /* true when 0x70 came first */
} cgcam_probe_hit_t;

/* Sweeps the transaction encodings, the banks and the register space looking
 * for the 0x7025 signature, and remembers the first hit so that
 * cgcam_read_reg and cgcam_product_id work afterwards. Returns how many hits
 * were found, or a negative esp_err_t. banks_to_try of 1 stays on whatever
 * bank the sensor powered up in. */
int cgcam_probe(cgcam_probe_hit_t *hits, int max_hits, int banks_to_try);

/* Reads len bytes starting at reg, chip select held for the whole burst. */
esp_err_t cgcam_burst_read(uint8_t reg, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
