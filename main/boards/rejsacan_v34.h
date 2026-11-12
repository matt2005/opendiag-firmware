/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/*
 * RejsaCAN v3.4: a CAN only board. One transceiver, on the OBD socket. No
 * AUX_CAN header, no K-Line, no J1850, no high side or low side drivers, no
 * battery or HS sense input, and no WS2812 LED (GPIO 48 is not an LED here).
 * The board file is boards/board_rejsacan.c. Profile rules:
 * boards/board_spec.h.
 */

#define BOARD_ID "rejsacan"
#define BOARD_NAME "RejsaCAN v3.4"

/* Features. Shared code tests these. */
#define BOARD_HAS_J1850 0
#define BOARD_HAS_KLINE 0
#define BOARD_HAS_HS_LS_DRIVERS 0
#define BOARD_HAS_BOOST 0
#define BOARD_HAS_WS2812 0
#define BOARD_HAS_VBATT_SENSE 0 /* not confirmed on this board */
#define BOARD_HAS_HS_VSENSE 0
#define BOARD_HAS_AUX_CAN 0
#define BOARD_CAN_BUSES 1

/* CAN0: the only transceiver, on the OBD socket. RS is active high (standby).
 */
#define PIN_CAN0_TX 14
#define PIN_CAN0_RX 13
#define PIN_CAN0_SILENT 38 /* Active High */

/* No AUX_CAN pins. */
#define PIN_CAN1_TX PIN_NONE
#define PIN_CAN1_RX PIN_NONE
#define PIN_CAN1_SILENT PIN_NONE

/* GPIO 1 and 2 are SDA and SCL on this board, so there is no sense input.
 * The board ID ADC channel is still configured (see board_rejsacan.c). */
#define PIN_HS_VOLTAGE_SENSE PIN_NONE
#define PIN_OBD_VOLTAGE_SENSE PIN_NONE
#define PIN_BOARDID 9

/* No OBD drivers */
#define PIN_HS_OBD_6 PIN_NONE
#define PIN_HS_OBD_9 PIN_NONE
#define PIN_HS_OBD_11 PIN_NONE
#define PIN_HS_OBD_12 PIN_NONE
#define PIN_HS_OBD_13 PIN_NONE
#define PIN_HS_OBD_14 PIN_NONE
#define PIN_HS_BOOST_EN PIN_NONE
#define PIN_HS_VOLTAGE_ADJUST PIN_NONE
#define PIN_LS_OBD_9 PIN_NONE
#define PIN_LS_OBD_15 PIN_NONE

/* No status LED */
#define PIN_WS2812_LED PIN_NONE
