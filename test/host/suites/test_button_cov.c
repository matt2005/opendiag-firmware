/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file test_button.c
 * @brief The front panel button's press timing.
 *
 * The button is the last way back when a link is wedged, so what matters is
 * that a hold fires exactly once and a brush against it fires not at all.
 *
 * The firmware source is included rather than linked, for the usual reason:
 * the state machine is static and the only public entry point starts a task
 * that never returns. `button_step()` is that task's body - one wait, edge or
 * threshold, per call - and fake_gpio_set_level() feeds it edges the same way
 * a real ISR would.
 */

#include <setjmp.h>

#include "td_test.h"

#include "fake_clock.h"
#include "fake_gpio.h"
#include "pinout.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Failure injection for button_init() and a way out of button_task()'s endless
 * loop. The real stand-ins are called through unless a test arms a failure.
 */
static bool g_fail_task_create;
static bool g_fail_intr_type;
static TaskFunction_t g_task_fn;
static int g_take_calls;
static int g_take_limit; /* 0: never leave the task loop */
static jmp_buf g_leave_task;

static BaseType_t test_xTaskCreate(TaskFunction_t fn, const char *name,
                                   uint32_t stack, void *param,
                                   UBaseType_t prio, TaskHandle_t *created) {
    g_task_fn = fn;
    if (g_fail_task_create) {
        return pdFAIL;
    }
    return xTaskCreate(fn, name, stack, param, prio, created);
}

static esp_err_t test_gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t t) {
    if (g_fail_intr_type) {
        return ESP_FAIL;
    }
    return gpio_set_intr_type(pin, t);
}

static uint32_t test_ulTaskNotifyTake(BaseType_t clear, TickType_t wait) {
    if (g_take_limit && ++g_take_calls >= g_take_limit) {
        longjmp(g_leave_task, 1);
    }
    return ulTaskNotifyTake(clear, wait);
}

#define xTaskCreate test_xTaskCreate
#define gpio_set_intr_type test_gpio_set_intr_type
#define ulTaskNotifyTake test_ulTaskNotifyTake

#include "button.c"

#undef xTaskCreate
#undef gpio_set_intr_type
#undef ulTaskNotifyTake

static int g_fired_count;
static int g_long_count;
static int g_click_count;

static void on_hold(void) { g_fired_count++; }

static void on_long_hold(void) { g_long_count++; }

static void on_click(void) { g_click_count++; }

static void press(void) { fake_gpio_set_level(PIN_BUTTON, 0); /* active low */ }

static void release(void) { fake_gpio_set_level(PIN_BUTTON, 1); }

/** Runs @p n steps of the task body. */
static void step_times(int n) {
    for (int i = 0; i < n; i++) {
        button_step();
    }
}

void td_setup(void) {
    fake_gpio_reset();
    idf_stub_reset_task_starts();

    g_fired_count = 0;
    g_long_count = 0;
    g_click_count = 0;
    g_press_start_us = -1;
    g_fired = false;
    g_fired_long = false;

    button_init(); /* arms the fake ISR too - see fake_gpio.c */

    g_on_hold = on_hold;
    g_on_long_hold = on_long_hold;
    g_on_click = on_click;
}


/* ------------------------------------------------------------------ *
 * Setup
 * ------------------------------------------------------------------ */

TEST(init_takes_the_pin_and_configures_it_as_an_input) {
    /* The button owns GPIO 0 outright; board.c does not touch it. */
    TEST_ASSERT_TRUE(fake_gpio_was_reset(PIN_BUTTON));
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_INPUT, fake_gpio_direction(PIN_BUTTON));
}

TEST(init_starts_exactly_one_task) {
    TEST_ASSERT_EQUAL_INT(1, idf_stub_task_starts());
}

/* ------------------------------------------------------------------ *
 * Press timing
 * ------------------------------------------------------------------ */

TEST(a_hold_fires_once_it_reaches_the_threshold) {
    press();
    step_times(1); /* debounced press */
    step_times(1); /* waits out BUTTON_HOLD_MS */

    TEST_ASSERT_EQUAL_INT(1, g_fired_count);
}

TEST(holding_it_down_does_not_fire_again) {
    press();
    step_times(1);
    step_times(1); /* hold fires */
    step_times(1); /* waits out BUTTON_LONG_HOLD_MS, fires long-hold */

    /* Neither threshold re-fires while still held. */
    TEST_ASSERT_EQUAL_INT(1, g_fired_count);
    TEST_ASSERT_EQUAL_INT(1, g_long_count);
}

TEST(a_short_press_fires_nothing) {
    for (int i = 0; i < 5; i++) {
        press();
        step_times(1); /* debounced press */
        release();
        step_times(1); /* debounced release: too short to be a click */
    }

    TEST_ASSERT_EQUAL_INT(0, g_fired_count);
    TEST_ASSERT_EQUAL_INT(0, g_click_count);
}

TEST(a_release_forgets_how_long_it_had_been_held) {
    /* Two nearly-long presses must not add up to one long one. */
    press();
    step_times(1);
    fake_clock_advance_ms(BUTTON_HOLD_MS - 1);
    release();
    step_times(1);
    TEST_ASSERT_EQUAL_INT(0, g_fired_count);

    press();
    step_times(1);
    fake_clock_advance_ms(BUTTON_HOLD_MS - 1);
    release();
    step_times(1);
    TEST_ASSERT_EQUAL_INT(0, g_fired_count);
}

TEST(a_second_hold_fires_again) {
    press();
    step_times(1);
    step_times(1); /* hold fires */
    TEST_ASSERT_EQUAL_INT(1, g_fired_count);

    release();
    step_times(1);

    press();
    step_times(1);
    step_times(1);
    TEST_ASSERT_EQUAL_INT(2, g_fired_count);
}

/* ------------------------------------------------------------------ *
 * Wiring
 * ------------------------------------------------------------------ */

TEST(a_hold_with_no_handler_installed_does_nothing) {
    g_on_hold = NULL;

    press();
    step_times(1);
    step_times(1);

    TEST_ASSERT_EQUAL_INT(0, g_fired_count);
}

/* ------------------------------------------------------------------ *
 * The click, which nothing has claimed yet
 * ------------------------------------------------------------------ */

TEST(a_press_and_release_is_a_click) {
    press();
    step_times(1);
    fake_clock_advance_ms(BUTTON_CLICK_MIN_MS);
    release();
    step_times(1);

    TEST_ASSERT_EQUAL_INT(1, g_click_count);
    TEST_ASSERT_EQUAL_INT(0, g_fired_count);
}

TEST(a_hold_is_not_also_a_click) {
    /*
     * The two gestures share one button. The hold has already acted by the
     * time the thumb comes up - it opened the pairing window - so a release
     * after it must not be taken for a click and act a second time.
     */
    press();
    step_times(1);
    step_times(1); /* hold fires */
    release();
    step_times(1);

    TEST_ASSERT_EQUAL_INT(1, g_fired_count);
    TEST_ASSERT_EQUAL_INT(0, g_click_count);
}

TEST(a_bounce_too_short_to_be_deliberate_is_not_a_click) {
    for (int i = 0; i < 5; i++) {
        press();
        step_times(1);
        fake_clock_advance_ms(BUTTON_CLICK_MIN_MS - 1);
        release();
        step_times(1);
    }

    TEST_ASSERT_EQUAL_INT(0, g_click_count);
}

TEST(clicks_fire_once_each) {
    for (int i = 0; i < 3; i++) {
        press();
        step_times(1);
        fake_clock_advance_ms(BUTTON_CLICK_MIN_MS);
        release();
        step_times(1);
    }

    TEST_ASSERT_EQUAL_INT(3, g_click_count);
}

TEST(a_click_with_no_handler_installed_does_nothing) {
    g_on_click = NULL;

    press();
    step_times(1);
    fake_clock_advance_ms(BUTTON_CLICK_MIN_MS);
    release();
    step_times(1);

    TEST_ASSERT_EQUAL_INT(0, g_click_count);
}

/* ------------------------------------------------------------------ *
 * The long hold, which forgets every paired phone
 * ------------------------------------------------------------------ */

TEST(a_long_hold_fires_once_it_reaches_its_own_threshold) {
    press();
    step_times(1);
    step_times(1); /* hold fires */
    step_times(1); /* waits out the remainder to BUTTON_LONG_HOLD_MS */

    TEST_ASSERT_EQUAL_INT(1, g_long_count);
}

TEST(the_short_hold_fires_on_the_way_past) {
    /*
     * Deliberate, and only safe because of what the two do: the short hold
     * opens the pairing window and the long one forgets every phone, and
     * having forgotten every phone the window is exactly where you want to be.
     */
    press();
    step_times(1);
    step_times(1);
    step_times(1);

    TEST_ASSERT_EQUAL_INT(1, g_fired_count);
    TEST_ASSERT_EQUAL_INT(1, g_long_count);
}

TEST(an_ordinary_hold_does_not_forget_anything) {
    /* Two seconds is a gesture people will make daily. Ten is not. */
    press();
    step_times(1);
    step_times(1); /* hold fires */
    release();
    step_times(1);

    TEST_ASSERT_EQUAL_INT(1, g_fired_count);
    TEST_ASSERT_EQUAL_INT(0, g_long_count);
}

TEST(a_second_long_hold_fires_again) {
    press();
    step_times(1);
    step_times(1);
    step_times(1);
    release();
    step_times(1);

    press();
    step_times(1);
    step_times(1);
    step_times(1);

    TEST_ASSERT_EQUAL_INT(2, g_long_count);
}

TEST(a_long_hold_with_no_handler_installed_does_nothing) {
    g_on_long_hold = NULL;

    press();
    step_times(1);
    step_times(1);
    step_times(1);

    TEST_ASSERT_EQUAL_INT(0, g_long_count);
}

/* ------------------------------------------------------------------ *
 * Coverage: the ISR, the bounce paths and the callback setters
 * ------------------------------------------------------------------ */

static int g_alt_hold_count;
static int g_alt_long_count;
static int g_alt_click_count;

static void alt_hold(void) { g_alt_hold_count++; }

static void alt_long_hold(void) { g_alt_long_count++; }

static void alt_click(void) { g_alt_click_count++; }

/** Clears a notification left pending by an earlier test. */
static void drain_notifications(void) {
    while (ulTaskNotifyTake(pdTRUE, 0) != 0) {
    }
}

TEST(the_isr_wakes_the_task_with_one_notification) {
    /* The fake GPIO notifies the task directly, so the ISR body itself is
     * only reached by calling it, which is what the vector table would do. */
    drain_notifications();

    button_isr(NULL);

    TEST_ASSERT_EQUAL_INT(1, ulTaskNotifyTake(pdTRUE, 0));
    TEST_ASSERT_EQUAL_INT(0, ulTaskNotifyTake(pdTRUE, 0));
}

TEST(a_glitch_while_idle_does_not_start_a_press) {
    /* Both edges land before the task wakes: a spike, and the level is up
     * again by the time the debounce is over. */
    press();
    release();
    step_times(1);

    TEST_ASSERT_EQUAL_INT(-1, g_press_start_us);
    TEST_ASSERT_FALSE(g_fired);
    TEST_ASSERT_EQUAL_INT(0, g_click_count);

    /* Still idle, so the next genuine press is the one that starts a press. */
    press();
    step_times(1);

    TEST_ASSERT_TRUE(g_press_start_us >= 0);
}

TEST(a_bounce_on_release_does_not_end_the_press) {
    press();
    step_times(1); /* debounced press */
    fake_clock_advance_ms(200);
    release();
    press();       /* the contact bounces back down */
    step_times(1); /* release debounce sees the button still down */

    TEST_ASSERT_TRUE(g_press_start_us >= 0);
    TEST_ASSERT_EQUAL_INT(0, g_click_count);
    TEST_ASSERT_EQUAL_INT(0, g_fired_count);

    release();
    step_times(1); /* the real release, well past the click minimum */

    TEST_ASSERT_EQUAL_INT(1, g_click_count);
    TEST_ASSERT_EQUAL_INT(-1, g_press_start_us);
}

TEST(the_setters_install_the_callbacks_the_task_calls) {
    button_set_hold_callback(alt_hold);
    button_set_long_hold_callback(alt_long_hold);
    button_set_click_callback(alt_click);

    press();
    step_times(1);
    step_times(1); /* hold fires through the replacement */
    step_times(1); /* long hold fires through the replacement */
    release();
    step_times(1); /* a release after the long hold is not a click */

    TEST_ASSERT_EQUAL_INT(1, g_alt_hold_count);
    TEST_ASSERT_EQUAL_INT(1, g_alt_long_count);
    TEST_ASSERT_EQUAL_INT(0, g_alt_click_count);
    TEST_ASSERT_EQUAL_INT(0, g_fired_count);
    TEST_ASSERT_EQUAL_INT(0, g_long_count);
    TEST_ASSERT_EQUAL_INT(0, g_click_count);

    press();
    step_times(1);
    fake_clock_advance_ms(BUTTON_CLICK_MIN_MS + 50);
    release();
    step_times(1);

    TEST_ASSERT_EQUAL_INT(1, g_alt_click_count);
    TEST_ASSERT_EQUAL_INT(0, g_click_count);
}

/* ------------------------------------------------------------------ *
 * button_init() failures and the task loop
 * ------------------------------------------------------------------ */

TEST(init_reports_a_failed_task_start_and_attaches_no_isr) {
    fake_gpio_reset();
    idf_stub_reset_task_starts();
    g_fail_task_create = true;
    button_init();
    g_fail_task_create = false;

    /* Without the task there is nothing for an edge to wake, so no ISR is
     * attached rather than notifying a NULL handle. */
    TEST_ASSERT_EQUAL_INT(0, idf_stub_task_starts());
    idf_stub_notify_reset();
    release();
    press();
    TEST_ASSERT_FALSE(idf_stub_notify_pending());
}

TEST(init_survives_a_failed_isr_attach) {
    fake_gpio_reset();
    idf_stub_reset_task_starts();
    g_fail_intr_type = true;
    button_init();
    g_fail_intr_type = false;

    TEST_ASSERT_EQUAL_INT(1, idf_stub_task_starts());
    idf_stub_notify_reset();
    release();
    press();
    TEST_ASSERT_FALSE(idf_stub_notify_pending());
}

TEST(the_task_body_loops_over_button_step) {
    g_task_fn = NULL;
    button_init();
    TEST_ASSERT_NOT_NULL((void *)g_task_fn);

    press(); /* edge: wakes the idle wait */
    g_take_calls = 0;
    g_take_limit = 3; /* idle wake, hold wait, then the third call leaves */
    if (setjmp(g_leave_task) == 0) {
        g_task_fn(NULL);
        TEST_FAIL("task body returned");
    }
    g_take_limit = 0;

    /* Two steps ran: the press was latched, then the hold threshold fired. */
    TEST_ASSERT_EQUAL_INT(1, g_fired_count);
    TEST_ASSERT_EQUAL_INT(0, g_long_count);
}
