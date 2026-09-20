/* cgusb: framing and messages of docs/usb-protocol.md, version 1.
 * Plain C, no ESP-IDF, so the host tests of test/host run the same code the
 * module runs. See D-005 for the CRC parameters and D-007 for the tests. */

#include "cgusb.h"

#include <string.h>

/* The wire layout is fixed by the document, so a padded struct would be a
 * silent interoperability bug. Fail the build instead. */
#define CGUSB_ASSERT_SIZE(type, expected) \
    typedef char cgusb_size_check_##type[(sizeof(type) == (expected)) ? 1 : -1]

CGUSB_ASSERT_SIZE(cgusb_hello_t, 1);
CGUSB_ASSERT_SIZE(cgusb_beacons_t, 5);
CGUSB_ASSERT_SIZE(cgusb_time_mark_t, 8);
CGUSB_ASSERT_SIZE(cgusb_ack_t, 2);
CGUSB_ASSERT_SIZE(cgusb_channel_t, 1);
CGUSB_ASSERT_SIZE(cgusb_hello_ack_t, 11);
CGUSB_ASSERT_SIZE(cgusb_shot_t, 30);
CGUSB_ASSERT_SIZE(cgusb_status_t, 20);
CGUSB_ASSERT_SIZE(cgusb_pistol_seen_t, 7);
CGUSB_ASSERT_SIZE(cgusb_bench_start_t, 4);
CGUSB_ASSERT_SIZE(cgusb_bench_report_t, 46);

/* ----------------------------------------------------------------- crc -- */

uint16_t cgusb_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;

    if (data == NULL) {
        return crc;
    }
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* ---------------------------------------------------------------- cobs -- */

size_t cgusb_cobs_encode(const uint8_t *src, size_t len, uint8_t *dst)
{
    if (src == NULL || dst == NULL) {
        return 0;
    }

    size_t read_index = 0;
    size_t write_index = 1;
    size_t code_index = 0;
    uint8_t code = 1;

    while (read_index < len) {
        if (src[read_index] == 0) {
            dst[code_index] = code;
            code_index = write_index++;
            code = 1;
            read_index++;
        } else {
            dst[write_index++] = src[read_index++];
            code++;
            if (code == 0xFF) {
                dst[code_index] = code;
                code_index = write_index++;
                code = 1;
            }
        }
    }
    dst[code_index] = code;
    return write_index;
}

size_t cgusb_cobs_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t dst_cap)
{
    if (src == NULL || dst == NULL || len == 0) {
        return 0;
    }

    size_t read_index = 0;
    size_t write_index = 0;

    while (read_index < len) {
        const uint8_t code = src[read_index];

        /* A zero byte is the delimiter and can never sit inside a block, and
         * a code that reaches past the end means the block was truncated. */
        if (code == 0 || read_index + code > len) {
            return 0;
        }
        read_index++;

        for (uint8_t i = 1; i < code; i++) {
            if (write_index >= dst_cap) {
                return 0;
            }
            dst[write_index++] = src[read_index++];
        }

        /* A run shorter than 0xFF stood for a zero in the original, except
         * when it was the last run of the block. */
        if (code != 0xFF && read_index < len) {
            if (write_index >= dst_cap) {
                return 0;
            }
            dst[write_index++] = 0;
        }
    }
    return write_index;
}

/* -------------------------------------------------------------- frames -- */

int cgusb_payload_len(uint8_t type)
{
    switch (type) {
    case CGUSB_MSG_HELLO:
        return (int)sizeof(cgusb_hello_t);
    case CGUSB_MSG_BEACONS:
        return (int)sizeof(cgusb_beacons_t);
    case CGUSB_MSG_TIME_MARK:
        return (int)sizeof(cgusb_time_mark_t);
    case CGUSB_MSG_ACK:
        return (int)sizeof(cgusb_ack_t);
    case CGUSB_MSG_CHANNEL:
        return (int)sizeof(cgusb_channel_t);
    case CGUSB_MSG_STATUS_REQ:
        return 0;
    case CGUSB_MSG_REBOOT:
        return 0;
    case CGUSB_MSG_HELLO_ACK:
        return (int)sizeof(cgusb_hello_ack_t);
    case CGUSB_MSG_SHOT:
        return (int)sizeof(cgusb_shot_t);
    case CGUSB_MSG_STATUS:
        return (int)sizeof(cgusb_status_t);
    case CGUSB_MSG_PISTOL_SEEN:
        return (int)sizeof(cgusb_pistol_seen_t);
    case CGUSB_MSG_BENCH_START:
        return (int)sizeof(cgusb_bench_start_t);
    case CGUSB_MSG_BENCH_REPORT:
        return (int)sizeof(cgusb_bench_report_t);
    case CGUSB_MSG_ERROR:
        return -1; /* one code byte plus 0 to 32 detail bytes */
    default:
        return CGUSB_E_TYPE;
    }
}

/* True when payload_len is allowed for this type. */
static bool length_fits(uint8_t type, size_t payload_len)
{
    const int fixed = cgusb_payload_len(type);

    if (fixed == CGUSB_E_TYPE) {
        return false;
    }
    if (fixed >= 0) {
        return payload_len == (size_t)fixed;
    }
    /* error is the only variable message in version 1 */
    return payload_len >= 1u && payload_len <= 1u + CGUSB_ERROR_DETAIL_MAX;
}

int cgusb_frame_encode(uint8_t type, const void *payload, size_t payload_len,
                       uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out == NULL || out_len == NULL) {
        return CGUSB_E_ARG;
    }
    if (payload_len > 0 && payload == NULL) {
        return CGUSB_E_ARG;
    }
    if (payload_len > CGUSB_MAX_PAYLOAD) {
        return CGUSB_E_LENGTH;
    }
    if (!length_fits(type, payload_len)) {
        return (cgusb_payload_len(type) == CGUSB_E_TYPE) ? CGUSB_E_TYPE : CGUSB_E_LENGTH;
    }

    uint8_t raw[CGUSB_MAX_FRAME];
    size_t raw_len = 0;

    raw[raw_len++] = type;
    if (payload_len > 0) {
        memcpy(&raw[raw_len], payload, payload_len);
        raw_len += payload_len;
    }

    const uint16_t crc = cgusb_crc16(raw, raw_len);
    raw[raw_len++] = (uint8_t)(crc & 0xFFu);
    raw[raw_len++] = (uint8_t)((crc >> 8) & 0xFFu);

    /* worst case COBS growth plus a delimiter at each end */
    if (out_cap < raw_len + (raw_len / 254u) + 3u) {
        return CGUSB_E_SPACE;
    }

    /* D-008: the leading delimiter closes whatever was on the port before
     * this frame, so log text cannot merge into the frame's COBS block. */
    out[0] = 0x00;
    const size_t encoded = cgusb_cobs_encode(raw, raw_len, &out[1]);
    if (encoded == 0) {
        return CGUSB_E_COBS;
    }
    out[1u + encoded] = 0x00;
    *out_len = encoded + 2u;
    return CGUSB_OK;
}

int cgusb_frame_decode(const uint8_t *encoded, size_t encoded_len, uint8_t *type,
                       uint8_t *payload, size_t payload_cap, size_t *payload_len)
{
    if (encoded == NULL || type == NULL || payload_len == NULL) {
        return CGUSB_E_ARG;
    }
    if (payload_cap > 0 && payload == NULL) {
        return CGUSB_E_ARG;
    }

    uint8_t raw[CGUSB_MAX_FRAME];
    const size_t raw_len = cgusb_cobs_decode(encoded, encoded_len, raw, sizeof(raw));

    /* the shortest legal frame is a type byte and the two CRC bytes */
    if (raw_len < 3u) {
        return CGUSB_E_COBS;
    }

    const size_t body_len = raw_len - 2u;
    const uint16_t want = (uint16_t)((uint16_t)raw[body_len] | ((uint16_t)raw[body_len + 1u] << 8));
    if (cgusb_crc16(raw, body_len) != want) {
        return CGUSB_E_CRC;
    }

    const uint8_t got_type = raw[0];
    const size_t got_len = body_len - 1u;

    if (cgusb_payload_len(got_type) == CGUSB_E_TYPE) {
        return CGUSB_E_TYPE;
    }
    if (!length_fits(got_type, got_len)) {
        return CGUSB_E_LENGTH;
    }
    if (got_len > payload_cap) {
        return CGUSB_E_SPACE;
    }

    *type = got_type;
    *payload_len = got_len;
    if (got_len > 0) {
        memcpy(payload, &raw[1], got_len);
    }
    return CGUSB_OK;
}

/* ------------------------------------------------------------ receiver -- */

void cgusb_rx_init(cgusb_rx_t *rx)
{
    if (rx != NULL) {
        memset(rx, 0, sizeof(*rx));
    }
}

cgusb_rx_result_t cgusb_rx_feed(cgusb_rx_t *rx, uint8_t byte)
{
    if (rx == NULL) {
        return CGUSB_RX_IDLE;
    }

    if (byte != 0x00) {
        if (rx->raw_len < sizeof(rx->raw)) {
            rx->raw[rx->raw_len++] = byte;
        } else {
            /* Longer than any legal frame. Keep eating until the delimiter
             * so the next frame starts clean. */
            rx->overflow = true;
            rx->bytes_dropped++;
        }
        return CGUSB_RX_IDLE;
    }

    /* A delimiter closes whatever came before it. */
    const size_t len = rx->raw_len;
    const bool overflowed = rx->overflow;

    rx->raw_len = 0;
    rx->overflow = false;

    if (len == 0) {
        /* Two delimiters in a row, or the first byte after a reset. Not an
         * error: it is how a sender flushes a half written frame. */
        return CGUSB_RX_IDLE;
    }
    if (overflowed) {
        rx->frames_bad++;
        rx->last_error = CGUSB_E_SPACE;
        rx->bytes_dropped += len;
        return CGUSB_RX_ERROR;
    }

    const int rc = cgusb_frame_decode(rx->raw, len, &rx->type, rx->payload,
                                      sizeof(rx->payload), &rx->payload_len);
    if (rc != CGUSB_OK) {
        rx->frames_bad++;
        rx->last_error = rc;
        rx->bytes_dropped += len;
        return CGUSB_RX_ERROR;
    }

    rx->frames_ok++;
    rx->last_error = CGUSB_OK;
    return CGUSB_RX_FRAME;
}

/* ------------------------------------------------------------- helpers -- */

int cgusb_encode_error(uint8_t code, const uint8_t *detail, size_t detail_len,
                       uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (detail_len > CGUSB_ERROR_DETAIL_MAX) {
        return CGUSB_E_LENGTH;
    }
    if (detail_len > 0 && detail == NULL) {
        return CGUSB_E_ARG;
    }

    uint8_t payload[1u + CGUSB_ERROR_DETAIL_MAX];
    payload[0] = code;
    if (detail_len > 0) {
        memcpy(&payload[1], detail, detail_len);
    }
    return cgusb_frame_encode(CGUSB_MSG_ERROR, payload, 1u + detail_len, out, out_cap, out_len);
}

const char *cgusb_type_name(uint8_t type)
{
    switch (type) {
    case CGUSB_MSG_HELLO:
        return "hello";
    case CGUSB_MSG_BEACONS:
        return "beacons";
    case CGUSB_MSG_TIME_MARK:
        return "time_mark";
    case CGUSB_MSG_ACK:
        return "ack";
    case CGUSB_MSG_CHANNEL:
        return "channel";
    case CGUSB_MSG_STATUS_REQ:
        return "status_req";
    case CGUSB_MSG_REBOOT:
        return "reboot";
    case CGUSB_MSG_HELLO_ACK:
        return "hello_ack";
    case CGUSB_MSG_SHOT:
        return "shot";
    case CGUSB_MSG_STATUS:
        return "status";
    case CGUSB_MSG_PISTOL_SEEN:
        return "pistol_seen";
    case CGUSB_MSG_ERROR:
        return "error";
    case CGUSB_MSG_BENCH_START:
        return "bench_start";
    case CGUSB_MSG_BENCH_REPORT:
        return "bench_report";
    default:
        return "unknown";
    }
}
