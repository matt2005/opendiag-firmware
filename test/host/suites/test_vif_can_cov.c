/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file test_vif_can_cov.c
 * @brief vif.c on a CAN only build: the paths the flag-only suite leaves out.
 *
 * Built only with -DREJSACAN=ON (OPENDIAG_BOARD=rejsacan). The stock suites
 * open K-line and J1850, which this build refuses, so the CAN halves of
 * test_vif.c, test_vif_cov.c and test_vif_link.c are repeated here: the
 * front-end table, the link service and its retries, claims and their
 * release, the transmit worker calls, the pin refusals and the debug dump.
 *
 * Both owners are bound to this one thread, as in the other vif suites. The
 * link's task is never started - the host stub records the handle and
 * returns - so vif_link_service() stands in for its loop.
 */

#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "td_test.h"

#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "fake_board.h"
#include "fake_bus.h"
#include "fake_bus_tx_worker.h"
#include "fake_can_bus.h"
#include "fake_clock.h"
#include "fake_led.h"
#include "fake_port.h"

#include "vif.h"

#define OBD_PIN_HS 6
#define OBD_PIN_HS_OTHER 9
#define OBD_PIN_LS 15

static const vif_bus_cfg_t can500 = {.bitrate = 500000};

/* ------------------------------------------------------------------ *
 * Front-ends that only record what they were asked to do
 * ------------------------------------------------------------------ */

static struct {
    int started;
    int stopped;
    int fed;
    bool refuse_start;
    /* Refuse every start after this many have succeeded; -1 for never. */
    int refuse_after;
} fe_a, fe_b;

static bool fe_a_start(void) {
    bool ok = !fe_a.refuse_start &&
              (fe_a.refuse_after < 0 || fe_a.started < fe_a.refuse_after);

    fe_a.started++;
    return ok;
}

static void fe_a_feed(const uint8_t *data, size_t len) {
    (void)data;
    fe_a.fed += (int)len;
}

static void fe_a_stop(void) { fe_a.stopped++; }

static bool fe_b_start(void) {
    fe_b.started++;
    return !fe_b.refuse_start;
}

static void fe_b_stop(void) { fe_b.stopped++; }

static const vif_frontend_t frontend_a = {
    .name = "fe_a",
    .start = fe_a_start,
    .feed = fe_a_feed,
    .stop = fe_a_stop,
};

static const vif_frontend_t frontend_b = {
    .name = "fe_b",
    .start = fe_b_start,
    .stop = fe_b_stop,
};

/* ------------------------------------------------------------------ *
 * Fixture
 * ------------------------------------------------------------------ */

static TaskHandle_t g_main_task;
static int other_task;

/** Install a grammar and run the pass of the link task that applies it. */
static void set_frontend(const vif_frontend_t *fe) {
    vif_link_set_frontend(fe);
    vif_link_service();
}

void td_setup(void) {
    static bool ports_registered;

    idf_stub_set_current_task(NULL);
    g_main_task = xTaskGetCurrentTaskHandle();

    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_iface_init());
    if (!ports_registered) {
        fake_port_register_all();
        ports_registered = true;
    }

    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_init());
    idf_stub_set_created_task(g_main_task);
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_link_start(&frontend_a));
    vif_shell_bind();

    fake_clock_reset();
    fake_board_reset();
    fake_led_reset();
    fake_can_reset();
    fake_bus_reset_all();
    fake_port_reset();
    fake_bus_tx_hold(false);
    fake_bus_tx_hold_completion(false);

    memset(&fe_a, 0, sizeof(fe_a));
    memset(&fe_b, 0, sizeof(fe_b));
    fe_a.refuse_after = -1;
}

void td_teardown(void) {
    fake_bus_tx_hold(false);
    fake_bus_tx_hold_completion(false);
    idf_stub_set_current_task(NULL);
    idf_stub_set_created_task(NULL);
    TEST_ASSERT_MSG(idf_stub_lock_balance() == 0, "vif left %d locks held",
                    idf_stub_lock_balance());
}

/* ------------------------------------------------------------------ *
 * The front-end table
 * ------------------------------------------------------------------ */

TEST(the_front_end_table_registers_finds_and_fills_up) {
    static char names[VIF_MAX_FRONTENDS + 1][8];
    static vif_frontend_t fes[VIF_MAX_FRONTENDS + 1];
    vif_frontend_t nameless = {.name = NULL};
    vif_frontend_t dup = {.name = "REG0"};

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, vif_frontend_register(NULL));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          vif_frontend_register(&nameless));

    for (size_t i = 0; i < VIF_MAX_FRONTENDS + 1; i++) {
        snprintf(names[i], sizeof(names[i]), "reg%u", (unsigned)i);
        fes[i].name = names[i];
    }

    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_frontend_register(&fes[0]));
    /* The same front-end again is not an error. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_frontend_register(&fes[0]));
    /* A different one under the same name, in any case, is. */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, vif_frontend_register(&dup));

    for (size_t i = 1; i < VIF_MAX_FRONTENDS; i++) {
        TEST_ASSERT_EQUAL_INT(ESP_OK, vif_frontend_register(&fes[i]));
    }
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NO_MEM,
                          vif_frontend_register(&fes[VIF_MAX_FRONTENDS]));

    TEST_ASSERT_TRUE(vif_frontend_find("reg2") == &fes[2]);
    TEST_ASSERT_TRUE(vif_frontend_find("REG3") == &fes[3]);
    TEST_ASSERT_NULL(vif_frontend_find("nope"));
    TEST_ASSERT_NULL(vif_frontend_find(NULL));

    TEST_ASSERT_TRUE(vif_frontend_at(0) == &fes[0]);
    TEST_ASSERT_TRUE(vif_frontend_at(VIF_MAX_FRONTENDS - 1) ==
                     &fes[VIF_MAX_FRONTENDS - 1]);
    TEST_ASSERT_NULL(vif_frontend_at(VIF_MAX_FRONTENDS));
}

/* ------------------------------------------------------------------ *
 * The link
 * ------------------------------------------------------------------ */

TEST(link_start_is_idempotent) {
    TEST_ASSERT_TRUE(vif_link_default_frontend() == &frontend_a);
    /* Already started: a second call changes nothing. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_link_start(&frontend_b));
    TEST_ASSERT_TRUE(vif_link_default_frontend() == &frontend_a);
}

TEST(a_frontend_switch_stops_the_old_grammar_and_starts_the_new) {
    set_frontend(&frontend_a);
    TEST_ASSERT_EQUAL_STRING("fe_a", vif_link_frontend_name());
    TEST_ASSERT_EQUAL_INT(1, fe_a.started);

    vif_link_set_frontend(&frontend_b);
    TEST_ASSERT_TRUE(vif_link_service());
    TEST_ASSERT_EQUAL_INT(1, fe_a.stopped);
    TEST_ASSERT_EQUAL_INT(1, fe_b.started);
    TEST_ASSERT_EQUAL_STRING("fe_b", vif_link_frontend_name());

    /* Nothing pending: the next pass has nothing to do. */
    TEST_ASSERT_FALSE(vif_link_service());
}

TEST(a_switch_installs_no_grammar_when_asked_for_none) {
    set_frontend(&frontend_a);
    set_frontend(NULL);
    TEST_ASSERT_EQUAL_INT(1, fe_a.stopped);
    TEST_ASSERT_NULL(vif_link_frontend_name());
}

TEST(a_switch_releases_the_claims_the_old_grammar_left_open) {
    set_frontend(&frontend_a);
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    TEST_ASSERT_TRUE(fake_can_is_up());

    set_frontend(&frontend_b);
    TEST_ASSERT_FALSE(fake_can_is_up());
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
}

TEST(a_refused_start_puts_the_previous_grammar_back) {
    set_frontend(&frontend_b);
    fe_a.refuse_start = false;
    set_frontend(&frontend_a);
    TEST_ASSERT_EQUAL_STRING("fe_a", vif_link_frontend_name());

    fe_b.refuse_start = true;
    fe_b.started = 0;
    fe_a.started = 0;
    set_frontend(&frontend_b);
    TEST_ASSERT_EQUAL_INT(1, fe_b.started);
    TEST_ASSERT_EQUAL_INT(1, fe_a.started);
    TEST_ASSERT_EQUAL_STRING("fe_a", vif_link_frontend_name());
}

TEST(a_refused_start_that_cannot_go_back_leaves_no_grammar) {
    set_frontend(&frontend_a);
    TEST_ASSERT_EQUAL_INT(1, fe_a.started);

    /* a ran once; its next start is refused, and so is b's. */
    fe_a.refuse_after = 1;
    fe_b.refuse_start = true;
    set_frontend(&frontend_b);
    TEST_ASSERT_EQUAL_INT(1, fe_b.started);
    TEST_ASSERT_EQUAL_INT(2, fe_a.started);
    TEST_ASSERT_NULL(vif_link_frontend_name());
}

TEST(the_link_service_does_nothing_off_the_link_thread) {
    vif_link_set_frontend(&frontend_b);

    idf_stub_set_current_task(&other_task);
    TEST_ASSERT_FALSE(vif_link_service());
    idf_stub_set_current_task(g_main_task);
    TEST_ASSERT_EQUAL_INT(0, fe_b.started);

    TEST_ASSERT_TRUE(vif_link_service());
    TEST_ASSERT_EQUAL_INT(1, fe_b.started);
}

TEST(the_link_port_attaches_detaches_and_carries_io) {
    comm_port_id_t port = fake_port_id(0);
    uint8_t in[4] = {0};

    TEST_ASSERT_TRUE(port != COMM_INVALID_PORT_ID);
    TEST_ASSERT_EQUAL_INT(COMM_INVALID_PORT_ID, vif_link_port());

    vif_link_set_port(port);
    TEST_ASSERT_EQUAL_INT(port, vif_link_port());

    vif_link_write("OK", 2);
    vif_link_flush();
    TEST_ASSERT_EQUAL_STRING("OK", fake_port_text(0));
    TEST_ASSERT_EQUAL_INT(1, fake_port_flush_count(0));

    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_port_rx(port, (const uint8_t *)"ab", 2));
    TEST_ASSERT_EQUAL_INT(2, (int)vif_link_read(in, sizeof(in), 0));
    TEST_ASSERT_EQUAL_INT('a', in[0]);
    TEST_ASSERT_EQUAL_INT('b', in[1]);

    vif_link_set_port(COMM_INVALID_PORT_ID);
    TEST_ASSERT_EQUAL_INT(COMM_INVALID_PORT_ID, vif_link_port());
}

TEST(link_down_reinstalls_the_default_and_drops_the_claims) {
    comm_port_id_t port = fake_port_id(0);

    vif_link_set_port(port);
    set_frontend(&frontend_b);
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    fe_a.started = 0;

    vif_link_down();
    TEST_ASSERT_TRUE(vif_link_service());
    TEST_ASSERT_EQUAL_INT(1, fe_b.stopped);
    TEST_ASSERT_EQUAL_INT(1, fe_a.started);
    TEST_ASSERT_EQUAL_STRING("fe_a", vif_link_frontend_name());
    TEST_ASSERT_FALSE(fake_can_is_up());
}

TEST(link_down_before_the_link_started_is_a_no_op) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_init());
    TEST_ASSERT_NULL(vif_link_default_frontend());
    vif_link_down();
    TEST_ASSERT_FALSE(vif_link_service());
}

/* ------------------------------------------------------------------ *
 * Claims
 * ------------------------------------------------------------------ */

TEST(bus_calls_reject_bad_owners_and_buses) {
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          vif_bus_open(VIF_OWNER_COUNT, VIF_BUS_CAN, &can500));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_NONE, &can500));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          vif_bus_close(VIF_OWNER_COUNT, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          vif_bus_close(VIF_OWNER_LINK, VIF_BUS_NONE));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          vif_bus_release_all(VIF_OWNER_COUNT));
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_COUNT, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_NONE));
    TEST_ASSERT_FALSE(vif_bus_is_open(VIF_OWNER_COUNT, VIF_BUS_CAN));
    TEST_ASSERT_FALSE(vif_bus_is_open(VIF_OWNER_LINK, VIF_BUS_NONE));
    TEST_ASSERT_EQUAL_STRING("none", vif_bus_name(VIF_BUS_NONE));
    TEST_ASSERT_EQUAL_STRING("?", vif_owner_name(VIF_OWNER_COUNT));
}

TEST(the_other_owner_cannot_take_or_close_a_held_bus) {
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
                          vif_bus_open(VIF_OWNER_SHELL, VIF_BUS_CAN, &can500));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
                          vif_bus_close(VIF_OWNER_SHELL, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(1, fake_can_setup_count());

    /* Closing a bus nobody holds is not an error. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_close(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_close(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_FALSE(vif_bus_is_open(VIF_OWNER_LINK, VIF_BUS_CAN));
}

TEST(reopening_a_held_bus_closes_it_first) {
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    TEST_ASSERT_EQUAL_INT(2, fake_can_setup_count());
    TEST_ASSERT_EQUAL_INT(1, fake_can_teardown_count());
    TEST_ASSERT_TRUE(vif_bus_is_open(VIF_OWNER_LINK, VIF_BUS_CAN));
}

TEST(a_failed_driver_start_leaves_no_claim_behind) {
    fake_can_fail_setup(true);
    TEST_ASSERT_TRUE(vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500) !=
                     ESP_OK);
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_FALSE(vif_bus_is_open(VIF_OWNER_LINK, VIF_BUS_CAN));

    fake_can_fail_setup(false);
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_SHELL, VIF_BUS_CAN, &can500));
}

TEST(release_all_gives_back_only_that_owners_claims) {
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));

    /* Nothing of the shell's to release. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_SHELL));
    TEST_ASSERT_TRUE(vif_bus_is_open(VIF_OWNER_LINK, VIF_BUS_CAN));

    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
    TEST_ASSERT_FALSE(fake_can_is_up());
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
}

TEST(vif_bus_info_reports_the_claim_and_a_null_buffer_is_ignored) {
    vif_bus_claim_t claims[VIF_BUS_GROUPS];

    vif_bus_info(NULL);

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_SHELL, VIF_BUS_CAN, &can500));
    vif_bus_info(claims);
    TEST_ASSERT_EQUAL_INT(VIF_BUS_CAN, claims[0].bus);
    TEST_ASSERT_EQUAL_INT(500000, (int)claims[0].bitrate);
    TEST_ASSERT_EQUAL_INT(VIF_OWNER_SHELL, claims[0].owner);
    for (unsigned i = 1; i < VIF_BUS_GROUPS; i++) {
        TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE, claims[i].bus);
    }
}

TEST(a_foreign_thread_may_not_touch_the_driver) {
    uint8_t data[1] = {0};
    bus_msg_t msg;
    uint32_t value = 0;

    bus_msg_tx(&msg, data, sizeof(data), 0x7DF);
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));

    idf_stub_set_current_task(&other_task);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
                          vif_bus_close(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(VIF_ERR_NO_CLAIM,
                          vif_bus_send(VIF_OWNER_LINK, VIF_BUS_CAN, &msg, 0));
    TEST_ASSERT_EQUAL_INT(
        VIF_ERR_NO_CLAIM,
        vif_bus_submit(VIF_OWNER_LINK, VIF_BUS_CAN, &msg, 0, 0));
    TEST_ASSERT_EQUAL_INT(VIF_ERR_NO_CLAIM,
                          vif_bus_tx_wait(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(
        VIF_ERR_NO_CLAIM,
        vif_bus_param_get(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_P_DATA_RATE,
                          &value));
    idf_stub_set_current_task(g_main_task);

    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(1, fake_can_setup_count());
}

/* ------------------------------------------------------------------ *
 * Recovery and release requests
 * ------------------------------------------------------------------ */

TEST(recover_all_releases_every_claim_and_restores_the_default) {
    set_frontend(&frontend_b);
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    fe_a.started = 0;

    vif_recover_all();
    TEST_ASSERT_TRUE(vif_link_service());
    TEST_ASSERT_FALSE(fake_can_is_up());

    /* The default goes back in the same pass as the recovery. */
    TEST_ASSERT_EQUAL_STRING("fe_a", vif_link_frontend_name());
    TEST_ASSERT_EQUAL_INT(1, fe_a.started);

    /* The shell's request is its own to act on. */
    TEST_ASSERT_FALSE(vif_link_service());
    TEST_ASSERT_TRUE(vif_shell_service());
    TEST_ASSERT_FALSE(vif_shell_service());
}

TEST(the_shell_service_releases_the_shells_bus_and_leaves_the_grammar) {
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_SHELL, VIF_BUS_CAN, &can500));
    set_frontend(&frontend_b);
    fe_a.started = 0;

    vif_recover_all();
    TEST_ASSERT_TRUE(vif_shell_service());
    TEST_ASSERT_FALSE(fake_can_is_up());
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_SHELL, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_STRING("fe_b", vif_link_frontend_name());
    TEST_ASSERT_EQUAL_INT(0, fe_a.started);
}

TEST(a_release_asked_for_from_another_thread_waits_for_the_owner) {
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));

    idf_stub_set_current_task(&other_task);
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
    TEST_ASSERT_FALSE(vif_shell_service());
    idf_stub_set_current_task(g_main_task);
    TEST_ASSERT_TRUE(fake_can_is_up());

    /* A release, not a recovery: the grammar stays as it was. */
    set_frontend(&frontend_b);
    fe_a.started = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    idf_stub_set_current_task(&other_task);
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
    idf_stub_set_current_task(g_main_task);
    TEST_ASSERT_TRUE(vif_link_service());
    TEST_ASSERT_FALSE(fake_can_is_up());
    TEST_ASSERT_EQUAL_INT(0, fe_a.started);
    TEST_ASSERT_EQUAL_STRING("fe_b", vif_link_frontend_name());
}

/* ------------------------------------------------------------------ *
 * The transmit worker and the calls that wait on it
 * ------------------------------------------------------------------ */

TEST(a_submitted_frame_goes_out_when_its_worker_is_waited_on) {
    const uint8_t payload[2] = {0xAA, 0xBB};
    bus_msg_t tx;

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    bus_msg_tx(&tx, payload, sizeof(payload), 0x7DF);

    TEST_ASSERT_EQUAL_INT(
        VIF_ERR_NO_CLAIM,
        vif_bus_submit(VIF_OWNER_SHELL, VIF_BUS_CAN, &tx, 0, 0));
    TEST_ASSERT_EQUAL_INT(
        VIF_ERR_NO_CLAIM,
        vif_bus_submit(VIF_OWNER_LINK, VIF_BUS_NONE, &tx, 0, 0));
    TEST_ASSERT_EQUAL_INT(VIF_ERR_NO_CLAIM,
                          vif_bus_tx_wait(VIF_OWNER_SHELL, VIF_BUS_CAN));

    TEST_ASSERT_EQUAL_INT(0, vif_bus_submit(VIF_OWNER_LINK, VIF_BUS_CAN, &tx, 0,
                                            0));
    TEST_ASSERT_EQUAL_INT(0, vif_bus_tx_wait(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x7DF, (int)fake_can_sent(0)->id);
}

TEST(tx_poll_reports_a_finished_submit_and_refuses_bad_callers) {
    const uint8_t payload[1] = {0x01};
    bus_tx_result_t result;
    bus_msg_t tx;

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    bus_msg_tx(&tx, payload, sizeof(payload), 0x123);
    TEST_ASSERT_EQUAL_INT(0, vif_bus_submit(VIF_OWNER_LINK, VIF_BUS_CAN, &tx, 0,
                                            0));

    TEST_ASSERT_FALSE(vif_bus_tx_poll(VIF_OWNER_LINK, VIF_BUS_CAN, NULL));
    TEST_ASSERT_FALSE(vif_bus_tx_poll(VIF_OWNER_SHELL, VIF_BUS_CAN, &result));

    TEST_ASSERT_TRUE(vif_bus_tx_poll(VIF_OWNER_LINK, VIF_BUS_CAN, &result));
    TEST_ASSERT_EQUAL_INT(0, result.status);
    TEST_ASSERT_TRUE(result.complete);
    TEST_ASSERT_EQUAL_INT(1, fake_can_sent_count());
}

TEST(a_transmit_in_flight_holds_up_parameter_and_ioctl_calls) {
    const uint8_t request[1] = {0x01};
    uint32_t value = 0;
    bus_msg_t tx;

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    bus_msg_tx(&tx, request, sizeof(request), 0x7DF);
    fake_bus_tx_hold(true);
    TEST_ASSERT_EQUAL_INT(0, vif_bus_submit(VIF_OWNER_LINK, VIF_BUS_CAN, &tx, 0,
                                            0));

    TEST_ASSERT_EQUAL_INT(BUS_ERR_TIMEOUT,
                          vif_bus_tx_wait(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(
        BUS_ERR_TIMEOUT,
        vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_P_DATA_RATE, 250000));
    TEST_ASSERT_EQUAL_INT(
        BUS_ERR_TIMEOUT,
        vif_bus_param_get(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_P_DATA_RATE,
                          &value));
    TEST_ASSERT_EQUAL_INT(
        BUS_ERR_TIMEOUT,
        vif_bus_ioctl(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_IOCTL_STOP_COMM, NULL,
                      NULL));
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());

    /* Cancelling from a thread without the claim does nothing; the owner's
     * cancel reaches the worker. */
    vif_bus_tx_cancel(VIF_OWNER_SHELL, VIF_BUS_CAN);
    vif_bus_tx_cancel(VIF_OWNER_LINK, VIF_BUS_CAN);
    fake_bus_tx_hold(false);
    TEST_ASSERT_EQUAL_INT(0, vif_bus_tx_wait(VIF_OWNER_LINK, VIF_BUS_CAN));
}

TEST(a_can_bus_without_a_parameter_or_ioctl_table_says_unsupported) {
    uint32_t value = 0;

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    TEST_ASSERT_EQUAL_INT(
        BUS_ERR_UNSUPPORTED,
        vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_P_DATA_RATE, 1));
    TEST_ASSERT_EQUAL_INT(
        BUS_ERR_UNSUPPORTED,
        vif_bus_param_get(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_P_DATA_RATE,
                          &value));
    TEST_ASSERT_EQUAL_INT(
        BUS_ERR_UNSUPPORTED,
        vif_bus_ioctl(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_IOCTL_STOP_COMM, NULL,
                      NULL));
    TEST_ASSERT_EQUAL_INT(
        VIF_ERR_NO_CLAIM,
        vif_bus_param_get(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_P_DATA_RATE, NULL));
}

TEST(send_and_recv_check_their_arguments_and_the_claim) {
    uint8_t buf[8];
    bus_msg_t msg;
    bus_msg_t empty = {0};

    bus_msg_init(&msg, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(VIF_ERR_NO_CLAIM,
                          vif_bus_recv(VIF_OWNER_LINK, VIF_BUS_CAN, &msg, 0));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));

    TEST_ASSERT_EQUAL_INT(VIF_ERR_NO_CLAIM,
                          vif_bus_recv(VIF_OWNER_LINK, VIF_BUS_CAN, NULL, 0));
    TEST_ASSERT_EQUAL_INT(VIF_ERR_NO_CLAIM,
                          vif_bus_recv(VIF_OWNER_LINK, VIF_BUS_CAN, &empty, 0));
    TEST_ASSERT_EQUAL_INT(VIF_ERR_NO_CLAIM,
                          vif_bus_send(VIF_OWNER_LINK, VIF_BUS_CAN, NULL, 0));

    msg.cap = 0;
    TEST_ASSERT_EQUAL_INT(BUS_ERR_BAD_ARG,
                          vif_bus_recv(VIF_OWNER_LINK, VIF_BUS_CAN, &msg, 0));
}

TEST(stats_on_can_reach_the_driver_and_other_buses_are_refused) {
    bus_stats_t stats;

    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED,
                          vif_bus_stats(VIF_BUS_CAN, &stats));
    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED,
                          vif_bus_reset_stats(VIF_BUS_CAN));
    fake_can_enable_stats(true);
    TEST_ASSERT_EQUAL_INT(0, vif_bus_stats(VIF_BUS_CAN, &stats));
    TEST_ASSERT_EQUAL_INT(1, fake_can_stats_calls());
    TEST_ASSERT_EQUAL_INT(BUS_ERR_BAD_ARG, vif_bus_stats(VIF_BUS_CAN, NULL));
    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED,
                          vif_bus_stats(VIF_BUS_J1850_VPW, &stats));
    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED,
                          vif_bus_stats(VIF_BUS_NONE, &stats));

    TEST_ASSERT_EQUAL_INT(0, vif_bus_reset_stats(VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(1, fake_can_reset_stats_calls());
    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED,
                          vif_bus_reset_stats(VIF_BUS_J1850_PWM));
    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED,
                          vif_bus_reset_stats(VIF_BUS_NONE));
}

TEST(parameters_reach_the_can_driver_once_it_has_them) {
    uint32_t value = 0;

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    fake_can_enable_params(true);
    TEST_ASSERT_EQUAL_INT(
        0, vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_P_DATA_RATE,
                             250000));
    TEST_ASSERT_EQUAL_INT(
        0, vif_bus_param_get(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_P_DATA_RATE,
                             &value));
    TEST_ASSERT_EQUAL_INT(250000, (int)value);
}

/* ------------------------------------------------------------------ *
 * Pins
 * ------------------------------------------------------------------ */

TEST(pin_set_refuses_every_bad_request_before_the_board_is_touched) {
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_ARG,
        vif_pin_set(VIF_OWNER_COUNT, OBD_PIN_HS, VIF_PIN_OFF, 0));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          vif_pin_set(VIF_OWNER_LINK, 1, VIF_PIN_OFF, 0));
    /* Voltage only goes on a high side pin, ground only on a low side one. */
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_ARG,
        vif_pin_set(VIF_OWNER_LINK, OBD_PIN_LS, VIF_PIN_VOLTAGE, 5000));
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_ARG,
        vif_pin_set(VIF_OWNER_LINK, OBD_PIN_HS, VIF_PIN_GROUND, 0));
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_ARG,
        vif_pin_set(VIF_OWNER_LINK, OBD_PIN_HS_OTHER, VIF_PIN_GROUND, 0));

    TEST_ASSERT_EQUAL_INT(
        ESP_OK, vif_pin_set(VIF_OWNER_LINK, OBD_PIN_LS, VIF_PIN_OFF, 0));
    TEST_ASSERT_EQUAL_INT(
        ESP_OK, vif_pin_set(VIF_OWNER_SHELL, OBD_PIN_HS_OTHER, VIF_PIN_OFF, 0));
    TEST_ASSERT_FALSE(vif_any_pin_active());
    TEST_ASSERT_EQUAL_INT(-1, fake_board_hs_state(OBD_PIN_HS));
    TEST_ASSERT_EQUAL_INT(-1, fake_board_ls_state(OBD_PIN_LS));
}

TEST(releasing_pins_is_harmless_with_none_held) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_pin_release_all(VIF_OWNER_LINK));
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_pin_release_all(VIF_OWNER_SHELL));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          vif_pin_release_all(VIF_OWNER_COUNT));
    TEST_ASSERT_FALSE(vif_any_pin_active());
    TEST_ASSERT_EQUAL_INT(LED_STATE_IDLE, fake_led_state());
}

TEST(battery_and_calibration_calls_follow_the_board) {
    fake_board_set_vbatt_mv(12345);
    /* RejsaCAN has no confirmed battery sense input: no reading, as board.c. */
    TEST_ASSERT_EQUAL_INT(-1, vif_vbatt_mv());
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_SUPPORTED, vif_calibrate_vbatt());
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, vif_calibration_set(NULL, 1));
}

/* ------------------------------------------------------------------ *
 * Diagnostics
 * ------------------------------------------------------------------ */

TEST(the_debug_dump_runs_with_and_without_a_link_or_claims) {
    /* Nothing attached, no grammar yet. */
    vif_print_debug_info();

    vif_link_set_port(fake_port_id(0));
    set_frontend(&frontend_b);
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
                          vif_bus_open(VIF_OWNER_SHELL, VIF_BUS_CAN, &can500));
    vif_print_debug_info();

    /* Nothing held and the transport detached again. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
    vif_link_set_port(COMM_INVALID_PORT_ID);
    vif_print_debug_info();
}

/* ------------------------------------------------------------------ *
 * What a CAN only build refuses
 * ------------------------------------------------------------------ */

TEST(other_buses_are_refused_by_name_and_never_reach_a_driver) {
    uint8_t data[1] = {0};
    bus_msg_t msg;
    uint32_t value = 0;
    bus_tx_result_t result;
    const vif_bus_t others[] = {VIF_BUS_KLINE, VIF_BUS_J1850_PWM,
                                VIF_BUS_J1850_VPW};

    bus_msg_tx(&msg, data, sizeof(data), 0);
    TEST_ASSERT_EQUAL_STRING("CAN", vif_bus_name(VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_STRING("K-Line", vif_bus_name(VIF_BUS_KLINE));
    TEST_ASSERT_EQUAL_STRING("J1850 PWM", vif_bus_name(VIF_BUS_J1850_PWM));
    TEST_ASSERT_EQUAL_STRING("J1850 VPW", vif_bus_name(VIF_BUS_J1850_VPW));
    TEST_ASSERT_EQUAL_STRING("link", vif_owner_name(VIF_OWNER_LINK));
    TEST_ASSERT_EQUAL_STRING("shell", vif_owner_name(VIF_OWNER_SHELL));

    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        vif_bus_t bus = others[i];

        TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_SUPPORTED,
                              vif_bus_open(VIF_OWNER_LINK, bus, NULL));
        TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_close(VIF_OWNER_LINK, bus));
        TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                              vif_bus_current(VIF_OWNER_LINK, bus));
        TEST_ASSERT_FALSE(vif_bus_is_open(VIF_OWNER_LINK, bus));
        TEST_ASSERT_EQUAL_INT(VIF_ERR_NO_CLAIM,
                              vif_bus_send(VIF_OWNER_LINK, bus, &msg, 0));
        TEST_ASSERT_EQUAL_INT(VIF_ERR_NO_CLAIM,
                              vif_bus_recv(VIF_OWNER_LINK, bus, &msg, 0));
        TEST_ASSERT_EQUAL_INT(
            VIF_ERR_NO_CLAIM,
            vif_bus_submit(VIF_OWNER_LINK, bus, &msg, 0, 0));
        TEST_ASSERT_FALSE(vif_bus_tx_poll(VIF_OWNER_LINK, bus, &result));
        TEST_ASSERT_EQUAL_INT(VIF_ERR_NO_CLAIM,
                              vif_bus_tx_wait(VIF_OWNER_LINK, bus));
        vif_bus_tx_cancel(VIF_OWNER_LINK, bus);
        TEST_ASSERT_EQUAL_INT(
            VIF_ERR_NO_CLAIM,
            vif_bus_param_set(VIF_OWNER_LINK, bus, BUS_P_DATA_RATE, 1));
        TEST_ASSERT_EQUAL_INT(
            VIF_ERR_NO_CLAIM,
            vif_bus_param_get(VIF_OWNER_LINK, bus, BUS_P_DATA_RATE, &value));
        TEST_ASSERT_EQUAL_INT(
            VIF_ERR_NO_CLAIM,
            vif_bus_ioctl(VIF_OWNER_LINK, bus, BUS_IOCTL_STOP_COMM, NULL,
                          NULL));
    }
    TEST_ASSERT_EQUAL_INT(VIF_ERR_NO_CLAIM,
                          vif_bus_send(VIF_OWNER_LINK, VIF_BUS_NONE, &msg, 0));
    TEST_ASSERT_EQUAL_INT(
        VIF_ERR_NO_CLAIM,
        vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_NONE, BUS_P_DATA_RATE, 1));
    TEST_ASSERT_EQUAL_INT(0, fake_bus_setup_count(FAKE_BUS_KLINE));
    TEST_ASSERT_EQUAL_INT(0, fake_bus_setup_count(FAKE_BUS_J1850_PWM));
}

TEST(calibration_entry_points_are_refused_or_checked_and_the_board_is_read) {
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          vif_calibrate_hs(VIF_OWNER_COUNT));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_SUPPORTED,
                          vif_calibrate_hs(VIF_OWNER_LINK));
    (void)vif_hs_vsense_mv();
    TEST_ASSERT_FALSE(vif_any_pin_active());
    /* No pin is energised, so the call goes through to the board. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_calibration_set("vbatt_scale", 1));
}

TEST(a_staged_frame_is_received_by_the_owner_of_the_claim) {
    const uint8_t payload[2] = {0x12, 0x34};
    uint8_t buf[8];
    bus_msg_t msg;

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    fake_can_stage_stale(0x7E8, sizeof(payload), payload);
    bus_msg_init(&msg, buf, sizeof(buf));

    TEST_ASSERT_TRUE(vif_bus_recv(VIF_OWNER_LINK, VIF_BUS_CAN, &msg, 0) > 0);
    TEST_ASSERT_EQUAL_INT(0x7E8, (int)msg.id);
    TEST_ASSERT_EQUAL_INT(0x12, buf[0]);
    TEST_ASSERT_EQUAL_INT(0x34, buf[1]);
}

/* ------------------------------------------------------------------ *
 * A driver that will not close
 * ------------------------------------------------------------------ */

TEST(a_bus_that_will_not_close_keeps_its_claim_and_reports_why) {
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    fake_can_teardown_result(ESP_ERR_TIMEOUT);

    /* Reopening has to close first, and that failed. */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    TEST_ASSERT_EQUAL_INT(1, fake_can_setup_count());
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT,
                          vif_bus_release_all(VIF_OWNER_LINK));
    TEST_ASSERT_EQUAL_INT(VIF_BUS_CAN,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));

    fake_can_teardown_result(ESP_OK);
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
}

TEST(a_switch_waits_for_a_close_that_failed_and_then_lands) {
    set_frontend(&frontend_a);
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    fake_can_teardown_result(ESP_ERR_TIMEOUT);

    set_frontend(&frontend_b);
    TEST_ASSERT_EQUAL_INT(1, fe_a.stopped);
    TEST_ASSERT_EQUAL_INT(0, fe_b.started);
    TEST_ASSERT_NULL(vif_link_frontend_name());

    /* The request survives, so the next pass finishes it. */
    fake_can_teardown_result(ESP_OK);
    TEST_ASSERT_TRUE(vif_link_service());
    TEST_ASSERT_EQUAL_INT(1, fe_b.started);
    TEST_ASSERT_EQUAL_STRING("fe_b", vif_link_frontend_name());
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
}

TEST(a_recovery_that_cannot_close_the_bus_is_retried) {
    set_frontend(&frontend_b);
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    fake_can_teardown_result(ESP_ERR_TIMEOUT);
    fe_a.started = 0;

    vif_recover_all();
    TEST_ASSERT_TRUE(vif_link_service());
    TEST_ASSERT_EQUAL_INT(VIF_BUS_CAN,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(0, fe_a.started);

    fake_can_teardown_result(ESP_OK);
    TEST_ASSERT_TRUE(vif_link_service());
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_STRING("fe_a", vif_link_frontend_name());
}

TEST(the_link_task_that_cannot_be_created_is_reported) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_init());
    idf_stub_fail_task_create(true);
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, vif_link_start(&frontend_a));
    idf_stub_fail_task_create(false);
    /* The default stands, so the shell can still ask what the link falls back
     * to. */
    TEST_ASSERT_TRUE(vif_link_default_frontend() == &frontend_a);
}

/* ------------------------------------------------------------------ *
 * The link task itself, run for a few passes
 *
 * The captured task body loops forever; a front-end callback jumps out of it
 * once the pass under test is done.
 * ------------------------------------------------------------------ */

static jmp_buf loop_done;
static int task_polls, task_fed, task_retry_polls;

static void exit_poll(void) {
    task_polls++;
    longjmp(loop_done, 1);
}

static void exit_feed(const uint8_t *data, size_t len) {
    (void)data;
    task_fed += (int)len;
    longjmp(loop_done, 1);
}

static void counting_feed(const uint8_t *data, size_t len) {
    (void)data;
    task_fed += (int)len;
}

static const vif_frontend_t fe_polling = {
    .name = "polling",
    .poll = exit_poll,
};

static const vif_frontend_t fe_polling_feed = {
    .name = "polling_feed",
    .feed = counting_feed,
    .poll = exit_poll,
};

static void switching_feed(const uint8_t *data, size_t len) {
    (void)data;
    task_fed += (int)len;
    vif_link_set_frontend(&fe_polling);
}

static void must_not_poll(void) { task_retry_polls++; }

static const vif_frontend_t fe_switching = {
    .name = "switching",
    .feed = switching_feed,
    .poll = must_not_poll,
    .poll_interval_ms = 10,
};

static const vif_frontend_t fe_no_poll = {
    .name = "no_poll",
    .feed = exit_feed,
};

static void run_link_task(void) {
    TaskFunction_t fn = idf_stub_created_task_fn();

    TEST_ASSERT_NOT_NULL(fn);
    task_polls = task_fed = task_retry_polls = 0;
    if (setjmp(loop_done) == 0) {
        fn(idf_stub_created_task_param());
        TEST_ASSERT_MSG(0, "the link task returned");
    }
}

static void attach_port_with_input(void) {
    comm_port_id_t port = fake_port_id(0);

    vif_link_set_port(port);
    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_port_rx(port, (const uint8_t *)"xy", 2));
}

TEST(the_link_task_idles_without_a_port_then_feeds_and_polls) {
    comm_port_client_gone(fake_port_id(0));
    vif_link_set_frontend(&fe_polling_feed);

    /* No transport yet: the task waits, and one arrives during the wait. */
    idf_stub_on_task_delay(attach_port_with_input);
    run_link_task();

    TEST_ASSERT_EQUAL_INT(2, task_fed);
    TEST_ASSERT_EQUAL_INT(1, task_polls);
    TEST_ASSERT_EQUAL_STRING("polling_feed", vif_link_frontend_name());
}

TEST(the_link_task_lets_a_requested_switch_land_before_polling) {
    comm_port_client_gone(fake_port_id(0));
    attach_port_with_input();
    vif_link_set_frontend(&fe_switching);

    run_link_task();

    /* feed() asked for another grammar, so the old one was not polled. */
    TEST_ASSERT_EQUAL_INT(2, task_fed);
    TEST_ASSERT_EQUAL_INT(0, task_retry_polls);
    TEST_ASSERT_EQUAL_INT(1, task_polls);
    TEST_ASSERT_EQUAL_STRING("polling", vif_link_frontend_name());
}

TEST(the_link_task_serves_a_grammar_that_only_feeds) {
    comm_port_client_gone(fake_port_id(0));
    attach_port_with_input();
    vif_link_set_frontend(&fe_no_poll);

    run_link_task();

    TEST_ASSERT_EQUAL_INT(2, task_fed);
    TEST_ASSERT_EQUAL_INT(0, task_polls);
}

TEST(the_link_will_not_start_without_a_default_to_fall_back_to) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_init());
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, vif_link_start(NULL));
    TEST_ASSERT_NULL(vif_link_default_frontend());
}

/* ------------------------------------------------------------------ *
 * Frame trains
 * ------------------------------------------------------------------ */

extern bus_ops_t can_bus_ops;

TEST(a_frame_train_goes_out_in_order_and_reports_each_frame) {
    const uint8_t first[1] = {0x01};
    const uint8_t second[2] = {0x02, 0x03};
    bus_msg_t msgs[2];
    bus_tx_result_t result;

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    bus_msg_tx(&msgs[0], first, sizeof(first), 0x100);
    bus_msg_tx(&msgs[1], second, sizeof(second), 0x200);

    /* Only the owner of the claim may submit, and only on a real bus. */
    TEST_ASSERT_EQUAL_INT(
        VIF_ERR_NO_CLAIM,
        vif_bus_submit_train(VIF_OWNER_SHELL, VIF_BUS_CAN, msgs, 2, 0, 0));
    TEST_ASSERT_EQUAL_INT(
        VIF_ERR_NO_CLAIM,
        vif_bus_submit_train(VIF_OWNER_LINK, VIF_BUS_NONE, msgs, 2, 0, 0));

    TEST_ASSERT_EQUAL_INT(
        0, vif_bus_submit_train(VIF_OWNER_LINK, VIF_BUS_CAN, msgs, 2, 0, 0));
    TEST_ASSERT_EQUAL_INT(0, vif_bus_tx_wait(VIF_OWNER_LINK, VIF_BUS_CAN));

    TEST_ASSERT_EQUAL_INT(2, fake_can_sent_count());
    TEST_ASSERT_EQUAL_INT(0x100, (int)fake_can_sent(0)->id);
    TEST_ASSERT_EQUAL_INT(0x200, (int)fake_can_sent(1)->id);
    TEST_ASSERT_TRUE(vif_bus_tx_poll(VIF_OWNER_LINK, VIF_BUS_CAN, &result));
    TEST_ASSERT_TRUE(result.complete);
    TEST_ASSERT_NOT_NULL(result.train);
    TEST_ASSERT_EQUAL_INT(2, (int)result.train->sent);
}

TEST(a_train_is_refused_by_a_driver_without_train_support) {
    const uint8_t data[1] = {0x01};
    bus_msg_t msg;
    bus_ops_t saved = can_bus_ops;
    int rc;

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
    bus_msg_tx(&msg, data, sizeof(data), 0x100);

    can_bus_ops.send_train = NULL;
    rc = vif_bus_submit_train(VIF_OWNER_LINK, VIF_BUS_CAN, &msg, 1, 0, 0);
    can_bus_ops = saved;

    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL_INT(0, fake_can_sent_count());
}

TEST(a_driver_without_tables_reports_unsupported_from_vif_itself) {
    uint32_t value = 0;
    bus_ops_t saved = can_bus_ops;
    int set_rc, get_rc, ioctl_rc;

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));

    can_bus_ops.set_param = NULL;
    can_bus_ops.get_param = NULL;
    can_bus_ops.ioctl = NULL;
    set_rc = vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_P_DATA_RATE, 1);
    get_rc = vif_bus_param_get(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_P_DATA_RATE,
                               &value);
    ioctl_rc = vif_bus_ioctl(VIF_OWNER_LINK, VIF_BUS_CAN, BUS_IOCTL_STOP_COMM,
                             NULL, NULL);
    can_bus_ops = saved;

    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED, set_rc);
    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED, get_rc);
    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED, ioctl_rc);
}

/* ------------------------------------------------------------------ *
 * The link task's sleep: front-end deadlines and the backstop
 * ------------------------------------------------------------------ */

static uint32_t wake_due_us;
static uint32_t work_done;
static bool poll_works;
static int deadline_polls;
static int counted_polls;

static bool wake_at_due(uint32_t *due_us) {
    *due_us = wake_due_us;
    return true;
}

static void deadline_poll(void) {
    deadline_polls++;
    longjmp(loop_done, 1);
}

static bool nothing_due(uint32_t *due_us) {
    (void)due_us;
    return false;
}

static uint32_t count_progress(void) { return work_done; }

/* Does its pass, then ends the loop on the next one. */
static void counted_poll(void) {
    if (counted_polls++)
        longjmp(loop_done, 1);
    if (poll_works)
        work_done++;
}

static const vif_frontend_t fe_timed = {
    .name = "timed",
    .poll = deadline_poll,
    .next_wake = wake_at_due,
};

static const vif_frontend_t fe_counted = {
    .name = "counted",
    .poll = counted_poll,
    .next_wake = nothing_due,
    .progress = count_progress,
};

/* Attach an empty port, install @p fe and run the task until its poll exits. */
static void run_link_task_on(const vif_frontend_t *fe) {
    comm_port_client_gone(fake_port_id(0));
    vif_link_set_port(fake_port_id(0));
    set_frontend(fe);
    idf_stub_notify_reset();
    deadline_polls = counted_polls = 0;
    run_link_task();
}

/* What vif_print_debug_info() writes, captured through a temp file. */
static const char *printed_debug(void) {
    static char text[1024];
    FILE *capture = tmpfile();
    int saved;
    size_t length;

    TEST_ASSERT_NOT_NULL(capture);
    fflush(stdout);
    saved = dup(STDOUT_FILENO);
    TEST_ASSERT_TRUE(saved >= 0);
    TEST_ASSERT_TRUE(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    vif_print_debug_info();
    fflush(stdout);
    TEST_ASSERT_TRUE(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    rewind(capture);
    length = fread(text, 1, sizeof(text) - 1, capture);
    text[length] = '\0';
    fclose(capture);
    return text;
}

TEST(a_frontend_deadline_ends_the_wait_between_ticks) {
    wake_due_us = (uint32_t)esp_timer_get_time() + 300;
    run_link_task_on(&fe_timed);

    /* The one-shot timer cut the sleep short: no whole tick went by. */
    TEST_ASSERT_EQUAL_INT(1, deadline_polls);
    TEST_ASSERT_EQUAL_INT(0, (int)fake_clock_ms());
    TEST_ASSERT_TRUE((int32_t)((uint32_t)esp_timer_get_time() - wake_due_us) >=
                     0);
}

TEST(a_deadline_already_due_skips_the_wait) {
    wake_due_us = (uint32_t)esp_timer_get_time();
    run_link_task_on(&fe_timed);

    TEST_ASSERT_EQUAL_INT(1, deadline_polls);
    TEST_ASSERT_EQUAL_INT(0, (int)fake_clock_ms());
}

TEST(a_deadline_inside_the_backstop_is_met_on_time) {
    wake_due_us = (uint32_t)esp_timer_get_time() + 10000;
    run_link_task_on(&fe_timed);

    TEST_ASSERT_EQUAL_INT(1, deadline_polls);
    TEST_ASSERT_EQUAL_INT(10, (int)fake_clock_ms());
}

TEST(a_deadline_beyond_the_backstop_waits_only_the_backstop) {
    wake_due_us = (uint32_t)esp_timer_get_time() + 500000;
    run_link_task_on(&fe_timed);

    TEST_ASSERT_EQUAL_INT(1, deadline_polls);
    TEST_ASSERT_EQUAL_INT(100, (int)fake_clock_ms());
}

TEST(work_found_by_a_backstop_poll_is_counted) {
    work_done = 0;
    poll_works = true;
    run_link_task_on(&fe_counted);

    TEST_ASSERT_EQUAL_INT(2, counted_polls);
    TEST_ASSERT_NOT_NULL(
        strstr(printed_debug(), "backstop polls 1, found work 1\n"));
}

TEST(a_backstop_poll_that_finds_nothing_is_counted_without_work) {
    work_done = 0;
    poll_works = false;
    run_link_task_on(&fe_counted);

    TEST_ASSERT_EQUAL_INT(200, (int)fake_clock_ms());
    TEST_ASSERT_NOT_NULL(
        strstr(printed_debug(), "backstop polls 1, found work 0\n"));
}

TEST(a_poll_after_an_event_is_not_the_backstops) {
    work_done = 0;
    poll_works = true;
    comm_port_client_gone(fake_port_id(0));
    vif_link_set_port(fake_port_id(0));
    set_frontend(&fe_counted);
    counted_polls = 0;
    xTaskNotifyGive(g_main_task);
    run_link_task();

    TEST_ASSERT_NOT_NULL(
        strstr(printed_debug(), "backstop polls 0, found work 0\n"));
}

TEST(the_debug_dump_marks_a_listen_only_claim) {
    const vif_bus_cfg_t listen = {.bitrate = 500000, .listen_only = true};

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &listen));
    TEST_ASSERT_NOT_NULL(
        strstr(printed_debug(), "CAN @ 500000 listen-only, held by link\n"));
}
