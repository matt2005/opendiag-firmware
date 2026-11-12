/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/*
 * Stock OpenDiag board (rev1): every transceiver, the high side and low side
 * drivers, the battery and HS sense inputs, and the WS2812 status LED.
 * The board file is boards/board_stock.c. Profile rules: boards/board_spec.h.
 */

#define BOARD_ID "stock"
#define BOARD_NAME "rev1"

/* Features. Shared code tests these. */
#define BOARD_HAS_J1850 1         /* J1850 PWM and VPW transceiver */
#define BOARD_HAS_KLINE 1         /* K-Line transceiver */
#define BOARD_HAS_HS_LS_DRIVERS 1 /* OBD high side and low side pin drivers */
#define BOARD_HAS_BOOST 1         /* high side boost enable */
#define BOARD_HAS_WS2812 1        /* WS2812 status LED */
#define BOARD_HAS_VBATT_SENSE 1   /* battery sense input (calibrated) */
#define BOARD_HAS_HS_VSENSE 1     /* high side voltage sense input */
#define BOARD_HAS_AUX_CAN 1       /* second CAN transceiver on AUX_CAN J3 */
#define BOARD_CAN_BUSES 2

/* CAN0: OBD socket transceiver. Silent (RS high) until a bus is opened. */
#define PIN_CAN0_TX 4
#define PIN_CAN0_RX 5
#define PIN_CAN0_SILENT 8 /* Active High */

/* CAN1: AUX_CAN header J3 (1 = H, 2 = L, 3 = GND), TJA1051T-3 U6, no
 * termination fitted. */
#define PIN_CAN1_TX 6
#define PIN_CAN1_RX 7
#define PIN_CAN1_SILENT 47 /* Active High, same part as CAN0 */

/* ADC inputs. The board ID is read from a register strap at boot. */
#define PIN_HS_VOLTAGE_SENSE 1
#define PIN_OBD_VOLTAGE_SENSE 2
#define PIN_BOARDID 9

/* High side driver */
#define PIN_HS_OBD_6 38
#define PIN_HS_OBD_9 39
#define PIN_HS_OBD_11 40
#define PIN_HS_OBD_12 41
#define PIN_HS_OBD_13 42
#define PIN_HS_OBD_14 45
#define PIN_HS_BOOST_EN 46
#define PIN_HS_VOLTAGE_ADJUST 10 /* PWM signal */

/* Low side drivers (active high). Pin 9 is fitted only in a build that sets
 * OPENDIAG_HW_LS_OBD_9=1 (see board_spec.h). */
#if OPENDIAG_HW_LS_OBD_9
#define PIN_LS_OBD_9 16
#else
#define PIN_LS_OBD_9 PIN_NONE
#endif
#define PIN_LS_OBD_15 3

/* Status LED */
#define PIN_WS2812_LED 48
