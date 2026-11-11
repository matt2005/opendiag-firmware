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
