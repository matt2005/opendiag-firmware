/* SPDX-License-Identifier: GPL-3.0-only */
/* Receive-side paths of the J2534 wire framing: frames that must be dropped,
 * requests that must be rejected, fragment acknowledgements, the input
 * timeout, magic resynchronisation and front-end registration. */
#include <string.h>
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fake_board.h"
#include "fake_bus.h"
#include "fake_bus_tx_worker.h"
#include "fake_can_bus.h"
#include "fake_clock.h"
#include "fake_port.h"
#include "j2534.h"
#include "j2534.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "td_test.h"

#define HDR 16u
#define FLAG_MORE 1u
#define FLAG_ACK 4u
#define FLAG_RESPONSE 0x80u
#define FRAG_MAX 192u

void td_setup(void) {
    static bool initialized;
    if (!initialized) {
        TEST_ASSERT_EQUAL_INT(ESP_OK, comm_iface_init());
        fake_port_register_all();
        initialized = true;
    }
    j2534_core_stop();
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_init());
    fake_bus_tx_hold(false);
    fake_bus_tx_hold_completion(false);
    fake_clock_reset();
    fake_can_reset();
    fake_bus_reset_all();
    fake_port_reset();
    idf_stub_set_created_task(xTaskGetCurrentTaskHandle());
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_link_start(&j2534_frontend));
    vif_shell_bind();
    vif_link_set_port(fake_port_id(0));
    TEST_ASSERT_TRUE(j2534_frontend.start());
}
void td_teardown(void) {
    j2534_frontend.stop();
    vif_bus_release_all(VIF_OWNER_LINK);
    vif_bus_release_all(VIF_OWNER_SHELL);
    vif_pin_release_all(VIF_OWNER_LINK);
    vif_pin_release_all(VIF_OWNER_SHELL);
    TEST_ASSERT_EQUAL_INT(0, idf_stub_lock_balance());
}

static uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t v = 0xffffffff;
    while (n--) {
        v ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            v = (v >> 1) ^ (0xedb88320u & (0u - (v & 1)));
    }
    return ~v;
}

/* Build a wire frame with every header field under the test's control. */
static size_t build(uint8_t *dst, unsigned version, unsigned op, unsigned flags,
                    uint32_t seq, unsigned offset, const uint8_t *data,
                    unsigned len) {
    memset(dst, 0, HDR + 4 + len);
    memcpy(dst, "J253", 4);
    dst[4] = version;
    dst[5] = op;
    dst[6] = flags;
    dst[7] = flags >> 8;
    j2534_put32(dst + 8, seq);
    dst[12] = offset;
    dst[13] = offset >> 8;
    dst[14] = len;
    dst[15] = len >> 8;
    if (len)
        memcpy(dst + HDR, data, len);
    j2534_put32(dst + HDR + len, crc(dst + 4, 12 + len));
    return HDR + 4 + len;
}

static const uint8_t caps[] = {0x0a, 0};
static const uint8_t blank[FRAG_MAX];

static size_t feed_frame(unsigned op, unsigned flags, uint32_t seq,
                         unsigned offset, const uint8_t *data, unsigned len) {
    uint8_t f[HDR + FRAG_MAX + 4];
    size_t n = build(f, 1, op, flags, seq, offset, data, len);
    fake_port_reset();
    j2534_frontend.feed(f, n);
    return n;
}

/* A fresh capabilities request must still be answered: the receiver is back
 * in a clean state. */
static void expect_working_receiver(void) {
    feed_frame(0, 0, 5, 0, caps, sizeof(caps));
    const uint8_t *r = (const uint8_t *)fake_port_text(0);
    TEST_ASSERT_TRUE(fake_port_len(0) > 20);
    TEST_ASSERT_EQUAL_INT(FLAG_RESPONSE, r[6]);
}

static void expect_reject(void) {
    const uint8_t *r = (const uint8_t *)fake_port_text(0);
    TEST_ASSERT_EQUAL_INT(22, fake_port_len(0));
    TEST_ASSERT_EQUAL_INT(FLAG_RESPONSE, r[6]);
    TEST_ASSERT_EQUAL_INT(0, r[12] | r[13] << 8);
    TEST_ASSERT_EQUAL_INT(2, r[14] | r[15] << 8);
    TEST_ASSERT_EQUAL_INT(8, r[HDR]);
    TEST_ASSERT_EQUAL_INT(J2534_MSG, r[HDR + 1]);
    TEST_ASSERT_EQUAL_INT(crc(r + 4, 14), j2534_u32(r + HDR + 2));
}

TEST(bad_crc_frame_is_dropped_silently) {
    uint8_t f[HDR + 4 + sizeof(caps)];
    size_t n = build(f, 1, 0, 0, 5, 0, caps, sizeof(caps));
    f[n - 4] ^= 0x80;
    fake_port_reset();
    j2534_frontend.feed(f, n);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    expect_working_receiver();
}

TEST(unknown_flag_bits_are_dropped_silently) {
    feed_frame(0, 8, 5, 0, caps, sizeof(caps));
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    feed_frame(0, FLAG_MORE | 0x100, 5, 0, caps, sizeof(caps));
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    expect_working_receiver();
}

TEST(more_fragment_is_acknowledged_with_the_received_total) {
    feed_frame(15, FLAG_MORE, 9, 0, blank, 10);
    const uint8_t *r = (const uint8_t *)fake_port_text(0);
    TEST_ASSERT_EQUAL_INT(20, fake_port_len(0));
    TEST_ASSERT_EQUAL_INT(FLAG_RESPONSE | FLAG_ACK, r[6]);
    TEST_ASSERT_EQUAL_INT(15, r[5]);
    TEST_ASSERT_EQUAL_INT(9, j2534_u32(r + 8));
    TEST_ASSERT_EQUAL_INT(10, r[12] | r[13] << 8);
    TEST_ASSERT_EQUAL_INT(0, r[14] | r[15] << 8);
    TEST_ASSERT_EQUAL_INT(crc(r + 4, 12), j2534_u32(r + HDR));
    feed_frame(15, FLAG_MORE, 9, 10, blank, 7);
    r = (const uint8_t *)fake_port_text(0);
    TEST_ASSERT_EQUAL_INT(17, r[12] | r[13] << 8);
}

TEST(fragment_with_wrong_offset_sequence_or_operation_is_ignored) {
    feed_frame(15, FLAG_MORE, 9, 0, blank, 10);
    feed_frame(15, FLAG_MORE, 9, 11, blank, 4);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    feed_frame(15, FLAG_MORE, 10, 10, blank, 4);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    feed_frame(14, FLAG_MORE, 9, 10, blank, 4);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    /* The original request is still being assembled. */
    feed_frame(15, FLAG_MORE, 9, 10, blank, 4);
    TEST_ASSERT_EQUAL_INT(20, fake_port_len(0));
}

TEST(continuation_without_a_first_fragment_is_ignored) {
    feed_frame(0, 0, 5, 3, caps, sizeof(caps));
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    expect_working_receiver();
}

TEST(empty_more_fragment_rejects_the_request) {
    feed_frame(15, FLAG_MORE, 9, 0, NULL, 0);
    expect_reject();
    /* Assembly was abandoned, so a continuation is now ignored. */
    feed_frame(15, FLAG_MORE, 9, 0, blank, 0);
    feed_frame(15, FLAG_MORE, 9, 5, blank, 3);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
}

TEST(oversize_request_is_rejected_when_the_total_exceeds_the_limit) {
    unsigned offset = 0, acks = 0;
    for (;;) {
        feed_frame(15, FLAG_MORE, 9, offset, blank, FRAG_MAX);
        const uint8_t *r = (const uint8_t *)fake_port_text(0);
        if (!(r[6] & FLAG_ACK))
            break;
        TEST_ASSERT_EQUAL_INT(offset + FRAG_MAX, r[12] | r[13] << 8);
        offset += FRAG_MAX;
        acks++;
        TEST_ASSERT_TRUE(acks < 1000);
    }
    TEST_ASSERT_TRUE(offset <= J2534_RPC_MAX);
    TEST_ASSERT_TRUE(offset + FRAG_MAX > J2534_RPC_MAX);
    expect_reject();
}

TEST(rejected_request_response_can_be_fetched_again) {
    feed_frame(15, FLAG_MORE, 9, 0, NULL, 0);
    expect_reject();
    feed_frame(15, 2, 9, 0, NULL, 0);
    expect_reject();
    /* Wrong sequence, wrong operation, or a payload on a fetch: no answer. */
    feed_frame(15, 2, 8, 0, NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    feed_frame(14, 2, 9, 0, NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    feed_frame(15, 2, 9, 0, blank, 1);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    /* An offset past the end of the response sends nothing. */
    feed_frame(15, 2, 9, 3, NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
}

TEST(input_pause_over_two_seconds_discards_partial_frame) {
    uint8_t f[HDR + 4 + sizeof(caps)];
    size_t n = build(f, 1, 0, 0, 5, 0, caps, sizeof(caps));
    fake_port_reset();
    j2534_frontend.feed(f, 10);
    fake_clock_advance_ms(2001);
    /* The tail alone is not a frame. */
    j2534_frontend.feed(f + 10, n - 10);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    expect_working_receiver();
}

TEST(input_pause_within_two_seconds_keeps_partial_frame) {
    uint8_t f[HDR + 4 + sizeof(caps)];
    size_t n = build(f, 1, 0, 0, 5, 0, caps, sizeof(caps));
    fake_port_reset();
    j2534_frontend.feed(f, 10);
    fake_clock_advance_ms(1999);
    j2534_frontend.feed(f + 10, n - 10);
    TEST_ASSERT_TRUE(fake_port_len(0) > 20);
}

TEST(input_pause_abandons_a_request_being_assembled) {
    feed_frame(15, FLAG_MORE, 9, 0, blank, 10);
    TEST_ASSERT_EQUAL_INT(20, fake_port_len(0));
    fake_clock_advance_ms(2001);
    feed_frame(15, 0, 9, 10, blank, 4);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
}

TEST(garbage_before_magic_is_skipped_and_partial_magic_resyncs) {
    uint8_t f[HDR + 4 + sizeof(caps)];
    size_t n = build(f, 1, 0, 0, 5, 0, caps, sizeof(caps));
    fake_port_reset();
    j2534_frontend.feed((const uint8_t *)"xx", 2);
    j2534_frontend.feed((const uint8_t *)"J2J", 3);
    j2534_frontend.feed(f, n);
    const uint8_t *r = (const uint8_t *)fake_port_text(0);
    TEST_ASSERT_TRUE(fake_port_len(0) > 20);
    TEST_ASSERT_EQUAL_INT(FLAG_RESPONSE, r[6]);
}

TEST(bad_version_header_is_discarded_without_waiting_for_a_body) {
    uint8_t f[HDR + 4 + sizeof(caps)];
    size_t n = build(f, 2, 0, 0, 5, 0, caps, sizeof(caps));
    fake_port_reset();
    j2534_frontend.feed(f, n);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    expect_working_receiver();
}

TEST(oversize_payload_length_header_is_discarded) {
    uint8_t f[HDR + FRAG_MAX + 4 + 8];
    size_t n = build(f, 1, 0, 0, 5, 0, blank, FRAG_MAX);
    /* Little-endian length, one byte over what the firmware accepts. */
    f[14] = (J2534_FRAGMENT_MAX + 1) & 0xff;
    f[15] = (J2534_FRAGMENT_MAX + 1) >> 8;
    fake_port_reset();
    j2534_frontend.feed(f, n);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    expect_working_receiver();
}

TEST(frame_split_across_feed_calls_is_reassembled) {
    uint8_t f[HDR + 4 + sizeof(caps)];
    size_t n = build(f, 1, 0, 0, 5, 0, caps, sizeof(caps));
    fake_port_reset();
    for (size_t i = 0; i < n; i++)
        j2534_frontend.feed(f + i, 1);
    TEST_ASSERT_TRUE(fake_port_len(0) > 20);
}

TEST(registering_the_frontend_makes_it_findable_by_name) {
    j2534_register();
    TEST_ASSERT(vif_frontend_find("j2534") == &j2534_frontend);
    j2534_register();
    TEST_ASSERT(vif_frontend_find("J2534") == &j2534_frontend);
}

/* ------------------------------------------------------------------ *
 * Requests the core holds until they have an answer
 *
 * A select with no channels and a threshold of one cannot be satisfied, so
 * the core holds it for its timeout.
 * ------------------------------------------------------------------ */

#define FLAG_WAKE 8u
#define SELECT_OP (opendiag_Request_select_tag - 1)
#define SELECT_SEQ 9u

/* Without an open device the core answers every other request with NOT_OPEN
 * at once, so a select could never be held. Open one first, as a host does. */
static void open_device(void) {
    opendiag_Request req = {.which_command = opendiag_Request_open_tag};
    uint8_t encoded[16], reply[J2534_RPC_MAX];
    pb_ostream_t out = pb_ostream_from_buffer(encoded, sizeof(encoded));

    TEST_ASSERT_TRUE(pb_encode(&out, opendiag_Request_fields, &req));
    size_t len = j2534_core_request(opendiag_Request_open_tag - 1, encoded,
                                    out.bytes_written, reply);
    opendiag_Response res = opendiag_Response_init_zero;
    pb_istream_t in = pb_istream_from_buffer(reply, len);
    TEST_ASSERT_TRUE(pb_decode(&in, opendiag_Response_fields, &res));
    TEST_ASSERT_EQUAL_INT(0, res.status);
}

static void feed_select(uint32_t seq, uint32_t timeout_ms) {
    opendiag_Request req = {.which_command = opendiag_Request_select_tag};
    uint8_t encoded[32];

    req.command.select =
        (opendiag_Select){.type = 1, .threshold = 1, .timeout_ms = timeout_ms};
    pb_ostream_t out = pb_ostream_from_buffer(encoded, sizeof(encoded));
    TEST_ASSERT_TRUE(pb_encode(&out, opendiag_Request_fields, &req));
    feed_frame(SELECT_OP, 0, seq, 0, encoded, (unsigned)out.bytes_written);
}

/* The one response frame on the port must answer the select with no channels. */
static void expect_empty_select_answer(uint32_t seq) {
    const uint8_t *r = (const uint8_t *)fake_port_text(0);
    opendiag_Response res = opendiag_Response_init_zero;

    TEST_ASSERT_TRUE(fake_port_len(0) >= 20);
    TEST_ASSERT_EQUAL_INT(FLAG_RESPONSE, r[6]);
    TEST_ASSERT_EQUAL_INT(SELECT_OP, r[5]);
    TEST_ASSERT_EQUAL_INT(seq, j2534_u32(r + 8));
    pb_istream_t in = pb_istream_from_buffer(r + HDR, r[14] | r[15] << 8);
    TEST_ASSERT_TRUE(pb_decode(&in, opendiag_Response_fields, &res));
    TEST_ASSERT_EQUAL_INT(0, res.status);
    TEST_ASSERT_EQUAL_INT(0, res.channels_count);
}

TEST(a_select_that_must_wait_is_held_and_answered_when_it_times_out) {
    open_device();
    feed_select(SELECT_SEQ, 5000);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));

    fake_port_reset();
    j2534_frontend.poll();
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));

    fake_clock_advance_ms(5001);
    j2534_frontend.poll();
    expect_empty_select_answer(SELECT_SEQ);

    /* Answered once: the next poll has nothing left to say. */
    fake_port_reset();
    j2534_frontend.poll();
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
}

TEST(a_wake_for_the_held_request_ends_its_wait_early) {
    open_device();
    feed_select(SELECT_SEQ, 5000);

    /* Wrong sequence, wrong operation or a payload: not for this request. */
    feed_frame(SELECT_OP, FLAG_WAKE, SELECT_SEQ + 1, 0, NULL, 0);
    feed_frame(SELECT_OP + 1, FLAG_WAKE, SELECT_SEQ, 0, NULL, 0);
    feed_frame(SELECT_OP, FLAG_WAKE, SELECT_SEQ, 0, blank, 1);
    j2534_frontend.poll();
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));

    feed_frame(SELECT_OP, FLAG_WAKE, SELECT_SEQ, 0, NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
    j2534_frontend.poll();
    expect_empty_select_answer(SELECT_SEQ);
    TEST_ASSERT_EQUAL_INT(0, (int)fake_clock_ms());
}

TEST(a_new_request_drops_the_held_one_without_answering_it) {
    open_device();
    feed_select(SELECT_SEQ, 5000);
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));

    expect_working_receiver();
    fake_port_reset();
    fake_clock_advance_ms(5001);
    j2534_frontend.poll();
    TEST_ASSERT_EQUAL_INT(0, fake_port_len(0));
}
