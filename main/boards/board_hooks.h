/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/*
 * Per-board hooks. Each board implements these once, in its own file
 * (boards/board_stock.c or boards/board_rejsacan.c). CMake compiles only the
 * selected board's file, so these names resolve to one definition. board.c
 * calls them from board_setup() in this order:
 *
 *   board_pins_init()        GPIO for the board's pins, before the ISR service
 *   board_adc_init()         ADC unit channels, before the calibration scheme
 *   board_adc_calibrated()   after the calibration scheme exists
 *
 * The public board.h functions (vbatt, HS, LS, calibration) are also
 * implemented per board, in the same file.
 */

#include "esp_adc/adc_oneshot.h"

/** @brief Configure the GPIO the board wires: its transceivers and drivers. */
void board_pins_init(void);

/** @brief Configure the ADC channels the board wires on @p unit. */
void board_adc_init(adc_oneshot_unit_handle_t unit);

/** @brief Board work that needs the ADC calibration scheme. */
void board_adc_calibrated(void);
