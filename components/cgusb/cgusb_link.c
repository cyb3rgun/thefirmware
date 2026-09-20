/* cgusb_link: UART transport under cgusb. See cgusb_link.h and D-006. */

#include "cgusb_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cgusb_link";

#define LOG_LINE_MAX 320

static struct {
    bool started;
    int uart_num;
    SemaphoreHandle_t tx_lock;
    cgusb_link_frame_cb_t on_frame;
    void *ctx;
    cgusb_rx_t rx;
    vprintf_like_t prev_log;
    char log_line[LOG_LINE_MAX];
} s_link;

/* Writes raw bytes while holding the transmit lock. Never logs, because the
 * logger takes the same lock. */
static void write_locked(const uint8_t *data, size_t len)
{
    xSemaphoreTake(s_link.tx_lock, portMAX_DELAY);
    uart_write_bytes(s_link.uart_num, (const char *)data, len);
    xSemaphoreGive(s_link.tx_lock);
}

/* ESP-IDF logging is routed here so that a log line is written as one piece
 * between frames instead of splitting one. */
static int link_log_vprintf(const char *fmt, va_list args)
{
    if (!s_link.started) {
        return 0;
    }

    xSemaphoreTake(s_link.tx_lock, portMAX_DELAY);
    const int n = vsnprintf(s_link.log_line, sizeof(s_link.log_line), fmt, args);
    if (n > 0) {
        const size_t len = (n < (int)sizeof(s_link.log_line)) ? (size_t)n
                                                             : sizeof(s_link.log_line) - 1u;
        uart_write_bytes(s_link.uart_num, s_link.log_line, len);
    }
    xSemaphoreGive(s_link.tx_lock);
    return n;
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t chunk[256];

    for (;;) {
        const int n = uart_read_bytes(s_link.uart_num, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            const cgusb_rx_result_t r = cgusb_rx_feed(&s_link.rx, chunk[i]);
            if (r == CGUSB_RX_FRAME) {
                if (s_link.on_frame != NULL) {
                    s_link.on_frame(s_link.rx.type, s_link.rx.payload, s_link.rx.payload_len,
                                    s_link.ctx);
                }
            } else if (r == CGUSB_RX_ERROR) {
                /* Dropped and counted. The core is told once, with the
                 * reason, so a wiring or baud rate mistake is visible
                 * instead of silent. */
                const uint8_t detail = (uint8_t)(-s_link.rx.last_error);
                cgusb_link_send_error(CGUSB_ERR_BAD_FRAME, &detail, 1);
            }
        }
    }
}

esp_err_t cgusb_link_start(const cgusb_link_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_link.started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_link.uart_num = cfg->uart_num;
    s_link.on_frame = cfg->on_frame;
    s_link.ctx = cfg->ctx;
    cgusb_rx_init(&s_link.rx);

    s_link.tx_lock = xSemaphoreCreateMutex();
    if (s_link.tx_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t uart_cfg = {
        .baud_rate = cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(cfg->uart_num, cfg->rx_buf_size, cfg->tx_buf_size, 0,
                                        NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(cfg->uart_num, &uart_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }

    s_link.started = true;

    if (cfg->capture_log) {
        s_link.prev_log = esp_log_set_vprintf(link_log_vprintf);
    }

    if (xTaskCreate(rx_task, "cgusb_rx", cfg->task_stack, NULL, cfg->task_prio, NULL) != pdPASS) {
        s_link.started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "link up on UART%d at %d baud", cfg->uart_num, cfg->baud);
    return ESP_OK;
}

esp_err_t cgusb_link_send(uint8_t type, const void *payload, size_t payload_len)
{
    if (!s_link.started) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t out[CGUSB_MAX_ENCODED];
    size_t out_len = 0;

    const int rc = cgusb_frame_encode(type, payload, payload_len, out, sizeof(out), &out_len);
    if (rc != CGUSB_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    write_locked(out, out_len);
    return ESP_OK;
}

esp_err_t cgusb_link_send_error(uint8_t code, const uint8_t *detail, size_t detail_len)
{
    if (!s_link.started) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t out[CGUSB_MAX_ENCODED];
    size_t out_len = 0;

    const int rc = cgusb_encode_error(code, detail, detail_len, out, sizeof(out), &out_len);
    if (rc != CGUSB_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    write_locked(out, out_len);
    return ESP_OK;
}

void cgusb_link_stats(uint32_t *frames_ok, uint32_t *frames_bad, uint32_t *bytes_dropped)
{
    if (frames_ok != NULL) {
        *frames_ok = s_link.rx.frames_ok;
    }
    if (frames_bad != NULL) {
        *frames_bad = s_link.rx.frames_bad;
    }
    if (bytes_dropped != NULL) {
        *bytes_dropped = s_link.rx.bytes_dropped;
    }
}
