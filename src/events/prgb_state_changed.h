/*
 * Raised (on each half) whenever the per-key RGB state changes: an &prgb command, or
 * the saved state being loaded at boot. Used by the OLED status screen.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/event_manager.h>

#include "../prgb.h"

struct zmk_prgb_state_changed {
    struct prgb_status status;
};

ZMK_EVENT_DECLARE(zmk_prgb_state_changed);
