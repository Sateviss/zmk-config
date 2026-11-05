"""Generate src/display/oled_icons.c and oled_icons.h from the pixel art below.

usage: python3 scripts/oled_icons.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_C = os.path.join(HERE, "..", "src", "display", "oled_icons.c")
OUT_H = os.path.join(HERE, "..", "src", "display", "oled_icons.h")
ICONS = [  # (codepoint, name, comment, y offset of the bottom row from the baseline, rows)
    (0xE000, "WIFI", "wireless (Wi-Fi style fan)", 1, [
        ".#######.",
        "#.......#",
        "..#####..",
        ".#.....#.",
        "...###...",
        ".........",
        "....#....",
    ]),
    (0xE001, "RADIO", "radio mast sending waves", -1, [
        ".#.......#.",
        "#..#...#..#",
        "#.#..#..#.#",
        "#.#.###.#.#",
        "#.#..#..#.#",
        "#..#.#.#..#",
        ".#...#...#.",
        ".....#.....",
        "....#.#....",
        "...#...#...",
    ]),
]

def pack(rows):
    bits = "".join(r.replace("#", "1").replace(".", "0") for r in rows)
    bits += "0" * (-len(bits) % 8)
    return [int(bits[i:i + 8], 2) for i in range(0, len(bits), 8)]

def font(name, base, line_height, base_line, cap):
    return f"""
const lv_font_t {name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_height},
    .base_line = {base_line},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -1,
    .underline_thickness = 1,
    .dsc = &{cap},
    .fallback = &{base},
}};
"""

out = []
out.append("""/*
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
""")
bitmap = []
dsc = ['    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}, /* reserved */']
for cp, name, comment, ofs_y, rows in ICONS:
    w, h = len(rows[0]), len(rows)
    assert all(len(r) == w for r in rows)
    idx = len(bitmap)
    data = pack(rows)
    bitmap += data
    art = "\n".join(f"     *   {r}" for r in rows)
    # 1 px space after the icon
    dsc.append(f"    /* U+{cp:04X} {comment}\n{art}\n     */\n"
               f"    {{.bitmap_index = {idx}, .adv_w = {(w + 1) * 16}, .box_w = {w}, .box_h = {h}, .ofs_x = 0, .ofs_y = {ofs_y}}},")
first = ICONS[0][0]
assert [c for c, *_ in ICONS] == list(range(first, first + len(ICONS)))
out.append("static const uint8_t glyph_bitmap[] = {\n    " +
           ", ".join(f"0x{b:02x}" for b in bitmap) + ",\n};\n")
out.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n" + "\n".join(dsc) + "\n};\n")
out.append(f"""static const lv_font_fmt_txt_cmap_t cmaps[] = {{
    {{.range_start = 0x{first:04X}, .range_length = {len(ICONS)}, .glyph_id_start = 1,
     .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}},
}};

static const lv_font_fmt_txt_dsc_t icons_dsc = {{
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .cmap_num = 1,
    .bpp = 1,
    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,
}};
""")
# line heights / baselines copied from LVGL's Montserrat 12 and 10
out.append(font("oled_font_12", "lv_font_montserrat_12", 15, 3, "icons_dsc"))
out.append(font("oled_font_10", "lv_font_montserrat_10", 11, 2, "icons_dsc"))
open(OUT_C, "w").write("\n".join(out))

hdr = ["/*\n * OLED status screen: 1-bit icons (see oled_icons.c).\n *\n * SPDX-License-Identifier: MIT\n */\n",
       "#pragma once\n", "#include <lvgl.h>\n",
       "/* Montserrat 12 / 10 plus the icons below. */",
       "extern const lv_font_t oled_font_12;", "extern const lv_font_t oled_font_10;\n"]
for cp, name, comment, ofs_y, rows in ICONS:
    b = chr(cp).encode()
    hdr.append(f"#define OLED_ICON_{name} \"" + "".join(f"\\x{x:02X}" for x in b) + f"\" /* {comment} */")
open(OUT_H, "w").write("\n".join(hdr) + "\n")
