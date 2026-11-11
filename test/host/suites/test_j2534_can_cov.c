/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file test_j2534_can_cov.c
 * @brief J2534 receive events, connect checks and device lifecycle on CAN.
 *
 * Built only with -DREJSACAN=ON. These are the CAN halves of the stock cases
 * in test_j2534_misc_cov.c that need a bus status the plain CAN fake never
 * produced (link down, overflow, break, start of message, loopback type) or a
 * driver that refuses to close. The harness is a trimmed copy of the one in
 * test_j2534_rejsacan_cov.c.
 */
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

static opendiag_Request req;
static opendiag_Response res;
static uint8_t encoded[J2534_RPC_MAX], reply[J2534_RPC_MAX];
static uint32_t dev;

static void command(unsigned tag) {
    memset(&req, 0, sizeof(req));
    req.which_command = tag;
}
static uint32_t call(void) {
    pb_ostream_t output = pb_ostream_from_buffer(encoded, sizeof(encoded));
    TEST_ASSERT_TRUE(pb_encode(&output, opendiag_Request_fields, &req));
    size_t len = j2534_core_request(req.which_command - 1, encoded,
                                    output.bytes_written, reply);
    pb_istream_t input = pb_istream_from_buffer(reply, len);
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_TRUE(pb_decode(&input, opendiag_Response_fields, &res));
    return res.status;
}

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
    command(opendiag_Request_open_tag);
    TEST_ASSERT_EQUAL_INT(0, call());
    dev = res.id;
}
void td_teardown(void) {
    /* A refused close keeps the claim; let the release below succeed. */
    fake_can_teardown_result(ESP_OK);
    fake_bus_tx_hold(false);
    fake_bus_tx_hold_completion(false);
    j2534_frontend.stop();
    vif_bus_release_all(VIF_OWNER_LINK);
    vif_bus_release_all(VIF_OWNER_SHELL);
    vif_pin_release_all(VIF_OWNER_LINK);
    vif_pin_release_all(VIF_OWNER_SHELL);
    TEST_ASSERT_EQUAL_INT(0, idf_stub_lock_balance());
}

static uint32_t connect_can(uint32_t flags) {
    command(opendiag_Request_connect_tag);
    req.command.connect = (opendiag_Connect){.device = dev,
                                             .protocol = J2534_CAN,
                                             .flags = flags,
                                             .baudrate = 500000,
                                             .connector = 1,
                                             .pins_count = 2,
                                             .pins = {6, 14}};
    TEST_ASSERT_EQUAL_INT(0, call());
    return res.id;
}

static uint32_t connect_with(uint32_t protocol, uint32_t flags, uint32_t rate,
                             uint32_t pin_count, uint32_t pin1,
                             uint32_t pin2) {
    command(opendiag_Request_connect_tag);
    req.command.connect = (opendiag_Connect){.device = dev,
                                             .protocol = protocol,
                                             .flags = flags,
                                             .baudrate = rate,
                                             .connector = 1,
                                             .pins_count = pin_count,
                                             .pins = {pin1, pin2}};
    return call();
}

/* Logical channel with local id 0xe8 + n and remote id 0xe0 + n. */
static uint32_t logical_at(uint32_t physical, uint8_t n) {
    command(opendiag_Request_logical_connect_tag);
    opendiag_LogicalConnect *c = &req.command.logical_connect;
    const uint8_t local[4] = {0, 0, 0x07, (uint8_t)(0xe8 + n)};
    const uint8_t remote[4] = {0, 0, 0x07, (uint8_t)(0xe0 + n)};
    c->physical = physical;
    c->protocol = J2534_ISOTP;
    c->remote_flags = J2534_PAD;
    c->local_address.size = c->remote_address.size = 4;
    memcpy(c->local_address.bytes, local, sizeof(local));
    memcpy(c->remote_address.bytes, remote, sizeof(remote));
    TEST_ASSERT_EQUAL_INT(0, call());
    return res.id;
}

static uint32_t filter(uint32_t ch, uint32_t type, uint8_t mask,
                       uint8_t pattern) {
    command(opendiag_Request_start_filter_tag);
    req.command.start_filter = (opendiag_Filter){.channel = ch, .type = type};
    req.command.start_filter.mask.size = req.command.start_filter.pattern.size =
        1;
    req.command.start_filter.mask.bytes[0] = mask;
    req.command.start_filter.pattern.bytes[0] = pattern;
    TEST_ASSERT_EQUAL_INT(0, call());
    return res.id;
}

static void queue(uint32_t ch, uint32_t proto, uint32_t flags,
                  const uint8_t *data, size_t len) {
    command(opendiag_Request_queue_tag);
    req.command.queue.channel = ch;
    req.command.queue.has_message = true;
    opendiag_Message *m = &req.command.queue.message;
    m->protocol = proto;
    m->handle = 42;
    m->tx_flags = flags;
    m->data.size = len;
    memcpy(m->data.bytes, data, len);
}

static uint32_t read_message(uint32_t ch) {
    command(opendiag_Request_read_tag);
    req.command.read.id = ch;
    return call();
}

static uint32_t ioctl_call(uint32_t target, uint32_t id) {
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = target;
    req.command.ioctl.id = id;
    return call();
}

static uint32_t start_periodic(uint32_t ch, uint32_t interval_ms) {
    command(opendiag_Request_start_periodic_tag);
    opendiag_Periodic *p = &req.command.start_periodic;
    p->channel = ch;
    p->interval_ms = interval_ms;
    p->has_message = true;
    p->message.protocol = J2534_ISOTP;
    p->message.tx_flags = J2534_PAD;
    p->message.data.size = 6;
    memcpy(p->message.data.bytes, "\0\0\x07\xe0\x01\x02", 6);
    TEST_ASSERT_EQUAL_INT(0, call());
    return res.id;
}

static const uint8_t request_frame[] = {0, 0, 7, 0xdf, 0x55};

/* ------------------------------------------------------------------------ *
 * Connect checks for buses the Rejsa build does not have.
 * ------------------------------------------------------------------------ */

TEST(connect_checks_the_data_rate_of_pwm_vpw_and_iso_before_the_bus_exists) {
    TEST_ASSERT_EQUAL_INT(J2534_BAUD,
                          connect_with(J2534_PWM, 0, 9600, 2, 2, 10));
    TEST_ASSERT_EQUAL_INT(J2534_BAUD, connect_with(J2534_VPW, 0, 9600, 1, 2, 0));
    TEST_ASSERT_EQUAL_INT(J2534_BAUD,
                          connect_with(J2534_ISO9141, 0, 1, 2, 7, 15));
    TEST_ASSERT_EQUAL_INT(J2534_BAUD,
                          connect_with(J2534_ISO14230, J2534_K_ONLY,
                                       1000000, 1, 7, 0));
    /* Nothing was claimed and no channel is left behind. */
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(0, fake_can_setup_count());
    connect_can(0);
}

TEST(pwm_and_vpw_connects_with_a_valid_rate_are_refused_by_the_board) {
    TEST_ASSERT_EQUAL_INT(J2534_NOT_SUPPORTED,
                          connect_with(J2534_PWM, 0, 41600, 2, 2, 10));
    TEST_ASSERT_EQUAL_INT(J2534_NOT_SUPPORTED,
                          connect_with(J2534_VPW, 0, 10400, 1, 2, 0));
    TEST_ASSERT_EQUAL_INT(J2534_NOT_SUPPORTED,
                          connect_with(J2534_ISO9141, 0, 10400, 2, 7, 15));
}

/* ------------------------------------------------------------------------ *
 * Receive status events.
 * ------------------------------------------------------------------------ */

TEST(link_down_fails_the_queued_transmit_reports_error_and_blocks_until_bus_on) {
    uint32_t ch = connect_can(0);
    queue(ch, J2534_CAN, 0, request_frame, sizeof(request_frame));
    TEST_ASSERT_EQUAL_INT(0, call());
    fake_can_stage_stale_status(0, 0, NULL, BUS_RX_LINK_DOWN);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_FAILED, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(42, res.message.handle);
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_ERROR, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(4, res.message.data.size);
    TEST_ASSERT_EQUAL_MEM("\x01\x00\x00\x00", res.message.data.bytes, 4);
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));

    /* While the link is down nothing is sent, however often it is polled. */
    queue(ch, J2534_CAN, 0, request_frame, sizeof(request_frame));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());

    /* The driver cannot recover yet; the link stays down. */
    TEST_ASSERT_EQUAL_INT(J2534_INIT, ioctl_call(ch, BUS_IOCTL_BUS_ON));
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());

    fake_can_allow_bus_on(true);
    TEST_ASSERT_EQUAL_INT(0, ioctl_call(ch, BUS_IOCTL_BUS_ON));
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_SUCCESS, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(42, res.message.handle);
}

TEST(link_down_reaches_logical_channels_and_abandons_their_segmented_transmit) {
    uint32_t ch = connect_can(0);
    uint32_t lg = logical_at(ch, 0);
    const uint8_t msg[] = {0, 0, 7, 0xe0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    queue(lg, J2534_ISOTP, J2534_PAD, msg, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x10, fake_can_sent(0)->data[0]);

    fake_can_stage_stale_status(0, 0, NULL, BUS_RX_LINK_DOWN);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(lg));
    TEST_ASSERT_EQUAL_INT(J2534_TX_FAILED, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(42, res.message.handle);
    TEST_ASSERT_EQUAL_INT(0, read_message(lg));
    TEST_ASSERT_EQUAL_INT(J2534_ERROR, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(lg));
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_ERROR, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));

    /* A flow control that arrives late finds no transmit to continue. */
    const uint8_t fc[] = {0x30, 0, 0, 0, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, fc);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
}

TEST(link_down_drops_a_half_received_segmented_message) {
    uint32_t ch = connect_can(0);
    uint32_t lg = logical_at(ch, 0);
    const uint8_t first[] = {0x10, 10, 1, 2, 3, 4, 5, 6};
    const uint8_t last[] = {0x21, 7, 8, 9, 10, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, first);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(lg));
    TEST_ASSERT_EQUAL_INT(J2534_START, res.message.rx_status);
    fake_can_stage_stale_status(0, 0, NULL, BUS_RX_LINK_DOWN);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(lg));
    TEST_ASSERT_EQUAL_INT(J2534_ERROR, res.message.rx_status);
    /* The consecutive frame has no first frame to belong to now. */
    fake_can_stage_stale(0x7e8, 8, last);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(lg));
}

TEST(break_and_start_of_message_are_events_without_a_filter) {
    uint32_t ch = connect_can(0);
    const uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    fake_can_stage_stale_status(0x7e8, 8, data, BUS_RX_BREAK);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_BREAK, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(0, res.message.data.size);
    fake_can_stage_stale_status(0x7e8, 8, data, BUS_RX_START_OF_MSG);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_START, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(0, res.message.data.size);
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
}

TEST(a_lost_frame_marker_flags_the_physical_channel_and_every_logical_one) {
    uint32_t ch = connect_can(0);
    uint32_t a = logical_at(ch, 0);
    uint32_t b = logical_at(ch, 1);
    fake_can_stage_stale_status(0, 0, NULL, BUS_RX_BUFFER_OVERFLOW);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_OVERFLOW, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_OVERFLOW, read_message(a));
    TEST_ASSERT_EQUAL_INT(J2534_OVERFLOW, read_message(b));
    /* The loss is reported once. */
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(a));
}

TEST(a_frame_that_follows_lost_ones_carries_the_overflow_flag_once) {
    uint32_t ch = connect_can(0);
    filter(ch, 1, 0, 0);
    const uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    fake_can_stage_stale_status(0x100, 8, data, BUS_RX_BUFFER_OVERFLOW);
    fake_can_stage_stale(0x101, 8, data);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_RX_OVERFLOW, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(0x100, res.message.data.bytes[2] << 8 |
                                     res.message.data.bytes[3]);
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(0, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
}

TEST(own_transmit_loopback_frames_are_marked_and_skip_the_repeat_matcher) {
    uint32_t ch = connect_can(0);
    filter(ch, 1, 0, 0);
    const uint8_t data[3] = {1, 2, 3};
    fake_can_stage_stale_status(0x7e0, 3, data, BUS_RX_TX_MSG_TYPE);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_MSG, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(7, res.message.data.size);
    TEST_ASSERT_EQUAL_MEM("\0\0\x07\xe0\x01\x02\x03", res.message.data.bytes, 7);
}

/* ------------------------------------------------------------------------ *
 * Scheduling and transmit ordering.
 * ------------------------------------------------------------------------ */

TEST(equal_due_periodics_on_two_logical_channels_go_out_in_id_order) {
    uint32_t ch = connect_can(0);
    uint32_t a = logical_at(ch, 0);
    uint32_t b = logical_at(ch, 1);
    /* b's job is older than a's, though a is searched first. */
    start_periodic(b, 50);
    start_periodic(a, 50);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
}

TEST(two_pending_flow_controls_are_sent_earliest_deadline_first) {
    uint32_t ch = connect_can(0);
    logical_at(ch, 0);
    logical_at(ch, 1);
    const uint8_t first_a[] = {0x10, 10, 1, 2, 3, 4, 5, 6};
    const uint8_t first_b[] = {0x10, 10, 1, 2, 3, 4, 5, 6};
    fake_can_stage_stale(0x7e8, 8, first_a);
    fake_can_stage_stale(0x7e9, 8, first_b);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x7e0, fake_can_sent(0)->id);
    TEST_ASSERT_EQUAL_INT(0x7e1, fake_can_sent(1)->id);
}

TEST(a_segmented_frame_that_the_bus_has_not_finished_is_still_confirmed) {
    uint32_t ch = connect_can(0);
    uint32_t lg = logical_at(ch, 0);
    const uint8_t msg[] = {0, 0, 7, 0xe0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const uint8_t fc[] = {0x30, 0, 0, 0, 0, 0, 0, 0};
    queue(lg, J2534_ISOTP, J2534_PAD, msg, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(0, call());
    /* The frame is on the wire, but the bus has not reported it complete. */
    fake_bus_tx_hold_completion(true);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(lg));
    /* The first frame counts as confirmed, so the flow control is accepted. */
    fake_bus_tx_hold_completion(false);
    fake_can_stage_stale(0x7e8, 8, fc);
    j2534_core_poll();
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x21, fake_can_sent(1)->data[0]);
    TEST_ASSERT_EQUAL_INT(0, read_message(lg));
    TEST_ASSERT_EQUAL_INT(J2534_TX_SUCCESS, res.message.rx_status);
}

static void release_bus(void *arg) {
    (void)arg;
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
}

TEST(a_transmit_the_bus_refuses_to_take_fails_without_reaching_the_wire) {
    uint32_t ch = connect_can(0);
    queue(ch, J2534_CAN, 0, request_frame, sizeof(request_frame));
    TEST_ASSERT_EQUAL_INT(0, call());
    /* Another task takes the bus away while the poll is receiving. */
    fake_can_on_receive(release_bus, NULL);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, read_message(ch));
    const vif_bus_cfg_t cfg = {.bitrate = 500000, .manual_recovery = true};
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &cfg));
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_FAILED, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(42, res.message.handle);
    /* The queue is free again: the next request goes out. */
    queue(ch, J2534_CAN, 0, request_frame, sizeof(request_frame));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
}

TEST(repeat_masks_on_a_physical_can_channel_accept_only_the_29bit_flag) {
    uint32_t ch = connect_can(0);
    j2534_repeat_setup_t setup = {
        .interval_ms = 20,
        .message = {.protocol = J2534_CAN,
                    .len = 5,
                    .data = {0, 0, 0x07, 0xdf, 0x01}},
        .mask = {.protocol = J2534_CAN, .len = 1, .data = {0}},
        .pattern = {.protocol = J2534_CAN, .len = 1, .data = {0}},
    };
    uint32_t id = 0;
    setup.mask.flags = setup.pattern.flags = J2534_ADDR;
    TEST_ASSERT_EQUAL_INT(J2534_FLAGS, j2534_core_repeat_start(ch, &setup, &id));
    TEST_ASSERT_EQUAL_INT(0, id);
    setup.mask.flags = setup.pattern.flags = 0;
    TEST_ASSERT_EQUAL_INT(0, j2534_core_repeat_start(ch, &setup, &id));
    TEST_ASSERT_TRUE(id != 0);
}

/* ------------------------------------------------------------------------ *
 * Requests that name a channel which is no longer live, and the device
 * lifecycle when the driver refuses to close.
 * ------------------------------------------------------------------------ */

TEST(a_channel_whose_bus_was_released_elsewhere_answers_channel_error) {
    uint32_t ch = connect_can(0);
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, read_message(ch));
    queue(ch, J2534_CAN, 0, request_frame, sizeof(request_frame));
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, call());
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, ioctl_call(ch, BUS_IOCTL_CLEAR_RX_QUEUE));
    /* Polling a dead channel sends nothing. */
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
    /* It can still be disconnected. */
    command(opendiag_Request_disconnect_tag);
    req.command.disconnect.id = ch;
    TEST_ASSERT_EQUAL_INT(0, call());
}

TEST(disconnect_reports_failed_when_the_driver_will_not_close) {
    uint32_t ch = connect_can(0);
    uint32_t lg = logical_at(ch, 0);
    fake_can_teardown_result(ESP_FAIL);
    command(opendiag_Request_disconnect_tag);
    req.command.disconnect.id = ch;
    TEST_ASSERT_EQUAL_INT(J2534_FAILED, call());
    /* The channels survive the refusal, but the half-closed bus is not live. */
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, read_message(lg));
    fake_can_teardown_result(ESP_OK);
    command(opendiag_Request_disconnect_tag);
    req.command.disconnect.id = ch;
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, read_message(lg));
}

TEST(close_reports_failed_when_the_driver_will_not_close_and_keeps_the_device) {
    uint32_t ch = connect_can(0);
    fake_can_teardown_result(ESP_FAIL);
    command(opendiag_Request_close_tag);
    req.command.close.id = dev;
    TEST_ASSERT_EQUAL_INT(J2534_FAILED, call());
    /* Still open, though its bus is not live, so a second open is refused. */
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, read_message(ch));
    command(opendiag_Request_open_tag);
    TEST_ASSERT_EQUAL_INT(J2534_IN_USE, call());
    fake_can_teardown_result(ESP_OK);
    command(opendiag_Request_close_tag);
    req.command.close.id = dev;
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(J2534_NOT_OPEN, read_message(ch));
}

TEST(open_reports_failed_while_a_stale_claim_cannot_be_torn_down) {
    connect_can(0);
    /* The service stops without closing the bus; the claim is left behind. */
    j2534_core_stop();
    fake_can_teardown_result(ESP_FAIL);
    command(opendiag_Request_open_tag);
    TEST_ASSERT_EQUAL_INT(J2534_FAILED, call());
    TEST_ASSERT_EQUAL_INT(0, res.id);
    fake_can_teardown_result(ESP_OK);
    command(opendiag_Request_open_tag);
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_TRUE(res.id != 0);
    TEST_ASSERT_FALSE(fake_can_is_up());
}
