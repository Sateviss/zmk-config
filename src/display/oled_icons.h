/*
 * OLED status screen: 1-bit icons (see oled_icons.c).
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>

/* Montserrat 12 / 10 plus the icons below. */
extern const lv_font_t oled_font_12;
extern const lv_font_t oled_font_10;

#define OLED_ICON_WIFI "\xEE\x80\x80" /* wireless (Wi-Fi style fan) */
#define OLED_ICON_RADIO "\xEE\x80\x81" /* radio mast sending waves */
