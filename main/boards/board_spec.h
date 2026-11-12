/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/*
 * The board spec: what a board profile (main/boards/<name>.h) must define.
 *
 * A profile defines these names, and the shared code uses nothing else about
 * the board:
 *
 *   BOARD_ID              lowercase id, e.g. stock. CMake finds the profile by
 *                         it, and builds boards/board_<id>.c.
 *   BOARD_NAME            hardware name for the banner and AT @1, e.g. "rev1".
 *   BOARD_HAS_<FEATURE>   1 if the board has the feature, 0 if not. Shared code
 *                         tests these, never the board id. The features are
 *                         J1850, KLINE, HS_LS_DRIVERS, BOOST, WS2812,
 *                         VBATT_SENSE, HS_VSENSE, AUX_CAN.
 *   BOARD_CAN_BUSES       number of CAN transceivers fitted.
 *   PIN_<NAME>            every pin the shared code names. A pin the board
 *                         does not have is PIN_NONE, not a made-up number.
 *
 * board_check.h lists the names a profile must define and fails the build if
 * one is missing. It also derives BOARD_CAN_ONLY from the features.
 */

/* A pin the board does not have. */
#define PIN_NONE (-1)

/* Low side pin 9 (Honda SCS) needs a hardware rework that fits a low side
 * driver on GPIO16. It is a build option rather than a board property, so its
 * default lives here and applies to every board. */
#ifndef OPENDIAG_HW_LS_OBD_9
#define OPENDIAG_HW_LS_OBD_9 0
#endif
