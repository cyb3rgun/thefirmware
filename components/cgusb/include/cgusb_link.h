/* cgusb_link: the UART transport under cgusb, for ESP-IDF.
 *
 * cgusb.h is the format; this is the port. It installs the UART driver, runs
 * a receive task that feeds cgusb_rx_feed byte by byte, and serialises every
 * write behind one mutex.
 *
 * D-006: the console shares UART0 with the protocol link on the Heltec
 * bench. The link therefore takes over ESP-IDF logging as well, so a log
 * line can never land in the middle of a frame and cost a shot. Log output
 * still appears on the port, between frames, where the host tool prints it
 * as text.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cgusb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the link receive task for every frame that passed the CRC. */
typedef void (*cgusb_link_frame_cb_t)(uint8_t type, const uint8_t *payload, size_t len,
                                      void *ctx);

typedef struct {
    int uart_num;
    int tx_pin; /* -1 keeps the pin the bootloader already configured */
    int rx_pin;
    int baud;
    int rx_buf_size;
    int tx_buf_size;
    int task_stack;
    int task_prio;
    /* Route ESP-IDF logging through this link. Leave it on for UART0. */
    bool capture_log;
    cgusb_link_frame_cb_t on_frame;
    void *ctx;
} cgusb_link_config_t;

#define CGUSB_LINK_DEFAULT_CONFIG()      \
    (cgusb_link_config_t)                \
    {                                    \
        .uart_num = 0, .tx_pin = -1,     \
        .rx_pin = -1, .baud = 921600,    \
        .rx_buf_size = 2048,             \
        .tx_buf_size = 4096,             \
        .task_stack = 4096,              \
        .task_prio = 10,                 \
        .capture_log = true,             \
        .on_frame = NULL, .ctx = NULL,   \
    }

esp_err_t cgusb_link_start(const cgusb_link_config_t *cfg);

/* Frames payload and writes it. Safe from any task. */
esp_err_t cgusb_link_send(uint8_t type, const void *payload, size_t payload_len);

esp_err_t cgusb_link_send_error(uint8_t code, const uint8_t *detail, size_t detail_len);

/* Counters of the receiver, for status and for the bench. */
void cgusb_link_stats(uint32_t *frames_ok, uint32_t *frames_bad, uint32_t *bytes_dropped);

#ifdef __cplusplus
}
#endif
