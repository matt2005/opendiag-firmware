/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file test_vif_rejsacan.c
 * @brief The CAN only RejsaCAN build: what vif.c refuses and what it allows.
 *
 * Built only with -DREJSACAN=ON (OPENDIAG_BOARD_REJSACAN). The stock suites
 * assume K-line, J1850 and the pin drivers, so they do not apply here.
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

#include "vif.h"

#define OBD_PIN_HS 6
#define OBD_PIN_LS 15

static const vif_bus_cfg_t can500 = {.bitrate = 500000};
static const vif_frontend_t frontend = {.name = "fe"};
static int calibration_calls;

static void calibration_hook(void) { calibration_calls++; }

void td_setup(void) {
    idf_stub_set_current_task(NULL);
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_init());
    idf_stub_set_created_task(xTaskGetCurrentTaskHandle());
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_link_start(&frontend));
    vif_shell_bind();

    fake_clock_reset();
    fake_board_reset();
    fake_led_reset();
    fake_can_reset();
    fake_bus_reset_all();
    calibration_calls = 0;
    fake_board_on_calibration(calibration_hook);
}

void td_teardown(void) {
    fake_board_on_calibration(NULL);
    idf_stub_set_created_task(NULL);
    TEST_ASSERT_MSG(idf_stub_lock_balance() == 0, "vif left %d locks held",
                    idf_stub_lock_balance());
}

TEST(a_refused_k_line_open_never_reaches_the_driver_and_can_still_opens) {
    vif_bus_claim_t claims[VIF_BUS_GROUPS];

    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_SUPPORTED,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_KLINE, NULL));
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_NOT_SUPPORTED,
        vif_bus_open(VIF_OWNER_SHELL, VIF_BUS_J1850_PWM, NULL));
    TEST_ASSERT_EQUAL_INT(0, fake_bus_setup_count(FAKE_BUS_KLINE));
    TEST_ASSERT_EQUAL_INT(0, fake_bus_setup_count(FAKE_BUS_J1850_PWM));

    vif_bus_info(claims);
    for (unsigned i = 0; i < VIF_BUS_GROUPS; i++) {
        TEST_ASSERT_EQUAL_INT(VIF_BUS_NONE, claims[i].bus);
    }

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          vif_bus_open(VIF_OWNER_LINK, VIF_BUS_CAN, &can500));
}

TEST(pin_set_refuses_drive_modes_holds_no_pin_and_accepts_off) {
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_NOT_SUPPORTED,
        vif_pin_set(VIF_OWNER_LINK, OBD_PIN_HS, VIF_PIN_VOLTAGE, 12000));
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_NOT_SUPPORTED,
        vif_pin_set(VIF_OWNER_LINK, OBD_PIN_LS, VIF_PIN_GROUND, 0));
    TEST_ASSERT_FALSE(vif_any_pin_active());
    TEST_ASSERT_EQUAL_INT(-1, fake_board_hs_state(OBD_PIN_HS));
    TEST_ASSERT_EQUAL_INT(-1, fake_board_ls_state(OBD_PIN_LS));

    TEST_ASSERT_EQUAL_INT(
        ESP_OK, vif_pin_set(VIF_OWNER_LINK, OBD_PIN_HS, VIF_PIN_OFF, 0));

    /* Bad input is still bad input, not "not supported". */
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_ARG,
        vif_pin_set(VIF_OWNER_COUNT, OBD_PIN_HS, VIF_PIN_VOLTAGE, 12000));
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_ARG,
        vif_pin_set(VIF_OWNER_LINK, 99, VIF_PIN_VOLTAGE, 12000));
}

TEST(hs_calibration_is_refused_without_running_and_pins_still_answer) {
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_SUPPORTED,
                          vif_calibrate_hs(VIF_OWNER_SHELL));
    TEST_ASSERT_EQUAL_INT(0, calibration_calls);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          vif_calibrate_hs(VIF_OWNER_COUNT));

    TEST_ASSERT_EQUAL_INT(
        ESP_OK, vif_pin_set(VIF_OWNER_SHELL, OBD_PIN_HS, VIF_PIN_OFF, 0));
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_NOT_SUPPORTED,
        vif_pin_set(VIF_OWNER_SHELL, OBD_PIN_HS, VIF_PIN_VOLTAGE, 12000));
}

TEST(non_can_stats_calls_report_unsupported) {
    bus_stats_t stats;

    /* The CAN half (0 on success) needs get_stats on the CAN fake, which it
     * does not have, so only the refusals are checked here. */
    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED,
                          vif_bus_stats(VIF_BUS_KLINE, &stats));
    TEST_ASSERT_EQUAL_INT(BUS_ERR_UNSUPPORTED,
                          vif_bus_reset_stats(VIF_BUS_KLINE));
}
