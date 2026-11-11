/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file test_vif_ctrl_can_cov.c
 * @brief The control plane's MODE, BUS and RESET with CAN as the only bus.
 *
 * This is what both the shell's "mode" command and the BLE control
 * characteristic run, so the answers here are the ones a client actually sees.
 *
 * The front-ends are stand-ins rather than the real ELM327 and SLCAN: what is
 * under test is the command set and what it does to the link, not either
 * grammar. vif.c itself is real, so a switch really does go through the claim
 * rules - which means it lands on the link task's next pass, and this thread
 * stands in for that task.
 */

#include <string.h>

#include "td_test.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include "fake_board.h"
#include "fake_bus.h"
#include "fake_can_bus.h"
#include "fake_clock.h"
#include "fake_led.h"
#include "fake_port.h"

#include "can_frame.h"
#include "can_xfer.h"
#include "vif.h"
#include "vif_ctrl.h"

#define OBD_PIN_HS 6

static const vif_bus_cfg_t can500 = {.bitrate = 500000};

static const vif_frontend_t frontend_a = {.name = "elm327"};
static const vif_frontend_t frontend_b = {.name = "slcan"};

static char g_out[VIF_CTRL_REPLY_MAX];

/** Runs one control command and lets the link task apply what it asked for. */
static const char *ctrl(const char *cmd) {
    memset(g_out, 0, sizeof(g_out));
    vif_ctrl_exec(cmd, strlen(cmd), g_out, sizeof(g_out));
    vif_link_service();

    return g_out;
}

/** Runs one and reports whether it was understood. */
static bool ctrl_ok(const char *cmd) {
    bool ok;

    memset(g_out, 0, sizeof(g_out));
    ok = vif_ctrl_exec(cmd, strlen(cmd), g_out, sizeof(g_out));
    vif_link_service();

    return ok;
}

static void ports_init_once(void) {
    static bool done;

    if (done) {
        return;
    }
    done = true;

    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_iface_init());
    fake_port_register_all();
}

void td_setup(void) {
    /* A real port, because the control plane's answers go out on one. */
    ports_init_once();

    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_init());

    fake_clock_reset();
    fake_board_reset();
    fake_led_reset();
    fake_can_reset();
    fake_bus_reset_all();

    vif_frontend_register(&frontend_a);
    vif_frontend_register(&frontend_b);

    /* This thread is the link task, and the shell's, so a claim taken on
     * either owner's behalf reaches the driver. */
    idf_stub_set_created_task(xTaskGetCurrentTaskHandle());
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_link_start(&frontend_a));
    vif_shell_bind();

    vif_link_set_port(fake_port_id(0));
    vif_link_service();
}

void td_teardown(void) {
    TEST_ASSERT_MSG(idf_stub_lock_balance() == 0, "left %d locks held",
                    idf_stub_lock_balance());
}

/* ------------------------------------------------------------------ *
 * MODE, BUS and RESET over CAN alone
 *
 * test_vif_ctrl.c covers these with K-line, J1850 and pins, which the
 * RejsaCAN board cannot open. Here the same paths run with CAN only, so both
 * board variants reach them.
 * ------------------------------------------------------------------ */

TEST(switching_releases_the_can_claim) {
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));

    TEST_ASSERT_TRUE(ctrl_ok("MODE=slcan"));

    TEST_ASSERT_EQUAL_STRING("slcan", vif_link_frontend_name());
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(1, fake_can_teardown_count());
    TEST_ASSERT_FALSE(fake_can_is_up());
}

TEST(reset_releases_the_can_claim_and_restores_the_default) {
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    ctrl_ok("MODE=slcan");
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));

    TEST_ASSERT_EQUAL_STRING("RESET elm327", ctrl("RESET"));

    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_FALSE(fake_can_is_up());
    TEST_ASSERT_EQUAL_STRING("elm327", vif_link_frontend_name());
}

TEST(bus_reports_the_can_claim_and_its_holder) {
    TEST_ASSERT_EQUAL_STRING("BUS", ctrl("BUS"));

    vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500);
    TEST_ASSERT_EQUAL_STRING("BUS CAN@500000/link", ctrl("BUS"));
}

TEST(bus_names_the_shell_when_the_shell_holds_can) {
    vif_bus_open(VIF_OWNER_SHELL, VIF_BUS_CAN, &can500);

    TEST_ASSERT_EQUAL_STRING("BUS CAN@500000/shell", ctrl("BUS"));
}

TEST(bus_answer_is_cut_to_fit_a_small_buffer) {
    char small[8];

    vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500);
    memset(small, 'x', sizeof(small));
    vif_ctrl_exec("BUS", 3, small, sizeof(small));

    TEST_ASSERT_EQUAL_INT(0, small[sizeof(small) - 1]);
    TEST_ASSERT_EQUAL_INT(0, strncmp(small, "BUS CAN", 7));
}

TEST(a_command_with_no_answer_buffer_is_refused) {
    char buf[8];

    TEST_ASSERT_FALSE(vif_ctrl_exec("ID", 2, NULL, sizeof(buf)));
    TEST_ASSERT_FALSE(vif_ctrl_exec("ID", 2, buf, 0));
}

TEST(a_command_that_is_missing_blank_or_too_long_is_empty) {
    char too_long[VIF_CTRL_REPLY_MAX];

    memset(too_long, 'A', sizeof(too_long));

    memset(g_out, 0, sizeof(g_out));
    TEST_ASSERT_FALSE(vif_ctrl_exec(NULL, 2, g_out, sizeof(g_out)));
    TEST_ASSERT_EQUAL_STRING("ERR empty", g_out);

    TEST_ASSERT_FALSE(ctrl_ok("  \r\n"));
    TEST_ASSERT_EQUAL_STRING("ERR empty", g_out);

    /* Longer than any command word and its argument. */
    TEST_ASSERT_FALSE(vif_ctrl_exec(too_long, sizeof(too_long), g_out,
                                    sizeof(g_out)));
    TEST_ASSERT_EQUAL_STRING("ERR empty", g_out);
}

TEST(reset_reports_a_bus_that_will_not_close_and_keeps_the_grammar) {
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    ctrl_ok("MODE=slcan");
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    fake_can_teardown_result(ESP_ERR_TIMEOUT);

    TEST_ASSERT_FALSE(ctrl_ok("RESET"));

    TEST_ASSERT_EQUAL_INT(0, strncmp(g_out, "ERR bus close failed: ", 22));
    /* The claim is still held and the grammar was not switched back. */
    TEST_ASSERT_EQUAL_INT(VIF_BUS_CAN,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_STRING("slcan", vif_link_frontend_name());

    fake_can_teardown_result(ESP_OK);
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
}

/* ------------------------------------------------------------------ *
 * can_frame_send_confirmed (can_xfer.h)
 * ------------------------------------------------------------------ */

TEST(a_confirmed_send_goes_out_and_returns_zero) {
    struct can_frame f = {.id = 0x7E0, .dlc = 2, .data = {0x01, 0x00}};

    vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500);

    TEST_ASSERT_EQUAL_INT(0, can_frame_send_confirmed(&f));
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x7E0, (int)fake_can_sent(0)->id);
    TEST_ASSERT_EQUAL_INT(VIF_BUS_CAN,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
}

TEST(a_confirmed_send_without_a_frame_is_a_bad_argument) {
    TEST_ASSERT_EQUAL_INT(BUS_ERR_BAD_ARG, can_frame_send_confirmed(NULL));
}

TEST(a_failed_confirmed_send_keeps_the_claim) {
    struct can_frame f = {.id = 0x7E0, .dlc = 1, .data = {0x3E}};

    vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500);
    fake_can_fail_confirmed_send();

    TEST_ASSERT_EQUAL_INT(BUS_ERR_TX_FAILED, can_frame_send_confirmed(&f));
    TEST_ASSERT_EQUAL_INT(VIF_BUS_CAN,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
}

TEST(an_aborted_confirmed_send_closes_the_claim) {
    struct can_frame f = {.id = 0x7E0, .dlc = 1, .data = {0x3E}};

    vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500);
    fake_can_abort_confirmed_send();

    TEST_ASSERT_EQUAL_INT(BUS_ERR_TX_ABORTED, can_frame_send_confirmed(&f));
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_FALSE(fake_can_is_up());
}

TEST(leading_blanks_are_dropped_before_the_command_word) {
    TEST_ASSERT_EQUAL_STRING("BUS", ctrl("   BUS"));
}
