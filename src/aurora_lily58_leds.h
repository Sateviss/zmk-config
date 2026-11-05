/*
 * Per-key LED layout of the splitkb Aurora Lily58 (rev1), per half.
 *
 * Each half has its own WS2812 chain: LEDs 0-4 underglow, then 29 per-key LEDs.
 * Entry i below is chain LED (ZMK_PRGB_UNDERGLOW_LEDS + i): the ZMK key position
 * (0-57, keymap order) of the key it sits under and its x/y on QMK's 224x64
 * rgb_matrix grid (global, so the right half's x continues from the left's).
 *
 * Derived from QMK keyboards/splitkb/aurora/lily58/rev1/keyboard.json and ZMK's
 * splitkb_aurora_lily58 matrix transform; scripts/check_led_map.py re-derives it
 * from both sources and checks this file against them.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

struct prgb_key_led {
    uint8_t position;
    uint8_t x;
    uint8_t y;
};

#define PRGB_KEY_LEDS 29

/* clang-format off */
#if IS_ENABLED(CONFIG_SHIELD_SPLITKB_AURORA_LILY58_LEFT)

static const struct prgb_key_led prgb_key_leds[PRGB_KEY_LEDS] = {
    /* LED 5-10: top row, outer -> inner */
    { 0,   0,  8}, { 1,  14,  6}, { 2,  29,  2}, { 3,  43,  0}, { 4,  58,  2}, { 5,  72,  4},
    /* LED 11-16 */
    {12,   0, 23}, {13,  14, 21}, {14,  29, 17}, {15,  43, 15}, {16,  58, 17}, {17,  72, 19},
    /* LED 17-22 */
    {24,   0, 38}, {25,  14, 36}, {26,  29, 32}, {27,  43, 30}, {28,  58, 32}, {29,  72, 34},
    /* LED 23-28 */
    {36,   0, 53}, {37,  14, 51}, {38,  29, 47}, {39,  43, 45}, {40,  58, 47}, {41,  72, 49},
    /* LED 29: inner key (encoder side) */
    {42,  87, 41},
    /* LED 30-33: thumbs, outer -> inner */
    {50,  36, 62}, {51,  51, 62}, {52,  65, 64}, {53,  87, 64},
};

#elif IS_ENABLED(CONFIG_SHIELD_SPLITKB_AURORA_LILY58_RIGHT)

static const struct prgb_key_led prgb_key_leds[PRGB_KEY_LEDS] = {
    /* LED 5-10: top row, inner -> outer */
    { 6, 152,  4}, { 7, 166,  2}, { 8, 181,  0}, { 9, 195,  2}, {10, 210,  6}, {11, 224,  8},
    /* LED 11-16 */
    {18, 152, 19}, {19, 166, 17}, {20, 181, 15}, {21, 195, 17}, {22, 210, 21}, {23, 224, 23},
    /* LED 17-22 */
    {30, 152, 34}, {31, 166, 32}, {32, 181, 30}, {33, 195, 32}, {34, 210, 36}, {35, 224, 38},
    /* LED 23: inner key (encoder side), comes before the bottom row on this half */
    {43, 137, 41},
    /* LED 24-29: bottom row, inner -> outer */
    {44, 152, 49}, {45, 166, 47}, {46, 181, 45}, {47, 195, 47}, {48, 210, 51}, {49, 224, 53},
    /* LED 30-33: thumbs, inner -> outer */
    {54, 137, 64}, {55, 159, 64}, {56, 173, 62}, {57, 188, 62},
};

#else
#error "Per-key RGB: no LED layout for this shield"
#endif
/* clang-format on */
