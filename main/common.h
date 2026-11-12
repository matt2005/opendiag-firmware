/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "board_profile.h"

/* Product identity, reported by AT @1 and by the control plane's ID. */
#define OPENDIAG_MANUFACTURE "Maximus64"
#define OPENDIAG_PRODUCT "OpenDiag"
/* Hardware name from the board profile (BOARD_NAME). */
#define OPENDIAG_HARDWARE BOARD_NAME
