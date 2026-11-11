/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Product identity, reported by AT @1 and by the control plane's ID. */
#define OPENDIAG_MANUFACTURE "Maximus64"
#define OPENDIAG_PRODUCT "OpenDiag"
#ifdef OPENDIAG_BOARD_REJSACAN
#define OPENDIAG_HARDWARE "RejsaCAN v3.4"
#else
#define OPENDIAG_HARDWARE "rev1"
#endif
