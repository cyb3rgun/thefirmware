/* cgbeacon: the four beacon clusters and the multiplex clock. */

#include "cgbeacon.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "cgusb.h" /* CGUSB_BEACON_MODE_ */

static const char *TAG = "cgbeacon";

static struct {
    cgbeacon_config_t cfg;
    bool ready;

    uint8_t mask;
    uint8_t mode;
    uint16_t period_ms;
    uint8_t slot;

    bool lit;
    bool blanked;

    esp_timer_handle_t timer;

    /* Shared clock anchor: unix_ms was true at local time anchor_us. */
    uint64_t anchor_unix_ms;
    int64_t anchor_us;
    bool have_clock;
} s;

static void drive(uint8_t mask)
{
    for (int i = 0; i < CGBEACON_CLUSTERS; i++) {
        const bool on = (mask & (1u << i)) != 0;
        const int level = s.cfg.active_low ? (on ? 0 : 1) : (on ? 1 : 0);
        gpio_set_level((gpio_num_t)s.cfg.gpio[i], level);
    }
}

/* Milliseconds on the shared clock when one is known, on local time until
 * then. The multiplex phase is taken from this, so two modules that have
 * heard the same time_mark light their clusters in the same window. */
static uint64_t clock_ms(void)
{
    const int64_t now_us = esp_timer_get_time();

    if (s.have_clock) {
        return s.anchor_unix_ms + (uint64_t)((now_us - s.anchor_us) / 1000);
    }
    return (uint64_t)(now_us / 1000);
}

static void apply_multiplex(void)
{
    const int slots = (s.cfg.slots > 0) ? s.cfg.slots : 1;
    const uint32_t period = (s.period_ms > 0) ? s.period_ms : 1u;
    const uint32_t width = (period >= (uint32_t)slots) ? period / (uint32_t)slots : 1u;

    /* Slots in the room plan are counted from 1. A slot of 0 in multiplex
     * mode is a plan that has not reached this module yet, so it stays dark
     * rather than colliding with slot 1. */
    if (s.slot == 0) {
        if (s.lit) {
            s.lit = false;
            drive(0);
        }
        return;
    }

    const uint32_t phase = (uint32_t)(clock_ms() % period);
    const uint32_t window = phase / width;
    const bool mine = (window == (uint32_t)(s.slot - 1u));

    if (mine != s.lit) {
        s.lit = mine;
        drive(mine ? s.mask : 0u);
    }
}

static void tick(void *arg)
{
    (void)arg;
    if (s.blanked || s.mode != CGUSB_BEACON_MODE_MULTIPLEX) {
        return;
    }
    apply_multiplex();
}

static void stop_timer(void)
{
    if (s.timer != NULL) {
        esp_timer_stop(s.timer);
    }
}

static esp_err_t start_timer(void)
{
    const int slots = (s.cfg.slots > 0) ? s.cfg.slots : 1;
    const uint32_t period = (s.period_ms > 0) ? s.period_ms : 1u;
    uint32_t width_us = (period * 1000u) / (uint32_t)slots;

    /* Two ticks per window, so a window is never stepped over when the
     * timer runs a little late. */
    uint64_t tick_us = width_us / 2u;
    if (tick_us < 1000u) {
        tick_us = 1000u;
    }

    stop_timer();
    return esp_timer_start_periodic(s.timer, tick_us);
}

esp_err_t cgbeacon_init(const cgbeacon_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s, 0, sizeof(s));
    s.cfg = *cfg;
    if (s.cfg.slots < 1) {
        s.cfg.slots = 1;
    }

    uint64_t pins = 0;
    for (int i = 0; i < CGBEACON_CLUSTERS; i++) {
        pins |= 1ULL << s.cfg.gpio[i];
    }

    const gpio_config_t io = {
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    drive(0);

    const esp_timer_create_args_t args = {
        .callback = tick,
        .name = "cgbeacon",
        .dispatch_method = ESP_TIMER_TASK,
    };
    err = esp_timer_create(&args, &s.timer);
    if (err != ESP_OK) {
        return err;
    }

    s.mode = CGUSB_BEACON_MODE_STEADY;
    s.period_ms = 40;
    s.ready = true;

    ESP_LOGI(TAG, "clusters on GPIO %d %d %d %d, %d slots per period",
             s.cfg.gpio[0], s.cfg.gpio[1], s.cfg.gpio[2], s.cfg.gpio[3], s.cfg.slots);
    return ESP_OK;
}

esp_err_t cgbeacon_set(uint8_t mask, uint8_t mode, uint16_t period_ms, uint8_t slot)
{
    if (!s.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode != CGUSB_BEACON_MODE_STEADY && mode != CGUSB_BEACON_MODE_MULTIPLEX) {
        return ESP_ERR_INVALID_ARG;
    }

    s.mask = (uint8_t)(mask & 0x0Fu);
    s.mode = mode;
    s.period_ms = (period_ms > 0) ? period_ms : 40u;
    s.slot = slot;
    s.blanked = false;

    if (mode == CGUSB_BEACON_MODE_STEADY) {
        stop_timer();
        s.lit = (s.mask != 0);
        drive(s.mask);
        ESP_LOGI(TAG, "steady, mask 0x%02X", s.mask);
        return ESP_OK;
    }

    s.lit = false;
    drive(0);
    const esp_err_t err = start_timer();
    ESP_LOGI(TAG, "multiplex, mask 0x%02X, period %u ms, slot %u of %d", s.mask,
             (unsigned)s.period_ms, (unsigned)s.slot, s.cfg.slots);
    return err;
}

void cgbeacon_get(uint8_t *mask, uint8_t *mode, uint16_t *period_ms, uint8_t *slot)
{
    if (mask != NULL) {
        *mask = s.mask;
    }
    if (mode != NULL) {
        *mode = s.mode;
    }
    if (period_ms != NULL) {
        *period_ms = s.period_ms;
    }
    if (slot != NULL) {
        *slot = s.slot;
    }
}

void cgbeacon_set_clock(uint64_t unix_ms)
{
    s.anchor_unix_ms = unix_ms;
    s.anchor_us = esp_timer_get_time();
    s.have_clock = true;
}

bool cgbeacon_lit(void)
{
    return s.lit;
}

esp_err_t cgbeacon_blank(void)
{
    if (!s.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    s.blanked = true;
    s.lit = false;
    drive(0);
    return ESP_OK;
}
