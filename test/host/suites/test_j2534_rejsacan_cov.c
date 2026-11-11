/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file test_j2534_rejsacan.c
 * @brief J2534 answers on the CAN only RejsaCAN build.
 *
 * Built only with -DREJSACAN=ON. The harness is a trimmed copy of the one in
 * test_j2534.c.
 */
#include <string.h>
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
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
static uint32_t connect_iso9141(void) {
    command(opendiag_Request_connect_tag);
    req.command.connect = (opendiag_Connect){.device = dev,
                                             .protocol = J2534_ISO9141,
                                             .flags = J2534_K_ONLY,
                                             .baudrate = 10400,
                                             .connector = 1,
                                             .pins_count = 1,
                                             .pins = {7}};
    return call();
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
    j2534_frontend.stop();
    vif_bus_release_all(VIF_OWNER_LINK);
    vif_bus_release_all(VIF_OWNER_SHELL);
    vif_pin_release_all(VIF_OWNER_LINK);
    vif_pin_release_all(VIF_OWNER_SHELL);
    TEST_ASSERT_EQUAL_INT(0, idf_stub_lock_balance());
}

TEST(programming_voltage_on_pin_13_is_not_supported) {
    command(opendiag_Request_voltage_tag);
    req.command.voltage = (opendiag_Voltage){dev, 1, 13, 12000};
    TEST_ASSERT_EQUAL_INT(J2534_NOT_SUPPORTED, call());
    req.command.voltage.millivolts = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(J2534_NOT_SUPPORTED, call());
}

TEST(a_k_line_connect_is_not_supported_and_leaves_no_channel) {
    /* More attempts than there are physical channels: a leaked channel would
     * turn one of these into J2534_LIMIT. */
    for (unsigned i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(J2534_NOT_SUPPORTED, connect_iso9141());
    }
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_KLINE));
}

/* ------------------------------------------------------------------------ *
 * CAN and ISO-TP coverage of main/j2534.c.
 *
 * The helpers and most cases below are the CAN halves of the stock suites
 * test_j2534_cfg_cov.c and test_j2534_misc_cov.c. Cases that need K-line,
 * PWM or VPW stay with those suites; the Rejsa build has no such buses.
 * ------------------------------------------------------------------------ */

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

static uint32_t connect_with(const opendiag_Connect *c) {
    command(opendiag_Request_connect_tag);
    req.command.connect = *c;
    return call();
}

static uint32_t logical(uint32_t physical, bool extended) {
    command(opendiag_Request_logical_connect_tag);
    opendiag_LogicalConnect *c = &req.command.logical_connect;
    c->physical = physical;
    c->protocol = J2534_ISOTP;
    c->local_flags = extended ? J2534_ADDR : 0;
    c->remote_flags = c->local_flags | J2534_PAD;
    c->local_address.size = c->remote_address.size = extended ? 5 : 4;
    memcpy(c->local_address.bytes, "\0\0\x07\xe8\xf1", 5);
    memcpy(c->remote_address.bytes, "\0\0\x07\xe0\x10", 5);
    TEST_ASSERT_EQUAL_INT(0, call());
    return res.id;
}

/* Logical channel on ids 0x7e8/0x7e0 with explicit connection flags. */
static uint32_t logical_flags(uint32_t physical, uint32_t flags) {
    command(opendiag_Request_logical_connect_tag);
    opendiag_LogicalConnect *c = &req.command.logical_connect;
    c->physical = physical;
    c->protocol = J2534_ISOTP;
    c->flags = flags;
    c->remote_flags = J2534_PAD;
    c->local_address.size = c->remote_address.size = 4;
    memcpy(c->local_address.bytes, "\0\0\x07\xe8", 4);
    memcpy(c->remote_address.bytes, "\0\0\x07\xe0", 4);
    TEST_ASSERT_EQUAL_INT(0, call());
    return res.id;
}

static uint32_t logical_status(uint32_t physical, uint8_t local_last,
                               uint8_t remote_last, bool legacy) {
    command(opendiag_Request_logical_connect_tag);
    opendiag_LogicalConnect *c = &req.command.logical_connect;
    const uint8_t local[4] = {0, 0, 0x07, local_last};
    const uint8_t remote[4] = {0, 0, 0x07, remote_last};
    c->physical = physical;
    c->protocol = J2534_ISOTP;
    c->local_flags = 0;
    c->remote_flags = J2534_PAD;
    c->legacy_channel = legacy;
    c->local_address.size = c->remote_address.size = 4;
    memcpy(c->local_address.bytes, local, sizeof(local));
    memcpy(c->remote_address.bytes, remote, sizeof(remote));
    return call();
}

static uint32_t logical_at(uint32_t physical, uint8_t local_last,
                           uint8_t remote_last, bool legacy) {
    TEST_ASSERT_EQUAL_INT(0,
                          logical_status(physical, local_last, remote_last,
                                         legacy));
    return res.id;
}

static uint32_t logical_with_ids(uint32_t physical, uint8_t local_id,
                                 uint8_t remote_id) {
    command(opendiag_Request_logical_connect_tag);
    opendiag_LogicalConnect *c = &req.command.logical_connect;
    c->physical = physical;
    c->protocol = J2534_ISOTP;
    c->local_address.size = c->remote_address.size = 4;
    memcpy(c->local_address.bytes, "\0\0\x07\xe8", 4);
    memcpy(c->remote_address.bytes, "\0\0\x07\xe0", 4);
    c->local_address.bytes[3] = local_id;
    c->remote_address.bytes[3] = remote_id;
    TEST_ASSERT_EQUAL_INT(0, call());
    return res.id;
}

static uint32_t logical_request(uint32_t physical, uint32_t protocol,
                                uint32_t flags, uint8_t local_id,
                                uint32_t local_size) {
    command(opendiag_Request_logical_connect_tag);
    opendiag_LogicalConnect *c = &req.command.logical_connect;
    c->physical = physical;
    c->protocol = protocol;
    c->flags = flags;
    c->local_address.size = local_size;
    c->remote_address.size = 4;
    memcpy(c->local_address.bytes, "\0\0\x07\xe8", 4);
    memcpy(c->remote_address.bytes, "\0\0\x07\xe0", 4);
    c->local_address.bytes[3] = local_id;
    return call();
}

static uint32_t legacy_logical(uint32_t physical, uint8_t local_id,
                               uint8_t remote_id) {
    command(opendiag_Request_logical_connect_tag);
    opendiag_LogicalConnect *c = &req.command.logical_connect;
    c->physical = physical;
    c->protocol = J2534_ISOTP;
    c->legacy_channel = true;
    c->local_address.size = c->remote_address.size = 4;
    memcpy(c->local_address.bytes, "\0\0\x07\xe8", 4);
    memcpy(c->remote_address.bytes, "\0\0\x07\xe0", 4);
    c->local_address.bytes[3] = local_id;
    c->remote_address.bytes[3] = remote_id;
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

/* A pass-all filter of @p len bytes, mask and pattern both zero. */
static uint32_t filter_bytes(uint32_t ch, uint32_t type, uint32_t flags,
                             size_t len) {
    command(opendiag_Request_start_filter_tag);
    req.command.start_filter =
        (opendiag_Filter){.channel = ch, .type = type, .flags = flags};
    req.command.start_filter.mask.size = (pb_size_t)len;
    req.command.start_filter.pattern.size = (pb_size_t)len;
    TEST_ASSERT_EQUAL_INT(0, call());
    return res.id;
}

static uint32_t start_filter_raw(uint32_t ch, uint32_t type, uint32_t flags,
                                 size_t mask_len, size_t pattern_len) {
    command(opendiag_Request_start_filter_tag);
    req.command.start_filter =
        (opendiag_Filter){.channel = ch, .type = type, .flags = flags};
    req.command.start_filter.mask.size = mask_len;
    req.command.start_filter.pattern.size = pattern_len;
    return call();
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

static uint32_t start_periodic_can(uint32_t ch, uint32_t interval_ms,
                                   uint32_t protocol) {
    command(opendiag_Request_start_periodic_tag);
    opendiag_Periodic *p = &req.command.start_periodic;
    p->channel = ch;
    p->interval_ms = interval_ms;
    p->has_message = true;
    p->message.protocol = protocol;
    p->message.data.size = 5;
    memcpy(p->message.data.bytes, "\0\0\x07\xdf\x55", 5);
    return call();
}

static uint32_t hds_init(uint32_t channel, size_t length, bool no_response) {
    command(opendiag_Request_fast_init_tag);
    req.command.fast_init.channel = channel;
    req.command.fast_init.no_response = no_response;
    req.command.fast_init.data.size = (pb_size_t)length;
    memset(req.command.fast_init.data.bytes, 0x55, length);
    return call();
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

static size_t frame(uint8_t *dst, unsigned op, unsigned flags, unsigned offset,
                    const uint8_t *data, unsigned len) {
    memset(dst, 0, 20 + len);
    memcpy(dst, "J253", 4);
    dst[4] = 1;
    dst[5] = op;
    dst[6] = flags;
    j2534_put32(dst + 8, 77);
    dst[12] = offset;
    dst[13] = offset >> 8;
    dst[14] = len;
    dst[15] = len >> 8;
    memcpy(dst + 16, data, len);
    j2534_put32(dst + 16 + len, crc(dst + 4, 12 + len));
    return 20 + len;
}

static uint32_t wire_call(void) {
    pb_ostream_t enc = pb_ostream_from_buffer(encoded, sizeof(encoded));
    TEST_ASSERT_TRUE(pb_encode(&enc, opendiag_Request_fields, &req));
    unsigned op = req.which_command - 1;
    uint8_t f[212];
    size_t n = frame(f, op, 0, 0, encoded, enc.bytes_written);
    fake_port_reset();
    j2534_frontend.feed(f, n);
    size_t used = 0;
    for (;;) {
        const uint8_t *data = (const uint8_t *)fake_port_text(0);
        TEST_ASSERT_TRUE(fake_port_len(0) >= 20);
        unsigned len = data[14] | (unsigned)data[15] << 8;
        TEST_ASSERT_EQUAL_INT(used, data[12] | (unsigned)data[13] << 8);
        TEST_ASSERT_EQUAL_INT(20 + len, fake_port_len(0));
        TEST_ASSERT_EQUAL_INT(crc(data + 4, 12 + len),
                              j2534_u32(data + 16 + len));
        TEST_ASSERT_TRUE(used + len <= sizeof(reply));
        memcpy(reply + used, data + 16, len);
        used += len;
        if (!(data[6] & 1))
            break;
        n = frame(f, op, 2, used, encoded, 0);
        fake_port_reset();
        j2534_frontend.feed(f, n);
    }
    pb_istream_t input = pb_istream_from_buffer(reply, used);
    TEST_ASSERT_TRUE(pb_decode(&input, opendiag_Response_fields, &res));
    return res.status;
}

/* Configuration and ioctl helpers. The TST_ values are the wire values of the
 * private enums in j2534.c. */
enum {
    TST_IOCTL_GET_CONFIG = 1,
    TST_IOCTL_SET_CONFIG = 2,
    TST_IOCTL_CLEAR_FILTERS = 10,
    TST_CFG_BLOCK_SIZE = 0x1e,
    TST_CFG_STMIN = 0x1f,
    TST_CFG_WAIT_LIMIT = 0x25,
    TST_CFG_PAD_VALUE = 0x2b,
    TST_CFG_N_AS = 0x2c,
    TST_CFG_N_AR = 0x2d,
    TST_CFG_N_BS = 0x2e,
    TST_CFG_N_CR = 0x2f,
};

static uint32_t ioctl_call(uint32_t target, uint32_t id) {
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = target;
    req.command.ioctl.id = id;
    return call();
}

static uint32_t ioctl_with_data(uint32_t target, uint32_t id,
                                const uint8_t *data, size_t len) {
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = target;
    req.command.ioctl.id = id;
    req.command.ioctl.data.size = (pb_size_t)len;
    memcpy(req.command.ioctl.data.bytes, data, len);
    return call();
}

static uint32_t config_call(uint32_t target, bool set, uint32_t param,
                            uint32_t value) {
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = target;
    req.command.ioctl.id = set ? TST_IOCTL_SET_CONFIG : TST_IOCTL_GET_CONFIG;
    req.command.ioctl.config_count = 1;
    req.command.ioctl.config[0] =
        (opendiag_Config){.parameter = param, .value = value};
    return call();
}

static uint32_t config_get(uint32_t target, uint32_t param) {
    TEST_ASSERT_EQUAL_INT(0, config_call(target, false, param, 0));
    TEST_ASSERT_EQUAL_INT(1, res.config_count);
    TEST_ASSERT_EQUAL_INT(param, res.config[0].parameter);
    return res.config[0].value;
}

static void config_set(uint32_t target, uint32_t param, uint32_t value) {
    TEST_ASSERT_EQUAL_INT(0, config_call(target, true, param, value));
}

static void disconnect_channel(uint32_t channel) {
    command(opendiag_Request_disconnect_tag);
    req.command.disconnect.id = channel;
    TEST_ASSERT_EQUAL_INT(0, call());
}

/* Repeat helpers. A CAN repeat sends 0x7df with one data byte and matches
 * every received frame, because a zero mask ignores all bits. */
static j2534_repeat_setup_t repeat_setup(uint32_t protocol,
                                         uint32_t condition) {
    j2534_repeat_setup_t setup = {
        .interval_ms = 20,
        .condition = condition,
        .message = {.protocol = protocol,
                    .len = 5,
                    .data = {0x68, 0x6a, 0xf1, 1, 0}},
        .mask = {.protocol = protocol, .len = 1, .data = {0xff}},
        .pattern = {.protocol = protocol, .len = 1, .data = {0x48}},
    };
    return setup;
}

static j2534_repeat_setup_t can_repeat(uint32_t condition) {
    j2534_repeat_setup_t setup = repeat_setup(J2534_CAN, condition);
    memcpy(setup.message.data, "\0\0\x07\xdf\x01", 5);
    setup.mask.data[0] = 0;
    setup.pattern.data[0] = 0;
    return setup;
}

static uint32_t start_repeat(uint32_t channel, j2534_repeat_setup_t *setup) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(0, j2534_core_repeat_start(channel, setup, &id));
    TEST_ASSERT_TRUE(id != 0);
    return id;
}

static bool repeat_active(uint32_t channel, uint32_t id) {
    bool active = false;
    TEST_ASSERT_EQUAL_INT(0, j2534_core_repeat_query(channel, id, &active));
    return active;
}

static bool legacy_scenario(bool legacy) {
    uint32_t physical = connect_can(0);
    uint32_t a = logical_at(physical, 0xe8, 0xe0, legacy);
    uint32_t b = logical_at(physical, 0xe9, 0xe1, legacy);
    j2534_repeat_setup_t setup = repeat_setup(J2534_ISOTP, 0);
    const uint8_t target[] = {0, 0, 7, 0xe1, 0x3e};
    setup.message.flags = 0;
    setup.message.len = sizeof(target);
    memcpy(setup.message.data, target, sizeof(target));
    setup.mask.data[0] = 0;
    setup.pattern.data[0] = 0;
    uint32_t id = start_repeat(b, &setup);
    j2534_core_poll();
    fake_clock_advance_ms(1);
    const uint8_t first[] = {0x10, 10, 1, 2, 3, 4, 5, 6};
    const uint8_t last[] = {0x21, 7, 8, 9, 10, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, first);
    j2534_core_poll();
    TEST_ASSERT_TRUE(repeat_active(b, id));
    fake_can_stage_stale(0x7e8, 8, last);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(a));
    TEST_ASSERT_EQUAL_INT(J2534_START, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(0, read_message(a));
    TEST_ASSERT_EQUAL_INT(14, res.message.data.size);
    bool active = repeat_active(b, id);
    disconnect_channel(physical);
    return active;
}

/* ------------------------------------------------------------------------ *
 * Ported CAN cases.
 * ------------------------------------------------------------------------ */

TEST(capabilities_are_explicit_about_limits) {
    command(opendiag_Request_capabilities_tag);
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_TRUE(res.has_capabilities);
    TEST_ASSERT_FALSE(res.capabilities.full_j2534_compliance);
    TEST_ASSERT_EQUAL_INT(2, res.capabilities.logical_channels);
    TEST_ASSERT_EQUAL_INT(6, res.capabilities.protocols_count);
    TEST_ASSERT_EQUAL_INT(260, res.capabilities.fast_init_max_data);
}

TEST(device_and_channel_handles_are_validated) {
    command(opendiag_Request_open_tag);
    TEST_ASSERT_EQUAL_INT(J2534_IN_USE, call());
    command(opendiag_Request_close_tag);
    req.command.close.id = dev + 1;
    TEST_ASSERT_EQUAL_INT(J2534_DEVICE, call());
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, read_message(0));
    command(opendiag_Request_close_tag);
    req.command.close.id = dev;
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(J2534_NOT_OPEN, read_message(123));
}

TEST(conflicting_open_does_not_reconfigure_existing_driver) {
    uint32_t ch = connect_can(0);
    command(opendiag_Request_connect_tag);
    req.command.connect = (opendiag_Connect){.device = dev,
                                             .protocol = J2534_CAN,
                                             .baudrate = 250000,
                                             .connector = 1,
                                             .pins_count = 2,
                                             .pins = {6, 14}};
    TEST_ASSERT_EQUAL_INT(J2534_CONFLICT, call());
    TEST_ASSERT_EQUAL_INT(500000, fake_can_last_baud());
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
}

TEST(shell_claim_is_respected) {
    vif_bus_cfg_t cfg = {.bitrate = 500000};
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_SHELL, VIF_BUS_CAN, &cfg));
    command(opendiag_Request_connect_tag);
    req.command.connect = (opendiag_Connect){.device = dev,
                                             .protocol = J2534_CAN,
                                             .baudrate = 500000,
                                             .connector = 1,
                                             .pins_count = 2,
                                             .pins = {6, 14}};
    TEST_ASSERT_EQUAL_INT(J2534_CONFLICT, call());
    TEST_ASSERT_TRUE(vif_bus_is_open(VIF_OWNER_SHELL, VIF_BUS_CAN));
}

TEST(connect_rejects_bad_device_connector_baud_protocol_pins_and_flags) {
    opendiag_Connect c = {.device = dev + 1,
                          .protocol = J2534_CAN,
                          .baudrate = 500000,
                          .connector = 1,
                          .pins_count = 2,
                          .pins = {6, 14}};
    TEST_ASSERT_EQUAL_INT(J2534_DEVICE, connect_with(&c));
    c.device = dev;
    c.connector = 2;
    TEST_ASSERT_EQUAL_INT(J2534_PIN, connect_with(&c));
    c.connector = 1;
    c.pins_count = 1;
    /* A wrong pin count is a resource conflict (J2534_CONFLICT), not a wrong
     * pin; upstream connect_distinguishes_resource_count_from_wrong_pins. */
    TEST_ASSERT_EQUAL_INT(J2534_CONFLICT, connect_with(&c));
    c.pins_count = 2;
    c.pins[0] = 7;
    TEST_ASSERT_EQUAL_INT(J2534_PIN, connect_with(&c));
    c.pins[0] = 6;
    c.baudrate = 100000;
    TEST_ASSERT_EQUAL_INT(J2534_BAUD, connect_with(&c));
    c.baudrate = 500000;
    c.flags = J2534_CHECKSUM_DISABLED;
    TEST_ASSERT_EQUAL_INT(J2534_FLAGS, connect_with(&c));
    c.flags = 0;
    c.protocol = J2534_ISOTP;
    TEST_ASSERT_EQUAL_INT(J2534_PROTOCOL, connect_with(&c));
    TEST_ASSERT_FALSE(vif_any_pin_active());
    connect_can(0);
}

TEST(connect_open_failure_releases_the_claim_and_reports_conflict) {
    opendiag_Connect c = {.device = dev,
                          .protocol = J2534_CAN,
                          .baudrate = 500000,
                          .connector = 1,
                          .pins_count = 2,
                          .pins = {6, 14}};
    fake_can_fail_setup(true);
    TEST_ASSERT_EQUAL_INT(J2534_CONFLICT, connect_with(&c));
    TEST_ASSERT_FALSE(fake_can_is_up());
    TEST_ASSERT_FALSE(vif_bus_is_open(VIF_OWNER_LINK, VIF_BUS_CAN));
    fake_can_fail_setup(false);
    TEST_ASSERT_EQUAL_INT(0, connect_with(&c));
    TEST_ASSERT_TRUE(fake_can_is_up());
    TEST_ASSERT_TRUE(vif_bus_is_open(VIF_OWNER_LINK, VIF_BUS_CAN));
}

TEST(default_filter_blocks_and_block_overrides_pass) {
    uint32_t ch = connect_can(0);
    const uint8_t data[] = {1, 2, 3};
    fake_can_stage_stale(0x7e8, 3, data);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    filter(ch, 1, 0, 0);
    fake_can_stage_stale(0x7e8, 3, data);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(7, res.message.data.size);
    TEST_ASSERT_EQUAL_MEM("\0\0\x07\xe8\x01\x02\x03", res.message.data.bytes,
                          7);
    TEST_ASSERT_EQUAL_INT(7, res.message.extra_data_index);
    filter(ch, 2, 0, 0);
    fake_can_stage_stale(0x7e8, 3, data);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
}

TEST(queued_is_not_transmitted_until_poll_and_completion_bypasses_filter) {
    uint32_t ch = connect_can(0);
    queue(ch, J2534_CAN, 0, (const uint8_t *)"\0\0\x07\xdf\x01\x02", 6);
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(J2534_FULL, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_SUCCESS, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(42, res.message.handle);
    TEST_ASSERT_EQUAL_INT(0, res.message.data.size);
}

TEST(failed_send_produces_failed_indication) {
    uint32_t ch = connect_can(0);
    queue(ch, J2534_CAN, 0, (const uint8_t *)"\0\0\x07\xdf", 4);
    TEST_ASSERT_EQUAL_INT(0, call());
    fake_can_fail_next_send();
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_FAILED, res.message.rx_status);
}

TEST(invalid_can_identifiers_and_protocols_are_rejected) {
    uint32_t ch = connect_can(0);
    queue(ch, J2534_CAN, 0, (const uint8_t *)"\0\0\x08\x00", 4);
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
    req.command.queue.message.protocol = J2534_ISOTP;
    TEST_ASSERT_EQUAL_INT(J2534_MSG_PROTOCOL, call());
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
}

TEST(physical_can_queue_rejects_bad_flags_identifiers_and_length) {
    uint32_t ch = connect_can(J2534_CAN_BOTH);
    const uint8_t std_frame[] = {0, 0, 7, 0xdf, 1};
    queue(ch, J2534_CAN, J2534_ADDR, std_frame, sizeof(std_frame));
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
    const uint8_t thirteen[13] = {0, 0, 7, 0xdf, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    queue(ch, J2534_CAN, 0, thirteen, sizeof(thirteen));
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
    /* 0x20000000 is one past the 29-bit identifier range. */
    const uint8_t too_wide[] = {0x20, 0, 0, 0, 1};
    queue(ch, J2534_CAN, J2534_29BIT, too_wide, sizeof(too_wide));
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
    const uint8_t wide[] = {0x18, 0xda, 0xf1, 0x10, 0x01};
    queue(ch, J2534_CAN, J2534_29BIT, wide, sizeof(wide));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(CAN_EFF_FLAG | 0x18daf110u, fake_can_sent(0)->id);
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent(0)->dlc);
}

TEST(logical_single_frame_receive_is_independent_of_physical_filter) {
    uint32_t p = connect_can(0), ch = logical(p, false);
    const uint8_t data[] = {3, 0x41, 0x0c, 0x55, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, data);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_ISOTP, res.message.protocol);
    TEST_ASSERT_EQUAL_MEM("\0\0\x07\xe8\x41\x0c\x55", res.message.data.bytes,
                          7);
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(p));
}

TEST(logical_multiframe_receive_emits_start_and_flow_control) {
    uint32_t ch = logical(connect_can(0), false);
    const uint8_t first[] = {0x10, 10, 1, 2, 3, 4, 5, 6};
    const uint8_t last[] = {0x21, 7, 8, 9, 10, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, first);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x7e0, fake_can_sent(0)->id);
    TEST_ASSERT_EQUAL_INT(0x30, fake_can_sent(0)->data[0]);
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_START, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(0, res.message.extra_data_index);
    fake_can_stage_stale(0x7e8, 8, last);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(14, res.message.data.size);
    for (unsigned i = 0; i < 10; i++)
        TEST_ASSERT_EQUAL_INT(i + 1, res.message.data.bytes[i + 4]);
}

TEST(logical_transmit_waits_for_flow_control) {
    uint32_t ch = logical(connect_can(0), false);
    const uint8_t msg[] = {0, 0, 7, 0xe0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    queue(ch, J2534_ISOTP, J2534_PAD, msg, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x10, fake_can_sent(0)->data[0]);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    const uint8_t fc[] = {0x30, 0, 0, 0, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, fc);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x21, fake_can_sent(1)->data[0]);
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_SUCCESS, res.message.rx_status);
}

TEST(parent_disconnect_invalidates_logical_handle) {
    uint32_t p = connect_can(0), ch = logical(p, false);
    command(opendiag_Request_disconnect_tag);
    req.command.disconnect.id = p;
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, read_message(ch));
    TEST_ASSERT_FALSE(fake_can_is_up());
}

TEST(protobuf_rejects_truncation_and_wrong_command) {
    const uint8_t truncated[] = {0x22, 10, 1};
    size_t n = j2534_core_request(3, truncated, sizeof(truncated), reply);
    pb_istream_t in = pb_istream_from_buffer(reply, n);
    TEST_ASSERT_TRUE(pb_decode(&in, opendiag_Response_fields, &res));
    TEST_ASSERT_EQUAL_INT(J2534_MSG, res.status);
    const uint8_t open[] = {0x12, 0};
    n = j2534_core_request(3, open, sizeof(open), reply);
    in = pb_istream_from_buffer(reply, n);
    TEST_ASSERT_TRUE(pb_decode(&in, opendiag_Response_fields, &res));
    TEST_ASSERT_EQUAL_INT(J2534_MSG, res.status);
}

TEST(maximum_isotp_payload_survives_protobuf_and_reassembly) {
    uint32_t ch = logical(connect_can(0), false);
    uint8_t first[] = {0x1f, 0xff, 0, 1, 2, 3, 4, 5};
    fake_can_stage_stale(0x7e8, 8, first);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_START, res.message.rx_status);
    unsigned offset = 6, seq = 1;
    while (offset < 4095) {
        uint8_t cf[8] = {0};
        cf[0] = 0x20 | (seq++ & 15);
        unsigned n = 4095 - offset < 7 ? 4095 - offset : 7;
        for (unsigned i = 0; i < n; i++)
            cf[i + 1] = (uint8_t)(offset + i);
        fake_can_stage_stale(0x7e8, 8, cf);
        j2534_core_poll();
        offset += n;
    }
    command(opendiag_Request_read_tag);
    req.command.read.id = ch;
    TEST_ASSERT_EQUAL_INT(0, wire_call());
    TEST_ASSERT_EQUAL_INT(4099, res.message.data.size);
    for (unsigned i = 0; i < 4095; i++)
        TEST_ASSERT_EQUAL_INT((uint8_t)i, res.message.data.bytes[i + 4]);
}

TEST(extended_address_is_preserved) {
    uint32_t ch = logical(connect_can(0), true);
    const uint8_t sf[] = {0xf1, 2, 0x41, 0x00, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, sf);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_ADDR, res.message.rx_status);
    TEST_ASSERT_EQUAL_MEM("\0\0\x07\xe8\xf1\x41\0", res.message.data.bytes, 7);
}

TEST(flow_control_timeout_reports_failure) {
    uint32_t ch = logical(connect_can(0), false);
    uint8_t data[14] = {0, 0, 7, 0xe0};
    queue(ch, J2534_ISOTP, J2534_PAD, data, sizeof(data));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    fake_clock_advance_ms(1001);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_FAILED, res.message.rx_status);
}

TEST(periodic_messages_stop_and_clear) {
    uint32_t ch = connect_can(0);
    command(opendiag_Request_start_periodic_tag);
    opendiag_Periodic *p = &req.command.start_periodic;
    p->channel = ch;
    p->interval_ms = 10;
    p->has_message = true;
    p->message.protocol = J2534_CAN;
    p->message.data.size = 5;
    memcpy(p->message.data.bytes, "\0\0\x07\xdf\x55", 5);
    TEST_ASSERT_EQUAL_INT(0, call());
    uint32_t id = res.id;
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    fake_clock_advance_ms(10);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    command(opendiag_Request_stop_periodic_tag);
    req.command.stop_periodic = (opendiag_Object){ch, id};
    TEST_ASSERT_EQUAL_INT(0, call());
    fake_clock_advance_ms(20);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(J2534_MSG_ID, call());
}

TEST(periodic_start_checks_message_interval_protocol_and_quota) {
    uint32_t ch = connect_can(0);
    command(opendiag_Request_start_periodic_tag);
    opendiag_Periodic *p = &req.command.start_periodic;
    p->channel = ch;
    p->interval_ms = 10;
    p->has_message = false;
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
    TEST_ASSERT_EQUAL_INT(J2534_INTERVAL, start_periodic_can(ch, 4, J2534_CAN));
    TEST_ASSERT_EQUAL_INT(J2534_INTERVAL,
                          start_periodic_can(ch, 65536, J2534_CAN));
    TEST_ASSERT_EQUAL_INT(J2534_MSG_PROTOCOL,
                          start_periodic_can(ch, 10, J2534_ISO9141));
    uint32_t ids[J2534_PERIODIC_MAX];
    for (unsigned i = 0; i < J2534_PERIODIC_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(0, start_periodic_can(ch, 10, J2534_CAN));
        ids[i] = res.id;
    }
    TEST_ASSERT_EQUAL_INT(J2534_LIMIT, start_periodic_can(ch, 10, J2534_CAN));
    command(opendiag_Request_stop_periodic_tag);
    req.command.stop_periodic = (opendiag_Object){ch, ids[0]};
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(0, start_periodic_can(ch, 10, J2534_CAN));
}

TEST(clear_tx_cancels_queued_message) {
    uint32_t ch = connect_can(0);
    queue(ch, J2534_CAN, 0, (uint8_t *)"\0\0\x07\xdf", 4);
    TEST_ASSERT_EQUAL_INT(0, call());
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = ch;
    req.command.ioctl.id = 7;
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
}

TEST(receive_overflow_is_reported_after_preserving_queued_messages) {
    uint32_t ch = connect_can(0);
    filter(ch, 1, 0, 0);
    const uint8_t data[8] = {0};
    for (unsigned i = 0; i < 220; i++) {
        fake_can_stage_stale(0x100, 8, data);
        j2534_core_poll();
    }
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    fake_can_stage_stale(0x100, 8, data);
    j2534_core_poll();
    bool saw_overflow = false;
    while (read_message(ch) == J2534_OK)
        saw_overflow |= !!(res.message.rx_status & J2534_RX_OVERFLOW);
    TEST_ASSERT_TRUE(saw_overflow);
}

TEST(full_receive_queue_reports_overflow_once_after_it_drains) {
    uint32_t ch = connect_can(0);
    filter(ch, 1, 0, 0);
    const uint8_t data[8] = {0};
    /* A 12-byte CAN record costs 40 bytes of the 8192-byte ring, so 204 fit.
     * The 205th and later are dropped, and the drop is reported once. */
    for (unsigned i = 0; i < 210; i++) {
        fake_can_stage_stale(0x100, 8, data);
        j2534_core_poll();
    }
    for (unsigned i = 0; i < 204; i++) {
        TEST_ASSERT_EQUAL_INT(0, read_message(ch));
        TEST_ASSERT_EQUAL_INT(0, res.message.rx_status & J2534_RX_OVERFLOW);
    }
    TEST_ASSERT_EQUAL_INT(J2534_OVERFLOW, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
}

TEST(receive_ring_wraps_without_corrupting_frames) {
    uint32_t ch = connect_can(0);
    filter(ch, 1, 0, 0);
    for (unsigned i = 0; i < 300; i++) {
        uint8_t data[8] = {(uint8_t)i, 2, 3, 4, 5, 6, 7, 8};
        fake_can_stage_stale(0x100, 8, data);
        j2534_core_poll();
        TEST_ASSERT_EQUAL_INT(0, read_message(ch));
        TEST_ASSERT_EQUAL_INT(12, res.message.data.size);
        /* Bytes 0..3 of the record are the identifier, then the 8 data bytes. */
        TEST_ASSERT_EQUAL_INT((uint8_t)i, res.message.data.bytes[4]);
        TEST_ASSERT_EQUAL_INT(8, res.message.data.bytes[11]);
    }
}

TEST(can_remote_oversized_and_extended_frames_are_not_delivered_to_api) {
    uint32_t ch = connect_can(0);
    filter(ch, 1, 0, 0);
    static const uint8_t d[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    static const uint8_t want[12] = {0, 0, 7, 0xe8, 1, 2, 3, 4, 5, 6, 7, 8};
    fake_can_stage_stale(0x7e8 | CAN_RTR_FLAG, 0, d);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    fake_can_stage_stale(0x7e8, 9, d);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    fake_can_stage_stale(CAN_EFF_FLAG | 0x18daf110u, 8, d);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    fake_can_stage_stale(0x7e8, 8, d);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(12, res.message.data.size);
    TEST_ASSERT_EQUAL_MEM(want, res.message.data.bytes, 12);
}

TEST(flow_control_waits_for_bus_admission_before_the_next_segment) {
    uint32_t ch = logical(connect_can(0), false);
    static const uint8_t first[] = {0x10, 10, 1, 2, 3, 4, 5, 6};
    fake_bus_tx_hold(true);
    fake_can_stage_stale(0x7e8, 8, first);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
    fake_bus_tx_hold(false);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x7e0, fake_can_sent(0)->id);
    TEST_ASSERT_EQUAL_INT(0x30, fake_can_sent(0)->data[0]);
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_START, res.message.rx_status);
}

TEST(logical_repeats_are_sent_in_due_order_not_slot_order) {
    uint32_t physical = connect_can(0);
    uint32_t older_slot = logical_with_ids(physical, 0xe8, 0xe0);
    uint32_t newer_slot = logical_with_ids(physical, 0xe9, 0xe1);
    j2534_repeat_setup_t first = repeat_setup(J2534_ISOTP, 0);
    j2534_repeat_setup_t second = repeat_setup(J2534_ISOTP, 0);
    memcpy(first.message.data, "\0\0\x07\xe0\x01", 5);
    memcpy(second.message.data, "\0\0\x07\xe1\x01", 5);
    /* The job in the higher slot is started first, so it is due first even
     * though the lower slot is scanned first. */
    start_repeat(newer_slot, &second);
    start_repeat(older_slot, &first);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x7e1, fake_can_sent(0)->id);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x7e0, fake_can_sent(1)->id);
}

TEST(queue_without_a_message_is_rejected_before_any_transmit) {
    uint32_t ch = connect_can(0);
    command(opendiag_Request_queue_tag);
    req.command.queue.channel = ch;
    req.command.queue.has_message = false;
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
}

TEST(empty_can_queue_message_is_rejected_without_transmitting) {
    uint32_t ch = connect_can(0);
    const uint8_t none[1] = {0};
    queue(ch, J2534_CAN, 0, none, 0);
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
}

TEST(filter_table_limits_validate_types_shapes_and_stop_by_id) {
    uint32_t p = connect_can(0);
    uint32_t lg = logical(p, false);
    TEST_ASSERT_EQUAL_INT(J2534_FILTER_TYPE, start_filter_raw(lg, 1, 0, 1, 1));
    TEST_ASSERT_EQUAL_INT(J2534_FILTER_TYPE, start_filter_raw(p, 3, 0, 1, 1));
    TEST_ASSERT_EQUAL_INT(J2534_MSG, start_filter_raw(p, 1, 0, 0, 0));
    TEST_ASSERT_EQUAL_INT(J2534_MSG, start_filter_raw(p, 1, 0, 1, 2));
    TEST_ASSERT_EQUAL_INT(J2534_MSG, start_filter_raw(p, 1, J2534_ADDR, 1, 1));
    TEST_ASSERT_EQUAL_INT(J2534_MSG, start_filter_raw(p, 1, J2534_29BIT, 1, 1));
    uint32_t ids[J2534_FILTER_MAX];
    for (unsigned i = 0; i < J2534_FILTER_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(0, start_filter_raw(p, 1, 0, 1, 1));
        ids[i] = res.id;
    }
    TEST_ASSERT_EQUAL_INT(J2534_LIMIT, start_filter_raw(p, 1, 0, 1, 1));
    command(opendiag_Request_stop_filter_tag);
    req.command.stop_filter = (opendiag_Object){p, ids[0]};
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(J2534_FILTER_ID, call());
    TEST_ASSERT_EQUAL_INT(0, start_filter_raw(p, 1, 0, 1, 1));
}

TEST(pass_filter_mismatch_is_skipped_before_a_later_match) {
    uint32_t ch = connect_can(0);
    const uint8_t data[] = {1, 2, 3};
    filter(ch, 1, 0xff, 0x01);
    fake_can_stage_stale(0x7e8, 3, data);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    filter(ch, 1, 0, 0);
    fake_can_stage_stale(0x7e8, 3, data);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(7, res.message.data.size);
    const uint8_t expect[] = {0, 0, 7, 0xe8, 1, 2, 3};
    TEST_ASSERT_EQUAL_MEM(expect, res.message.data.bytes, sizeof(expect));
}

TEST(filter_longer_than_the_frame_never_passes) {
    uint32_t ch = connect_can(0);
    filter_bytes(ch, 1, 0, 6);
    const uint8_t one[] = {0x41};
    fake_can_stage_stale(0x7e8, 1, one);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    const uint8_t two[] = {0x41, 0x42};
    fake_can_stage_stale(0x7e8, 2, two);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(6, res.message.data.size);
}

TEST(physical_filter_respects_the_29bit_flag_on_can_both_channels) {
    uint32_t ch = connect_can(J2534_CAN_BOTH);
    command(opendiag_Request_start_filter_tag);
    req.command.start_filter =
        (opendiag_Filter){.channel = ch, .type = 1, .flags = J2534_29BIT};
    req.command.start_filter.mask.size = req.command.start_filter.pattern.size =
        1;
    TEST_ASSERT_EQUAL_INT(0, call());
    const uint8_t data[] = {1, 2, 3};
    fake_can_stage_stale(0x7e8, 3, data);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    fake_can_stage_stale(CAN_EFF_FLAG | 0x18daf110u, 3, data);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_TRUE(res.message.rx_status & J2534_29BIT);
    TEST_ASSERT_EQUAL_INT(7, res.message.data.size);
    TEST_ASSERT_EQUAL_MEM("\x18\xda\xf1\x10\x01\x02\x03", res.message.data.bytes,
                          7);
}

TEST(logical_config_reports_defaults_and_rejects_unknown_parameters) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    TEST_ASSERT_EQUAL_INT(0, config_get(ch, TST_CFG_BLOCK_SIZE));
    TEST_ASSERT_EQUAL_INT(0, config_get(ch, TST_CFG_STMIN));
    TEST_ASSERT_EQUAL_INT(255, config_get(ch, TST_CFG_WAIT_LIMIT));
    TEST_ASSERT_EQUAL_INT(0, config_get(ch, TST_CFG_PAD_VALUE));
    TEST_ASSERT_EQUAL_INT(1000, config_get(ch, TST_CFG_N_AS));
    TEST_ASSERT_EQUAL_INT(1000, config_get(ch, TST_CFG_N_AR));
    TEST_ASSERT_EQUAL_INT(1000, config_get(ch, TST_CFG_N_BS));
    TEST_ASSERT_EQUAL_INT(1000, config_get(ch, TST_CFG_N_CR));
    TEST_ASSERT_EQUAL_INT(J2534_PARAM, config_call(ch, false, 0x30, 0));
    TEST_ASSERT_EQUAL_INT(J2534_PARAM, config_call(ch, true, 0x30, 1));
    TEST_ASSERT_EQUAL_INT(J2534_PARAM,
                          config_call(ch, false, BUS_P_DATA_RATE, 0));
    TEST_ASSERT_EQUAL_INT(J2534_PARAM,
                          config_call(ch, true, BUS_P_DATA_RATE, 500000));
}

TEST(logical_config_set_round_trips_each_parameter_and_enforces_ranges) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);

    config_set(ch, TST_CFG_BLOCK_SIZE, 3);
    TEST_ASSERT_EQUAL_INT(3, config_get(ch, TST_CFG_BLOCK_SIZE));
    config_set(ch, TST_CFG_BLOCK_SIZE, 255);
    TEST_ASSERT_EQUAL_INT(255, config_get(ch, TST_CFG_BLOCK_SIZE));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          config_call(ch, true, TST_CFG_BLOCK_SIZE, 256));
    TEST_ASSERT_EQUAL_INT(255, config_get(ch, TST_CFG_BLOCK_SIZE));

    config_set(ch, TST_CFG_STMIN, 0x7f);
    TEST_ASSERT_EQUAL_INT(0x7f, config_get(ch, TST_CFG_STMIN));
    config_set(ch, TST_CFG_STMIN, 0xf1);
    TEST_ASSERT_EQUAL_INT(0xf1, config_get(ch, TST_CFG_STMIN));
    config_set(ch, TST_CFG_STMIN, 0xf9);
    TEST_ASSERT_EQUAL_INT(0xf9, config_get(ch, TST_CFG_STMIN));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          config_call(ch, true, TST_CFG_STMIN, 0x80));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          config_call(ch, true, TST_CFG_STMIN, 0xf0));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          config_call(ch, true, TST_CFG_STMIN, 0xfa));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          config_call(ch, true, TST_CFG_STMIN, 0x100));
    TEST_ASSERT_EQUAL_INT(0xf9, config_get(ch, TST_CFG_STMIN));

    config_set(ch, TST_CFG_WAIT_LIMIT, 7);
    TEST_ASSERT_EQUAL_INT(7, config_get(ch, TST_CFG_WAIT_LIMIT));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          config_call(ch, true, TST_CFG_WAIT_LIMIT, 256));
    TEST_ASSERT_EQUAL_INT(7, config_get(ch, TST_CFG_WAIT_LIMIT));

    config_set(ch, TST_CFG_PAD_VALUE, 0xaa);
    TEST_ASSERT_EQUAL_INT(0xaa, config_get(ch, TST_CFG_PAD_VALUE));
    config_set(ch, TST_CFG_PAD_VALUE, 255);
    TEST_ASSERT_EQUAL_INT(255, config_get(ch, TST_CFG_PAD_VALUE));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          config_call(ch, true, TST_CFG_PAD_VALUE, 256));
    TEST_ASSERT_EQUAL_INT(255, config_get(ch, TST_CFG_PAD_VALUE));

    config_set(ch, TST_CFG_N_AS, 25);
    TEST_ASSERT_EQUAL_INT(25, config_get(ch, TST_CFG_N_AS));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE, config_call(ch, true, TST_CFG_N_AS, 0));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          config_call(ch, true, TST_CFG_N_AS, 60001));
    config_set(ch, TST_CFG_N_AS, 60000);
    TEST_ASSERT_EQUAL_INT(60000, config_get(ch, TST_CFG_N_AS));

    config_set(ch, TST_CFG_N_AR, 30);
    TEST_ASSERT_EQUAL_INT(30, config_get(ch, TST_CFG_N_AR));
    config_set(ch, TST_CFG_N_BS, 40);
    TEST_ASSERT_EQUAL_INT(40, config_get(ch, TST_CFG_N_BS));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE, config_call(ch, true, TST_CFG_N_BS, 0));
    TEST_ASSERT_EQUAL_INT(40, config_get(ch, TST_CFG_N_BS));
    config_set(ch, TST_CFG_N_CR, 50);
    TEST_ASSERT_EQUAL_INT(50, config_get(ch, TST_CFG_N_CR));
    TEST_ASSERT_EQUAL_INT(30, config_get(ch, TST_CFG_N_AR));
}

TEST(logical_config_is_refused_while_a_request_is_queued_or_in_flight) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    const uint8_t request[] = {0, 0, 7, 0xe0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    queue(ch, J2534_ISOTP, J2534_PAD, request, sizeof(request));
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(J2534_CONFLICT,
                          config_call(ch, true, TST_CFG_BLOCK_SIZE, 4));
    TEST_ASSERT_EQUAL_INT(0, config_get(ch, TST_CFG_BLOCK_SIZE));
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(J2534_CONFLICT,
                          config_call(ch, true, TST_CFG_BLOCK_SIZE, 4));
    const uint8_t fc[] = {0x30, 0, 0};
    fake_can_stage_stale(0x7e8, 3, fc);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_SUCCESS, res.message.rx_status);
    config_set(ch, TST_CFG_BLOCK_SIZE, 4);
    TEST_ASSERT_EQUAL_INT(4, config_get(ch, TST_CFG_BLOCK_SIZE));
}

TEST(logical_config_is_refused_while_a_segmented_reception_is_open) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    const uint8_t first[] = {0x10, 10, 1, 2, 3, 4, 5, 6};
    const uint8_t last[] = {0x21, 7, 8, 9, 10, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, first);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_START, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(J2534_CONFLICT,
                          config_call(ch, true, TST_CFG_STMIN, 5));
    TEST_ASSERT_EQUAL_INT(0, config_get(ch, TST_CFG_STMIN));
    fake_can_stage_stale(0x7e8, 8, last);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(14, res.message.data.size);
    config_set(ch, TST_CFG_STMIN, 5);
    TEST_ASSERT_EQUAL_INT(5, config_get(ch, TST_CFG_STMIN));
}

TEST(flow_control_frame_carries_the_configured_block_size_and_stmin) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    config_set(ch, TST_CFG_BLOCK_SIZE, 2);
    config_set(ch, TST_CFG_STMIN, 5);
    const uint8_t first[] = {0x10, 10, 1, 2, 3, 4, 5, 6};
    fake_can_stage_stale(0x7e8, 8, first);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x30, fake_can_sent(0)->data[0]);
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent(0)->data[1]);
    TEST_ASSERT_EQUAL_INT(5, fake_can_sent(0)->data[2]);
}

TEST(single_frame_padding_uses_the_configured_pad_value) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    config_set(ch, TST_CFG_PAD_VALUE, 0xaa);
    const uint8_t sf[] = {0, 0, 7, 0xe0, 1, 2, 3};
    queue(ch, J2534_ISOTP, J2534_PAD, sf, sizeof(sf));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    const struct can_frame *frame = fake_can_sent(0);
    TEST_ASSERT_EQUAL_INT(0x03, frame->data[0]);
    TEST_ASSERT_EQUAL_MEM("\x01\x02\x03", frame->data + 1, 3);
    for (unsigned i = 4; i < 8; i++)
        TEST_ASSERT_EQUAL_INT(0xaa, frame->data[i]);
}

TEST(config_ioctl_validates_stray_arguments_and_reports_every_entry) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    const uint8_t one[] = {1};
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          ioctl_with_data(ch, TST_IOCTL_GET_CONFIG, one, 1));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          ioctl_with_data(ch, TST_IOCTL_SET_CONFIG, one, 1));
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = ch;
    req.command.ioctl.id = TST_IOCTL_GET_CONFIG;
    req.command.ioctl.connector = 1;
    TEST_ASSERT_EQUAL_INT(J2534_VALUE, call());
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = ch;
    req.command.ioctl.id = TST_IOCTL_SET_CONFIG;
    req.command.ioctl.pin = 1;
    TEST_ASSERT_EQUAL_INT(J2534_VALUE, call());

    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = ch;
    req.command.ioctl.id = TST_IOCTL_GET_CONFIG;
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(0, res.config_count);

    config_set(ch, TST_CFG_STMIN, 6);
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = ch;
    req.command.ioctl.id = TST_IOCTL_GET_CONFIG;
    req.command.ioctl.config_count = 2;
    req.command.ioctl.config[0] =
        (opendiag_Config){.parameter = TST_CFG_BLOCK_SIZE};
    req.command.ioctl.config[1] = (opendiag_Config){.parameter = TST_CFG_STMIN};
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(2, res.config_count);
    TEST_ASSERT_EQUAL_INT(TST_CFG_BLOCK_SIZE, res.config[0].parameter);
    TEST_ASSERT_EQUAL_INT(0, res.config[0].value);
    TEST_ASSERT_EQUAL_INT(TST_CFG_STMIN, res.config[1].parameter);
    TEST_ASSERT_EQUAL_INT(6, res.config[1].value);

    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = ch;
    req.command.ioctl.id = TST_IOCTL_SET_CONFIG;
    req.command.ioctl.config_count = 2;
    req.command.ioctl.config[0] =
        (opendiag_Config){.parameter = TST_CFG_BLOCK_SIZE, .value = 4};
    req.command.ioctl.config[1] =
        (opendiag_Config){.parameter = TST_CFG_STMIN, .value = 0x80};
    TEST_ASSERT_EQUAL_INT(J2534_VALUE, call());
    TEST_ASSERT_EQUAL_INT(6, config_get(ch, TST_CFG_STMIN));
}

TEST(non_config_ioctl_rejects_stray_config_connector_pin_and_data) {
    uint32_t ch = connect_can(0);
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = ch;
    req.command.ioctl.id = BUS_IOCTL_CLEAR_TX_QUEUE;
    req.command.ioctl.config_count = 1;
    req.command.ioctl.config[0] =
        (opendiag_Config){.parameter = TST_CFG_BLOCK_SIZE, .value = 1};
    TEST_ASSERT_EQUAL_INT(J2534_VALUE, call());
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = ch;
    req.command.ioctl.id = BUS_IOCTL_CLEAR_TX_QUEUE;
    req.command.ioctl.connector = 1;
    TEST_ASSERT_EQUAL_INT(J2534_VALUE, call());
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = ch;
    req.command.ioctl.id = BUS_IOCTL_CLEAR_TX_QUEUE;
    req.command.ioctl.pin = 1;
    TEST_ASSERT_EQUAL_INT(J2534_VALUE, call());
    const uint8_t one[] = {1};
    TEST_ASSERT_EQUAL_INT(J2534_MSG,
                          ioctl_with_data(ch, BUS_IOCTL_CLEAR_TX_QUEUE, one, 1));
    TEST_ASSERT_EQUAL_INT(J2534_OK, ioctl_call(ch, BUS_IOCTL_CLEAR_TX_QUEUE));
}

TEST(physical_config_exposes_only_the_bus_parameters) {
    uint32_t ch = connect_can(0);
    TEST_ASSERT_EQUAL_INT(J2534_PARAM,
                          config_call(ch, false, TST_CFG_BLOCK_SIZE, 0));
    TEST_ASSERT_EQUAL_INT(J2534_PARAM,
                          config_call(ch, true, TST_CFG_BLOCK_SIZE, 4));
    TEST_ASSERT_EQUAL_INT(J2534_PARAM,
                          config_call(ch, false, BUS_P_CHECKSUM_RX, 0));
    TEST_ASSERT_EQUAL_INT(J2534_PARAM, config_call(ch, false, BUS_P_P1_MAX, 0));
    TEST_ASSERT_EQUAL_INT(500000, config_get(ch, BUS_P_DATA_RATE));
    TEST_ASSERT_EQUAL_INT(J2534_OK,
                          config_call(ch, true, BUS_P_DATA_RATE, 500000));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          config_call(ch, true, BUS_P_DATA_RATE, 250000));
    TEST_ASSERT_EQUAL_INT(500000, config_get(ch, BUS_P_DATA_RATE));

    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          config_call(ch, true, BUS_P_LOOPBACK, 2));
    TEST_ASSERT_EQUAL_INT(0, config_get(ch, BUS_P_LOOPBACK));
    config_set(ch, BUS_P_LOOPBACK, 1);
    TEST_ASSERT_EQUAL_INT(1, config_get(ch, BUS_P_LOOPBACK));
}

TEST(can_only_ioctls_reject_other_bus_work_and_bus_on_reports_init) {
    uint32_t can = connect_can(0);
    TEST_ASSERT_EQUAL_INT(J2534_INIT, ioctl_call(can, BUS_IOCTL_BUS_ON));
    TEST_ASSERT_EQUAL_INT(J2534_IOCTL, ioctl_call(can, 0x7e));
    TEST_ASSERT_EQUAL_INT(J2534_IOCTL,
                          ioctl_call(can, BUS_IOCTL_CLEAR_FUNCT_TABLE));
    TEST_ASSERT_EQUAL_INT(J2534_IOCTL,
                          ioctl_call(can, BUS_IOCTL_FAST_INIT));
    TEST_ASSERT_EQUAL_INT(J2534_IOCTL,
                          ioctl_call(can, BUS_IOCTL_FIVE_BAUD_INIT));
    TEST_ASSERT_EQUAL_INT(J2534_IOCTL,
                          ioctl_call(can, BUS_IOCTL_ADD_FUNCT_ADDR));
    TEST_ASSERT_EQUAL_INT(J2534_IOCTL,
                          ioctl_call(can, BUS_IOCTL_DEL_FUNCT_ADDR));
    command(opendiag_Request_fast_init_tag);
    req.command.fast_init.channel = can;
    TEST_ASSERT_EQUAL_INT(J2534_IOCTL, call());
}

TEST(clear_filters_restores_the_default_block_on_a_physical_channel) {
    uint32_t ch = connect_can(0);
    uint32_t lc = logical_at(ch, 0xe8, 0xe0, false);
    TEST_ASSERT_EQUAL_INT(J2534_IOCTL, ioctl_call(lc, TST_IOCTL_CLEAR_FILTERS));
    const uint8_t data[] = {1, 2, 3};
    filter(ch, 1, 0, 0);
    fake_can_stage_stale(0x7e8, 3, data);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_OK, ioctl_call(ch, TST_IOCTL_CLEAR_FILTERS));
    fake_can_stage_stale(0x7e8, 3, data);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
}

TEST(clear_periodic_removes_every_job_and_cancels_a_held_scheduled_frame) {
    uint32_t ch = connect_can(0);
    command(opendiag_Request_start_periodic_tag);
    opendiag_Periodic *p = &req.command.start_periodic;
    p->channel = ch;
    p->interval_ms = 10;
    p->has_message = true;
    p->message.protocol = J2534_CAN;
    p->message.data.size = 5;
    memcpy(p->message.data.bytes, "\0\0\x07\xdf\x55", 5);
    TEST_ASSERT_EQUAL_INT(0, call());
    uint32_t id = res.id;
    fake_bus_tx_hold(true);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_OK, ioctl_call(ch, BUS_IOCTL_CLEAR_PERIODIC));
    fake_bus_tx_hold(false);
    j2534_core_poll();
    fake_clock_advance_ms(10);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
    command(opendiag_Request_stop_periodic_tag);
    req.command.stop_periodic = (opendiag_Object){ch, id};
    TEST_ASSERT_EQUAL_INT(J2534_MSG_ID, call());
}

TEST(clear_rx_on_a_physical_channel_drains_every_pending_frame) {
    uint32_t ch = connect_can(0);
    filter(ch, 1, 0, 0);
    const uint8_t data[8] = {0};
    for (unsigned i = 0; i < 40; i++)
        fake_can_stage_stale(0x100, 8, data);
    TEST_ASSERT_EQUAL_INT(J2534_OK, ioctl_call(ch, BUS_IOCTL_CLEAR_RX_QUEUE));
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
}

TEST(clear_rx_on_a_logical_channel_abandons_its_open_reception) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    const uint8_t first[] = {0x10, 10, 1, 2, 3, 4, 5, 6};
    const uint8_t last[] = {0x21, 7, 8, 9, 10, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, first);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(J2534_OK, ioctl_call(ch, BUS_IOCTL_CLEAR_RX_QUEUE));
    fake_can_stage_stale(0x7e8, 8, last);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    const uint8_t sf[] = {3, 1, 2, 3, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, sf);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(7, res.message.data.size);
    TEST_ASSERT_EQUAL_MEM("\0\0\x07\xe8\x01\x02\x03", res.message.data.bytes, 7);
}

TEST(clear_tx_on_a_logical_channel_resets_its_segmented_transmit) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    const uint8_t request[] = {0, 0, 7, 0xe0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    queue(ch, J2534_ISOTP, J2534_PAD, request, sizeof(request));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(J2534_OK, ioctl_call(ch, BUS_IOCTL_CLEAR_TX_QUEUE));
    const uint8_t fc[] = {0x30, 0, 0};
    fake_can_stage_stale(0x7e8, 3, fc);
    j2534_core_poll();
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    const uint8_t sf[] = {0, 0, 7, 0xe0, 1, 2};
    queue(ch, J2534_ISOTP, J2534_PAD, sf, sizeof(sf));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x02, fake_can_sent(1)->data[0]);
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent(1)->data[1]);
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent(1)->data[2]);
}

TEST(clear_tx_keeps_a_late_completion_from_clearing_new_ordinary_data) {
    uint32_t ch = connect_can(0);
    const uint8_t first[] = {0, 0, 7, 0xdf, 0};
    queue(ch, J2534_CAN, 0, first, sizeof(first));
    TEST_ASSERT_EQUAL_INT(0, call());
    fake_bus_tx_hold_completion(true);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_OK, ioctl_call(ch, BUS_IOCTL_CLEAR_TX_QUEUE));
    const uint8_t second[] = {0, 0, 7, 0xdf, 1};
    queue(ch, J2534_CAN, 0, second, sizeof(second));
    TEST_ASSERT_EQUAL_INT(0, call());
    fake_bus_tx_hold_completion(false);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x7df, fake_can_sent(1)->id);
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent(1)->data[0]);
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_SUCCESS, res.message.rx_status);
}

TEST(logical_table_limit_is_reported_and_released_by_disconnect) {
    uint32_t physical = connect_can(0);
    uint32_t first = logical_at(physical, 0xe8, 0xe0, false);
    logical_at(physical, 0xe9, 0xe1, false);
    TEST_ASSERT_EQUAL_INT(J2534_LIMIT,
                          logical_status(physical, 0xea, 0xe2, false));
    command(opendiag_Request_logical_disconnect_tag);
    req.command.logical_disconnect.id = first;
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_TRUE(logical_at(physical, 0xea, 0xe2, false) != 0);
}

TEST(logical_disconnect_accepts_only_logical_handles) {
    uint32_t p = connect_can(0);
    uint32_t ch = logical(p, false);
    command(opendiag_Request_logical_disconnect_tag);
    req.command.logical_disconnect.id = p;
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, call());
    TEST_ASSERT_TRUE(fake_can_is_up());
    req.command.logical_disconnect.id = ch;
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, read_message(ch));
    TEST_ASSERT_TRUE(fake_can_is_up());
}

TEST(logical_connect_rejects_protocol_flags_and_descriptor) {
    uint32_t can = connect_can(0);
    TEST_ASSERT_EQUAL_INT(J2534_PROTOCOL,
                          logical_request(can, J2534_CAN, 0, 0xe8, 4));
    TEST_ASSERT_EQUAL_INT(J2534_FLAGS,
                          logical_request(can, J2534_ISOTP, 2, 0xe8, 4));
    TEST_ASSERT_EQUAL_INT(J2534_DESCRIPTOR,
                          logical_request(can, J2534_ISOTP, 0, 0xe8, 3));
    TEST_ASSERT_EQUAL_INT(0, logical_request(can, J2534_ISOTP, 0, 0xe8, 4));
    TEST_ASSERT_EQUAL_INT(J2534_NOT_UNIQUE,
                          logical_request(can, J2534_ISOTP, 0, 0xe8, 4));
    TEST_ASSERT_EQUAL_INT(0, logical_request(can, J2534_ISOTP, 0, 0xe9, 4));
    TEST_ASSERT_EQUAL_INT(J2534_LIMIT,
                          logical_request(can, J2534_ISOTP, 0, 0xea, 4));
}

TEST(logical_connect_29bit_addressing_round_trips_on_a_29bit_physical) {
    uint32_t physical = connect_can(J2534_29BIT);
    command(opendiag_Request_logical_connect_tag);
    opendiag_LogicalConnect *c = &req.command.logical_connect;
    c->physical = physical;
    c->protocol = J2534_ISOTP;
    c->local_flags = J2534_29BIT;
    c->remote_flags = J2534_29BIT | J2534_PAD;
    c->local_address.size = c->remote_address.size = 4;
    memcpy(c->local_address.bytes, "\x18\xda\xf1\x10", 4);
    memcpy(c->remote_address.bytes, "\x18\xda\x10\xf1", 4);
    TEST_ASSERT_EQUAL_INT(0, call());
    uint32_t ch = res.id;

    const uint8_t request[] = {0x18, 0xda, 0x10, 0xf1, 1, 2, 3};
    queue(ch, J2534_ISOTP, J2534_29BIT | J2534_PAD, request, sizeof(request));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(CAN_EFF_FLAG | 0x18da10f1u, fake_can_sent(0)->id);
    TEST_ASSERT_EQUAL_INT(0x03, fake_can_sent(0)->data[0]);
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent(0)->data[1]);
    /* The transmit indication is queued ahead of the reply. */
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_SUCCESS, res.message.rx_status);

    const uint8_t answer[] = {0x02, 0x41, 0x0c, 0, 0, 0, 0, 0};
    fake_can_stage_stale(CAN_EFF_FLAG | 0x18daf110u, 8, answer);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_TRUE(res.message.rx_status & J2534_29BIT);
    TEST_ASSERT_EQUAL_INT(6, res.message.data.size);
    TEST_ASSERT_EQUAL_MEM("\x18\xda\xf1\x10\x41\x0c", res.message.data.bytes,
                          6);
}

TEST(logical_addressing_rejects_mismatched_flags_and_short_payloads) {
    uint32_t phys = connect_can(0);
    uint32_t ch = logical(phys, false);
    queue(ch, J2534_ISOTP, J2534_PAD, (const uint8_t *)"\0\0\x07\xe0", 4);
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
    queue(ch, J2534_ISOTP, J2534_PAD | J2534_29BIT,
          (const uint8_t *)"\0\0\x07\xe0\x01", 5);
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
    queue(ch, J2534_ISOTP, J2534_ADDR | J2534_PAD,
          (const uint8_t *)"\0\0\x07\xe0\x01", 5);
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
    queue(ch, J2534_ISOTP, J2534_PAD, (const uint8_t *)"\0\0\x07\xe0\x01\x02",
          6);
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x7e0, fake_can_sent(0)->id);
}

TEST(logical_payload_above_the_isotp_maximum_is_refused) {
    uint32_t ch = logical(connect_can(0), false);
    static uint8_t big[4100];
    memset(big, 0, sizeof(big));
    big[0] = 0;
    big[1] = 0;
    big[2] = 7;
    big[3] = 0xe0;
    queue(ch, J2534_ISOTP, J2534_PAD, big, sizeof(big));
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
}

TEST(logical_queue_checks_target_and_addressing_before_queueing) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    const uint8_t wrong[12] = {0, 0, 7, 0xe1, 1, 2, 3, 4, 5, 6, 7, 8};
    queue(ch, J2534_ISOTP, J2534_PAD, wrong, sizeof(wrong));
    TEST_ASSERT_EQUAL_INT(J2534_MSG_NOT_ALLOWED, call());
    const uint8_t extended[] = {0, 0, 7, 0xe0, 1, 2};
    queue(ch, J2534_ISOTP, J2534_ADDR, extended, sizeof(extended));
    TEST_ASSERT_EQUAL_INT(J2534_MSG_NOT_ALLOWED, call());
    const uint8_t right[12] = {0, 0, 7, 0xe0, 1, 2, 3, 4, 5, 6, 7, 8};
    queue(ch, J2534_ISOTP, J2534_PAD, right, sizeof(right));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x10, fake_can_sent(0)->data[0]);
}

TEST(logical_periodic_payload_is_limited_to_a_single_frame) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    command(opendiag_Request_start_periodic_tag);
    opendiag_Periodic *p = &req.command.start_periodic;
    const uint8_t twelve[12] = {0, 0, 7, 0xe0, 1, 2, 3, 4, 5, 6, 7, 8};
    p->channel = ch;
    p->interval_ms = 10;
    p->has_message = true;
    p->message.protocol = J2534_ISOTP;
    p->message.handle = 9;
    p->message.data.size = sizeof(twelve);
    memcpy(p->message.data.bytes, twelve, sizeof(twelve));
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
    p->message.data.size = 11;
    TEST_ASSERT_EQUAL_INT(0, call());
    uint32_t id = res.id;
    command(opendiag_Request_stop_periodic_tag);
    req.command.stop_periodic = (opendiag_Object){ch, id};
    TEST_ASSERT_EQUAL_INT(0, call());
}

TEST(logical_periodic_sends_single_frames_on_schedule) {
    uint32_t ch = logical(connect_can(0), false);
    command(opendiag_Request_start_periodic_tag);
    opendiag_Periodic *p = &req.command.start_periodic;
    p->channel = ch;
    p->interval_ms = 10;
    p->has_message = true;
    p->message.protocol = J2534_ISOTP;
    p->message.tx_flags = J2534_PAD;
    p->message.data.size = 6;
    memcpy(p->message.data.bytes, "\0\0\x07\xe0\x01\x02", 6);
    TEST_ASSERT_EQUAL_INT(0, call());
    uint32_t id = res.id;
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x7e0, fake_can_sent(0)->id);
    TEST_ASSERT_EQUAL_INT(0x02, fake_can_sent(0)->data[0]);
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent(0)->data[1]);
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent(0)->data[2]);
    fake_clock_advance_ms(10);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    command(opendiag_Request_stop_periodic_tag);
    req.command.stop_periodic = (opendiag_Object){ch, id};
    TEST_ASSERT_EQUAL_INT(0, call());
    fake_clock_advance_ms(20);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
}

TEST(periodic_stopped_while_awaiting_admission_never_transmits) {
    uint32_t ch = connect_can(0);
    uint32_t id = start_periodic_can(ch, 10, J2534_CAN);
    id = res.id;
    fake_bus_tx_hold(true);
    j2534_core_poll();
    command(opendiag_Request_stop_periodic_tag);
    req.command.stop_periodic = (opendiag_Object){ch, id};
    TEST_ASSERT_EQUAL_INT(0, call());
    fake_bus_tx_hold(false);
    j2534_core_poll();
    fake_clock_advance_ms(10);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
}

TEST(periodic_indications_carry_the_handle_for_success_and_failure) {
    uint32_t ch = connect_can(0);
    command(opendiag_Request_start_periodic_tag);
    opendiag_Periodic *p = &req.command.start_periodic;
    p->channel = ch;
    p->interval_ms = 10;
    p->has_message = true;
    p->message.protocol = J2534_CAN;
    p->message.handle = 77;
    p->message.data.size = 5;
    memcpy(p->message.data.bytes, "\0\0\x07\xdf\x55", 5);
    TEST_ASSERT_EQUAL_INT(0, call());
    uint32_t id = res.id;
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_SUCCESS, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(77, res.message.handle);
    TEST_ASSERT_EQUAL_INT(0, res.message.data.size);
    fake_can_fail_next_send();
    fake_clock_advance_ms(10);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_FAILED, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(77, res.message.handle);
    command(opendiag_Request_stop_periodic_tag);
    req.command.stop_periodic = (opendiag_Object){ch, id};
    TEST_ASSERT_EQUAL_INT(0, call());
}

TEST(loopback_reports_a_successful_can_transmit_as_tx_message) {
    uint32_t ch = connect_can(0);
    filter(ch, 1, 0, 0);
    const uint8_t frame_bytes[] = {0, 0, 7, 0xdf, 1, 2};
    queue(ch, J2534_CAN, 0, frame_bytes, sizeof(frame_bytes));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_SUCCESS, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));

    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = ch;
    req.command.ioctl.id = 2;
    req.command.ioctl.config_count = 1;
    req.command.ioctl.config[0] =
        (opendiag_Config){.parameter = BUS_P_LOOPBACK, .value = 1};
    TEST_ASSERT_EQUAL_INT(0, call());
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl.target = ch;
    req.command.ioctl.id = 1;
    req.command.ioctl.config_count = 1;
    req.command.ioctl.config[0] =
        (opendiag_Config){.parameter = BUS_P_LOOPBACK, .value = 0};
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(1, res.config_count);
    TEST_ASSERT_EQUAL_INT(1, res.config[0].value);

    queue(ch, J2534_CAN, 0, frame_bytes, sizeof(frame_bytes));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_MSG, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(0, res.message.handle);
    TEST_ASSERT_EQUAL_INT(sizeof(frame_bytes), res.message.data.size);
    TEST_ASSERT_EQUAL_MEM(frame_bytes, res.message.data.bytes,
                          sizeof(frame_bytes));
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_SUCCESS, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(42, res.message.handle);
}

TEST(half_duplex_logical_transmit_waits_for_an_inbound_segment) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    const uint8_t first[] = {0x10, 10, 1, 2, 3, 4, 5, 6};
    const uint8_t last[] = {0x21, 7, 8, 9, 10, 0, 0, 0};
    const uint8_t msg[] = {0, 0, 7, 0xe0, 0x3e};
    fake_can_stage_stale(0x7e8, 8, first);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_START, res.message.rx_status);
    queue(ch, J2534_ISOTP, J2534_PAD, msg, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    fake_can_stage_stale(0x7e8, 8, last);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(14, res.message.data.size);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x01, fake_can_sent(1)->data[0]);
    TEST_ASSERT_EQUAL_INT(0x3e, fake_can_sent(1)->data[1]);
}

TEST(half_duplex_logical_ignores_inbound_frames_while_transmitting) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    const uint8_t request[] = {0, 0, 7, 0xe0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    queue(ch, J2534_ISOTP, J2534_PAD, request, sizeof(request));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    const uint8_t single[] = {2, 0x41, 0x0c, 0, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, single);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    const uint8_t fc[] = {0x30, 0, 0, 0, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, fc);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x21, fake_can_sent(1)->data[0]);
}

TEST(full_duplex_logical_accepts_inbound_while_transmitting) {
    /* Wire value of FULL_DUPLEX in j2534.h (j2534.c's LOGICAL_FULL_DUPLEX). */
    uint32_t ch = logical_flags(connect_can(0), 0x00000001u);
    const uint8_t request[] = {0, 0, 7, 0xe0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    queue(ch, J2534_ISOTP, J2534_PAD, request, sizeof(request));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    const uint8_t single[] = {2, 0x41, 0x0c, 0, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, single);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(6, res.message.data.size);
    TEST_ASSERT_EQUAL_MEM("\0\0\x07\xe8\x41\x0c", res.message.data.bytes, 6);
    const uint8_t fc[] = {0x30, 0, 0, 0, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, fc);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x21, fake_can_sent(1)->data[0]);
}

TEST(flow_control_overflow_fails_the_logical_transmit) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    uint8_t data[14] = {0, 0, 7, 0xe0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    queue(ch, J2534_ISOTP, J2534_PAD, data, sizeof(data));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x10, fake_can_sent(0)->data[0]);
    const uint8_t overflow[] = {0x32, 0, 0, 0, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, overflow);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_FAILED, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(42, res.message.handle);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
}

TEST(logical_transmit_fails_when_its_first_frame_cannot_be_sent) {
    uint32_t ch = logical(connect_can(0), false);
    uint8_t data[14] = {0, 0, 7, 0xe0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    queue(ch, J2534_ISOTP, J2534_PAD, data, sizeof(data));
    TEST_ASSERT_EQUAL_INT(0, call());
    fake_can_fail_next_send();
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_FAILED, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(42, res.message.handle);
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
}

TEST(logical_reception_times_out_and_late_segments_are_ignored) {
    uint32_t ch = logical(connect_can(0), false);
    const uint8_t first[] = {0x10, 10, 1, 2, 3, 4, 5, 6};
    const uint8_t last[] = {0x21, 7, 8, 9, 10, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, first);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_START, res.message.rx_status);
    fake_clock_advance_ms(1001);
    j2534_core_poll();
    fake_can_stage_stale(0x7e8, 8, last);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
}

TEST(legacy_reception_on_one_logical_also_feeds_its_peers_receives) {
    uint32_t physical = connect_can(0);
    uint32_t reader = legacy_logical(physical, 0xe8, 0xe0);
    uint32_t peer = legacy_logical(physical, 0xe9, 0xe1);
    j2534_repeat_setup_t setup = repeat_setup(J2534_ISOTP, 0);
    memcpy(setup.message.data, "\0\0\x07\xe1\x01", 5);
    setup.mask.len = setup.pattern.len = 4;
    memset(setup.mask.data, 0xff, 4);
    memcpy(setup.pattern.data, "\0\0\x07\xe8", 4);
    uint32_t id = start_repeat(peer, &setup);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_TRUE(repeat_active(peer, id));
    static const uint8_t response[] = {0x01, 0x41, 0, 0, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, response);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(reader));
    TEST_ASSERT_EQUAL_INT(5, res.message.data.size);
    TEST_ASSERT_EQUAL_MEM("\0\0\x07\xe8\x41", res.message.data.bytes, 5);
    TEST_ASSERT_FALSE(repeat_active(peer, id));
}

TEST(legacy_peer_completes_repeat_jobs_for_segments_it_did_not_receive) {
    TEST_ASSERT_FALSE(legacy_scenario(true));
    TEST_ASSERT_TRUE(legacy_scenario(false));
}

TEST(poll_after_the_device_is_closed_transmits_nothing) {
    uint32_t ch = connect_can(0);
    static const uint8_t request[] = {0, 0, 7, 0xdf};
    queue(ch, J2534_CAN, 0, request, sizeof(request));
    TEST_ASSERT_EQUAL_INT(0, call());
    command(opendiag_Request_close_tag);
    req.command.close.id = dev;
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
    TEST_ASSERT_FALSE(fake_can_is_up());
}

TEST(fast_init_is_refused_on_can_channels) {
    uint32_t can = connect_can(0);
    TEST_ASSERT_EQUAL_INT(J2534_IOCTL, hds_init(can, 1, false));
}

TEST(logical_transmit_is_aborted_by_an_overflow_flow_control) {
    uint32_t ch = logical(connect_can(0), false);
    static const uint8_t msg[] = {0, 0, 7, 0xe0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    queue(ch, J2534_ISOTP, J2534_PAD, msg, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    static const uint8_t overflow[] = {0x32, 0, 0, 0, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, overflow);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_FAILED, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
}

TEST(version_and_last_error_text_are_bound_to_the_device_and_last_failure) {
    command(opendiag_Request_version_tag);
    req.command.version.id = dev + 1;
    TEST_ASSERT_EQUAL_INT(J2534_DEVICE, call());
    req.command.version.id = dev;
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_STRING(
        "OpenDIAG host-test; protobuf wire 3; J2534 05.00 subset", res.text);
    command(opendiag_Request_close_tag);
    req.command.close.id = dev + 1;
    TEST_ASSERT_EQUAL_INT(J2534_DEVICE, call());
    command(opendiag_Request_last_error_tag);
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_STRING("J2534 status 0x1a", res.text);
}

TEST(read_voltage_ioctls_check_device_connector_pin_and_data) {
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl = (opendiag_Ioctl){.target = dev + 1, .id = 3,
                                         .connector = 1, .pin = 16};
    TEST_ASSERT_EQUAL_INT(J2534_DEVICE, call());
    req.command.ioctl.target = dev;
    req.command.ioctl.data.size = 1;
    TEST_ASSERT_EQUAL_INT(J2534_VALUE, call());
    req.command.ioctl.data.size = 0;
    req.command.ioctl.config_count = 1;
    TEST_ASSERT_EQUAL_INT(J2534_VALUE, call());
    req.command.ioctl.config_count = 0;
    req.command.ioctl.connector = 0;
    TEST_ASSERT_EQUAL_INT(J2534_PIN, call());
    req.command.ioctl.connector = 1;
    req.command.ioctl.pin = 15;
    TEST_ASSERT_EQUAL_INT(J2534_PIN, call());
    req.command.ioctl = (opendiag_Ioctl){.target = dev, .id = 14, .connector = 1};
    TEST_ASSERT_EQUAL_INT(J2534_PIN, call());
}

TEST(read_vbatt_is_refused_because_rejsacan_has_no_battery_sense) {
    fake_board_set_vbatt_mv(12345);
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl = (opendiag_Ioctl){.target = dev, .id = 3,
                                         .connector = 1, .pin = 16};
    TEST_ASSERT_EQUAL_INT(J2534_NOT_SUPPORTED, call());
    fake_board_set_vbatt_mv(12600);
}

TEST(read_voltage_without_a_reading_fails_per_board) {
    fake_board_set_vbatt_mv(-1);
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl = (opendiag_Ioctl){.target = dev, .id = 3,
                                         .connector = 1, .pin = 16};
#ifdef OPENDIAG_BOARD_REJSACAN
    TEST_ASSERT_EQUAL_INT(J2534_NOT_SUPPORTED, call());
#else
    TEST_ASSERT_EQUAL_INT(J2534_FAILED, call());
#endif
    fake_board_set_vbatt_mv(12600);
}

TEST(programming_voltage_reading_is_absent_on_rejsa_and_zero_on_stock) {
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl = (opendiag_Ioctl){.target = dev, .id = 14};
#ifdef OPENDIAG_BOARD_REJSACAN
    TEST_ASSERT_EQUAL_INT(J2534_NOT_SUPPORTED, call());
#else
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(0, res.millivolts);
#endif
}

TEST(programming_voltage_rejects_another_device_and_a_non_connector_one) {
    command(opendiag_Request_voltage_tag);
    req.command.voltage = (opendiag_Voltage){dev + 1, 1, 6, 12000};
    TEST_ASSERT_EQUAL_INT(J2534_DEVICE, call());
    req.command.voltage = (opendiag_Voltage){dev, 2, 6, 12000};
    TEST_ASSERT_EQUAL_INT(J2534_PIN, call());
    req.command.voltage = (opendiag_Voltage){dev, 1, 1, 12000};
    TEST_ASSERT_EQUAL_INT(J2534_PIN, call());
}

/* Rejsa has no programming-voltage hardware, so every valid pin answers
 * J2534_NOT_SUPPORTED. Stock boards take the pin through their own checks. */
TEST(programming_voltage_is_answered_on_every_pin_without_driving_it) {
    connect_can(0);
    command(opendiag_Request_voltage_tag);
    req.command.voltage = (opendiag_Voltage){dev, 1, 6, 12000};
#ifdef OPENDIAG_BOARD_REJSACAN
    const uint32_t pins[] = {6, 9, 11, 12, 13, 14, 15};
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        req.command.voltage.pin = pins[i];
        TEST_ASSERT_EQUAL_INT(J2534_NOT_SUPPORTED, call());
        req.command.voltage.millivolts = UINT32_MAX;
        TEST_ASSERT_EQUAL_INT(J2534_NOT_SUPPORTED, call());
        req.command.voltage.millivolts = 12000;
    }
#else
    TEST_ASSERT_EQUAL_INT(J2534_PIN_IN_USE, call());
#endif
    TEST_ASSERT_FALSE(vif_any_pin_active());
}

TEST(repeat_start_rejects_unknown_channel_null_setup_and_protocol_mismatch) {
    uint32_t id = 0;
    uint32_t can = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(0);
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL,
                          j2534_core_repeat_start(123, &setup, &id));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE, j2534_core_repeat_start(can, NULL, &id));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          j2534_core_repeat_start(can, &setup, NULL));
    setup.mask.protocol = J2534_ISOTP;
    TEST_ASSERT_EQUAL_INT(J2534_MSG_PROTOCOL,
                          j2534_core_repeat_start(can, &setup, &id));
    setup = can_repeat(0);
    setup.message.protocol = J2534_ISOTP;
    TEST_ASSERT_EQUAL_INT(J2534_MSG_PROTOCOL,
                          j2534_core_repeat_start(can, &setup, &id));
    TEST_ASSERT_EQUAL_INT(0, id);
}

TEST(repeat_start_rejects_bad_shapes_intervals_and_identifiers) {
    uint32_t id = 0;
    uint32_t can = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(0);
    setup.interval_ms = 4;
    TEST_ASSERT_EQUAL_INT(J2534_INTERVAL,
                          j2534_core_repeat_start(can, &setup, &id));
    setup.interval_ms = 65536;
    TEST_ASSERT_EQUAL_INT(J2534_INTERVAL,
                          j2534_core_repeat_start(can, &setup, &id));
    setup = can_repeat(2);
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          j2534_core_repeat_start(can, &setup, &id));
    setup = can_repeat(0);
    setup.message.len = 0;
    TEST_ASSERT_EQUAL_INT(J2534_MSG, j2534_core_repeat_start(can, &setup, &id));
    setup = can_repeat(0);
    setup.message.len = 13;
    TEST_ASSERT_EQUAL_INT(J2534_MSG, j2534_core_repeat_start(can, &setup, &id));
    setup = can_repeat(0);
    setup.mask.len = 0;
    TEST_ASSERT_EQUAL_INT(J2534_MSG, j2534_core_repeat_start(can, &setup, &id));
    setup = can_repeat(0);
    setup.mask.len = 13;
    TEST_ASSERT_EQUAL_INT(J2534_MSG, j2534_core_repeat_start(can, &setup, &id));
    setup = can_repeat(0);
    setup.pattern.len = 2;
    TEST_ASSERT_EQUAL_INT(J2534_MSG, j2534_core_repeat_start(can, &setup, &id));
    setup = can_repeat(0);
    setup.pattern.flags = J2534_29BIT;
    TEST_ASSERT_EQUAL_INT(J2534_MSG, j2534_core_repeat_start(can, &setup, &id));
    setup = can_repeat(0);
    memcpy(setup.message.data, "\x08\x00\x00\x00\x01", 5);
    TEST_ASSERT_EQUAL_INT(J2534_MSG, j2534_core_repeat_start(can, &setup, &id));
    TEST_ASSERT_EQUAL_INT(0, id);
}

TEST(repeat_start_rejects_flags_the_channel_cannot_match) {
    uint32_t id = 0;
    uint32_t can = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(0);
    setup.mask.flags = setup.pattern.flags = J2534_29BIT;
    TEST_ASSERT_EQUAL_INT(J2534_FLAGS,
                          j2534_core_repeat_start(can, &setup, &id));
    uint32_t lg = logical(can, false);
    setup = repeat_setup(J2534_ISOTP, 0);
    memcpy(setup.message.data, "\0\0\x07\xe0\x01", 5);
    setup.mask.flags = setup.pattern.flags = J2534_ADDR;
    TEST_ASSERT_EQUAL_INT(J2534_FLAGS, j2534_core_repeat_start(lg, &setup, &id));
    TEST_ASSERT_EQUAL_INT(0, id);
}

TEST(repeat_start_limits_mask_length_to_the_protocol_frame) {
    uint32_t can = connect_can(0);
    uint32_t lg = logical(can, false);
    uint32_t id = 0;
    j2534_repeat_setup_t setup = repeat_setup(J2534_ISOTP, 0);
    memcpy(setup.message.data, "\0\0\x07\xe0\x01", 5);
    setup.mask.data[0] = setup.pattern.data[0] = 0;
    setup.mask.len = setup.pattern.len = 12;
    TEST_ASSERT_EQUAL_INT(J2534_MSG, j2534_core_repeat_start(lg, &setup, &id));
    setup.mask.len = setup.pattern.len = 11;
    TEST_ASSERT_EQUAL_INT(0, j2534_core_repeat_start(lg, &setup, &id));
    TEST_ASSERT_TRUE(id != 0);
}

TEST(repeat_query_and_stop_check_channel_argument_and_job_kind) {
    bool active = true;
    uint32_t can = connect_can(0);
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL,
                          j2534_core_repeat_query(123, 1, &active));
    TEST_ASSERT_EQUAL_INT(J2534_VALUE, j2534_core_repeat_query(can, 1, NULL));
    TEST_ASSERT_EQUAL_INT(J2534_MSG_ID,
                          j2534_core_repeat_query(can, 999, &active));
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, j2534_core_repeat_stop(123, 1));
    command(opendiag_Request_start_periodic_tag);
    opendiag_Periodic *p = &req.command.start_periodic;
    p->channel = can;
    p->interval_ms = 10;
    p->has_message = true;
    p->message.protocol = J2534_CAN;
    p->message.data.size = 5;
    memcpy(p->message.data.bytes, "\0\0\x07\xdf\x55", 5);
    TEST_ASSERT_EQUAL_INT(0, call());
    uint32_t periodic = res.id;
    TEST_ASSERT_EQUAL_INT(J2534_MSG_ID,
                          j2534_core_repeat_query(can, periodic, &active));
    TEST_ASSERT_EQUAL_INT(J2534_MSG_ID, j2534_core_repeat_stop(can, periodic));
    command(opendiag_Request_stop_periodic_tag);
    req.command.stop_periodic = (opendiag_Object){can, periodic};
    TEST_ASSERT_EQUAL_INT(0, call());
}

TEST(repeat_wire_requests_start_query_and_stop_by_id) {
    uint32_t ch = connect_can(0);
    command(opendiag_Request_start_repeat_tag);
    opendiag_Repeat *r = &req.command.start_repeat;
    r->channel = ch;
    r->interval_ms = 20;
    r->has_message = true;
    r->message.protocol = J2534_CAN;
    r->message.data.size = 5;
    memcpy(r->message.data.bytes, "\0\0\x07\xdf\x01", 5);
    r->has_pattern = true;
    r->pattern.protocol = J2534_CAN;
    r->pattern.data.size = 1;
    r->pattern.data.bytes[0] = 0;
    TEST_ASSERT_EQUAL_INT(J2534_MSG, call());
    r->has_mask = true;
    r->mask.protocol = J2534_CAN;
    r->mask.data.size = 1;
    r->mask.data.bytes[0] = 0;
    TEST_ASSERT_EQUAL_INT(0, call());
    uint32_t id = res.id;
    TEST_ASSERT_TRUE(id != 0);
    command(opendiag_Request_query_repeat_tag);
    req.command.query_repeat = (opendiag_Object){ch, id};
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_TRUE(res.repeat_active);
    command(opendiag_Request_stop_repeat_tag);
    req.command.stop_repeat = (opendiag_Object){ch, id};
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(J2534_MSG_ID, call());
    command(opendiag_Request_query_repeat_tag);
    req.command.query_repeat = (opendiag_Object){ch, id};
    TEST_ASSERT_EQUAL_INT(J2534_MSG_ID, call());
}

TEST(repeat_can_and_isotp_use_independent_jobs_and_native_framing) {
    uint32_t physical = connect_can(0);
    uint32_t channel = logical(physical, false);
    j2534_repeat_setup_t setup = repeat_setup(J2534_ISOTP, 0);
    memcpy(setup.message.data, "\0\0\x07\xe0\x01", 5);
    setup.mask.data[0] = 0;
    setup.pattern.data[0] = 0;
    uint32_t logical_id = start_repeat(channel, &setup);
    setup.message.protocol = setup.mask.protocol = setup.pattern.protocol =
        J2534_CAN;
    uint32_t physical_id = start_repeat(physical, &setup);
    j2534_core_poll();
    j2534_core_poll();
    const struct can_frame *frame = fake_can_sent(0);
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_EQUAL_INT(0x7e0, frame->id);
    TEST_ASSERT_EQUAL_INT(1, frame->data[0]);
    fake_clock_advance_ms(1);
    const uint8_t response[] = {1, 0x41};
    fake_can_stage_stale(0x7e8, sizeof(response), response);
    j2534_core_poll();
    TEST_ASSERT_FALSE(repeat_active(channel, logical_id));
    TEST_ASSERT_FALSE(repeat_active(physical, physical_id));
}

TEST(repeat_can_29bit_masks_are_bit_patterns_not_transmit_identifiers) {
    uint32_t channel = connect_can(J2534_29BIT);
    j2534_repeat_setup_t setup = repeat_setup(J2534_CAN, 0);
    setup.message.flags = setup.mask.flags = setup.pattern.flags = J2534_29BIT;
    memcpy(setup.message.data, "\x18\xdb\x33\xf1\x01", 5);
    setup.mask.len = setup.pattern.len = 4;
    memset(setup.mask.data, 0xff, 4);
    memcpy(setup.pattern.data, "\x18\xda\xf1\x10", 4);
    uint32_t id = start_repeat(channel, &setup);
    j2534_core_poll();
    fake_clock_advance_ms(1);
    const uint8_t response[] = {0x41};
    fake_can_stage_stale(CAN_EFF_FLAG | 0x18daf110, sizeof(response), response);
    j2534_core_poll();
    TEST_ASSERT_FALSE(repeat_active(channel, id));
}

TEST(
    repeat_isotp_extended_address_uses_single_frame_without_mutating_channel_tx) {
    uint32_t physical = connect_can(0);
    uint32_t channel = logical(physical, true);
    j2534_repeat_setup_t setup = repeat_setup(J2534_ISOTP, 0);
    setup.message.flags = J2534_ADDR | J2534_PAD;
    setup.mask.flags = setup.pattern.flags = J2534_ADDR;
    setup.message.len = 6;
    memcpy(setup.message.data, "\0\0\x07\xe0\x10\x01", 6);
    setup.mask.data[0] = setup.pattern.data[0] = 0;
    uint32_t id = start_repeat(channel, &setup);
    j2534_core_poll();
    const struct can_frame *frame = fake_can_sent(0);
    TEST_ASSERT_EQUAL_INT(0x10, frame->data[0]);
    TEST_ASSERT_EQUAL_INT(1, frame->data[1]);
    fake_clock_advance_ms(1);
    const uint8_t response[] = {0xf1, 1, 0x41};
    fake_can_stage_stale(0x7e8, sizeof(response), response);
    j2534_core_poll();
    TEST_ASSERT_FALSE(repeat_active(channel, id));
}

TEST(repeat_validates_shapes_and_retains_quota_until_stop) {
    uint32_t channel = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(0);
    uint32_t id = 0;
    setup.interval_ms = 4;
    TEST_ASSERT_EQUAL_INT(J2534_INTERVAL,
                          j2534_core_repeat_start(channel, &setup, &id));
    setup.interval_ms = 20;
    setup.condition = 2;
    TEST_ASSERT_EQUAL_INT(J2534_VALUE,
                          j2534_core_repeat_start(channel, &setup, &id));
    setup.condition = 0;
    setup.pattern.len = 2;
    TEST_ASSERT_EQUAL_INT(J2534_MSG,
                          j2534_core_repeat_start(channel, &setup, &id));
    setup.pattern.len = 1;
    for (unsigned i = 0; i < J2534_REPEAT_MAX; i++)
        id = start_repeat(channel, &setup);
    TEST_ASSERT_EQUAL_INT(J2534_LIMIT,
                          j2534_core_repeat_start(channel, &setup, &id));
    TEST_ASSERT_EQUAL_INT(0, j2534_core_repeat_stop(channel, id));
    start_repeat(channel, &setup);
}

TEST(repeat_on_can_is_rescheduled_from_the_end_of_each_transmit) {
    uint32_t channel = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(0);
    start_repeat(channel, &setup);
    memset(&setup, 0, sizeof(setup));
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    fake_clock_advance_ms(19);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    fake_clock_advance_ms(1);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    const struct can_frame *frame = fake_can_sent(1);
    TEST_ASSERT_EQUAL_INT(0x7df, frame->id);
    TEST_ASSERT_EQUAL_INT(1, frame->dlc);
    TEST_ASSERT_EQUAL_INT(1, frame->data[0]);
}

TEST(repeat_while_without_a_reply_stops_and_until_stops_on_a_match) {
    uint32_t channel = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(1);
    uint32_t id = start_repeat(channel, &setup);
    j2534_core_poll();
    fake_clock_advance_ms(21);
    j2534_core_poll();
    TEST_ASSERT_FALSE(repeat_active(channel, id));
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    setup.condition = 0;
    id = start_repeat(channel, &setup);
    j2534_core_poll();
    fake_clock_advance_ms(1);
    const uint8_t response[] = {0x41};
    fake_can_stage_stale(0x7e8, sizeof(response), response);
    j2534_core_poll();
    TEST_ASSERT_FALSE(repeat_active(channel, id));
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(channel));
}

TEST(pending_repeat_can_be_stopped_without_transmitting) {
    uint32_t channel = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(0);
    uint32_t id = start_repeat(channel, &setup);
    fake_bus_tx_hold(true);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, j2534_core_repeat_stop(channel, id));
    fake_bus_tx_hold(false);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
}

TEST(response_cancels_a_repeat_waiting_for_bus_admission) {
    uint32_t channel = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(0);
    uint32_t id = start_repeat(channel, &setup);
    fake_bus_tx_hold(true);
    j2534_core_poll();
    fake_clock_advance_ms(1);
    const uint8_t response[] = {0x41};
    fake_can_stage_stale(0x7e8, sizeof(response), response);
    /* Let admission race the core's next receive drain. */
    fake_bus_tx_hold(false);
    j2534_core_poll();
    TEST_ASSERT_FALSE(repeat_active(channel, id));
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
}

TEST(failed_repeat_transmit_stops_the_job_and_keeps_its_id) {
    uint32_t channel = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(0);
    uint32_t failed = start_repeat(channel, &setup);
    fake_can_fail_next_send();
    j2534_core_poll();
    TEST_ASSERT_FALSE(repeat_active(channel, failed));
    TEST_ASSERT_EQUAL_INT(0, j2534_core_repeat_stop(channel, failed));
}

TEST(stopped_repeat_completion_cannot_change_a_new_job_in_the_reused_slot) {
    uint32_t channel = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(0);
    uint32_t old_id = start_repeat(channel, &setup);
    fake_bus_tx_hold_completion(true);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, j2534_core_repeat_stop(channel, old_id));
    uint32_t new_id = start_repeat(channel, &setup);
    TEST_ASSERT_TRUE(old_id != new_id);
    fake_bus_tx_hold_completion(false);
    j2534_core_poll();
    TEST_ASSERT_TRUE(repeat_active(channel, new_id));
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
}

TEST(clear_periodics_does_not_clear_repeat_ids_or_pending_repeat) {
    uint32_t channel = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(0);
    uint32_t id = start_repeat(channel, &setup);
    fake_bus_tx_hold(true);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(J2534_OK, ioctl_call(channel, BUS_IOCTL_CLEAR_PERIODIC));
    fake_bus_tx_hold(false);
    j2534_core_poll();
    TEST_ASSERT_TRUE(repeat_active(channel, id));
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
}

TEST(clear_rx_still_applies_repeat_conditions_to_removed_responses) {
    uint32_t channel = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(0);
    uint32_t id = start_repeat(channel, &setup);
    j2534_core_poll();
    fake_clock_advance_ms(1);
    const uint8_t data[] = {0x41};
    fake_can_stage_stale(0x7e8, sizeof(data), data);
    TEST_ASSERT_EQUAL_INT(J2534_OK, ioctl_call(channel, BUS_IOCTL_CLEAR_RX_QUEUE));
    TEST_ASSERT_FALSE(repeat_active(channel, id));
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(channel));
}

TEST(repeat_can_stopped_with_a_held_transmit_never_reaches_the_bus) {
    uint32_t channel = connect_can(0);
    j2534_repeat_setup_t setup = can_repeat(0);
    uint32_t id = start_repeat(channel, &setup);
    fake_bus_tx_hold(true);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, j2534_core_repeat_stop(channel, id));
    j2534_core_poll();
    fake_bus_tx_hold(false);
    j2534_core_poll();
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(J2534_MSG_ID, j2534_core_repeat_stop(channel, id));
}

TEST(configuration_ioctls_on_a_cleared_bus_keep_the_same_limits) {
    uint32_t ch = logical_at(connect_can(0), 0xe8, 0xe0, false);
    config_set(ch, TST_CFG_N_CR, 50);
    TEST_ASSERT_EQUAL_INT(J2534_OK, ioctl_call(ch, BUS_IOCTL_CLEAR_RX_QUEUE));
    TEST_ASSERT_EQUAL_INT(50, config_get(ch, TST_CFG_N_CR));
}

TEST(select_reports_only_readable_live_channels_and_rejects_bad_requests) {
    uint32_t can = connect_can(0);
    uint32_t lg = logical(can, false);
    filter(can, 1, 0, 0);
    command(opendiag_Request_select_tag);
    req.command.select = (opendiag_Select){.type = 2, .channels_count = 1,
                                           .channels = {can}};
    TEST_ASSERT_EQUAL_INT(J2534_SELECT, call());
    req.command.select.type = 1;
    req.command.select.channels[0] = 123;
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, call());
    req.command.select = (opendiag_Select){.type = 1, .channels_count = 2,
                                           .channels = {lg, can}};
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(0, res.channels_count);
    /* 0x7e9 is not the logical channel's receive identifier, so only the
     * physical channel has something to read. */
    static const uint8_t d[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    fake_can_stage_stale(0x7e9, 8, d);
    j2534_core_poll();
    req.command.select = (opendiag_Select){.type = 1, .channels_count = 2,
                                           .channels = {lg, can}};
    TEST_ASSERT_EQUAL_INT(0, call());
    TEST_ASSERT_EQUAL_INT(1, res.channels_count);
    TEST_ASSERT_EQUAL_INT(can, res.channels[0]);
}

TEST(read_on_an_unopened_device_and_logical_channels_are_refused) {
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, read_message(0));
}

#ifdef OPENDIAG_BOARD_REJSACAN
TEST(rejsa_has_no_sense_input_for_programming_voltage) {
    command(opendiag_Request_ioctl_tag);
    req.command.ioctl = (opendiag_Ioctl){.target = dev, .id = 14};
    TEST_ASSERT_EQUAL_INT(J2534_NOT_SUPPORTED, call());
}
#endif

/* ------------------------------------------------------------------------ *
 * Wake-up deadlines, ISO-TP trains, and reads and selects that wait.
 * ------------------------------------------------------------------------ */

static uint32_t connect_can_at(uint32_t flags, uint32_t rate) {
    command(opendiag_Request_connect_tag);
    req.command.connect = (opendiag_Connect){.device = dev,
                                             .protocol = J2534_CAN,
                                             .flags = flags,
                                             .baudrate = rate,
                                             .connector = 1,
                                             .pins_count = 2,
                                             .pins = {6, 14}};
    TEST_ASSERT_EQUAL_INT(0, call());
    return res.id;
}

/* FF plus five CFs; the flow control frame is staged by each test. */
static uint32_t queue_segmented(void) {
    uint32_t ch = logical(connect_can(0), false);
    uint8_t msg[4 + 40] = {0, 0, 7, 0xe0};
    for (unsigned i = 4; i < sizeof(msg); i++)
        msg[i] = (uint8_t)i;
    queue(ch, J2534_ISOTP, J2534_PAD, msg, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    return ch;
}

static void stage_flow_control(uint8_t block_size, uint8_t stmin) {
    const uint8_t fc[] = {0x30, block_size, stmin, 0, 0, 0, 0, 0};
    fake_can_stage_stale(0x7e8, 8, fc);
}

/* Microseconds from now until the core wants polling again; -1 for never. */
static int64_t next_wake_in_us(void) {
    uint32_t due;
    if (!j2534_core_next_wake(&due))
        return -1;
    return (int32_t)(due - (uint32_t)esp_timer_get_time());
}

TEST(nothing_to_do_asks_for_no_wakeup) {
    logical(connect_can(0), false);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(-1, next_wake_in_us());
}

TEST(next_wake_is_false_once_the_device_is_closed) {
    connect_can(0);
    command(opendiag_Request_close_tag);
    req.command.close.id = dev;
    TEST_ASSERT_EQUAL_INT(0, call());
    uint32_t due;
    TEST_ASSERT_FALSE(j2534_core_next_wake(&due));
}

TEST(frames_beyond_the_receive_budget_ask_for_another_pass_now) {
    uint32_t p = connect_can(0);
    filter(p, 1, 0, 0);
    const uint8_t frame[8] = {1};
    for (int i = 0; i < 40; i++)
        fake_can_stage_stale(0x123, 8, frame);
    j2534_core_poll();
    uint32_t due;
    TEST_ASSERT_TRUE(j2534_core_next_wake(&due));
    TEST_ASSERT_TRUE((int32_t)(due - (uint32_t)esp_timer_get_time()) <= 0);
    j2534_core_poll();
    TEST_ASSERT_FALSE(j2534_core_next_wake(&due));
}

TEST(every_received_frame_counts_as_progress) {
    uint32_t p = connect_can(0);
    filter(p, 1, 0, 0);
    const uint8_t frame[8] = {1};
    uint32_t before = j2534_core_progress();
    fake_can_stage_stale(0x123, 8, frame);
    j2534_core_poll();
    TEST_ASSERT_TRUE(j2534_core_progress() != before);
    before = j2534_core_progress();
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(before, j2534_core_progress());
}

TEST(zero_stmin_sends_the_rest_of_the_message_as_one_train) {
    uint32_t ch = queue_segmented();
    stage_flow_control(0, 0);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(6, fake_can_sent_count());
    for (int i = 1; i < 6; i++)
        TEST_ASSERT_EQUAL_INT(0x20 + i, fake_can_sent(i)->data[0]);
    TEST_ASSERT_EQUAL_INT(43, fake_can_sent(5)->data[6]);
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_SUCCESS, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(-1, next_wake_in_us());
}

static void assert_wakes_for_stmin(uint8_t stmin, int64_t separation_us) {
    queue_segmented();
    stage_flow_control(0, stmin);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    int64_t wait = next_wake_in_us();
    TEST_ASSERT_TRUE(wait > separation_us - 50 && wait <= separation_us);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    fake_clock_advance_ms(separation_us / 1000 + 1);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(3, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x22, fake_can_sent(2)->data[0]);
}

TEST(next_wake_is_the_peers_stmin_after_a_consecutive_frame) {
    assert_wakes_for_stmin(5, 5000);
}

TEST(next_wake_honours_sub_millisecond_stmin) {
    assert_wakes_for_stmin(0xf5, 500);
}

TEST(stmin_runs_from_when_the_frame_left_not_when_it_was_collected) {
    queue_segmented();
    stage_flow_control(0, 5);
    fake_can_tx_end_lag_us(300);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    int64_t wait = next_wake_in_us();
    TEST_ASSERT_TRUE(wait > 4650 && wait <= 4700);
}

TEST(next_wake_is_the_flow_control_deadline_while_one_is_awaited) {
    queue_segmented();
    int64_t wait = next_wake_in_us();
    TEST_ASSERT_TRUE(wait > 0 && wait <= 1000000);
}

TEST(next_wake_is_the_receive_deadline_of_an_open_reception) {
    logical(connect_can(0), false);
    const uint8_t first[] = {0x10, 10, 1, 2, 3, 4, 5, 6};
    fake_can_stage_stale(0x7e8, 8, first);
    j2534_core_poll();
    int64_t wait = next_wake_in_us();
    TEST_ASSERT_TRUE(wait > 0 && wait <= 1000000);
}

TEST(next_wake_is_the_next_periodic_message) {
    uint32_t ch = connect_can(0);
    TEST_ASSERT_EQUAL_INT(0, start_periodic_can(ch, 100, J2534_CAN));
    j2534_core_poll();
    int64_t wait = next_wake_in_us();
    TEST_ASSERT_TRUE(wait > 0 && wait <= 100000);
}

/* A long message after a zero STmin flow control: the frames of the first
 * train, which must fit the time budget at @p rate. */
static int first_train_at(uint32_t rate) {
    uint32_t ch = logical(connect_can_at(0, rate), false);
    uint8_t msg[4 + 300] = {0, 0, 7, 0xe0};
    queue(ch, J2534_ISOTP, J2534_PAD, msg, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    stage_flow_control(0, 0);
    j2534_core_poll();
    return fake_can_sent_count() - 1;
}

/* 11-bit, eight bytes, worst-case stuffing: 135 bits. */
TEST(a_train_holds_the_bus_for_at_most_four_milliseconds_at_500k) {
    TEST_ASSERT_EQUAL_INT(14, first_train_at(500000));
}

TEST(a_train_holds_the_bus_for_at_most_four_milliseconds_at_125k) {
    TEST_ASSERT_EQUAL_INT(3, first_train_at(125000));
}

TEST(a_frame_longer_than_the_budget_goes_out_alone) {
    TEST_ASSERT_EQUAL_INT(1, first_train_at(50000));
}

TEST(trains_follow_each_other_until_the_message_is_sent) {
    uint32_t ch = logical(connect_can(0), false);
    uint8_t msg[4 + 300] = {0, 0, 7, 0xe0};
    queue(ch, J2534_ISOTP, J2534_PAD, msg, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(0, call());
    j2534_core_poll();
    stage_flow_control(0, 0);
    for (int i = 0; i < 10 && fake_can_sent_count() < 43; i++)
        j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(43, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x2a, fake_can_sent(42)->data[0]);
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_SUCCESS, res.message.rx_status);
}

TEST(a_train_stops_at_the_block_size_and_waits_for_the_n_bs_timeout) {
    queue_segmented();
    stage_flow_control(2, 0);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(3, fake_can_sent_count());
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(3, fake_can_sent_count());
    int64_t wait = next_wake_in_us();
    TEST_ASSERT_TRUE(wait > 999000 && wait <= 1000000);
    stage_flow_control(2, 0);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(5, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x23, fake_can_sent(3)->data[0]);
    TEST_ASSERT_EQUAL_INT(0x24, fake_can_sent(4)->data[0]);
}

TEST(a_frame_failing_mid_train_fails_the_message_once_and_sends_no_more) {
    uint32_t ch = queue_segmented();
    stage_flow_control(0, 0);
    fake_can_fail_send_after(2);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(3, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_FAILED, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(3, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(-1, next_wake_in_us());
}

/* Sends req; true when it is held, false when it was answered at once, with
 * the answer in res. */
static bool request_is_held(void) {
    pb_ostream_t output = pb_ostream_from_buffer(encoded, sizeof(encoded));
    TEST_ASSERT_TRUE(pb_encode(&output, opendiag_Request_fields, &req));
    size_t len = j2534_core_request(req.which_command - 1, encoded,
                                    output.bytes_written, reply);
    if (len == J2534_CORE_HELD)
        return true;
    pb_istream_t input = pb_istream_from_buffer(reply, len);
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_TRUE(pb_decode(&input, opendiag_Response_fields, &res));
    return false;
}

/* The held request's answer after one poll; false while it still waits. */
static bool poll_held(void) {
    size_t len;
    j2534_core_poll();
    if (!j2534_core_take_held(reply, &len))
        return false;
    pb_istream_t input = pb_istream_from_buffer(reply, len);
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_TRUE(pb_decode(&input, opendiag_Response_fields, &res));
    return true;
}

static void read_waiting(uint32_t ch, uint32_t timeout_ms) {
    command(opendiag_Request_read_tag);
    req.command.read = (opendiag_Read){.id = ch, .timeout_ms = timeout_ms};
}

static const uint8_t single_frame[] = {3, 0x41, 0x0c, 0x55, 0, 0, 0, 0};

TEST(a_read_without_timeout_answers_empty_at_once) {
    uint32_t ch = logical(connect_can(0), false);
    read_waiting(ch, 0);
    TEST_ASSERT_FALSE(request_is_held());
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, res.status);
}

TEST(a_read_with_timeout_is_answered_when_the_message_arrives) {
    uint32_t ch = logical(connect_can(0), false);
    read_waiting(ch, 100);
    TEST_ASSERT_TRUE(request_is_held());
    TEST_ASSERT_FALSE(poll_held());
    int64_t wait = next_wake_in_us();
    TEST_ASSERT_TRUE(wait > 99000 && wait <= 100000);
    fake_clock_advance_ms(40);
    fake_can_stage_stale(0x7e8, 8, single_frame);
    TEST_ASSERT_TRUE(poll_held());
    TEST_ASSERT_EQUAL_INT(0, res.status);
    TEST_ASSERT_EQUAL_MEM("\0\0\x07\xe8\x41\x0c\x55", res.message.data.bytes,
                          7);
    TEST_ASSERT_EQUAL_INT(-1, next_wake_in_us());
    TEST_ASSERT_FALSE(poll_held());
}

TEST(a_message_already_queued_is_read_without_holding) {
    uint32_t ch = logical(connect_can(0), false);
    fake_can_stage_stale(0x7e8, 8, single_frame);
    j2534_core_poll();
    read_waiting(ch, 100);
    TEST_ASSERT_FALSE(request_is_held());
    TEST_ASSERT_EQUAL_INT(0, res.status);
    TEST_ASSERT_EQUAL_INT(1, res.count);
}

TEST(a_held_read_answers_empty_at_its_deadline) {
    uint32_t ch = logical(connect_can(0), false);
    read_waiting(ch, 100);
    TEST_ASSERT_TRUE(request_is_held());
    fake_clock_advance_ms(99);
    TEST_ASSERT_FALSE(poll_held());
    fake_clock_advance_ms(1);
    TEST_ASSERT_TRUE(poll_held());
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, res.status);
}

TEST(a_wake_ends_a_held_read_as_if_its_timeout_ran_out) {
    uint32_t ch = logical(connect_can(0), false);
    read_waiting(ch, 5000);
    TEST_ASSERT_TRUE(request_is_held());
    TEST_ASSERT_FALSE(poll_held());
    j2534_core_wake_held();
    TEST_ASSERT_TRUE(poll_held());
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, res.status);
}

TEST(a_held_read_reports_its_channel_going_away) {
    uint32_t p = connect_can(0), ch = logical(p, false);
    read_waiting(ch, 100);
    TEST_ASSERT_TRUE(request_is_held());
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
    TEST_ASSERT_TRUE(poll_held());
    TEST_ASSERT_EQUAL_INT(J2534_CHANNEL, res.status);
}

TEST(cancelling_or_stopping_drops_a_held_request) {
    uint32_t ch = logical(connect_can(0), false);
    read_waiting(ch, 100);
    TEST_ASSERT_TRUE(request_is_held());
    j2534_core_cancel_held();
    fake_can_stage_stale(0x7e8, 8, single_frame);
    TEST_ASSERT_FALSE(poll_held());
    TEST_ASSERT_EQUAL_INT(-1, next_wake_in_us());
    read_waiting(ch, 100);
    TEST_ASSERT_FALSE(request_is_held());
    TEST_ASSERT_EQUAL_INT(0, res.status);
    read_waiting(ch, 100);
    TEST_ASSERT_TRUE(request_is_held());
    j2534_core_stop();
    TEST_ASSERT_FALSE(poll_held());
}

TEST(waits_are_capped_at_the_j2534_maximum) {
    uint32_t ch = logical(connect_can(0), false);
    read_waiting(ch, 60000);
    TEST_ASSERT_TRUE(request_is_held());
    int64_t wait = next_wake_in_us();
    TEST_ASSERT_TRUE(wait > 29999000 && wait <= 30000000);
}

TEST(a_select_waits_for_its_threshold_of_readable_channels) {
    uint32_t one = logical(connect_can(0), false);
    req.command.logical_connect.local_address.bytes[3] = 0xe9;
    req.command.logical_connect.remote_address.bytes[3] = 0xe1;
    TEST_ASSERT_EQUAL_INT(0, call());
    uint32_t two = res.id;
    command(opendiag_Request_select_tag);
    req.command.select = (opendiag_Select){.type = 1,
                                           .channels_count = 2,
                                           .channels = {one, two},
                                           .threshold = 2,
                                           .timeout_ms = 100};
    TEST_ASSERT_TRUE(request_is_held());
    fake_can_stage_stale(0x7e8, 8, single_frame);
    TEST_ASSERT_FALSE(poll_held());
    fake_can_stage_stale(0x7e9, 8, single_frame);
    TEST_ASSERT_TRUE(poll_held());
    TEST_ASSERT_EQUAL_INT(0, res.status);
    TEST_ASSERT_EQUAL_INT(2, res.channels_count);
}

TEST(a_select_without_threshold_or_timeout_answers_at_once) {
    uint32_t ch = logical(connect_can(0), false);
    command(opendiag_Request_select_tag);
    req.command.select = (opendiag_Select){
        .type = 1, .channels_count = 1, .channels = {ch}, .timeout_ms = 100};
    TEST_ASSERT_FALSE(request_is_held());
    req.command.select.threshold = 1;
    req.command.select.timeout_ms = 0;
    TEST_ASSERT_FALSE(request_is_held());
}

TEST(a_held_select_times_out_with_the_channels_that_are_readable) {
    uint32_t ch = logical(connect_can(0), false);
    command(opendiag_Request_select_tag);
    req.command.select = (opendiag_Select){.type = 1,
                                           .channels_count = 1,
                                           .channels = {ch},
                                           .threshold = 1,
                                           .timeout_ms = 20};
    TEST_ASSERT_TRUE(request_is_held());
    fake_clock_advance_ms(20);
    TEST_ASSERT_TRUE(poll_held());
    TEST_ASSERT_EQUAL_INT(0, res.status);
    TEST_ASSERT_EQUAL_INT(0, res.channels_count);
}

TEST(a_logical_channel_cannot_be_the_parent_of_another) {
    uint32_t lc = logical_at(connect_can(0), 0xe8, 0xe0, false);
    TEST_ASSERT_EQUAL_INT(J2534_LOGICAL,
                          logical_request(lc, J2534_ISOTP, 0, 0xe9, 4));
}

TEST(bus_on_is_refused_on_a_logical_channel) {
    uint32_t lc = logical_at(connect_can(0), 0xe8, 0xe0, false);
    TEST_ASSERT_EQUAL_INT(J2534_IOCTL, ioctl_call(lc, BUS_IOCTL_BUS_ON));
}

TEST(a_bus_parameter_the_driver_accepts_is_set_and_read_back) {
    uint32_t ch = connect_can(0);
    fake_can_enable_params(true);
    TEST_ASSERT_EQUAL_INT(J2534_OK, config_call(ch, true, BUS_P_P1_MAX, 7));
    TEST_ASSERT_EQUAL_INT(7, config_get(ch, BUS_P_P1_MAX));
    fake_can_enable_params(false);
}

/* A released bus keeps its channel record, so each connect takes the next
 * physical slot until the table is full. */
TEST(a_full_physical_table_refuses_a_connect_with_limit) {
    for (unsigned i = 0; i < J2534_PHYSICAL_MAX; i++) {
        connect_can(0);
        TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
    }
    const opendiag_Connect c = {.device = dev,
                                .protocol = J2534_CAN,
                                .flags = 0,
                                .baudrate = 500000,
                                .connector = 1,
                                .pins_count = 2,
                                .pins = {6, 14}};
    TEST_ASSERT_EQUAL_INT(J2534_LIMIT, connect_with(&c));
}

TEST(a_held_read_past_its_deadline_asks_for_no_wakeup) {
    uint32_t ch = logical(connect_can(0), false);
    read_waiting(ch, 100);
    TEST_ASSERT_TRUE(request_is_held());
    fake_clock_advance_ms(150);
    TEST_ASSERT_EQUAL_INT(-1, next_wake_in_us());
    TEST_ASSERT_TRUE(poll_held());
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, res.status);
}

static void release_link_bus(void *arg) {
    (void)arg;
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
}

/* The bus goes away while the flow control that starts the train is being
 * received, so the train is refused at admission: the message fails once and
 * nothing more goes out. */
TEST(a_train_the_bus_refuses_fails_the_message_once) {
    uint32_t ch = queue_segmented();
    /* An idle pass lets the first frame's completion be collected. */
    j2534_core_poll();
    stage_flow_control(0, 0);
    fake_can_on_receive(release_link_bus, NULL);
    j2534_core_poll();
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    const vif_bus_cfg_t cfg = {.bitrate = 500000, .manual_recovery = true};
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &cfg));
    TEST_ASSERT_EQUAL_INT(0, read_message(ch));
    TEST_ASSERT_EQUAL_INT(J2534_TX_FAILED, res.message.rx_status);
    TEST_ASSERT_EQUAL_INT(42, res.message.handle);
    TEST_ASSERT_EQUAL_INT(J2534_EMPTY, read_message(ch));
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
}

/* Last in the file: the impostor keeps the name "j2534" for the rest of the
 * run, so it must not run before a test that expects the registry as it was. */
TEST(a_front_end_name_already_taken_is_refused_and_not_replaced) {
    static const vif_frontend_t impostor = {.name = "j2534"};

    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_frontend_register(&impostor));
    j2534_register();
    TEST_ASSERT_TRUE(vif_frontend_find("j2534") == &impostor);
}
