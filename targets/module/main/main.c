/* Target module, S01 bench.
 *
 * Drives the four beacon clusters, hears pistols over ESP-NOW, acknowledges
 * them by unicast, and forwards the shot to the core over the USB link of
 * docs/usb-protocol.md. See docs/concept.md sections 2 to 4.
 *
 * Flash method, D-004:
 *   tools\cgflash.ps1 module COM6 build flash monitor
 */

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "cgbeacon.h"
#include "cgoled.h"
#include "cgproto.h"
#include "cgusb.h"
#include "cgusb_link.h"

static const char *TAG = "module";

#define FW_MAJOR 0
#define FW_MINOR 1
#define FW_PATCH 0

/* usb-protocol.md section 4: the core acknowledges within 500 ms or the
 * module resends, up to three times, then drops the shot. */
#define CORE_ACK_TIMEOUT_MS 500
#define CORE_MAX_RESENDS 3
#define PENDING_MAX 16

/* A pistol resends until it is acknowledged, so the same shot arrives more
 * than once. Every copy is acknowledged, only the first is forwarded. */
#define SEEN_MAX 32
#define SEEN_TTL_US (5 * 1000 * 1000)

/* ESP-NOW holds 20 peers. The broadcast address takes one. */
#define PEER_MAX 15

#define NVS_NAMESPACE "cgmodule"
#define NVS_KEY_CHANNEL "channel"

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct {
    bool used;
    uint16_t seq;
    cgusb_shot_t frame;
    int64_t sent_us;
    int retries;
} pending_t;

typedef struct {
    uint8_t mac[6];
    uint16_t seq;
    int64_t at_us;
} seen_t;

typedef struct {
    uint8_t src[6];
    int8_t rssi;
    uint8_t len;
    uint8_t data[CGPROTO_MAX_PACKET];
} radio_rx_t;

static struct {
    uint8_t mac[6];
    uint8_t channel;
    uint8_t slot;
    uint16_t next_seq;
    bool core_hello;

    uint32_t shots_heard;   /* distinct shots heard on the radio */
    uint32_t shots_acked;   /* shots the core acknowledged, D-010 */
    uint32_t shots_dropped; /* the core never acknowledged, after the resends */
    uint32_t core_resends;
    uint32_t radio_acks;
    uint32_t duplicates;
    uint32_t pistols_seen;

    uint8_t last_pistol[6];
    int8_t last_rssi;
    int64_t last_time_mark_us;

    /* bench only, D-014 */
    uint16_t bench_run_id;
    uint32_t bench_heard_at_start;
    uint32_t bench_acked_at_start;
} g;

static pending_t s_pending[PENDING_MAX];
static SemaphoreHandle_t s_pending_lock;
static seen_t s_seen[SEEN_MAX];
static uint8_t s_peers[PEER_MAX][6];
static int s_peer_count;
static int s_peer_next;
static QueueHandle_t s_radio_q;

/* ------------------------------------------------------------------ nvs -- */

static uint8_t channel_load(uint8_t fallback)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return fallback;
    }

    uint8_t value = fallback;
    if (nvs_get_u8(handle, NVS_KEY_CHANNEL, &value) != ESP_OK) {
        value = fallback;
    }
    nvs_close(handle);
    return value;
}

static void channel_store(uint8_t channel)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_u8(handle, NVS_KEY_CHANNEL, channel);
    nvs_commit(handle);
    nvs_close(handle);
}

/* ---------------------------------------------------------------- peers -- */

/* concept.md principle 3: pistols are never pre registered. The peer is
 * added when a shot arrives, because ESP-IDF needs one to send a unicast
 * acknowledgement, and the oldest is evicted when the table is full. */
static esp_err_t peer_ensure(const uint8_t mac[6])
{
    for (int i = 0; i < s_peer_count; i++) {
        if (memcmp(s_peers[i], mac, 6) == 0) {
            return ESP_OK;
        }
    }

    esp_now_peer_info_t peer = {
        .channel = g.channel,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, mac, 6);

    if (s_peer_count < PEER_MAX) {
        const esp_err_t err = esp_now_add_peer(&peer);
        if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) {
            memcpy(s_peers[s_peer_count++], mac, 6);
            return ESP_OK;
        }
        return err;
    }

    /* Full: drop the oldest entry and take its place. */
    esp_now_del_peer(s_peers[s_peer_next]);
    const esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        return err;
    }
    memcpy(s_peers[s_peer_next], mac, 6);
    s_peer_next = (s_peer_next + 1) % PEER_MAX;
    return ESP_OK;
}

/* ----------------------------------------------------------- duplicates -- */

static bool seen_before(const uint8_t mac[6], uint16_t seq)
{
    const int64_t now = esp_timer_get_time();
    int free_index = -1;
    int oldest = 0;

    for (int i = 0; i < SEEN_MAX; i++) {
        if (s_seen[i].at_us == 0) {
            if (free_index < 0) {
                free_index = i;
            }
            continue;
        }
        if (now - s_seen[i].at_us > SEEN_TTL_US) {
            s_seen[i].at_us = 0;
            if (free_index < 0) {
                free_index = i;
            }
            continue;
        }
        if (s_seen[i].seq == seq && memcmp(s_seen[i].mac, mac, 6) == 0) {
            s_seen[i].at_us = now;
            return true;
        }
        if (s_seen[i].at_us < s_seen[oldest].at_us) {
            oldest = i;
        }
    }

    const int slot = (free_index >= 0) ? free_index : oldest;
    memcpy(s_seen[slot].mac, mac, 6);
    s_seen[slot].seq = seq;
    s_seen[slot].at_us = now;
    return false;
}

/* --------------------------------------------------------- forwarding -- */

static void forward_send(pending_t *entry)
{
    entry->sent_us = esp_timer_get_time();
    cgusb_link_send(CGUSB_MSG_SHOT, &entry->frame, sizeof(entry->frame));
}

static void forward_queue(const cgusb_shot_t *shot)
{
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);

    pending_t *entry = NULL;
    for (int i = 0; i < PENDING_MAX; i++) {
        if (!s_pending[i].used) {
            entry = &s_pending[i];
            break;
        }
    }

    if (entry == NULL) {
        /* The core is not keeping up. Drop the oldest so the newest shot,
         * which is the one a player is waiting for, still has a chance. */
        int oldest = 0;
        for (int i = 1; i < PENDING_MAX; i++) {
            if (s_pending[i].sent_us < s_pending[oldest].sent_us) {
                oldest = i;
            }
        }
        entry = &s_pending[oldest];
        g.shots_dropped++;
        ESP_LOGW(TAG, "forward buffer full, dropped seq %u", (unsigned)entry->seq);
    }

    entry->used = true;
    entry->seq = shot->seq;
    entry->frame = *shot;
    entry->retries = 0;
    forward_send(entry);

    xSemaphoreGive(s_pending_lock);
}

static void forward_ack(uint16_t seq)
{
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    for (int i = 0; i < PENDING_MAX; i++) {
        if (s_pending[i].used && s_pending[i].seq == seq) {
            s_pending[i].used = false;
            g.shots_acked++;
            break;
        }
    }
    xSemaphoreGive(s_pending_lock);
}

static void forward_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!g.core_hello) {
            continue;
        }

        const int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_pending_lock, portMAX_DELAY);
        for (int i = 0; i < PENDING_MAX; i++) {
            pending_t *entry = &s_pending[i];
            if (!entry->used) {
                continue;
            }
            if (now - entry->sent_us < (int64_t)CORE_ACK_TIMEOUT_MS * 1000) {
                continue;
            }
            if (entry->retries >= CORE_MAX_RESENDS) {
                entry->used = false;
                g.shots_dropped++;
                const uint8_t detail[2] = {(uint8_t)(entry->seq & 0xFF),
                                           (uint8_t)(entry->seq >> 8)};
                cgusb_link_send_error(CGUSB_ERR_SHOT_DROPPED, detail, sizeof(detail));
                ESP_LOGW(TAG, "shot seq %u dropped, the core never acknowledged it",
                         (unsigned)entry->seq);
                continue;
            }
            entry->retries++;
            g.core_resends++;
            forward_send(entry);
        }
        xSemaphoreGive(s_pending_lock);
    }
}

/* ---------------------------------------------------------------- radio -- */

static void radio_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || data == NULL || len <= 0 || len > (int)CGPROTO_MAX_PACKET) {
        return;
    }

    /* The callback runs on the WiFi task, so it only copies and hands over.
     * Sending from here risks a deadlock inside ESP-NOW. */
    radio_rx_t rx;
    memcpy(rx.src, info->src_addr, 6);
    rx.rssi = (info->rx_ctrl != NULL) ? (int8_t)info->rx_ctrl->rssi : 0;
    rx.len = (uint8_t)len;
    memcpy(rx.data, data, (size_t)len);

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_radio_q, &rx, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static bool shot_is_ours(const cgproto_shot_t *shot)
{
    static const uint8_t ZERO_MAC[6] = {0};

    /* Once the pistol has heard this module's ack, it addresses it by mac. */
    if (memcmp(shot->target_mac, ZERO_MAC, 6) != 0) {
        return memcmp(shot->target_mac, g.mac, 6) == 0;
    }
    /* concept.md section 3: a shot with slot 0 is acknowledged by every
     * module that hears it and forwarded as unaimed. */
    if (shot->slot == CGPROTO_SLOT_NONE) {
        return true;
    }
    return shot->slot == g.slot;
}

static void radio_ack(const uint8_t pistol_mac[6], uint16_t pistol_seq)
{
    if (peer_ensure(pistol_mac) != ESP_OK) {
        ESP_LOGW(TAG, "no peer slot for the pistol, cannot acknowledge");
        return;
    }

    cgproto_ack_t ack = {
        .type = CGPROTO_ACK,
        .pistol_seq = pistol_seq,
        .slot = g.slot,
    };
    memcpy(ack.module_mac, g.mac, 6);
    cgproto_seal(&ack, sizeof(ack));

    if (esp_now_send(pistol_mac, (const uint8_t *)&ack, sizeof(ack)) == ESP_OK) {
        g.radio_acks++;
    }
}

static void handle_shot(const radio_rx_t *rx)
{
    const cgproto_shot_t *shot = (const cgproto_shot_t *)rx->data;

    if (!shot_is_ours(shot)) {
        return;
    }

    /* usb-protocol.md section 4: acknowledge the pistol first, always,
     * before anything else is done with the shot. */
    radio_ack(shot->pistol_mac, shot->pistol_seq);

    memcpy(g.last_pistol, shot->pistol_mac, 6);
    g.last_rssi = rx->rssi;

    if (seen_before(shot->pistol_mac, shot->pistol_seq)) {
        /* A resend from a pistol that missed the acknowledgement. It has now
         * been acknowledged again, and it is not a new shot. */
        g.duplicates++;
        return;
    }

    g.shots_heard++;

    if (!g.core_hello) {
        /* usb-protocol.md section 1: the module never speaks first. */
        ESP_LOGW(TAG, "shot heard but the core has not said hello, not forwarding");
        return;
    }

    cgusb_shot_t frame = {
        .seq = g.next_seq++,
        .pistol_seq = shot->pistol_seq,
        .slot = shot->slot,
        .flags = shot->flags,
        .ts_pistol_ms = shot->ts_pistol_ms,
        .x = shot->x,
        .y = shot->y,
        .buttons = shot->buttons,
        .rssi = rx->rssi,
    };
    memcpy(frame.pistol_id, shot->pistol_mac, 6);
    memcpy(frame.quat, shot->quat, sizeof(frame.quat));

    /* A shot that identified no beacons is scored as a miss by the core. */
    if (shot->slot == CGPROTO_SLOT_NONE) {
        frame.flags |= CGUSB_SHOT_FLAG_UNAIMED;
    }

    forward_queue(&frame);
}

/* Bench only, D-014. The pistol sits on a power bank with no cable to the
 * PC, so the run is started over the radio and its result comes back the
 * same way, and everything is read on the module's port. */
static void bench_start(uint16_t shots, uint16_t rate_ms)
{
    g.bench_run_id++;

    cgproto_start_t start = {
        .type = CGPROTO_START,
        .run_id = g.bench_run_id,
        .shots = shots,
        .rate_ms = rate_ms,
    };
    memcpy(start.module_mac, g.mac, 6);
    cgproto_seal(&start, sizeof(start));

    /* There is no acknowledgement for start, and a lost one stalls the
     * bench, so it goes out three times. The pistol takes the first copy
     * and ignores the rest by run id. */
    for (int i = 0; i < 3; i++) {
        esp_now_send(BROADCAST, (const uint8_t *)&start, sizeof(start));
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* The counters below are what the run will be compared against. */
    g.bench_heard_at_start = g.shots_heard;
    g.bench_acked_at_start = g.shots_acked;

    ESP_LOGI(TAG, "bench run %u started, %u shots every %u ms", (unsigned)g.bench_run_id,
             (unsigned)shots, (unsigned)rate_ms);
}

static void handle_report(const radio_rx_t *rx)
{
    const cgproto_report_t *r = (const cgproto_report_t *)rx->data;

    if (r->run_id != g.bench_run_id) {
        ESP_LOGW(TAG, "report for run %u arrived late, this is run %u",
                 (unsigned)r->run_id, (unsigned)g.bench_run_id);
        return;
    }

    /* The status line asked for on COM7. It is plain text, so it sits
     * between frames and the host tool passes it through (D-006). */
    ESP_LOGI(TAG,
             "REPORT run=%u pistol=%02X:%02X:%02X:%02X:%02X:%02X sent=%" PRIu32
             " acked=%" PRIu32 " resends=%" PRIu32 " lost=%" PRIu32 " median_us=%" PRIu32
             " p95_us=%" PRIu32 " mean_us=%" PRIu32 " min_us=%" PRIu32 " max_us=%" PRIu32
             " rssi_pistol=%d rssi_module=%d heard=%" PRIu32 " forwarded=%" PRIu32,
             (unsigned)r->run_id, r->pistol_mac[0], r->pistol_mac[1], r->pistol_mac[2],
             r->pistol_mac[3], r->pistol_mac[4], r->pistol_mac[5], r->sent, r->acked,
             r->resends, r->lost, r->median_us, r->p95_us, r->mean_us, r->min_us, r->max_us,
             r->rssi, rx->rssi, g.shots_heard - g.bench_heard_at_start,
             g.shots_acked - g.bench_acked_at_start);

    if (!g.core_hello) {
        return;
    }

    const cgusb_bench_report_t out = {
        .run_id = r->run_id,
        .sent = r->sent,
        .acked = r->acked,
        .resends = r->resends,
        .lost = r->lost,
        .median_us = r->median_us,
        .p95_us = r->p95_us,
        .mean_us = r->mean_us,
        .min_us = r->min_us,
        .max_us = r->max_us,
        .rssi_at_pistol = r->rssi,
        .rssi_at_module = rx->rssi,
    };
    cgusb_bench_report_t frame = out;
    memcpy(frame.pistol_id, r->pistol_mac, 6);
    cgusb_link_send(CGUSB_MSG_BENCH_REPORT, &frame, sizeof(frame));
}

static void handle_hello(const radio_rx_t *rx)
{
    const cgproto_hello_t *hello = (const cgproto_hello_t *)rx->data;

    g.pistols_seen++;
    memcpy(g.last_pistol, hello->pistol_mac, 6);
    g.last_rssi = rx->rssi;
    ESP_LOGI(TAG, "pistol %02X:%02X:%02X:%02X:%02X:%02X fw %u.%u.%u, rssi %d",
             hello->pistol_mac[0], hello->pistol_mac[1], hello->pistol_mac[2],
             hello->pistol_mac[3], hello->pistol_mac[4], hello->pistol_mac[5],
             hello->fw[0], hello->fw[1], hello->fw[2], rx->rssi);

    if (!g.core_hello) {
        return;
    }

    cgusb_pistol_seen_t seen = {.rssi = rx->rssi};
    memcpy(seen.pistol_id, hello->pistol_mac, 6);
    cgusb_link_send(CGUSB_MSG_PISTOL_SEEN, &seen, sizeof(seen));
}

static void radio_task(void *arg)
{
    (void)arg;
    radio_rx_t rx;

    for (;;) {
        if (xQueueReceive(s_radio_q, &rx, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!cgproto_check(rx.data, rx.len)) {
            /* Another system, another version, or a bad moment on the air. */
            continue;
        }
        switch (rx.data[0]) {
        case CGPROTO_SHOT:
            handle_shot(&rx);
            break;
        case CGPROTO_HELLO:
            handle_hello(&rx);
            break;
        case CGPROTO_REPORT:
            handle_report(&rx);
            break;
        default:
            break;
        }
    }
}

/* ----------------------------------------------------------------- core -- */

static void send_status(void)
{
    if (!g.core_hello) {
        return;
    }

    uint8_t mask = 0;
    uint8_t mode = 0;
    uint16_t period = 0;
    uint8_t slot = 0;
    cgbeacon_get(&mask, &mode, &period, &slot);

    const cgusb_status_t status = {
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000),
        .channel = g.channel,
        .beacons_mask = mask,
        .mode = mode,
        .temp = 0, /* the ESP32 D0WD has no usable internal sensor, D-011 */
        .free_heap = (uint32_t)esp_get_free_heap_size(),
        .shots_heard = g.shots_heard,
        .shots_acked = g.shots_acked,
    };
    cgusb_link_send(CGUSB_MSG_STATUS, &status, sizeof(status));
}

static void set_channel(uint8_t channel)
{
    if (channel < 1 || channel > 13) {
        cgusb_link_send_error(CGUSB_ERR_RADIO, &channel, 1);
        return;
    }
    if (channel == g.channel) {
        return;
    }

    g.channel = channel;
    channel_store(channel);

    /* The peers carry the channel, so they go with it. */
    for (int i = 0; i < s_peer_count; i++) {
        esp_now_del_peer(s_peers[i]);
    }
    s_peer_count = 0;
    s_peer_next = 0;

    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    esp_now_peer_info_t peer = {.channel = channel, .ifidx = WIFI_IF_STA, .encrypt = false};
    memcpy(peer.peer_addr, BROADCAST, 6);
    esp_now_del_peer(BROADCAST);
    esp_now_add_peer(&peer);

    ESP_LOGI(TAG, "channel %u", (unsigned)channel);
}

static void relay_time_mark(uint64_t unix_ms)
{
    cgbeacon_set_clock(unix_ms);

    /* usb-protocol.md section 4: out over the radio at most once a second. */
    const int64_t now = esp_timer_get_time();
    if (g.last_time_mark_us != 0 && now - g.last_time_mark_us < 1000000) {
        return;
    }
    g.last_time_mark_us = now;

    cgproto_time_mark_t mark = {.type = CGPROTO_TIME_MARK, .unix_ms = unix_ms};
    cgproto_seal(&mark, sizeof(mark));
    esp_now_send(BROADCAST, (const uint8_t *)&mark, sizeof(mark));
}

static void on_core_frame(uint8_t type, const uint8_t *payload, size_t len, void *ctx)
{
    (void)ctx;

    switch (type) {
    case CGUSB_MSG_HELLO: {
        const cgusb_hello_t *hello = (const cgusb_hello_t *)payload;
        if (hello->proto != CGUSB_PROTO_VERSION) {
            const uint8_t detail[2] = {hello->proto, CGUSB_PROTO_VERSION};
            g.core_hello = true; /* so the complaint can be sent at all */
            cgusb_link_send_error(CGUSB_ERR_PROTO_VERSION, detail, sizeof(detail));
            g.core_hello = false;
            ESP_LOGE(TAG, "the core speaks version %u, this module speaks %u", hello->proto,
                     CGUSB_PROTO_VERSION);
            return;
        }
        g.core_hello = true;

        cgusb_hello_ack_t ack = {
            .proto = CGUSB_PROTO_VERSION,
            .fw = {FW_MAJOR, FW_MINOR, FW_PATCH},
            .chip = CGUSB_CHIP_ESP32,
        };
        memcpy(ack.mac, g.mac, 6);
        cgusb_link_send(CGUSB_MSG_HELLO_ACK, &ack, sizeof(ack));
        ESP_LOGI(TAG, "core session open");
        break;
    }
    case CGUSB_MSG_BEACONS: {
        const cgusb_beacons_t *b = (const cgusb_beacons_t *)payload;
        g.slot = b->slot;
        if (cgbeacon_set(b->mask, b->mode, b->period_ms, b->slot) != ESP_OK) {
            const uint8_t detail[2] = {b->mask, b->mode};
            cgusb_link_send_error(CGUSB_ERR_BAD_LENGTH, detail, sizeof(detail));
        }
        break;
    }
    case CGUSB_MSG_TIME_MARK: {
        const cgusb_time_mark_t *mark = (const cgusb_time_mark_t *)payload;
        relay_time_mark(mark->unix_ms);
        break;
    }
    case CGUSB_MSG_ACK: {
        const cgusb_ack_t *ack = (const cgusb_ack_t *)payload;
        forward_ack(ack->seq);
        break;
    }
    case CGUSB_MSG_CHANNEL: {
        const cgusb_channel_t *ch = (const cgusb_channel_t *)payload;
        set_channel(ch->channel);
        break;
    }
    case CGUSB_MSG_STATUS_REQ:
        send_status();
        break;
    case CGUSB_MSG_BENCH_START: {
        const cgusb_bench_start_t *b = (const cgusb_bench_start_t *)payload;
        bench_start(b->shots, b->rate_ms);
        break;
    }
    case CGUSB_MSG_REBOOT:
        ESP_LOGW(TAG, "reboot asked for by the core");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
        break;
    default:
        cgusb_link_send_error(CGUSB_ERR_UNKNOWN_TYPE, &type, 1);
        break;
    }
    (void)len;
}

static void status_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_CGMODULE_STATUS_PERIOD_S * 1000));
        send_status();
    }
}

/* -------------------------------------------------------------- display -- */

static void display_task(void *arg)
{
    (void)arg;
    if (!cgoled_present()) {
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        uint8_t mask = 0;
        uint8_t mode = 0;
        uint16_t period = 0;
        uint8_t slot = 0;
        cgbeacon_get(&mask, &mode, &period, &slot);

        cgoled_clear();
        cgoled_text(0, 0, "CYB3RGUN MODULE");
        cgoled_rule(1);
        cgoled_printf(0, 2, "CH %-2u SLOT %-2u %s", g.channel, slot,
                      g.core_hello ? "CORE" : "----");
        cgoled_printf(0, 3, "%s MASK %X %ums",
                      (mode == CGUSB_BEACON_MODE_MULTIPLEX) ? "MUX" : "STDY", mask & 0x0F,
                      period);
        cgoled_printf(0, 4, "HEARD %" PRIu32, g.shots_heard);
        cgoled_printf(0, 5, "ACKED %" PRIu32, g.shots_acked);
        cgoled_printf(0, 6, "DUP %" PRIu32 " RS %" PRIu32 " DR %" PRIu32, g.duplicates,
                      g.core_resends, g.shots_dropped);
        if (g.last_rssi != 0) {
            cgoled_printf(0, 7, "%02X%02X%02X RSSI %d", g.last_pistol[3], g.last_pistol[4],
                          g.last_pistol[5], g.last_rssi);
        } else {
            cgoled_text(0, 7, "NO PISTOL YET");
        }
        cgoled_flush();

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* ----------------------------------------------------------------- boot -- */

static void radio_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* concept.md principle 4: modem sleep off wherever WiFi and ESP-NOW
     * share a chip, or the receiver misses shots between beacons. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(g.channel, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(radio_recv_cb));

    esp_now_peer_info_t peer = {.channel = g.channel, .ifidx = WIFI_IF_STA, .encrypt = false};
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

    g.channel = channel_load(CONFIG_CGMODULE_CHANNEL);
    g.slot = CONFIG_CGMODULE_SLOT;
    g.next_seq = 1;

    ESP_ERROR_CHECK(esp_read_mac(g.mac, ESP_MAC_WIFI_STA));

    s_pending_lock = xSemaphoreCreateMutex();
    s_radio_q = xQueueCreate(16, sizeof(radio_rx_t));
    configASSERT(s_pending_lock != NULL && s_radio_q != NULL);

    const cgoled_config_t oled = CGOLED_HELTEC_V2_CONFIG();
    if (cgoled_init(&oled) != ESP_OK) {
        ESP_LOGW(TAG, "no OLED, carrying on without it");
    }

    cgbeacon_config_t beacons = CGBEACON_HELTEC_V2_CONFIG();
    beacons.slots = CONFIG_CGBEACON_SLOTS;
    ESP_ERROR_CHECK(cgbeacon_init(&beacons));

    radio_start();

    cgusb_link_config_t link = CGUSB_LINK_DEFAULT_CONFIG();
    link.on_frame = on_core_frame;
    ESP_ERROR_CHECK(cgusb_link_start(&link));

    xTaskCreate(radio_task, "cg_radio", 4096, NULL, 12, NULL);
    xTaskCreate(forward_task, "cg_forward", 3072, NULL, 8, NULL);
    xTaskCreate(status_task, "cg_status", 3072, NULL, 5, NULL);
    xTaskCreate(display_task, "cg_display", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "module %02X:%02X:%02X:%02X:%02X:%02X, channel %u, slot %u, fw %d.%d.%d",
             g.mac[0], g.mac[1], g.mac[2], g.mac[3], g.mac[4], g.mac[5], g.channel, g.slot,
             FW_MAJOR, FW_MINOR, FW_PATCH);
    ESP_LOGI(TAG, "waiting for hello from the core");
}
