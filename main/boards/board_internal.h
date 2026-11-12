/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/*
 * State that board.c owns and the per-board files (boards/board_*.c) read.
 * Nothing outside main/board.c and main/boards/ includes this.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"

/* Calibration values are stored as integers scaled by this factor. */
#define CALIBRATION_SCALE_FACTOR 10000.0f
#define ADC_NUM_SAMPLES 10

typedef struct {
    int32_t vbatt_gain;
    int32_t vbatt_offset;
    int32_t hs_sense_gain;
    int32_t hs_sense_offset;
    int32_t hs_duty_gain;
    int32_t hs_duty_offset;
    bool calibrated;
} board_calibration_t;

/* Loaded from NVS (or the defaults) by board_setup(). Written by the calibrate
 * routines of the board file and by board_calibration_set() in board.c. */
extern board_calibration_t pc_calibration;

/* ADC unit 1 and its calibration scheme, created by board_setup(). */
extern adc_oneshot_unit_handle_t board_adc1;
extern adc_cali_handle_t board_adc1_cal;
