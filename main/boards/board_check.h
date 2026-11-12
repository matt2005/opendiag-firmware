/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/*
 * Checks the board profile against the spec in board_spec.h, and derives the
 * flags that are not in any profile. Included last by the generated
 * board_profile.h. A profile that misses a name stops the build here, not at
 * some later use of the name.
 */

#ifndef BOARD_ID
#error "board profile: BOARD_ID is not defined"
#endif
#ifndef BOARD_NAME
#error "board profile: BOARD_NAME is not defined"
#endif

#ifndef BOARD_HAS_J1850
#error "board profile: BOARD_HAS_J1850 is not defined (1 or 0)"
#endif
#ifndef BOARD_HAS_KLINE
#error "board profile: BOARD_HAS_KLINE is not defined (1 or 0)"
#endif
#ifndef BOARD_HAS_HS_LS_DRIVERS
#error "board profile: BOARD_HAS_HS_LS_DRIVERS is not defined (1 or 0)"
#endif
#ifndef BOARD_HAS_BOOST
#error "board profile: BOARD_HAS_BOOST is not defined (1 or 0)"
#endif
#ifndef BOARD_HAS_WS2812
#error "board profile: BOARD_HAS_WS2812 is not defined (1 or 0)"
#endif
#ifndef BOARD_HAS_VBATT_SENSE
#error "board profile: BOARD_HAS_VBATT_SENSE is not defined (1 or 0)"
#endif
#ifndef BOARD_HAS_HS_VSENSE
#error "board profile: BOARD_HAS_HS_VSENSE is not defined (1 or 0)"
#endif
#ifndef BOARD_HAS_AUX_CAN
#error "board profile: BOARD_HAS_AUX_CAN is not defined (1 or 0)"
#endif
#ifndef BOARD_CAN_BUSES
#error "board profile: BOARD_CAN_BUSES is not defined"
#endif

#ifndef PIN_CAN0_TX
#error "board profile: PIN_CAN0_TX is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_CAN0_RX
#error "board profile: PIN_CAN0_RX is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_CAN0_SILENT
#error "board profile: PIN_CAN0_SILENT is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_CAN1_TX
#error "board profile: PIN_CAN1_TX is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_CAN1_RX
#error "board profile: PIN_CAN1_RX is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_CAN1_SILENT
#error "board profile: PIN_CAN1_SILENT is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_WS2812_LED
#error "board profile: PIN_WS2812_LED is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_HS_VOLTAGE_SENSE
#error "board profile: PIN_HS_VOLTAGE_SENSE is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_OBD_VOLTAGE_SENSE
#error                                                                         \
    "board profile: PIN_OBD_VOLTAGE_SENSE is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_BOARDID
#error "board profile: PIN_BOARDID is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_HS_OBD_6
#error "board profile: PIN_HS_OBD_6 is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_HS_OBD_9
#error "board profile: PIN_HS_OBD_9 is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_HS_OBD_11
#error "board profile: PIN_HS_OBD_11 is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_HS_OBD_12
#error "board profile: PIN_HS_OBD_12 is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_HS_OBD_13
#error "board profile: PIN_HS_OBD_13 is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_HS_OBD_14
#error "board profile: PIN_HS_OBD_14 is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_HS_BOOST_EN
#error "board profile: PIN_HS_BOOST_EN is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_HS_VOLTAGE_ADJUST
#error                                                                         \
    "board profile: PIN_HS_VOLTAGE_ADJUST is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_LS_OBD_9
#error "board profile: PIN_LS_OBD_9 is not defined (a pin, or PIN_NONE)"
#endif
#ifndef PIN_LS_OBD_15
#error "board profile: PIN_LS_OBD_15 is not defined (a pin, or PIN_NONE)"
#endif

/* A board with no J1850 and no K-Line fits CAN and nothing else. Shared code
 * refuses the other buses on such a board, and maps the refusal to "not
 * supported" where the protocol layer asks for it. */
#define BOARD_CAN_ONLY (!BOARD_HAS_J1850 && !BOARD_HAS_KLINE)
