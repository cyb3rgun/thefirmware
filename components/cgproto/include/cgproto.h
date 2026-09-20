/* cgproto: the ESP-NOW packets of docs/concept.md section 3, version 1.
 *
 * Payloads are little endian and carry a CRC-16 at the end. The CRC is the
 * same function the USB link uses, so there is one checksum in the system
 * and one place to change it if the architect rules differently (D-005).
 *
 * Like cgusb.c this is plain C with no ESP-IDF dependency, so it can be
 * tested on the host.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CGPROTO_SHOT = 1,
    CGPROTO_ACK = 2,
    CGPROTO_TIME_MARK = 3,
    CGPROTO_HELLO = 4,

    /* Bench packets, never in production, concept.md section 3 as amended
     * on 20 September 2026 (D-014). They carry no game traffic and exist so
     * a measurement run needs nothing but the module's USB port: the pistol
     * can sit on a power bank at 5 m with no cable to the PC. */
    CGPROTO_START = 0xF0,     /* module to pistols, broadcast */
    CGPROTO_REPORT = 0xF1,    /* pistol to module, unicast: the run summary */
    /* concept.md says start is "acknowledged by the stub, resent by the
     * module until acknowledged" without naming the acknowledgement. This
     * is thefirmware's reading of it, in the bench range and on the radio
     * only, so theclient never sees it. Raised with the architect. */
    CGPROTO_START_ACK = 0xF2, /* pistol to module, unicast */
};

/* shot, flags byte. Mirrors CGUSB_SHOT_FLAG_UNAIMED so the module forwards
 * the flags through without translating them. */
#define CGPROTO_SHOT_FLAG_UNAIMED (1u << 0)

/* A slot of 0 means the pistol identified no beacons and was shooting in
 * steady mode. concept.md section 3: every module that hears such a shot
 * acknowledges it and forwards it with the unaimed flag. */
#define CGPROTO_SLOT_NONE 0u

#define CGPROTO_MAX_PACKET 64u

typedef struct __attribute__((packed)) {
    uint8_t type; /* CGPROTO_SHOT */
    uint8_t pistol_mac[6];
    uint16_t pistol_seq;
    uint8_t slot;          /* the multiplex slot the pistol identified */
    uint8_t target_mac[6]; /* zero until the pistol has heard an ack once */
    uint32_t ts_pistol_ms;
    uint16_t x;
    uint16_t y;
    int16_t quat[4];
    uint8_t buttons;
    uint8_t flags;
    uint16_t crc;
} cgproto_shot_t;

typedef struct __attribute__((packed)) {
    uint8_t type; /* CGPROTO_ACK */
    uint16_t pistol_seq;
    uint8_t module_mac[6];
    uint8_t slot;
    uint16_t crc;
} cgproto_ack_t;

typedef struct __attribute__((packed)) {
    uint8_t type; /* CGPROTO_TIME_MARK */
    uint64_t unix_ms;
    uint16_t crc;
} cgproto_time_mark_t;

typedef struct __attribute__((packed)) {
    uint8_t type; /* CGPROTO_HELLO */
    uint8_t pistol_mac[6];
    uint8_t fw[3];
    uint16_t crc;
} cgproto_hello_t;

/* Bench only. The module broadcasts start to begin a measurement run and
 * keeps broadcasting it until a pistol acknowledges. The run id comes from
 * the core and is echoed through every packet of the run, so a late report
 * from the run before is recognised as late rather than written into the
 * current row.
 *
 * The module's mac is not carried: ESP-NOW hands the receiver the source
 * address, which is the same thing and one field less to get wrong. */
typedef struct __attribute__((packed)) {
    uint8_t type; /* CGPROTO_START */
    uint32_t run_id;
    uint16_t shots;
    uint16_t interval_ms;
    uint16_t crc;
} cgproto_start_t;

typedef struct __attribute__((packed)) {
    uint8_t type; /* CGPROTO_START_ACK */
    uint8_t pistol_mac[6];
    uint32_t run_id;
    uint16_t crc;
} cgproto_start_ack_t;

/* The counters are 16 bit because a bench run is a few hundred shots, and
 * the microsecond percentiles are 32 bit because a bad one is milliseconds.
 * This mirrors bench_result of usb-protocol.md section 6 so the module can
 * forward it field for field. mean, min and max are extra, for the log on
 * the module rather than for the core. */
typedef struct __attribute__((packed)) {
    uint8_t type; /* CGPROTO_REPORT */
    uint8_t pistol_mac[6];
    uint32_t run_id;
    uint16_t sent;
    uint16_t acked;
    uint16_t resends;
    uint16_t lost;
    uint32_t median_us;
    uint32_t p95_us;
    uint32_t mean_us;
    uint32_t min_us;
    uint32_t max_us;
    int8_t rssi; /* the module's acknowledgement as the pistol heard it */
    uint16_t crc;
} cgproto_report_t;

/* Fills in the crc field of a packet that is otherwise complete. The packet
 * must be one of the structs above, and len its sizeof. */
void cgproto_seal(void *packet, size_t len);

/* True when len matches the type and the CRC is right. Anything else is a
 * packet from a different version, a different system or a bad radio moment,
 * and is dropped without a word. */
bool cgproto_check(const void *packet, size_t len);

/* Expected size of a packet of this type, or 0 for an unknown type. */
size_t cgproto_size(uint8_t type);

const char *cgproto_type_name(uint8_t type);

#ifdef __cplusplus
}
#endif
