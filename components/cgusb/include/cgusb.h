/* cgusb: the target module USB link of docs/usb-protocol.md, version 1.
 *
 * This header and cgusb.c are plain C with no ESP-IDF dependency, so the same
 * code that runs on the module is compiled and tested on the build host
 * (D-007). The transport that puts the bytes on a UART is cgusb_link.h.
 *
 * Wire format, usb-protocol.md section 1: one byte type, payload, two bytes
 * CRC-16/CCITT over type and payload, little endian. The whole thing is COBS
 * encoded and terminated with a zero byte.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CGUSB_PROTO_VERSION 1u

/* Largest payload the link carries. The biggest message in version 1 is
 * error, at one code byte plus up to 32 detail bytes. The headroom keeps a
 * later message from forcing a change on both sides at once. */
#define CGUSB_MAX_PAYLOAD 64u

/* type + payload + two CRC bytes */
#define CGUSB_MAX_FRAME (1u + CGUSB_MAX_PAYLOAD + 2u)

/* COBS adds one overhead byte per 254 bytes plus one leading code byte, and
 * the frame carries a zero delimiter at each end (D-008). */
#define CGUSB_MAX_ENCODED (CGUSB_MAX_FRAME + (CGUSB_MAX_FRAME / 254u) + 3u)

/* ---------------------------------------------------------------- types -- */

/* usb-protocol.md sections 2 and 3. The high bit marks the direction:
 * clear is core to module, set is module to core. */
enum {
    /* core to module */
    CGUSB_MSG_HELLO = 0x01,
    CGUSB_MSG_BEACONS = 0x02,
    CGUSB_MSG_TIME_MARK = 0x03,
    CGUSB_MSG_ACK = 0x04,
    CGUSB_MSG_CHANNEL = 0x05,
    CGUSB_MSG_STATUS_REQ = 0x06,
    CGUSB_MSG_REBOOT = 0x07,

    /* module to core */
    CGUSB_MSG_HELLO_ACK = 0x81,
    CGUSB_MSG_SHOT = 0x82,
    CGUSB_MSG_STATUS = 0x83,
    CGUSB_MSG_PISTOL_SEEN = 0x84,
    CGUSB_MSG_ERROR = 0x85,

    /* Bench only, added in S01-B01, D-014. They sit well outside the type
     * space version 1 uses, 0x01 to 0x07 and 0x81 to 0x85, so a conformant
     * implementation of the document never sends or expects them and is not
     * affected by their existence. They let a measurement run be started and
     * read entirely through the module's port, with the pistol on a power
     * bank and no cable to the PC. */
    CGUSB_MSG_BENCH_START = 0x7E,  /* core to module */
    CGUSB_MSG_BENCH_REPORT = 0xFE, /* module to core */
};

/* beacons, mode byte */
enum {
    CGUSB_BEACON_MODE_STEADY = 0,
    CGUSB_BEACON_MODE_MULTIPLEX = 1,
};

/* shot, flags byte */
#define CGUSB_SHOT_FLAG_UNAIMED (1u << 0)

/* hello_ack, chip byte */
enum {
    CGUSB_CHIP_ESP32 = 1,
    CGUSB_CHIP_ESP32S3 = 2,
    CGUSB_CHIP_ESP32P4 = 3,
};

/* error, code byte */
enum {
    CGUSB_ERR_BAD_CRC = 1,
    CGUSB_ERR_BAD_FRAME = 2,
    CGUSB_ERR_UNKNOWN_TYPE = 3,
    CGUSB_ERR_BAD_LENGTH = 4,
    CGUSB_ERR_PROTO_VERSION = 5,
    CGUSB_ERR_SHOT_DROPPED = 6,
    CGUSB_ERR_RADIO = 7,
};

/* return codes */
enum {
    CGUSB_OK = 0,
    CGUSB_E_ARG = -1,
    CGUSB_E_SPACE = -2,  /* the output buffer is too small */
    CGUSB_E_COBS = -3,   /* the encoding is not a valid COBS block */
    CGUSB_E_CRC = -4,    /* the CRC does not match */
    CGUSB_E_LENGTH = -5, /* the payload length does not fit the type */
    CGUSB_E_TYPE = -6,   /* the type byte is not part of version 1 */
};

#define CGUSB_ERROR_DETAIL_MAX 32u

/* ------------------------------------------------------------- messages -- */
/* Little endian on the wire, which is the native order of every chip in the
 * system, so the structs are copied rather than serialised field by field. */

typedef struct __attribute__((packed)) {
    uint8_t proto;
} cgusb_hello_t;

typedef struct __attribute__((packed)) {
    uint8_t mask;       /* one bit per cluster, bit 0 is cluster 0 */
    uint8_t mode;       /* CGUSB_BEACON_MODE_ */
    uint16_t period_ms; /* the full multiplex period, not the slot width */
    uint8_t slot;       /* this target's slot in the room plan */
} cgusb_beacons_t;

typedef struct __attribute__((packed)) {
    uint64_t unix_ms;
} cgusb_time_mark_t;

typedef struct __attribute__((packed)) {
    uint16_t seq;
} cgusb_ack_t;

typedef struct __attribute__((packed)) {
    uint8_t channel;
} cgusb_channel_t;

typedef struct __attribute__((packed)) {
    uint8_t proto;
    uint8_t fw[3]; /* major, minor, patch */
    uint8_t chip;  /* CGUSB_CHIP_ */
    uint8_t mac[6];
} cgusb_hello_ack_t;

typedef struct __attribute__((packed)) {
    uint16_t seq;         /* the module's own sequence number */
    uint8_t pistol_id[6]; /* the pistol's mac */
    uint16_t pistol_seq;  /* the sequence number the pistol used */
    uint8_t slot;
    uint8_t flags; /* CGUSB_SHOT_FLAG_ */
    uint32_t ts_pistol_ms;
    uint16_t x; /* aim point in the beacon frame, 0 is left */
    uint16_t y; /* aim point in the beacon frame, 0 is top */
    int16_t quat[4];
    uint8_t buttons;
    int8_t rssi;
} cgusb_shot_t;

typedef struct __attribute__((packed)) {
    uint32_t uptime_s;
    uint8_t channel;
    uint8_t beacons_mask;
    uint8_t mode;
    int8_t temp;
    uint32_t free_heap;
    uint32_t shots_heard;
    uint32_t shots_acked;
} cgusb_status_t;

typedef struct __attribute__((packed)) {
    uint8_t pistol_id[6];
    int8_t rssi;
} cgusb_pistol_seen_t;

/* error carries 0 to 32 detail bytes, so only the first detail_len bytes of
 * detail reach the wire. Use cgusb_encode_error to send one. */
typedef struct __attribute__((packed)) {
    uint8_t code;
    uint8_t detail[CGUSB_ERROR_DETAIL_MAX];
} cgusb_error_t;

/* Bench only, D-014. */
typedef struct __attribute__((packed)) {
    uint16_t shots;
    uint16_t rate_ms;
} cgusb_bench_start_t;

typedef struct __attribute__((packed)) {
    uint8_t pistol_id[6];
    uint16_t run_id;
    uint32_t sent;
    uint32_t acked;
    uint32_t resends;
    uint32_t lost;
    uint32_t median_us;
    uint32_t p95_us;
    uint32_t mean_us;
    uint32_t min_us;
    uint32_t max_us;
    int8_t rssi_at_pistol; /* the module's ack as the pistol heard it */
    int8_t rssi_at_module; /* the pistol's shots as the module heard them */
} cgusb_bench_report_t;

/* ----------------------------------------------------------- primitives -- */

/* CRC-16/CCITT as pinned in D-005: polynomial 0x1021, initial value 0xFFFF,
 * no reflection, no final xor. The check value for the string "123456789"
 * is 0x29B1. */
uint16_t cgusb_crc16(const uint8_t *data, size_t len);

/* Consistent Overhead Byte Stuffing. The encoder does not write the trailing
 * zero delimiter; cgusb_frame_encode appends it. dst must hold at least
 * len + len / 254 + 1 bytes. Returns the number of bytes written. */
size_t cgusb_cobs_encode(const uint8_t *src, size_t len, uint8_t *dst);

/* Decodes one COBS block, which must not contain the zero delimiter.
 * Returns the number of bytes written, or 0 when the block is malformed or
 * does not fit into dst_cap. */
size_t cgusb_cobs_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t dst_cap);

/* --------------------------------------------------------------- frames -- */

/* Payload length that version 1 fixes for a type. Returns the length, -1 for
 * a type whose payload is variable, or CGUSB_E_TYPE for an unknown type. */
int cgusb_payload_len(uint8_t type);

/* Builds type, payload and CRC, COBS encodes the result and wraps it in a
 * zero delimiter at each end (D-008). The leading delimiter closes off
 * whatever came before, so log output sharing the port cannot merge into the
 * frame's COBS block. out_len is the number of bytes to write to the port. */
int cgusb_frame_encode(uint8_t type, const void *payload, size_t payload_len,
                       uint8_t *out, size_t out_cap, size_t *out_len);

/* Takes one COBS block without its delimiter and recovers type and payload,
 * checking the CRC and the length that the type prescribes. */
int cgusb_frame_decode(const uint8_t *encoded, size_t encoded_len, uint8_t *type,
                       uint8_t *payload, size_t payload_cap, size_t *payload_len);

/* ------------------------------------------------------------- receiver -- */

typedef enum {
    CGUSB_RX_IDLE = 0, /* the byte was taken, no frame is complete yet */
    CGUSB_RX_FRAME,    /* type, payload and payload_len now hold a valid frame */
    CGUSB_RX_ERROR,    /* a delimiter closed something that was not a frame */
} cgusb_rx_result_t;

/* Byte at a time receiver. It never blocks and never allocates, so it runs
 * straight out of a UART read loop. Anything that is not a valid frame is
 * dropped at the next delimiter and counted, which is the resynchronisation
 * that usb-protocol.md section 1 asks for. */
typedef struct {
    uint8_t raw[CGUSB_MAX_ENCODED];
    size_t raw_len;
    bool overflow;

    uint8_t type;
    uint8_t payload[CGUSB_MAX_PAYLOAD];
    size_t payload_len;
    int last_error;

    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t bytes_dropped;
} cgusb_rx_t;

void cgusb_rx_init(cgusb_rx_t *rx);
cgusb_rx_result_t cgusb_rx_feed(cgusb_rx_t *rx, uint8_t byte);

/* -------------------------------------------------------------- helpers -- */

/* error is the one message with a variable payload, so it gets a builder. */
int cgusb_encode_error(uint8_t code, const uint8_t *detail, size_t detail_len,
                       uint8_t *out, size_t out_cap, size_t *out_len);

/* Name of a type, for logs and for the bench. Never NULL. */
const char *cgusb_type_name(uint8_t type);

#ifdef __cplusplus
}
#endif
