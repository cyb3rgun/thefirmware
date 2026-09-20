/* Unit tests for components/cgusb, run on the build host under Unity (D-007).
 *
 * The last group is the one that matters most: it replays the vectors that
 * tools/cgusb_host.py prints and checks that the C encoder produces exactly
 * the same bytes. The firmware and the host reference are then proven to
 * agree, instead of both being written from the same document and hoped to
 * match.
 *
 *   test/host/run.sh
 */

#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "cgusb.h"
#include "vectors.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------ crc -- */

static void test_crc_check_value(void)
{
    /* The check value every CRC catalogue lists for this variant (D-005). */
    TEST_ASSERT_EQUAL_HEX16(0x29B1, cgusb_crc16((const uint8_t *)"123456789", 9));
}

static void test_crc_of_nothing_is_the_initial_value(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, cgusb_crc16((const uint8_t *)"", 0));
}

static void test_crc_notices_a_flipped_bit(void)
{
    const uint8_t a[4] = {0x01, 0x02, 0x03, 0x04};
    const uint8_t b[4] = {0x01, 0x02, 0x03, 0x05};

    TEST_ASSERT_NOT_EQUAL(cgusb_crc16(a, sizeof(a)), cgusb_crc16(b, sizeof(b)));
}

static void test_crc_notices_swapped_bytes(void)
{
    const uint8_t a[4] = {0x01, 0x02, 0x03, 0x04};
    const uint8_t b[4] = {0x01, 0x03, 0x02, 0x04};

    TEST_ASSERT_NOT_EQUAL(cgusb_crc16(a, sizeof(a)), cgusb_crc16(b, sizeof(b)));
}

/* ----------------------------------------------------------------- cobs -- */

static void round_trip(const uint8_t *data, size_t len)
{
    uint8_t encoded[1024];
    uint8_t decoded[1024];

    const size_t enc_len = cgusb_cobs_encode(data, len, encoded);
    TEST_ASSERT_GREATER_THAN_size_t(0, enc_len);

    for (size_t i = 0; i < enc_len; i++) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, encoded[i], "a zero byte survived the encoder");
    }

    const size_t dec_len = cgusb_cobs_decode(encoded, enc_len, decoded, sizeof(decoded));
    TEST_ASSERT_EQUAL_size_t(len, dec_len);
    if (len > 0) {
        TEST_ASSERT_EQUAL_UINT8_ARRAY(data, decoded, len);
    }
}

static void test_cobs_round_trip_plain(void)
{
    const uint8_t data[] = {0x01, 0x02, 0x03};
    round_trip(data, sizeof(data));
}

static void test_cobs_round_trip_with_zeros(void)
{
    const uint8_t data[] = {0x11, 0x00, 0x22, 0x00, 0x00, 0x33};
    round_trip(data, sizeof(data));
}

static void test_cobs_round_trip_all_zeros(void)
{
    uint8_t data[64];
    memset(data, 0, sizeof(data));
    round_trip(data, sizeof(data));
}

static void test_cobs_round_trip_crosses_the_254_boundary(void)
{
    /* The one case the algorithm has a special rule for. */
    uint8_t data[600];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(1u + (i % 255u));
    }
    round_trip(data, sizeof(data));
    round_trip(data, 253);
    round_trip(data, 254);
    round_trip(data, 255);
}

static void test_cobs_decode_rejects_a_zero_inside_the_block(void)
{
    const uint8_t bad[] = {0x03, 0x11, 0x00, 0x22};
    uint8_t out[16];

    TEST_ASSERT_EQUAL_size_t(0, cgusb_cobs_decode(bad, sizeof(bad), out, sizeof(out)));
}

static void test_cobs_decode_rejects_a_code_past_the_end(void)
{
    const uint8_t bad[] = {0x09, 0x11, 0x22};
    uint8_t out[16];

    TEST_ASSERT_EQUAL_size_t(0, cgusb_cobs_decode(bad, sizeof(bad), out, sizeof(out)));
}

static void test_cobs_decode_refuses_to_overrun_the_output(void)
{
    const uint8_t data[32] = {0};
    uint8_t encoded[64];
    uint8_t small[8];

    const size_t enc_len = cgusb_cobs_encode(data, sizeof(data), encoded);
    TEST_ASSERT_EQUAL_size_t(0, cgusb_cobs_decode(encoded, enc_len, small, sizeof(small)));
}

/* --------------------------------------------------------------- frames -- */

static void test_frame_is_delimited_at_both_ends(void)
{
    const cgusb_ack_t ack = {.seq = 0x1234};
    uint8_t frame[CGUSB_MAX_ENCODED];
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(CGUSB_OK,
                          cgusb_frame_encode(CGUSB_MSG_ACK, &ack, sizeof(ack), frame,
                                             sizeof(frame), &len));
    TEST_ASSERT_GREATER_THAN_size_t(2, len);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, frame[0], "no leading delimiter (D-008)");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, frame[len - 1], "no trailing delimiter");
    for (size_t i = 1; i + 1 < len; i++) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, frame[i], "zero byte inside the frame body");
    }
}

static void test_frame_round_trip(void)
{
    const cgusb_shot_t shot = {
        .seq = 7,
        .pistol_id = {0x24, 0x6F, 0x28, 0x01, 0x02, 0x03},
        .pistol_seq = 99,
        .slot = 2,
        .flags = CGUSB_SHOT_FLAG_UNAIMED,
        .ts_pistol_ms = 123456,
        .x = 32768,
        .y = 16384,
        .quat = {1, -1, 2, -2},
        .buttons = 0x03,
        .rssi = -55,
    };
    uint8_t frame[CGUSB_MAX_ENCODED];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(CGUSB_OK, cgusb_frame_encode(CGUSB_MSG_SHOT, &shot, sizeof(shot),
                                                       frame, sizeof(frame), &len));

    uint8_t type = 0;
    uint8_t payload[CGUSB_MAX_PAYLOAD];
    size_t payload_len = 0;
    TEST_ASSERT_EQUAL_INT(CGUSB_OK, cgusb_frame_decode(&frame[1], len - 2u, &type, payload,
                                                       sizeof(payload), &payload_len));
    TEST_ASSERT_EQUAL_HEX8(CGUSB_MSG_SHOT, type);
    TEST_ASSERT_EQUAL_size_t(sizeof(shot), payload_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&shot, payload, sizeof(shot));
}

static void test_frame_decode_rejects_a_flipped_bit(void)
{
    const cgusb_ack_t ack = {.seq = 1};
    uint8_t frame[CGUSB_MAX_ENCODED];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(CGUSB_OK, cgusb_frame_encode(CGUSB_MSG_ACK, &ack, sizeof(ack), frame,
                                                       sizeof(frame), &len));
    frame[2] ^= 0x01;

    uint8_t type = 0;
    uint8_t payload[CGUSB_MAX_PAYLOAD];
    size_t payload_len = 0;
    TEST_ASSERT_EQUAL_INT(CGUSB_E_CRC, cgusb_frame_decode(&frame[1], len - 2u, &type, payload,
                                                          sizeof(payload), &payload_len));
}

static void test_frame_encode_rejects_a_wrong_length(void)
{
    const uint8_t junk[7] = {0};
    uint8_t frame[CGUSB_MAX_ENCODED];
    size_t len = 0;

    /* ack carries exactly two bytes, never seven. */
    TEST_ASSERT_EQUAL_INT(CGUSB_E_LENGTH, cgusb_frame_encode(CGUSB_MSG_ACK, junk, sizeof(junk),
                                                             frame, sizeof(frame), &len));
}

static void test_frame_encode_rejects_an_unknown_type(void)
{
    uint8_t frame[CGUSB_MAX_ENCODED];
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(CGUSB_E_TYPE,
                          cgusb_frame_encode(0x7F, NULL, 0, frame, sizeof(frame), &len));
}

static void test_frame_encode_refuses_a_buffer_that_is_too_small(void)
{
    const cgusb_status_t status = {0};
    uint8_t tiny[4];
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(CGUSB_E_SPACE, cgusb_frame_encode(CGUSB_MSG_STATUS, &status,
                                                            sizeof(status), tiny, sizeof(tiny),
                                                            &len));
}

static void test_empty_payload_messages(void)
{
    uint8_t frame[CGUSB_MAX_ENCODED];
    size_t len = 0;
    const uint8_t types[] = {CGUSB_MSG_STATUS_REQ, CGUSB_MSG_REBOOT};

    for (size_t i = 0; i < sizeof(types); i++) {
        TEST_ASSERT_EQUAL_INT(CGUSB_OK, cgusb_frame_encode(types[i], NULL, 0, frame,
                                                           sizeof(frame), &len));
        uint8_t type = 0;
        uint8_t payload[CGUSB_MAX_PAYLOAD];
        size_t payload_len = 1;
        TEST_ASSERT_EQUAL_INT(CGUSB_OK, cgusb_frame_decode(&frame[1], len - 2u, &type, payload,
                                                           sizeof(payload), &payload_len));
        TEST_ASSERT_EQUAL_HEX8(types[i], type);
        TEST_ASSERT_EQUAL_size_t(0, payload_len);
    }
}

static void test_error_takes_zero_to_thirty_two_detail_bytes(void)
{
    uint8_t detail[CGUSB_ERROR_DETAIL_MAX + 1];
    uint8_t frame[CGUSB_MAX_ENCODED];
    size_t len = 0;

    for (size_t i = 0; i < sizeof(detail); i++) {
        detail[i] = (uint8_t)i;
    }
    TEST_ASSERT_EQUAL_INT(CGUSB_OK, cgusb_encode_error(CGUSB_ERR_BAD_CRC, NULL, 0, frame,
                                                       sizeof(frame), &len));
    TEST_ASSERT_EQUAL_INT(CGUSB_OK, cgusb_encode_error(CGUSB_ERR_RADIO, detail,
                                                       CGUSB_ERROR_DETAIL_MAX, frame,
                                                       sizeof(frame), &len));
    TEST_ASSERT_EQUAL_INT(CGUSB_E_LENGTH,
                          cgusb_encode_error(CGUSB_ERR_RADIO, detail, CGUSB_ERROR_DETAIL_MAX + 1u,
                                             frame, sizeof(frame), &len));
}

/* ------------------------------------------------------------- receiver -- */

typedef struct {
    int frames;
    uint8_t last_type;
    uint8_t last_payload[CGUSB_MAX_PAYLOAD];
    size_t last_len;
} sink_t;

static void feed_all(cgusb_rx_t *rx, const uint8_t *data, size_t len, sink_t *sink)
{
    for (size_t i = 0; i < len; i++) {
        if (cgusb_rx_feed(rx, data[i]) == CGUSB_RX_FRAME) {
            sink->frames++;
            sink->last_type = rx->type;
            sink->last_len = rx->payload_len;
            memcpy(sink->last_payload, rx->payload, rx->payload_len);
        }
    }
}

static void test_receiver_finds_back_to_back_frames(void)
{
    cgusb_rx_t rx;
    sink_t sink = {0};
    uint8_t stream[512];
    size_t total = 0;

    cgusb_rx_init(&rx);
    for (int i = 0; i < 5; i++) {
        const cgusb_ack_t ack = {.seq = (uint16_t)i};
        size_t len = 0;
        TEST_ASSERT_EQUAL_INT(CGUSB_OK, cgusb_frame_encode(CGUSB_MSG_ACK, &ack, sizeof(ack),
                                                           &stream[total],
                                                           sizeof(stream) - total, &len));
        total += len;
    }

    feed_all(&rx, stream, total, &sink);
    TEST_ASSERT_EQUAL_INT(5, sink.frames);
    TEST_ASSERT_EQUAL_UINT32(5, rx.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0, rx.frames_bad);
}

static void test_receiver_survives_log_text_in_front_of_a_frame(void)
{
    /* The case D-008 exists for: the ESP-IDF log shares UART0 with the link
     * and carries no zero byte of its own. */
    const char *log = "I (312) module: beacons mask 0x0F mode 1\r\n";
    cgusb_rx_t rx;
    sink_t sink = {0};
    uint8_t frame[CGUSB_MAX_ENCODED];
    size_t len = 0;
    const cgusb_ack_t ack = {.seq = 0xBEEF};

    cgusb_rx_init(&rx);
    TEST_ASSERT_EQUAL_INT(CGUSB_OK, cgusb_frame_encode(CGUSB_MSG_ACK, &ack, sizeof(ack), frame,
                                                       sizeof(frame), &len));

    feed_all(&rx, (const uint8_t *)log, strlen(log), &sink);
    feed_all(&rx, frame, len, &sink);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sink.frames, "the log text ate the frame behind it");
    TEST_ASSERT_EQUAL_HEX8(CGUSB_MSG_ACK, sink.last_type);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, ((const cgusb_ack_t *)sink.last_payload)->seq);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rx.frames_bad, "the log text was not counted as junk");
}

static void test_receiver_resynchronises_after_a_truncated_frame(void)
{
    cgusb_rx_t rx;
    sink_t sink = {0};
    uint8_t good[CGUSB_MAX_ENCODED];
    size_t good_len = 0;
    const cgusb_channel_t ch = {.channel = 11};

    cgusb_rx_init(&rx);
    TEST_ASSERT_EQUAL_INT(CGUSB_OK, cgusb_frame_encode(CGUSB_MSG_CHANNEL, &ch, sizeof(ch), good,
                                                       sizeof(good), &good_len));

    /* half a frame, cut off mid block, then a whole one */
    feed_all(&rx, good, good_len - 2u, &sink);
    feed_all(&rx, good, good_len, &sink);

    TEST_ASSERT_EQUAL_INT(1, sink.frames);
    TEST_ASSERT_EQUAL_HEX8(CGUSB_MSG_CHANNEL, sink.last_type);
}

static void test_receiver_drops_an_oversized_frame_and_keeps_going(void)
{
    cgusb_rx_t rx;
    sink_t sink = {0};
    uint8_t good[CGUSB_MAX_ENCODED];
    size_t good_len = 0;
    const cgusb_ack_t ack = {.seq = 5};

    cgusb_rx_init(&rx);
    for (size_t i = 0; i < CGUSB_MAX_ENCODED * 3u; i++) {
        cgusb_rx_feed(&rx, 0x41);
    }
    TEST_ASSERT_EQUAL_INT(CGUSB_RX_ERROR, cgusb_rx_feed(&rx, 0x00));

    TEST_ASSERT_EQUAL_INT(CGUSB_OK, cgusb_frame_encode(CGUSB_MSG_ACK, &ack, sizeof(ack), good,
                                                       sizeof(good), &good_len));
    feed_all(&rx, good, good_len, &sink);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sink.frames, "the receiver did not recover from overflow");
}

static void test_receiver_ignores_runs_of_delimiters(void)
{
    cgusb_rx_t rx;
    cgusb_rx_init(&rx);

    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_INT(CGUSB_RX_IDLE, cgusb_rx_feed(&rx, 0x00));
    }
    TEST_ASSERT_EQUAL_UINT32(0, rx.frames_bad);
}

/* ------------------------------------------------- the python reference -- */

static void test_matches_the_python_reference_frames(void)
{
    char message[160];

    for (size_t i = 0; i < CGUSB_VECTOR_COUNT; i++) {
        const cgusb_vector_t *v = &cgusb_vectors[i];
        uint8_t frame[CGUSB_MAX_ENCODED];
        size_t len = 0;

        const int rc = cgusb_frame_encode(v->type, v->payload, v->payload_len, frame,
                                          sizeof(frame), &len);
        snprintf(message, sizeof(message), "%s: encoder returned %d", v->name, rc);
        TEST_ASSERT_EQUAL_INT_MESSAGE(CGUSB_OK, rc, message);

        snprintf(message, sizeof(message), "%s: C made %u bytes, python made %u", v->name,
                 (unsigned)len, (unsigned)v->frame_len);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(v->frame_len, len, message);

        snprintf(message, sizeof(message), "%s: frame bytes differ from the python reference",
                 v->name);
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(v->frame, frame, len, message);
    }
}

static void test_matches_the_python_reference_crc(void)
{
    for (size_t i = 0; i < CGUSB_VECTOR_COUNT; i++) {
        const cgusb_vector_t *v = &cgusb_vectors[i];
        uint8_t body[CGUSB_MAX_FRAME];

        body[0] = v->type;
        memcpy(&body[1], v->payload, v->payload_len);
        TEST_ASSERT_EQUAL_HEX16(v->crc, cgusb_crc16(body, v->payload_len + 1u));
    }
}

static void test_decodes_every_python_reference_frame(void)
{
    for (size_t i = 0; i < CGUSB_VECTOR_COUNT; i++) {
        const cgusb_vector_t *v = &cgusb_vectors[i];
        uint8_t type = 0;
        uint8_t payload[CGUSB_MAX_PAYLOAD];
        size_t payload_len = 0;

        TEST_ASSERT_EQUAL_INT(CGUSB_OK, cgusb_frame_decode(&v->frame[1], v->frame_len - 2u,
                                                           &type, payload, sizeof(payload),
                                                           &payload_len));
        TEST_ASSERT_EQUAL_HEX8(v->type, type);
        TEST_ASSERT_EQUAL_size_t(v->payload_len, payload_len);
        if (payload_len > 0) {
            TEST_ASSERT_EQUAL_UINT8_ARRAY(v->payload, payload, payload_len);
        }
    }
}

/* ------------------------------------------------------------------ run -- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_crc_check_value);
    RUN_TEST(test_crc_of_nothing_is_the_initial_value);
    RUN_TEST(test_crc_notices_a_flipped_bit);
    RUN_TEST(test_crc_notices_swapped_bytes);

    RUN_TEST(test_cobs_round_trip_plain);
    RUN_TEST(test_cobs_round_trip_with_zeros);
    RUN_TEST(test_cobs_round_trip_all_zeros);
    RUN_TEST(test_cobs_round_trip_crosses_the_254_boundary);
    RUN_TEST(test_cobs_decode_rejects_a_zero_inside_the_block);
    RUN_TEST(test_cobs_decode_rejects_a_code_past_the_end);
    RUN_TEST(test_cobs_decode_refuses_to_overrun_the_output);

    RUN_TEST(test_frame_is_delimited_at_both_ends);
    RUN_TEST(test_frame_round_trip);
    RUN_TEST(test_frame_decode_rejects_a_flipped_bit);
    RUN_TEST(test_frame_encode_rejects_a_wrong_length);
    RUN_TEST(test_frame_encode_rejects_an_unknown_type);
    RUN_TEST(test_frame_encode_refuses_a_buffer_that_is_too_small);
    RUN_TEST(test_empty_payload_messages);
    RUN_TEST(test_error_takes_zero_to_thirty_two_detail_bytes);

    RUN_TEST(test_receiver_finds_back_to_back_frames);
    RUN_TEST(test_receiver_survives_log_text_in_front_of_a_frame);
    RUN_TEST(test_receiver_resynchronises_after_a_truncated_frame);
    RUN_TEST(test_receiver_drops_an_oversized_frame_and_keeps_going);
    RUN_TEST(test_receiver_ignores_runs_of_delimiters);

    RUN_TEST(test_matches_the_python_reference_frames);
    RUN_TEST(test_matches_the_python_reference_crc);
    RUN_TEST(test_decodes_every_python_reference_frame);

    return UNITY_END();
}
