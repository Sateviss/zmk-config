/*
 * Per-key RGB lighting: commands for the &prgb behavior.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Effects */
#define PRGB_EFF_STATIC 0   /* underglow colour + key colour */
#define PRGB_EFF_BREATHE 1  /* underglow breathes, keys static */
#define PRGB_EFF_REACTIVE 2 /* a pressed key lights in the accent colour and fades out */
#define PRGB_EFF_RIPPLE 3   /* like reactive, plus a ring spreading out from the key */
#define PRGB_EFF_RAINBOW 4  /* keys: rainbow across the board by x position */
/* Added after the effects above so their numbers (saved, and in PRGB_EFF(e)) stay. */
#define PRGB_EFF_HEATMAP 5 /* presses warm keys (and neighbours) blue -> green -> red */
#define PRGB_EFF_COUNT 6

/* Relative commands (use these in a keymap) */
#define PRGB_TOG_CMD 0
#define PRGB_ON_CMD 1
#define PRGB_OFF_CMD 2
#define PRGB_EFF_NEXT_CMD 3
#define PRGB_EFF_PREV_CMD 4
#define PRGB_BRI_UP_CMD 5
#define PRGB_BRI_DN_CMD 6
#define PRGB_SPD_UP_CMD 7
#define PRGB_SPD_DN_CMD 8
#define PRGB_UG_HUE_UP_CMD 9
#define PRGB_UG_HUE_DN_CMD 10
#define PRGB_UG_SAT_UP_CMD 11
#define PRGB_UG_SAT_DN_CMD 12
#define PRGB_KEY_HUE_UP_CMD 13
#define PRGB_KEY_HUE_DN_CMD 14
#define PRGB_KEY_SAT_UP_CMD 15
#define PRGB_KEY_SAT_DN_CMD 16

/*
 * Absolute commands (value in param2). The central converts the relative commands
 * above into these before running them on both halves, so the halves stay in sync.
 */
#define PRGB_EFF_SET_CMD 17   /* param2: effect */
#define PRGB_BRT_SET_CMD 18   /* param2: brightness 0-100 (% of the configured max) */
#define PRGB_SPD_SET_CMD 19   /* param2: speed 1-5 */
#define PRGB_UG_COLOR_CMD 20  /* param2: PRGB_HS(hue 0-359, saturation 0-100) */
#define PRGB_KEY_COLOR_CMD 21 /* param2: PRGB_HS(hue 0-359, saturation 0-100) */

/*
 * Reactive/ripple background: what the keys rest at between presses, as a dim tint of
 * the accent (underglow) colour, in % of the brightness. 0 (default) = keys dark.
 * Added after the commands above so their numbers stay the same.
 */
#define PRGB_BG_NEXT_CMD 22 /* relative: next level of 0, 5, 10, 15, 25 %, then back to 0 */
#define PRGB_BG_SET_CMD 23  /* absolute, param2: background 0-100 */

/*
 * Indicators (added after the commands above so their numbers stay the same).
 * Layer key map: while a layer above the default one is active, keys light by what is
 * bound to them on that layer (and the Bluetooth/output keys by connection state), the
 * underglow takes the layer's tint; back on the default layer the effect resumes.
 * Underglow pulse: the underglow rests at half its level and flashes up to full on each
 * key press of that half, in every effect.
 */
#define PRGB_MAP_TOG_CMD 24   /* relative: layer key map on/off */
#define PRGB_PULSE_TOG_CMD 25 /* relative: underglow pulse on/off */
#define PRGB_MAP_SET_CMD 26   /* absolute, param2: 0 off, 1 on */
#define PRGB_PULSE_SET_CMD 27 /* absolute, param2: 0 off, 1 on */
/* Internal: the central relays the indicators to the peripheral with this command in
 * the low byte of param1 (plane number above it, see src/prgb_ind.h). Not for keymaps. */
#define PRGB_RELAY_CMD 28

#define PRGB_HS(h, s) (((h) << 8) | (s))

#define PRGB_TOG PRGB_TOG_CMD 0
#define PRGB_ON PRGB_ON_CMD 0
#define PRGB_OFF PRGB_OFF_CMD 0
#define PRGB_EFF_NEXT PRGB_EFF_NEXT_CMD 0
#define PRGB_EFF_PREV PRGB_EFF_PREV_CMD 0
#define PRGB_BRI_UP PRGB_BRI_UP_CMD 0
#define PRGB_BRI_DN PRGB_BRI_DN_CMD 0
#define PRGB_SPD_UP PRGB_SPD_UP_CMD 0
#define PRGB_SPD_DN PRGB_SPD_DN_CMD 0
#define PRGB_UG_HUE_UP PRGB_UG_HUE_UP_CMD 0
#define PRGB_UG_HUE_DN PRGB_UG_HUE_DN_CMD 0
#define PRGB_UG_SAT_UP PRGB_UG_SAT_UP_CMD 0
#define PRGB_UG_SAT_DN PRGB_UG_SAT_DN_CMD 0
#define PRGB_KEY_HUE_UP PRGB_KEY_HUE_UP_CMD 0
#define PRGB_KEY_HUE_DN PRGB_KEY_HUE_DN_CMD 0
#define PRGB_KEY_SAT_UP PRGB_KEY_SAT_UP_CMD 0
#define PRGB_KEY_SAT_DN PRGB_KEY_SAT_DN_CMD 0
#define PRGB_BG_NEXT PRGB_BG_NEXT_CMD 0
#define PRGB_MAP_TOG PRGB_MAP_TOG_CMD 0
#define PRGB_PULSE_TOG PRGB_PULSE_TOG_CMD 0

#define PRGB_EFF(e) PRGB_EFF_SET_CMD e
#define PRGB_UG_COLOR(h, s) PRGB_UG_COLOR_CMD PRGB_HS(h, s)
#define PRGB_KEY_COLOR(h, s) PRGB_KEY_COLOR_CMD PRGB_HS(h, s)
#define PRGB_BG(pct) PRGB_BG_SET_CMD pct
#define PRGB_MAP(on) PRGB_MAP_SET_CMD on
#define PRGB_PULSE(on) PRGB_PULSE_SET_CMD on
