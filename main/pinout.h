/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* The pins that differ between boards (CAN, LED, sense inputs, drivers) are in
 * the board profile (main/boards/<board>.h, see boards/board_spec.h). Include
 * this header for both sets of pin names. */
#include "board_profile.h"

/* Front panel button */
#define PIN_BUTTON 0

/* K-Line Transceiver. The K-Line driver is built for every board; a board
 * without the transceiver (BOARD_HAS_KLINE) never opens the bus, so these pins
 * are never driven there. */
#define PIN_KLINE_TX 17
#define PIN_KLINE_RX 18
#define PIN_KLINE_nSILENT 21

/* J1850 IO. The same as K-Line: built for every board, and never driven on a
 * board without the transceiver (BOARD_HAS_J1850). */
#define PIN_J1850_MODE 11
#define PIN_J1850_PWM_RX 12
#define PIN_J1850_VPW_RX 13
#define PIN_J1850_TX_N 14
#define PIN_J1850_TX_P 15
