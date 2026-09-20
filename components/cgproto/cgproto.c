/* cgproto: the ESP-NOW packets of docs/concept.md section 3. */

#include "cgproto.h"

#include <string.h>

#include "cgusb.h" /* for cgusb_crc16, the one checksum in the system */

#define CGPROTO_ASSERT_SIZE(type, expected) \
    typedef char cgproto_size_check_##type[(sizeof(type) == (expected)) ? 1 : -1]

/* concept.md section 3 calls shot "about 37 bytes". The exact layout of the
 * fields it lists, plus the CRC, is 36. */
CGPROTO_ASSERT_SIZE(cgproto_shot_t, 36);
CGPROTO_ASSERT_SIZE(cgproto_ack_t, 12);
CGPROTO_ASSERT_SIZE(cgproto_time_mark_t, 11);
CGPROTO_ASSERT_SIZE(cgproto_hello_t, 12);

size_t cgproto_size(uint8_t type)
{
    switch (type) {
    case CGPROTO_SHOT:
        return sizeof(cgproto_shot_t);
    case CGPROTO_ACK:
        return sizeof(cgproto_ack_t);
    case CGPROTO_TIME_MARK:
        return sizeof(cgproto_time_mark_t);
    case CGPROTO_HELLO:
        return sizeof(cgproto_hello_t);
    default:
        return 0;
    }
}

void cgproto_seal(void *packet, size_t len)
{
    if (packet == NULL || len < 3u) {
        return;
    }

    uint8_t *bytes = (uint8_t *)packet;
    const uint16_t crc = cgusb_crc16(bytes, len - 2u);

    bytes[len - 2u] = (uint8_t)(crc & 0xFFu);
    bytes[len - 1u] = (uint8_t)((crc >> 8) & 0xFFu);
}

bool cgproto_check(const void *packet, size_t len)
{
    if (packet == NULL || len < 3u) {
        return false;
    }

    const uint8_t *bytes = (const uint8_t *)packet;
    if (cgproto_size(bytes[0]) != len) {
        return false;
    }

    const uint16_t want = (uint16_t)((uint16_t)bytes[len - 2u] |
                                     ((uint16_t)bytes[len - 1u] << 8));
    return cgusb_crc16(bytes, len - 2u) == want;
}

const char *cgproto_type_name(uint8_t type)
{
    switch (type) {
    case CGPROTO_SHOT:
        return "shot";
    case CGPROTO_ACK:
        return "ack";
    case CGPROTO_TIME_MARK:
        return "time_mark";
    case CGPROTO_HELLO:
        return "hello";
    default:
        return "unknown";
    }
}
