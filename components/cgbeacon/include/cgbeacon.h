/* cgbeacon: the four beacon clusters of docs/concept.md section 4.
 *
 * Four clusters of three TSHG6400 at 850 nm, one IRLZ44N per cluster on
 * GPIO 12, 13, 14 and 25 of the Heltec V2. The OLED pins (4, 15, 16) and the
 * LoRa pins (5, 18, 19, 26, 27) stay untouched.
 *
 * Two modes, from the core's beacons message:
 *
 *   steady      every cluster in the mask is lit continuously.
 *   multiplex   the clusters in the mask are lit only during this target's
 *               slot of the period, so a pistol can tell several targets in
 *               one room apart by when it sees the dots.
 *
 * The multiplex phase is taken from the shared clock once a time_mark has
 * arrived (cgbeacon_set_clock), so two modules in one room land on the same
 * window without talking to each other. Before the first time_mark the phase
 * runs on local time, which is enough for a single target on the bench.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CGBEACON_CLUSTERS 4

/* How many targets share one multiplex period. usb-protocol.md gives the
 * module a slot and a period but no slot count, so it is a build time value
 * here and an open question in D-009. */
#ifndef CGBEACON_DEFAULT_SLOTS
#define CGBEACON_DEFAULT_SLOTS 4
#endif

typedef struct {
    int gpio[CGBEACON_CLUSTERS];
    int slots;       /* targets per period, at least 1 */
    bool active_low; /* false for an IRLZ44N driven from the pin */
} cgbeacon_config_t;

#define CGBEACON_HELTEC_V2_CONFIG()                      \
    (cgbeacon_config_t)                                  \
    {                                                    \
        .gpio = {12, 13, 14, 25},                        \
        .slots = CGBEACON_DEFAULT_SLOTS,                 \
        .active_low = false,                             \
    }

esp_err_t cgbeacon_init(const cgbeacon_config_t *cfg);

/* The beacons message of usb-protocol.md section 2. */
esp_err_t cgbeacon_set(uint8_t mask, uint8_t mode, uint16_t period_ms, uint8_t slot);

void cgbeacon_get(uint8_t *mask, uint8_t *mode, uint16_t *period_ms, uint8_t *slot);

/* Anchors the multiplex phase to the shared clock of time_mark. */
void cgbeacon_set_clock(uint64_t unix_ms);

/* True while this target's clusters are lit. For the display and the bench. */
bool cgbeacon_lit(void);

/* All clusters off, whatever the mode. The mode is remembered. */
esp_err_t cgbeacon_blank(void);

#ifdef __cplusplus
}
#endif
