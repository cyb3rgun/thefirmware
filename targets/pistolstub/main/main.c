/* Target pistolstub, S01 bench.
 *
 * Stands in for the pistol until the ESP32-S3 board exists. It sends shot
 * broadcasts on the PRG button and on a timer, waits for the module's
 * unicast acknowledgement and resends up to three times, exactly as
 * concept.md principle 3 describes.
 *
 * There is no camera and no IMU here, so the aim point is synthetic. What
 * is real is the radio path, which is what S01 measures.
 *
 * It also keeps the round trip statistics the measurement table of
 * S01-B01 task 6 asks for, so the numbers come off the board rather than
 * out of a stopwatch. A run is 100 shots by default, and the summary is
 * printed when the run completes and shown on the display.
 *
 * Flash method, D-004:
 *   tools\cgflash.ps1 pistolstub COM7 build flash monitor
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "cgoled.h"
#include "cgproto.h"

static const char *TAG = "pistolstub";

/* A Kconfig bool is defined as 1 when it is on and not defined at all when
 * it is off, so the off case needs a value before it can be used in an
 * ordinary if. */
#ifndef CONFIG_CGPISTOL_AUTOFIRE
#define CONFIG_CGPISTOL_AUTOFIRE 0
#endif
#ifndef CONFIG_CGPISTOL_TRACE
#define CONFIG_CGPISTOL_TRACE 0
#endif

#define FW_MAJOR 0
#define FW_MINOR 1
#define FW_PATCH 0

/* The PRG button of the Heltec V2, low when pressed. */
#define BUTTON_GPIO 0
#define BUTTON_LONG_MS 1000

#define MAX_ATTEMPTS (1 + 3) /* the first send plus three resends */
#define SAMPLES_MAX 256

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static struct {
    uint8_t mac[6];
    uint16_t next_seq;

    uint32_t sent;    /* shots begun */
    uint32_t acked;   /* shots the module acknowledged */
    uint32_t lost;    /* shots still unacknowledged after every resend */
    uint32_t resends; /* extra transmissions, not shots */

    /* The module that answered, learned from the first acknowledgement.
     * concept.md section 3: the pistol fills target_mac once it has heard
     * an ack from the module. */
    uint8_t target_mac[6];
    bool have_target;
    int8_t last_rssi;

    uint32_t rtt_us[SAMPLES_MAX];
    uint32_t rtt_count;
    uint32_t rtt_total; /* how many were taken, even past the window */
    uint64_t rtt_sum;
    uint32_t rtt_min;
    uint32_t rtt_max;
} g;

/* Set by the callback, sent by the shooter task: a start has to be
 * acknowledged, and esp_now_send must not be called from the callback. */
static volatile bool s_ack_start_pending;

/* Handshake between the shooter task and the radio callback. */
static SemaphoreHandle_t s_ack_sem;
static volatile uint16_t s_waiting_seq;
static volatile bool s_ack_seen;

/* A run started over the radio by the module, D-014. The callback only
 * records what arrived; the shooter task does the work. */
static volatile bool s_start_pending;
static volatile uint32_t s_start_run_id;
static volatile uint16_t s_start_shots;
static volatile uint16_t s_start_interval_ms;
static volatile bool s_have_run_id;
static uint32_t s_last_run_id;
static uint8_t s_start_module[6];

/* ------------------------------------------------------------ statistics -- */

static void stats_reset(void)
{
    g.sent = 0;
    g.acked = 0;
    g.lost = 0;
    g.resends = 0;
    g.rtt_count = 0;
    g.rtt_total = 0;
    g.rtt_sum = 0;
    g.rtt_min = 0;
    g.rtt_max = 0;
    ESP_LOGI(TAG, "statistics reset, a new run starts");
}

static void stats_add(uint32_t rtt)
{
    if (g.rtt_count < SAMPLES_MAX) {
        g.rtt_us[g.rtt_count++] = rtt;
    } else {
        /* Keep the most recent window. */
        memmove(&g.rtt_us[0], &g.rtt_us[1], (SAMPLES_MAX - 1) * sizeof(g.rtt_us[0]));
        g.rtt_us[SAMPLES_MAX - 1] = rtt;
    }
    g.rtt_total++;
    g.rtt_sum += rtt;
    if (g.rtt_min == 0 || rtt < g.rtt_min) {
        g.rtt_min = rtt;
    }
    if (rtt > g.rtt_max) {
        g.rtt_max = rtt;
    }
}

static int compare_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a;
    const uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* Median and the 95th percentile of the samples held right now. */
static void stats_percentiles(uint32_t *median, uint32_t *p95)
{
    *median = 0;
    *p95 = 0;
    if (g.rtt_count == 0) {
        return;
    }

    static uint32_t sorted[SAMPLES_MAX];
    const uint32_t n = g.rtt_count;
    memcpy(sorted, g.rtt_us, n * sizeof(sorted[0]));
    qsort(sorted, n, sizeof(sorted[0]), compare_u32);

    *median = (n % 2 == 1) ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;

    /* Nearest rank, which is the reading that does not invent a sample. */
    uint32_t rank = (n * 95u + 99u) / 100u;
    if (rank == 0) {
        rank = 1;
    }
    *p95 = sorted[rank - 1];
}

static void stats_print(void)
{
    uint32_t median = 0;
    uint32_t p95 = 0;
    stats_percentiles(&median, &p95);

    const uint32_t mean = (g.rtt_total > 0) ? (uint32_t)(g.rtt_sum / g.rtt_total) : 0;
    const uint32_t loss_ppm = (g.sent > 0) ? (uint32_t)((uint64_t)g.lost * 1000000u / g.sent) : 0;

    ESP_LOGI(TAG, "----- run summary -----");
    ESP_LOGI(TAG, "sent %" PRIu32 "  acked %" PRIu32 "  lost %" PRIu32 "  resends %" PRIu32,
             g.sent, g.acked, g.lost, g.resends);
    ESP_LOGI(TAG,
             "rtt us: median %" PRIu32 "  p95 %" PRIu32 "  mean %" PRIu32 "  min %" PRIu32
             "  max %" PRIu32,
             median, p95, mean, g.rtt_min, g.rtt_max);
    ESP_LOGI(TAG, "loss %" PRIu32 ".%04" PRIu32 " percent, rssi %d", loss_ppm / 10000u,
             loss_ppm % 10000u, g.last_rssi);
    ESP_LOGI(TAG, "-----------------------");
}

/* ---------------------------------------------------------------- radio -- */

static void radio_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || data == NULL || len <= 0 || len > (int)CGPROTO_MAX_PACKET) {
        return;
    }
    if (!cgproto_check(data, (size_t)len)) {
        return;
    }
    if (data[0] == CGPROTO_START) {
        const cgproto_start_t *start = (const cgproto_start_t *)data;

        /* The module keeps sending start until it is acknowledged, so every
         * copy is acknowledged and only the first begins a run. The module's
         * address comes from the packet rather than from a field in it. */
        if (!s_have_run_id || start->run_id != s_last_run_id) {
            s_last_run_id = start->run_id;
            s_have_run_id = true;
            s_start_run_id = start->run_id;
            s_start_shots = start->shots;
            s_start_interval_ms = start->interval_ms;
            memcpy(s_start_module, info->src_addr, 6);
            s_start_pending = true;
        }
        s_ack_start_pending = true;
        return;
    }

    if (data[0] != CGPROTO_ACK) {
        return;
    }

    const cgproto_ack_t *ack = (const cgproto_ack_t *)data;
    if (!s_ack_seen && ack->pistol_seq == s_waiting_seq) {
        memcpy(g.target_mac, ack->module_mac, 6);
        g.have_target = true;
        g.last_rssi = (info->rx_ctrl != NULL) ? (int8_t)info->rx_ctrl->rssi : 0;
        s_ack_seen = true;

        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_ack_sem, &woken);
        if (woken) {
            portYIELD_FROM_ISR();
        }
    }
}

/* Builds a shot. The aim point walks across the frame so that a bench trace
 * shows movement instead of one repeated number. */
static void build_shot(cgproto_shot_t *shot, uint16_t seq)
{
    static uint16_t walk;

    memset(shot, 0, sizeof(*shot));
    shot->type = CGPROTO_SHOT;
    memcpy(shot->pistol_mac, g.mac, 6);
    shot->pistol_seq = seq;
    shot->slot = CONFIG_CGPISTOL_SLOT;
    if (g.have_target) {
        memcpy(shot->target_mac, g.target_mac, 6);
    }
    shot->ts_pistol_ms = (uint32_t)(esp_timer_get_time() / 1000);
    walk = (uint16_t)(walk + 1237u);
    shot->x = walk;
    shot->y = (uint16_t)(0xFFFFu - walk);
    shot->quat[0] = 32767; /* an identity rotation, there is no IMU here */
    shot->buttons = 0x01;  /* the trigger */
    if (CONFIG_CGPISTOL_SLOT == 0) {
        shot->flags |= CGPROTO_SHOT_FLAG_UNAIMED;
    }
    cgproto_seal(shot, sizeof(*shot));
}

/* One shot, with the resends concept.md principle 3 asks for. Returns true
 * when the module acknowledged it. */
static bool fire(void)
{
    cgproto_shot_t shot;
    const uint16_t seq = g.next_seq++;

    build_shot(&shot, seq);

    s_waiting_seq = seq;
    s_ack_seen = false;
    xSemaphoreTake(s_ack_sem, 0); /* clear a stale give */

    g.sent++;
    const int64_t first_us = esp_timer_get_time();

    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            g.resends++;
        }

        const esp_err_t err = esp_now_send(BROADCAST, (const uint8_t *)&shot, sizeof(shot));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(err));
        }

        if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(CONFIG_CGPISTOL_ACK_TIMEOUT_MS)) == pdTRUE) {
            const uint32_t rtt = (uint32_t)(esp_timer_get_time() - first_us);
            g.acked++;
            stats_add(rtt);
            if (CONFIG_CGPISTOL_TRACE) {
                ESP_LOGI(TAG, "seq %u acked after %d send(s), %" PRIu32 " us", (unsigned)seq,
                         attempt + 1, rtt);
            }
            return true;
        }
    }

    g.lost++;
    ESP_LOGW(TAG, "seq %u lost after %d sends", (unsigned)seq, MAX_ATTEMPTS);
    return false;
}

static esp_err_t peer_ensure(const uint8_t mac[6])
{
    esp_now_peer_info_t peer = {
        .channel = CONFIG_CGPISTOL_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, mac, 6);

    const esp_err_t err = esp_now_add_peer(&peer);
    return (err == ESP_ERR_ESPNOW_EXIST) ? ESP_OK : err;
}

/* concept.md section 3: start is acknowledged by the stub and resent by the
 * module until it is. */
static void send_start_ack(uint32_t run_id, const uint8_t module_mac[6])
{
    if (peer_ensure(module_mac) != ESP_OK) {
        return;
    }

    cgproto_start_ack_t ack = {
        .type = CGPROTO_START_ACK,
        .run_id = run_id,
    };
    memcpy(ack.pistol_mac, g.mac, 6);
    cgproto_seal(&ack, sizeof(ack));
    esp_now_send(module_mac, (const uint8_t *)&ack, sizeof(ack));
}

/* Bench only, D-014. The run's numbers go back over the radio, so the
 * pistol needs no cable to the PC and can sit on a power bank at 5 m. */
static void send_report(uint32_t run_id, const uint8_t module_mac[6])
{
    uint32_t median = 0;
    uint32_t p95 = 0;
    stats_percentiles(&median, &p95);

    cgproto_report_t report = {
        .type = CGPROTO_REPORT,
        .run_id = run_id,
        .sent = (uint16_t)g.sent,
        .acked = (uint16_t)g.acked,
        .resends = (uint16_t)g.resends,
        .lost = (uint16_t)g.lost,
        .median_us = median,
        .p95_us = p95,
        .mean_us = (g.rtt_total > 0) ? (uint32_t)(g.rtt_sum / g.rtt_total) : 0,
        .min_us = g.rtt_min,
        .max_us = g.rtt_max,
        .rssi = g.last_rssi,
    };
    memcpy(report.pistol_mac, g.mac, 6);
    cgproto_seal(&report, sizeof(report));

    if (peer_ensure(module_mac) != ESP_OK) {
        ESP_LOGW(TAG, "cannot add the module as a peer");
        return;
    }

    /* There is no acknowledgement for a report either, so it goes three
     * times. The module keeps the first and drops the repeats by run id. */
    for (int i = 0; i < 3; i++) {
        esp_now_send(module_mac, (const uint8_t *)&report, sizeof(report));
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    ESP_LOGI(TAG, "report for run %" PRIu32 " sent to the module", run_id);
}

static void say_hello(void)
{
    cgproto_hello_t hello = {
        .type = CGPROTO_HELLO,
        .fw = {FW_MAJOR, FW_MINOR, FW_PATCH},
    };
    memcpy(hello.pistol_mac, g.mac, 6);
    cgproto_seal(&hello, sizeof(hello));

    esp_now_send(BROADCAST, (const uint8_t *)&hello, sizeof(hello));
    ESP_LOGI(TAG, "hello broadcast, for pairing by shooting");
}

/* ----------------------------------------------------------------- tasks -- */

static void shooter_task(void *arg)
{
    (void)arg;

    /* Let the module finish booting before the first shot of a run. */
    vTaskDelay(pdMS_TO_TICKS(1500));

    for (;;) {
        if (s_ack_start_pending) {
            s_ack_start_pending = false;
            send_start_ack(s_start_run_id, s_start_module);
        }

        if (s_start_pending) {
            s_start_pending = false;

            const uint32_t run_id = s_start_run_id;
            const uint16_t shots = s_start_shots;
            const uint16_t interval_ms =
                (s_start_interval_ms > 0) ? s_start_interval_ms : 200u;
            uint8_t module_mac[6];
            memcpy(module_mac, s_start_module, 6);

            ESP_LOGI(TAG, "run %" PRIu32 " from the module: %u shots every %u ms", run_id,
                     (unsigned)shots, (unsigned)interval_ms);
            stats_reset();

            for (uint16_t i = 0; i < shots; i++) {
                fire();
                vTaskDelay(pdMS_TO_TICKS(interval_ms));
            }

            stats_print();
            send_report(run_id, module_mac);
            continue;
        }

        if (!CONFIG_CGPISTOL_AUTOFIRE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        fire();

        if (CONFIG_CGPISTOL_RUN_SHOTS > 0 && g.sent > 0 &&
            g.sent % CONFIG_CGPISTOL_RUN_SHOTS == 0) {
            stats_print();
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_CGPISTOL_RATE_MS));
    }
}

static void button_task(void *arg)
{
    (void)arg;
    bool was_down = false;
    int64_t down_at = 0;
    bool long_done = false;

    for (;;) {
        const bool down = gpio_get_level(BUTTON_GPIO) == 0;
        const int64_t now = esp_timer_get_time();

        if (down && !was_down) {
            down_at = now;
            long_done = false;
        } else if (down && !long_done && (now - down_at) > (int64_t)BUTTON_LONG_MS * 1000) {
            /* Held: end this run and start a fresh one. */
            stats_print();
            stats_reset();
            long_done = true;
        } else if (!down && was_down && !long_done) {
            /* Tapped: one shot, whatever the timer is doing. */
            fire();
        }

        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void display_task(void *arg)
{
    (void)arg;
    if (!cgoled_present()) {
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        uint32_t median = 0;
        uint32_t p95 = 0;
        stats_percentiles(&median, &p95);

        cgoled_clear();
        cgoled_text(0, 0, "CYB3RGUN PISTOLSTUB");
        cgoled_rule(1);
        cgoled_printf(0, 2, "SENT  %" PRIu32, g.sent);
        cgoled_printf(0, 3, "ACKED %" PRIu32, g.acked);
        cgoled_printf(0, 4, "RESEND %" PRIu32 " LOST %" PRIu32, g.resends, g.lost);
        cgoled_printf(0, 5, "MED %" PRIu32 "US", median);
        cgoled_printf(0, 6, "P95 %" PRIu32 "US", p95);
        if (g.have_target) {
            cgoled_printf(0, 7, "MOD %02X%02X%02X RSSI %d", g.target_mac[3], g.target_mac[4],
                          g.target_mac[5], g.last_rssi);
        } else {
            cgoled_text(0, 7, "NO MODULE YET");
        }
        cgoled_flush();

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* ------------------------------------------------------------------ boot -- */

static void radio_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_CGPISTOL_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(radio_recv_cb));

    esp_now_peer_info_t peer = {
        .channel = CONFIG_CGPISTOL_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, BROADCAST, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    g.next_seq = 1;
    ESP_ERROR_CHECK(esp_read_mac(g.mac, ESP_MAC_WIFI_STA));

    s_ack_sem = xSemaphoreCreateBinary();
    configASSERT(s_ack_sem != NULL);

    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    const cgoled_config_t oled = CGOLED_HELTEC_V2_CONFIG();
    if (cgoled_init(&oled) != ESP_OK) {
        ESP_LOGW(TAG, "no OLED, carrying on without it");
    }

    radio_start();

    /* concept.md section 3: hello goes out when the trigger is held on power
     * up, which is how a pistol is paired by shooting. */
    if (gpio_get_level(BUTTON_GPIO) == 0) {
        say_hello();
    }

    xTaskCreate(shooter_task, "cg_shooter", 4096, NULL, 10, NULL);
    xTaskCreate(button_task, "cg_button", 3072, NULL, 9, NULL);
    xTaskCreate(display_task, "cg_display", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "pistolstub %02X:%02X:%02X:%02X:%02X:%02X, channel %d, slot %d, fw %d.%d.%d",
             g.mac[0], g.mac[1], g.mac[2], g.mac[3], g.mac[4], g.mac[5], CONFIG_CGPISTOL_CHANNEL,
             CONFIG_CGPISTOL_SLOT, FW_MAJOR, FW_MINOR, FW_PATCH);
    ESP_LOGI(TAG, "autofire %s, every %d ms, ack timeout %d ms, run %d shots",
             CONFIG_CGPISTOL_AUTOFIRE ? "on" : "off", CONFIG_CGPISTOL_RATE_MS,
             CONFIG_CGPISTOL_ACK_TIMEOUT_MS, CONFIG_CGPISTOL_RUN_SHOTS);
    ESP_LOGI(TAG, "PRG taps fire one shot, holding PRG ends the run and resets");
}
