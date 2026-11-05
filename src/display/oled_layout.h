/*
 * OLED status screen: layout and drawing (plain LVGL, no ZMK dependencies, so it can
 * also be built on a PC to render a preview, see scripts/oled_preview/).
 *
 * The screen is laid out in portrait, OLED_LAYOUT_W x OLED_LAYOUT_H (the 128x32 panel
 * turned by 90 degrees). Every setter only touches LVGL when the shown text actually
 * changes, so an update redraws one small area, not the whole screen.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

#define OLED_LAYOUT_W 32
#define OLED_LAYOUT_H 128

enum oled_conn_kind {
    OLED_CONN_NONE,  /* central: no output selected / available */
    OLED_CONN_USB,   /* central: USB output */
    OLED_CONN_BLE,   /* central: BLE profile */
    OLED_CONN_SPLIT, /* peripheral: link to the central half */
};

struct oled_conn {
    enum oled_conn_kind kind;
    bool connected;
    bool bonded;     /* BLE: profile has a bond (otherwise it is open / advertising) */
    uint8_t profile; /* BLE: 0-based profile index */
};

struct oled_lighting {
    bool present; /* lighting module built in */
    bool on;
    uint8_t brt;      /* 0-100 */
    uint8_t effect;   /* effect number, shown when it has no name */
    const char *name; /* effect name, may be NULL */
};

/* Create the widgets on `screen`. `central` adds the layer line (only the central
 * half knows the active layer). */
void oled_layout_create(lv_obj_t *screen, bool central);

void oled_layout_set_conn(const struct oled_conn *conn);

/* est_minutes: battery time left, OLED_EST_PENDING (-1) while not known yet, or
 * OLED_EST_NONE to show nothing; not shown while charging. */
#define OLED_EST_PENDING (-1)
#define OLED_EST_NONE (-2)

/* mv < 0: no reading yet. charging: USB power present (central only). */
void oled_layout_set_battery(int mv, bool charging, int est_minutes);

/* name may be NULL or empty: the layer number is shown instead. */
void oled_layout_set_layer(const char *name, uint8_t index);

void oled_layout_set_lighting(const struct oled_lighting *lighting);

/*
 * Rotate a 1 bit per pixel bitmap (LVGL I1 without its palette: rows of (w + 7) / 8
 * bytes, most significant bit = leftmost pixel) of w x h pixels by 90 degrees into
 * dst, which gets h x w pixels in the same format (rows of (h + 7) / 8 bytes).
 * cw: rotate clockwise (the source's top edge becomes the right edge).
 */
void oled_rotate_i1(const uint8_t *src, uint8_t *dst, int32_t w, int32_t h, bool cw);
