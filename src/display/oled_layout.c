/*
 * OLED status screen: layout and drawing (see oled_layout.h).
 *
 * Portrait, 32 x 128, top to bottom:
 *
 *   connection   central: BLE profile + state, or USB; peripheral: link to the central
 *   ----
 *   battery      icon (+ charge bolt while on USB power, central only)
 *   voltage      e.g. 3.92V
 *   ----
 *   layer        central only: active layer name
 *   ----
 *   effect       lighting effect name (up to two lines), or "Off"
 *   brightness   e.g. 80%
 *
 * 32 px fits about five letters of the 12 px font. A name that is too wide is shown
 * in the 10 px font; an effect name still too wide is split over two lines ("Rain" /
 * "bow"); anything else is cut to what fits. So names need no abbreviation tables.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "oled_icons.h"
#include "oled_layout.h"

#define W OLED_LAYOUT_W

#define FONT_BIG (&oled_font_12)   /* Montserrat 12 + oled_icons */
#define FONT_SMALL (&oled_font_10) /* Montserrat 10 + oled_icons */
#define FONT_UNIT (&lv_font_montserrat_8)

/* Connection icon (BLE profile, split link). */
#define OLED_ICON_CONN OLED_ICON_WIFI /* or OLED_ICON_RADIO */

#define LINE_H 15 /* line height of FONT_BIG */
#define GAP 2     /* space around a rule */

enum fit_mode {
    FIT_ONE_LINE, /* big, else small, else cut */
    FIT_TWO_LINES /* big, else small, else split in two lines, else cut */
};

struct line {
    lv_obj_t *label;
    int16_t y;
    enum fit_mode mode;
};

static struct line conn, batt_icon, layer, fx_name, fx_brt;
static lv_obj_t *volt_num, *volt_unit; /* "3.92" in FONT_BIG + "V" in FONT_UNIT */
static int16_t volt_y;
static bool created;

/* ---- helpers ------------------------------------------------------------------------ */

static int32_t text_w(const char *text, uint32_t len, const lv_font_t *font) {
    return lv_text_get_width(text, len, font, 0);
}

/* Length in bytes of the longest prefix of text (whole UTF-8 characters) that fits. */
static uint32_t fitting_len(const char *text, const lv_font_t *font) {
    uint32_t len = strlen(text);

    while (len > 0 && text_w(text, len, font) > W) {
        do {
            len--;
        } while (len > 0 && (text[len] & 0xC0) == 0x80);
    }
    return len;
}

static void set_font(lv_obj_t *label, const lv_font_t *font) {
    if (lv_obj_get_style_text_font(label, LV_PART_MAIN) != font) {
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }
}

static void set_text(lv_obj_t *label, const char *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font) {
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_label_set_text_static(label, "");
    return label;
}

static void rule(lv_obj_t *parent, int32_t y) {
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, W - 8, 1);
    lv_obj_set_pos(obj, 4, y);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_obj_get_style_text_color(parent, LV_PART_MAIN),
                              LV_PART_MAIN);
}

/* Create a centred text line at y; returns the y below it. */
static int16_t line_create(struct line *line, lv_obj_t *parent, int16_t y, enum fit_mode mode) {
    int16_t h = mode == FIT_TWO_LINES ? 2 * LINE_H : LINE_H;

    line->y = y;
    line->mode = mode;
    line->label = label_create(parent, FONT_BIG);
    lv_obj_set_size(line->label, W, h);
    lv_obj_set_style_text_align(line->label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(line->label, 0, y);
    return y + h;
}

/* Show text in the biggest form that fits the width (see the top of this file). Does
 * nothing if the text is unchanged, so repeated updates redraw nothing. */
static void line_set(struct line *line, const char *text) {
    char shown[40];
    const lv_font_t *font = FONT_BIG;
    uint32_t len = strlen(text);

    if (!created || line->label == NULL) {
        return;
    }

    snprintf(shown, sizeof(shown), "%s", text);

    if (text_w(text, len, FONT_BIG) > W) {
        /* two halves, the first one longer: "Rain" / "bow", "Reac" / "tive" */
        uint32_t half = (len + 1) / 2;

        while (half < len && (text[half] & 0xC0) == 0x80) {
            half++;
        }

        if (text_w(text, len, FONT_SMALL) <= W) {
            font = FONT_SMALL;
        } else if (line->mode == FIT_TWO_LINES && text_w(text, half, FONT_BIG) <= W &&
                   text_w(text + half, len - half, FONT_BIG) <= W) {
            snprintf(shown, sizeof(shown), "%.*s\n%s", (int)half, text, text + half);
        } else {
            font = FONT_SMALL;
            snprintf(shown, sizeof(shown), "%.*s", (int)fitting_len(text, font), text);
        }
    }

    /* keep the smaller font vertically centred on the line, and a single line centred
     * in a two line slot */
    int32_t dy = font == FONT_BIG ? 0 : (LINE_H - lv_font_get_line_height(font)) / 2;
    if (line->mode == FIT_TWO_LINES && strchr(shown, '\n') == NULL) {
        dy += LINE_H / 2;
    }
    if (lv_obj_get_y(line->label) != line->y + dy) {
        lv_obj_set_y(line->label, line->y + dy);
    }
    set_font(line->label, font);
    set_text(line->label, shown);
}

/* ---- API ---------------------------------------------------------------------------- */

void oled_layout_create(lv_obj_t *screen, bool central) {
    int16_t y = 1;

    created = false;
    layer.label = NULL;

    y = line_create(&conn, screen, y, FIT_ONE_LINE);
    rule(screen, y + GAP);
    y += 2 * GAP + 1;

    y = line_create(&batt_icon, screen, y, FIT_ONE_LINE);
    volt_y = y;
    volt_num = label_create(screen, FONT_BIG);
    volt_unit = label_create(screen, FONT_UNIT);
    lv_label_set_text_static(volt_unit, "V");
    y += LINE_H;
    rule(screen, y + GAP);
    y += 2 * GAP + 1;

    if (central) {
        y = line_create(&layer, screen, y, FIT_ONE_LINE);
        rule(screen, y + GAP);
        y += 2 * GAP + 1;
    }

    y = line_create(&fx_name, screen, y, FIT_TWO_LINES);
    line_create(&fx_brt, screen, y, FIT_ONE_LINE);

    created = true;
    oled_layout_set_battery(-1, false);
}

void oled_layout_set_conn(const struct oled_conn *c) {
    char text[24];

    switch (c->kind) {
    case OLED_CONN_USB:
        snprintf(text, sizeof(text), "%s", c->connected ? LV_SYMBOL_USB : LV_SYMBOL_USB "?");
        break;
    case OLED_CONN_BLE:
        snprintf(text, sizeof(text), OLED_ICON_CONN "%u%s", c->profile + 1,
                 !c->bonded ? LV_SYMBOL_SETTINGS
                            : (c->connected ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE));
        break;
    case OLED_CONN_SPLIT:
        snprintf(text, sizeof(text), OLED_ICON_CONN "%s",
                 c->connected ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
        break;
    case OLED_CONN_NONE:
    default:
        snprintf(text, sizeof(text), "%s", LV_SYMBOL_CLOSE);
        break;
    }
    line_set(&conn, text);
}

/* Rough Li-ion levels for the icon; the voltage below it is the real information. */
static const char *battery_symbol(int mv) {
    if (mv >= 4000) {
        return LV_SYMBOL_BATTERY_FULL;
    } else if (mv >= 3850) {
        return LV_SYMBOL_BATTERY_3;
    } else if (mv >= 3700) {
        return LV_SYMBOL_BATTERY_2;
    } else if (mv >= 3550) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

void oled_layout_set_battery(int mv, bool charging) {
    char text[16];

    if (!created) {
        return;
    }

    snprintf(text, sizeof(text), "%s%s", mv < 0 ? LV_SYMBOL_BATTERY_EMPTY : battery_symbol(mv),
             charging ? LV_SYMBOL_CHARGE : "");
    line_set(&batt_icon, text);

    if (mv < 0) {
        snprintf(text, sizeof(text), "-.--");
    } else {
        int cv = (mv + 5) / 10; /* round to 10 mV */
        snprintf(text, sizeof(text), "%d.%02d", cv / 100, cv % 100);
    }
    if (strcmp(lv_label_get_text(volt_num), text) == 0) {
        return;
    }
    lv_label_set_text(volt_num, text);

    /* "3.92" + small "V", centred together and sharing the baseline */
    int32_t num_w = text_w(text, strlen(text), FONT_BIG);
    int32_t unit_w = text_w("V", 1, FONT_UNIT);
    int32_t x = (W - num_w - unit_w) / 2;
    int32_t base = LINE_H - FONT_BIG->base_line; /* baseline, from the top of the line */
    int32_t unit_x = x + num_w;

    if (x < 0) {
        x = 0;
    }
    if (unit_x > W - unit_w) {
        unit_x = W - unit_w;
    }
    lv_obj_set_pos(volt_num, x, volt_y);
    lv_obj_set_pos(volt_unit, unit_x,
                   volt_y + base - (lv_font_get_line_height(FONT_UNIT) - FONT_UNIT->base_line));
}

void oled_layout_set_layer(const char *name, uint8_t index) {
    char text[16];

    if (name == NULL || name[0] == '\0') {
        snprintf(text, sizeof(text), "L%u", index);
        name = text;
    }
    line_set(&layer, name);
}

void oled_layout_set_lighting(const struct oled_lighting *l) {
    char text[16];

    if (!l->present) {
        line_set(&fx_name, "");
        line_set(&fx_brt, "");
    } else if (!l->on) {
        line_set(&fx_name, "Off");
        line_set(&fx_brt, "");
    } else {
        if (l->name != NULL && l->name[0] != '\0') {
            line_set(&fx_name, l->name);
        } else {
            snprintf(text, sizeof(text), "FX%u", l->effect);
            line_set(&fx_name, text);
        }
        snprintf(text, sizeof(text), "%u%%", l->brt);
        line_set(&fx_brt, text);
    }
}

void oled_rotate_i1(const uint8_t *src, uint8_t *dst, int32_t w, int32_t h, bool cw) {
    const int32_t src_stride = (w + 7) / 8;
    const int32_t dst_stride = (h + 7) / 8;

    memset(dst, 0, (size_t)dst_stride * w);

    for (int32_t y = 0; y < h; y++) {
        const uint8_t *row = src + y * src_stride;

        for (int32_t x = 0; x < w; x++) {
            if (!(row[x / 8] & (0x80 >> (x % 8)))) {
                continue;
            }
            /* cw: (x, y) -> (h - 1 - y, x); ccw: (x, y) -> (y, w - 1 - x) */
            int32_t dx = cw ? h - 1 - y : y;
            int32_t dy = cw ? x : w - 1 - x;

            dst[dy * dst_stride + dx / 8] |= 0x80 >> (dx % 8);
        }
    }
}
