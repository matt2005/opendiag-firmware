/* SPDX-License-Identifier: GPL-3.0-only */
#include <setjmp.h>
#include <string.h>

/* The host board has no pin 9 low side driver. Enabling it for this unit lets
 * the pin tests reach the cross-checks between the two driver banks. */
#define OPENDIAG_HW_LS_OBD_9 1

#include "fake_bus.h"
#include "fake_can_bus.h"
#include "fake_clock.h"
#include "fake_port.h"
#include "td_test.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bus_tx.h"

/*
 * Failure injection. Each wrapper calls the real stand-in unless a test arms
 * it, so the existing tests are unaffected.
 */
static bool fail_task_create;
static bool fail_timer_create;
static bool fail_mutex_create;
static bool fail_worker_create;
static bool fail_worker_close;
static bool leave_on_take; /* longjmp out of the 2nd idle wait */
static int take_calls;
static jmp_buf take_escape;

static BaseType_t test_xTaskCreate(TaskFunction_t fn, const char *name,
                                   uint32_t stack, void *param,
                                   UBaseType_t prio, TaskHandle_t *created) {
    if (fail_task_create) {
        return pdFAIL;
    }
    return xTaskCreate(fn, name, stack, param, prio, created);
}

static esp_err_t test_esp_timer_create(const esp_timer_create_args_t *args,
                                       esp_timer_handle_t *out) {
    if (fail_timer_create) {
        return ESP_ERR_NO_MEM;
    }
    return esp_timer_create(args, out);
}

static SemaphoreHandle_t test_xSemaphoreCreateMutex(void) {
    return fail_mutex_create ? NULL : xSemaphoreCreateMutex();
}

static bus_tx_worker_t *test_bus_tx_worker_create(const bus_ops_t *ops) {
    return fail_worker_create ? NULL : bus_tx_worker_create(ops);
}

static esp_err_t test_bus_tx_worker_close(bus_tx_worker_t *worker) {
    return fail_worker_close ? ESP_FAIL : bus_tx_worker_close(worker);
}

static uint32_t test_ulTaskNotifyTake(BaseType_t clear, TickType_t wait) {
    if (leave_on_take && ++take_calls >= 2) {
        longjmp(take_escape, 1);
    }
    return ulTaskNotifyTake(clear, wait);
}

#define xTaskCreate test_xTaskCreate
#define esp_timer_create test_esp_timer_create
#define xSemaphoreCreateMutex test_xSemaphoreCreateMutex
#define bus_tx_worker_create test_bus_tx_worker_create
#define bus_tx_worker_close test_bus_tx_worker_close
#define ulTaskNotifyTake test_ulTaskNotifyTake

/* Run the actual loop; poll() returns control to the test via longjmp. */
#include "vif.c"

#undef xTaskCreate
#undef esp_timer_create
#undef xSemaphoreCreateMutex
#undef bus_tx_worker_create
#undef bus_tx_worker_close
#undef ulTaskNotifyTake

static jmp_buf loop_done;
static comm_port_id_t port;
static void (*request_during_feed)(void);
static int old_chunks, new_bytes, old_stops, new_starts, old_polls;
static bool second_chunk_bus_open;
static int other_task;

static bool old_start(void) {
    const vif_bus_cfg_t cfg = {.bitrate = 500000};
    return vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &cfg) == ESP_OK;
}

static void old_feed(const uint8_t *data, size_t len) {
    old_chunks++;
    if (old_chunks == 1) {
        TaskHandle_t owner = xTaskGetCurrentTaskHandle();
        idf_stub_set_current_task(&other_task);
        request_during_feed();
        idf_stub_set_current_task(owner);
    } else {
        second_chunk_bus_open = vif_bus_is_open(VIF_OWNER_LINK, VIF_BUS_CAN);
    }
}

static void old_stop(void) { old_stops++; }

static void old_poll(void) {
    old_polls++;
    longjmp(loop_done, 1);
}

static bool new_start(void) {
    new_starts++;
    return true;
}

static void new_feed(const uint8_t *data, size_t len) { new_bytes += (int)len; }

static void new_poll(void) { longjmp(loop_done, 1); }

static const vif_frontend_t old_frontend = {
    .name = "old",
    .start = old_start,
    .feed = old_feed,
    .poll = old_poll,
    .stop = old_stop,
};

static const vif_frontend_t new_frontend = {
    .name = "new",
    .start = new_start,
    .feed = new_feed,
    .poll = new_poll,
};

void td_setup(void) {
    static bool ports_registered;

    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_iface_init());
    if (!ports_registered) {
        fake_port_register_all();
        ports_registered = true;
    }
    port = fake_port_id(0);
    TEST_ASSERT_TRUE(port != COMM_INVALID_PORT_ID);
    comm_port_client_gone(port);

    idf_stub_set_current_task(NULL);
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_init());
    fake_bus_reset_all();
    fake_can_reset();
    fake_clock_reset();
    fake_port_reset();
    old_chunks = new_bytes = old_stops = new_starts = old_polls = 0;
    second_chunk_bus_open = false;

    idf_stub_set_created_task(xTaskGetCurrentTaskHandle());
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_link_start(&new_frontend));
    vif_link_set_frontend(&old_frontend);
    vif_link_set_port(port);
    /* Those requests woke the task; each test starts from an idle link. */
    idf_stub_notify_reset();
}

void td_teardown(void) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
    TEST_ASSERT_EQUAL_INT(0, idf_stub_lock_balance());
    idf_stub_set_current_task(NULL);
    idf_stub_set_created_task(NULL);
}

static void run_with_request(void (*request)(void)) {
    uint8_t input[65];
    memset(input, 'x', sizeof(input));
    request_during_feed = request;
    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_port_rx(port, input, sizeof(input)));
    if (setjmp(loop_done) == 0) {
        link_task(NULL);
    }
}

static void switch_frontend(void) { vif_link_set_frontend(&new_frontend); }

static void reconnect(void) {
    vif_link_down();
    vif_link_set_port(COMM_INVALID_PORT_ID);
    vif_link_set_port(port);
    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_port_rx(port, (const uint8_t *)"N", 1));
}

static void release_bus(void) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_LINK));
}

static void check_switched(void) {
    TEST_ASSERT_EQUAL_INT(1, old_chunks);
    TEST_ASSERT_EQUAL_INT(1, old_stops);
    TEST_ASSERT_EQUAL_INT(1, new_starts);
    TEST_ASSERT_EQUAL_INT(1, new_bytes);
    TEST_ASSERT_EQUAL_INT(0, old_polls);
    TEST_ASSERT_FALSE(vif_bus_is_open(VIF_OWNER_LINK, VIF_BUS_CAN));
}

TEST(a_switch_during_feed_applies_before_the_next_chunk_or_poll) {
    run_with_request(switch_frontend);
    check_switched();
}

TEST(a_recovery_during_feed_applies_before_the_next_chunk_or_poll) {
    run_with_request(vif_recover_all);
    check_switched();
}

TEST(a_reconnected_clients_input_reaches_the_default_frontend) {
    run_with_request(reconnect);
    check_switched();
}

TEST(a_deferred_release_finishes_before_the_next_chunk) {
    run_with_request(release_bus);
    TEST_ASSERT_EQUAL_INT(2, old_chunks);
    TEST_ASSERT_FALSE(second_chunk_bus_open);
    TEST_ASSERT_EQUAL_INT(0, old_stops);
    TEST_ASSERT_EQUAL_INT(0, new_bytes);
}

TEST(a_frontend_can_observe_late_replies_at_the_idle_poll_cadence) {
    static const vif_frontend_t observer = {
        .name = "observer",
        .poll = old_poll,
        .poll_interval_ms = 50,
    };
    vif_link_set_frontend(&observer);
    idf_stub_notify_reset();
    if (setjmp(loop_done) == 0)
        link_task(NULL);
    TEST_ASSERT_EQUAL_INT(1, old_polls);
    TEST_ASSERT_EQUAL_INT(50, fake_clock_ms());
}

TEST(streaming_frontends_keep_the_default_poll_cadence) {
    if (setjmp(loop_done) == 0)
        link_task(NULL);
    TEST_ASSERT_EQUAL_INT(1, old_polls);
    TEST_ASSERT_EQUAL_INT(2, fake_clock_ms());
}

/* ------------------------------------------------------------------ *
 * What ends the link task's sleep
 * ------------------------------------------------------------------ */

TEST(input_on_the_link_port_wakes_the_task) {
    const uint8_t byte = 'x';
    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_port_rx(fake_port_id(1), &byte, 1));
    TEST_ASSERT_FALSE(idf_stub_notify_pending());
    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_port_rx(port, &byte, 1));
    TEST_ASSERT_TRUE(idf_stub_notify_pending());
}

TEST(input_dropped_for_a_full_buffer_still_wakes_the_task) {
    uint8_t chunk[FAKE_PORT_RX_BUF + 1];
    memset(chunk, 'x', sizeof(chunk));
    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_port_rx(port, chunk, sizeof(chunk)));
    TEST_ASSERT_EQUAL_INT(1, comm_port_rx_dropped(port));
    TEST_ASSERT_TRUE(idf_stub_notify_pending());
}

static const vif_bus_cfg_t can_500k = {.bitrate = 500000};

TEST(news_from_a_bus_the_link_holds_wakes_the_link_task) {
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can_500k));
    idf_stub_notify_reset();
    bus_notify(can_bus_ops.listener);
    TEST_ASSERT_TRUE(idf_stub_notify_pending());
    TEST_ASSERT_TRUE(idf_stub_notified_task() == g.task[VIF_OWNER_LINK]);
    idf_stub_notify_reset();
    BaseType_t woken = pdFALSE;
    bus_notify_from_isr(can_bus_ops.listener, &woken);
    TEST_ASSERT_TRUE(idf_stub_notified_task() == g.task[VIF_OWNER_LINK]);
}

TEST(a_closed_bus_wakes_nobody) {
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can_500k));
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_close(VIF_OWNER_LINK, VIF_BUS_CAN));
    idf_stub_notify_reset();
    bus_notify(can_bus_ops.listener);
    TEST_ASSERT_FALSE(idf_stub_notify_pending());
}

TEST(news_from_a_bus_the_shell_holds_leaves_the_link_asleep) {
    static int shell_thread;
    idf_stub_set_current_task((TaskHandle_t)&shell_thread);
    vif_shell_bind();
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_SHELL, VIF_BUS_KLINE, NULL));
    idf_stub_notify_reset();
    bus_notify(kline_bus_ops.listener);
    TEST_ASSERT_FALSE(idf_stub_notify_pending());
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_release_all(VIF_OWNER_SHELL));
    idf_stub_set_current_task(NULL);
}

TEST(control_requests_from_other_tasks_wake_the_task) {
    vif_link_set_frontend(&new_frontend);
    TEST_ASSERT_TRUE(idf_stub_notify_pending());
    idf_stub_notify_reset();
    vif_recover_all();
    TEST_ASSERT_TRUE(idf_stub_notify_pending());
}

static uint32_t wake_due_us;

static bool wake_at_due(uint32_t *due_us) {
    *due_us = wake_due_us;
    return true;
}

/* Runs the loop under a front-end that wants polling at @p in_us from now. */
static void run_with_deadline(int32_t in_us) {
    static const vif_frontend_t timed = {
        .name = "timed",
        .poll = old_poll,
        .next_wake = wake_at_due,
    };
    vif_link_set_frontend(&timed);
    TEST_ASSERT_TRUE(vif_link_service());
    idf_stub_notify_reset();
    wake_due_us = (uint32_t)esp_timer_get_time() + (uint32_t)in_us;
    if (setjmp(loop_done) == 0)
        link_task(NULL);
    TEST_ASSERT_EQUAL_INT(1, old_polls);
    TEST_ASSERT_FALSE(fake_esp_timer_armed(link_timer, NULL));
}

TEST(a_frontend_deadline_ends_the_wait_between_ticks) {
    run_with_deadline(300);
    TEST_ASSERT_EQUAL_INT(0, fake_clock_ms());
    TEST_ASSERT_TRUE((int32_t)((uint32_t)esp_timer_get_time() - wake_due_us) >=
                     0);
}

TEST(a_deadline_already_due_skips_the_wait) {
    run_with_deadline(-1);
    TEST_ASSERT_EQUAL_INT(0, fake_clock_ms());
    TEST_ASSERT_TRUE((int32_t)((uint32_t)esp_timer_get_time() - wake_due_us) <
                     1000);
}

TEST(a_deadline_inside_the_backstop_is_met_on_time) {
    run_with_deadline(10000);
    TEST_ASSERT_EQUAL_INT(10, fake_clock_ms());
}

TEST(a_deadline_beyond_the_backstop_waits_only_the_backstop) {
    run_with_deadline(500000);
    TEST_ASSERT_EQUAL_INT(VIF_BACKSTOP_MS, fake_clock_ms());
}

/* ------------------------------------------------------------------ *
 * The backstop: what slips past next_wake and the events
 * ------------------------------------------------------------------ */

static uint32_t work_done;
static bool poll_works;
static int counted_polls;

static uint32_t count_progress(void) { return work_done; }

/* Does its pass, then ends the loop on the next one. */
static void counted_poll(void) {
    if (counted_polls++)
        longjmp(loop_done, 1);
    if (poll_works)
        work_done++;
}

static bool nothing_due(uint32_t *due_us) {
    (void)due_us;
    return false;
}

static void run_counted(bool works, bool woken) {
    static const vif_frontend_t counted = {
        .name = "counted",
        .poll = counted_poll,
        .next_wake = nothing_due,
        .progress = count_progress,
    };
    vif_link_set_frontend(&counted);
    TEST_ASSERT_TRUE(vif_link_service());
    idf_stub_notify_reset();
    counted_polls = 0;
    poll_works = works;
    if (woken)
        xTaskNotifyGive(g.task[VIF_OWNER_LINK]);
    if (setjmp(loop_done) == 0)
        link_task(NULL);
}

TEST(a_frontend_with_nothing_due_sleeps_until_the_backstop) {
    run_counted(false, false);
    TEST_ASSERT_EQUAL_INT(2 * VIF_BACKSTOP_MS, fake_clock_ms());
    TEST_ASSERT_EQUAL_INT(1, g.link.backstop_polls);
    TEST_ASSERT_EQUAL_INT(0, g.link.backstop_work);
}

TEST(work_found_by_a_backstop_poll_is_counted) {
    run_counted(true, false);
    TEST_ASSERT_EQUAL_INT(1, g.link.backstop_polls);
    TEST_ASSERT_EQUAL_INT(1, g.link.backstop_work);
}

TEST(work_after_an_event_is_not_the_backstops) {
    run_counted(true, true);
    TEST_ASSERT_EQUAL_INT(0, g.link.backstop_polls);
    TEST_ASSERT_EQUAL_INT(0, g.link.backstop_work);
}

/* ------------------------------------------------------------------ *
 * Start-up failures
 * ------------------------------------------------------------------ */

TEST(link_start_before_init_is_refused) {
    g.inited = false;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, vif_link_start(&new_frontend));
    g.inited = true;
}

TEST(link_start_reports_a_timer_that_cannot_be_created) {
    esp_timer_handle_t saved_timer = link_timer;
    TaskHandle_t saved_task = g.task[VIF_OWNER_LINK];

    g.task[VIF_OWNER_LINK] = NULL; /* look like a link that never started */
    link_timer = NULL;
    fail_timer_create = true;
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, vif_link_start(&new_frontend));
    fail_timer_create = false;

    /* Nothing was started: no task, no default front-end. */
    TEST_ASSERT_NULL(g.task[VIF_OWNER_LINK]);
    TEST_ASSERT_NULL(link_timer);
    link_timer = saved_timer;
    g.task[VIF_OWNER_LINK] = saved_task;
}

TEST(link_start_reports_a_task_that_cannot_be_started) {
    TaskHandle_t saved_task = g.task[VIF_OWNER_LINK];

    g.task[VIF_OWNER_LINK] = NULL;
    fail_task_create = true;
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, vif_link_start(&new_frontend));
    fail_task_create = false;

    /* No task handle is recorded, so ownership checks cannot trust a task
     * that does not exist; the default front-end still stands. */
    TEST_ASSERT_NULL(g.task[VIF_OWNER_LINK]);
    TEST_ASSERT_TRUE(g.link.fe_default == &new_frontend);
    g.task[VIF_OWNER_LINK] = saved_task;
}

TEST(init_reports_a_claim_lock_that_cannot_be_created) {
    SemaphoreHandle_t saved = g.lock;

    g.lock = NULL;
    fail_mutex_create = true;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NO_MEM, vif_init());
    fail_mutex_create = false;
    TEST_ASSERT_NULL(g.lock);

    g.lock = saved;
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_init()); /* and it recovers */
    idf_stub_set_created_task(xTaskGetCurrentTaskHandle());
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_link_start(&new_frontend));
}

TEST(selecting_no_frontend_changes_nothing) {
    const vif_frontend_t *before = g.link.fe_default;

    vif_link_select_frontend(NULL);

    TEST_ASSERT_TRUE(g.link.fe_default == before);
    TEST_ASSERT_TRUE(g.link.fe_next == &old_frontend); /* td_setup's request */
}

TEST(a_link_with_no_port_idles_without_polling) {
    vif_link_set_port(COMM_INVALID_PORT_ID);
    idf_stub_notify_reset(); /* the set_port wake-up would end the wait */
    fake_clock_reset();
    leave_on_take = true;
    take_calls = 0;
    if (setjmp(take_escape) == 0) {
        link_task(NULL);
        TEST_FAIL("link task returned");
    }
    leave_on_take = false;

    /* One idle wait of the full idle period ran, and nothing was polled. */
    TEST_ASSERT_EQUAL_INT(VIF_IDLE_WAIT_MS, fake_clock_ms());
    TEST_ASSERT_EQUAL_INT(0, old_polls);
}

/* ------------------------------------------------------------------ *
 * Transmit submission edges
 * ------------------------------------------------------------------ */

static void open_can_for_link(void) {
    const vif_bus_cfg_t cfg = {.bitrate = 500000};

    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &cfg));
}

TEST(a_submit_that_cannot_get_a_worker_reports_no_space) {
    uint8_t byte = 0x55;
    bus_msg_t msg = {.data = &byte, .len = 1, .id = 0x7DF};

    open_can_for_link();
    fail_worker_create = true;
    TEST_ASSERT_EQUAL_INT(
        BUS_ERR_NO_SPACE, vif_bus_submit(VIF_OWNER_LINK, VIF_BUS_CAN, &msg, 0, 0));
    TEST_ASSERT_EQUAL_INT(BUS_ERR_NO_SPACE,
                          vif_bus_submit_train(VIF_OWNER_LINK, VIF_BUS_CAN,
                                               &msg, 1, 0, 0));
    fail_worker_create = false;
    TEST_ASSERT_NULL(g.claim[bus_group(VIF_BUS_CAN)].tx_worker);
}

TEST(a_train_needs_a_claim_and_a_driver_that_can_send_one) {
    uint8_t byte = 0x55;
    bus_msg_t msg = {.data = &byte, .len = 1, .id = 0x7DF};
    const vif_bus_cfg_t cfg = {.bitrate = 10400};

    /* Not held by anyone: refused before the driver is looked at. */
    TEST_ASSERT_EQUAL_INT(VIF_ERR_NO_CLAIM,
                          vif_bus_submit_train(VIF_OWNER_LINK, VIF_BUS_CAN,
                                               &msg, 1, 0, 0));
    /* Held, but K-Line has no train transmit. */
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_KLINE, &cfg));
    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED,
                          vif_bus_submit_train(VIF_OWNER_LINK, VIF_BUS_KLINE,
                                               &msg, 1, 0, 0));
}

TEST(a_close_whose_worker_cannot_stop_keeps_the_driver_and_claim) {
    open_can_for_link();
    fail_worker_close = true;
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, vif_bus_close(VIF_OWNER_LINK, VIF_BUS_CAN));
    fail_worker_close = false;

    /* The claim survives so the close can be retried, and the retry works. */
    TEST_ASSERT_EQUAL_INT(VIF_BUS_CAN,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_close(VIF_OWNER_LINK, VIF_BUS_CAN));
    TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE,
                          vif_bus_current(VIF_OWNER_LINK, VIF_BUS_CAN));
}

TEST(a_listen_only_claim_is_reported_and_printed) {
    vif_bus_cfg_t cfg = {.bitrate = 500000, .listen_only = true};
    vif_bus_claim_t claim[VIF_BUS_GROUPS];
    bool found = false;

    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &cfg));
    vif_bus_info(claim);
    for (int i = 0; i < VIF_BUS_GROUPS; i++) {
        if (claim[i].bus == VIF_BUS_CAN) {
            found = claim[i].listen_only;
        }
    }
    TEST_ASSERT_TRUE(found);
    vif_print_debug_info(); /* the " listen-only" suffix is printed */
}

/* ------------------------------------------------------------------ *
 * Pin 9 is on both driver banks when the low side mod is fitted
 * ------------------------------------------------------------------ */

TEST(pin_9_cannot_be_driven_as_voltage_and_ground_at_once) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_pin_set(VIF_OWNER_LINK, 9, VIF_PIN_VOLTAGE,
                                              12000));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
                          vif_pin_set(VIF_OWNER_LINK, 9, VIF_PIN_GROUND, 0));
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_pin_set(VIF_OWNER_LINK, 9, VIF_PIN_OFF, 0));

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_pin_set(VIF_OWNER_LINK, 9, VIF_PIN_GROUND, 0));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
                          vif_pin_set(VIF_OWNER_LINK, 9, VIF_PIN_VOLTAGE, 12000));
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_pin_set(VIF_OWNER_LINK, 9, VIF_PIN_OFF, 0));
}
