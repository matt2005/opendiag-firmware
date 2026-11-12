/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * RejsaCAN v3.4 board: CAN only. The only transceiver is CAN0 on the OBD
 * socket, and there are no OBD drivers, sense inputs or LED. Compiled only when
 * OPENDIAG_BOARD=rejsacan. The shared code is in board.c; the capability
 * checks that refuse the missing buses are in vif.c and the other shared files.
 */
#include <stdint.h>
#include "esp_adc/adc_oneshot.h"
#include "board.h"
#include "board_hooks.h"
#include "board_internal.h"
#include "driver/gpio.h"
#include "pinout.h"

void board_pins_init(void) {
    /* CAN0 transceiver: silent until a bus is actually opened, so the
     * adapter cannot drive a vehicle bus it was only plugged into. */
    gpio_reset_pin(PIN_CAN0_SILENT);
    gpio_set_level(PIN_CAN0_SILENT, 1);
    gpio_set_direction(PIN_CAN0_SILENT, GPIO_MODE_OUTPUT);
}

void board_adc_init(adc_oneshot_unit_handle_t unit) {
    /* Only the board ID channel is wired. GPIO 1 and 2 are SDA and SCL here. */
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_0,
        .bitwidth = ADC_BITWIDTH_12,
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(unit, ADC_CHANNEL_8, &config));
}

void board_adc_calibrated(void) {}

/* RejsaCAN has no confirmed battery sense input, so there is no reading. */
int32_t board_get_vbatt(void) { return -1; }

/* RejsaCAN has no high side sense input, so there is no reading. */
int32_t board_get_hs_vsense(void) { return -1; }

/* No OBD drivers on RejsaCAN: the HS and LS controls do nothing. */
void board_set_hs_boost_en(int state) { (void)state; }

void board_set_hs_voltage(uint32_t millivolt) { (void)millivolt; }

void board_set_hs_state(enum hs_pin pin, int state) {
    (void)pin;
    (void)state;
}

void board_set_ls_state(enum ls_pin pin, int state) {
    (void)pin;
    (void)state;
}

void board_hs_ls_reset_state(void) {}

void board_calibrate_hs(void) {}

/* The battery sense input is not confirmed on RejsaCAN, and the stored
 * calibration is for the stock divider. vif_calibrate_vbatt() refuses first. */
void board_calibrate_vbatt(void) {}
