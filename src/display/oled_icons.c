/*
 * OLED status screen: pixel-exact 1-bit icons, as two small LVGL fonts that fall back
 * to Montserrat for every other character. The Font Awesome glyphs that come with the
 * Montserrat fonts are anti-aliased outlines; at 10-12 px on a 1-bit panel some of them
 * (Bluetooth especially) turn into blobs.
 *
 * Generated from the pixel art in scripts/oled_icons.py; edit that and rerun it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "oled_icons.h"

static const uint8_t glyph_bitmap[] = {
    0x7f, 0x40, 0x4f, 0x88, 0x21, 0xc0, 0x00, 0x20, 0x40, 0x52, 0x26, 0x92, 0xd7, 0x5a, 0x4b, 0x2a, 0x51, 0x10, 0x20, 0x0a, 0x02, 0x20,
};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}, /* reserved */
    /* U+E000 wireless (Wi-Fi style fan)
     *   .#######.
     *   #.......#
     *   ..#####..
     *   .#.....#.
     *   ...###...
     *   .........
     *   ....#....
     */
    {.bitmap_index = 0, .adv_w = 160, .box_w = 9, .box_h = 7, .ofs_x = 0, .ofs_y = 1},
    /* U+E001 radio mast sending waves
     *   .#.......#.
     *   #..#...#..#
     *   #.#..#..#.#
     *   #.#.###.#.#
     *   #.#..#..#.#
     *   #..#.#.#..#
     *   .#...#...#.
     *   .....#.....
     *   ....#.#....
     *   ...#...#...
     */
    {.bitmap_index = 8, .adv_w = 192, .box_w = 11, .box_h = 10, .ofs_x = 0, .ofs_y = -1},
};

static const lv_font_fmt_txt_cmap_t cmaps[] = {
    {.range_start = 0xE000, .range_length = 2, .glyph_id_start = 1,
     .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},
};

static const lv_font_fmt_txt_dsc_t icons_dsc = {
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .cmap_num = 1,
    .bpp = 1,
    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,
};


const lv_font_t oled_font_12 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 15,
    .base_line = 3,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -1,
    .underline_thickness = 1,
    .dsc = &icons_dsc,
    .fallback = &lv_font_montserrat_12,
};


const lv_font_t oled_font_10 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 11,
    .base_line = 2,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -1,
    .underline_thickness = 1,
    .dsc = &icons_dsc,
    .fallback = &lv_font_montserrat_10,
};
