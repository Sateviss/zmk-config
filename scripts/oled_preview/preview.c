/*
 * Host preview of the OLED status screen: renders src/display/oled_layout.c with the
 * real LVGL (the one ZMK builds with) through the same rotate-on-flush path as the
 * firmware, into the 128x32 panel's frame buffer, and writes PBM images of it.
 * Build and run with run.sh.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "oled_layout.h"

#define PANEL_W 128
#define PANEL_H 32

static uint8_t panel[PANEL_H][PANEL_W]; /* LVGL I1 bits: 1 = white */
static uint8_t rot_buf[PANEL_W * PANEL_H / 8];
static bool cw = true;

/* Like Zephyr's lvgl_rounder_cb_mono for a vertically tiled panel (SSD1306). */
static void rounder_cb(lv_event_t *e) {
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~7;
    a->x2 |= 7;
    a->y1 &= ~7;
    a->y2 |= 7;
}

/* What the firmware's flush wrapper does, then "display_write" into panel[][] */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    int32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
    lv_area_t phys = *area;

    lv_display_rotate_area(disp, &phys);
    oled_rotate_i1(px_map + 8, rot_buf, w, h, cw);

    int32_t pw = lv_area_get_width(&phys), ph = lv_area_get_height(&phys);
    if (pw != h || ph != w || (phys.y1 % 8) || (ph % 8)) {
        fprintf(stderr, "bad area\n");
    }
    for (int32_t y = 0; y < ph; y++) {
        for (int32_t x = 0; x < pw; x++) {
            panel[phys.y1 + y][phys.x1 + x] = (rot_buf[y * ((pw + 7) / 8) + x / 8] >> (7 - x % 8)) & 1;
        }
    }
    lv_display_flush_ready(disp);
}

static void write_pbm(const char *path) {
    FILE *f = fopen(path, "w");
    fprintf(f, "P1\n%d %d\n", PANEL_W, PANEL_H);
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            fputc(panel[y][x] ? '0' : '1', f); /* PBM: 1 = black */
        }
        fputc('\n', f);
    }
    fclose(f);
}

struct scenario {
    const char *file;
    bool central;
    struct oled_conn conn;
    int mv;
    bool charging;
    const char *layer;
    struct oled_lighting light;
};

static const struct scenario scenarios[] = {
    {"left-ble", true, {OLED_CONN_BLE, true, true, 0}, 3921, false, "Base",
     {true, true, 80, 1, "Breathe"}},
    {"left-usb", true, {OLED_CONN_USB, true, true, 0}, 4153, true, "Control",
     {true, true, 100, 2, "Reactive"}},
    {"left-open", true, {OLED_CONN_BLE, false, false, 2}, 3688, false, "Extra",
     {true, true, 40, 3, "Ripple"}},
    {"right-on", false, {OLED_CONN_SPLIT, true, false, 0}, 4196, false, NULL,
     {true, true, 100, 4, "Rainbow"}},
    {"right-off", false, {OLED_CONN_SPLIT, false, false, 0}, 3512, false, NULL,
     {true, false, 50, 0, "Static"}},
    {"right-long", false, {OLED_CONN_SPLIT, true, false, 0}, 4012, false, NULL,
     {true, true, 70, 5, "Gradient"}},
    {"left-static", true, {OLED_CONN_BLE, true, true, 4}, 3800, false, "Base",
     {true, true, 5, 0, "Static"}},
};

int main(int argc, char **argv) {
    static uint8_t buf[8 + PANEL_W * PANEL_H * 64 / 100 / 8];
    const char *out = argc > 1 ? argv[1] : ".";

    if (argc > 2 && strcmp(argv[2], "ccw") == 0) {
        cw = false;
    }

    lv_init();
    lv_display_t *disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, disp);
    lv_display_set_rotation(disp, cw ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_90);
    lv_display_set_theme(disp, lv_theme_mono_init(disp, false, &lv_font_unscii_8,
                                                  &lv_font_montserrat_12,
                                                  &lv_font_montserrat_12));

    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        const struct scenario *s = &scenarios[i];
        lv_obj_t *old = lv_screen_active();
        lv_obj_t *scr = lv_obj_create(NULL);

        oled_layout_create(scr, s->central);
        lv_screen_load(scr);
        if (old) {
            lv_obj_delete(old);
        }
        oled_layout_set_conn(&s->conn);
        oled_layout_set_battery(s->mv, s->charging);
        if (s->central) {
            oled_layout_set_layer(s->layer, 0);
        }
        oled_layout_set_lighting(&s->light);

        memset(panel, 0, sizeof(panel));
        lv_obj_invalidate(scr);
        lv_refr_now(disp);

        char path[256];
        snprintf(path, sizeof(path), "%s/%s.pbm", out, s->file);
        write_pbm(path);
    }
    return 0;
}
