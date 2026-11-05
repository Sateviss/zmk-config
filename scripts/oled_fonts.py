"""Generate the OLED status screen fonts: src/display/oled_fonts.c and oled_fonts.h.

usage: python3 scripts/oled_fonts.py <lvgl source dir>
       (e.g. <west workspace>/modules/lib/gui/lvgl; needs Python with Pillow)

The panels are 1-bit. LVGL's built-in Montserrat fonts are 4-bit anti-aliased, and
LVGL turns every pixel that is at least half covered on, so strokes come out 2-3 px
thick and small letters and symbols run together. These fonts are rendered 1-bit
instead, by FreeType with hinting (Pillow's "1" mode), from the same Montserrat Medium
and Font Awesome files LVGL builds its fonts from: strokes are mostly 1 px wide.

Each font has ASCII, the LV_SYMBOL_* glyphs the screen uses (same code points, so the
LV_SYMBOL_* macros keep working) and the pixel-art icons below (OLED_ICON_* in the
header). Line heights and baselines match LVGL's Montserrat of the same size, so the
layout is unchanged.
"""
import os
import sys

from PIL import ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_C = os.path.join(HERE, "..", "src", "display", "oled_fonts.c")
OUT_H = os.path.join(HERE, "..", "src", "display", "oled_fonts.h")

# (name, px size, line height, base line): the values of LVGL's lv_font_montserrat_<size>
FONTS = [("oled_font_12", 12, 15, 3), ("oled_font_10", 10, 11, 2), ("oled_font_8", 8, 10, 2)]

TEXT = range(0x20, 0x7F)

# Font Awesome glyphs, as LV_SYMBOL_* in lv_symbol_def.h
SYMBOLS = {
    0xF00C: "OK",
    0xF00D: "CLOSE",
    0xF013: "SETTINGS",
    0xF0E7: "CHARGE",
    0xF240: "BATTERY_FULL",
    0xF241: "BATTERY_3",
    0xF242: "BATTERY_2",
    0xF243: "BATTERY_1",
    0xF244: "BATTERY_EMPTY",
    0xF287: "USB",
}

# Pixel-art icons: code point, name, comment, y of the bottom row relative to the
# baseline (up is positive), rows. The same art is used at every size.
ICONS = [
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


def render(font, cp):
    """1-bit glyph: (rows of '#'/'.', ofs_x, ofs_y from the baseline, advance in px)."""
    ch = chr(cp)
    adv = round(font.getlength(ch, mode="1"))
    mask, (ox, oy) = font.getmask2(ch, mode="1", anchor="ls")
    w, h = mask.size
    rows = ["".join("#" if mask.getpixel((x, y)) else "." for x in range(w)) for y in range(h)]
    # trim empty rows and columns
    while rows and "#" not in rows[0]:
        rows.pop(0)
        oy += 1
    while rows and "#" not in rows[-1]:
        rows.pop()
    if not rows:
        return [], 0, 0, adv
    while all(r[0] == "." for r in rows):
        rows = [r[1:] for r in rows]
        ox += 1
    while all(r[-1] == "." for r in rows):
        rows = [r[:-1] for r in rows]
    return rows, ox, -(oy + len(rows)), adv


def pack(rows):
    """LVGL plain 1 bpp: pixels packed row after row with no padding."""
    bits = "".join(r.replace("#", "1").replace(".", "0") for r in rows)
    bits += "0" * (-len(bits) % 8)
    return [int(bits[i:i + 8], 2) for i in range(0, len(bits), 8)]


def c_font(name, size, line_height, base_line, text_font, sym_font):
    glyphs = []  # (code point, rows, ofs_x, ofs_y, advance)
    for cp in TEXT:
        glyphs.append((cp, *render(text_font, cp)))
    for cp, _, _, ofs_y, rows in ICONS:
        glyphs.append((cp, rows, 0, ofs_y, len(rows[0]) + 1))
    for cp in SYMBOLS:
        # 1 px more on the left, so a symbol doesn't touch the digit before it ("1" + OK)
        rows, ofs_x, ofs_y, adv = render(sym_font, cp)
        glyphs.append((cp, rows, ofs_x + 1, ofs_y, adv + 1))

    top, bottom = line_height - base_line, -base_line
    for cp, rows, _, ofs_y, _ in glyphs:
        if rows and (ofs_y + len(rows) > top or ofs_y < bottom):
            print(f"{name}: U+{cp:04X} sticks out of the line ({ofs_y}..{ofs_y + len(rows)})",
                  file=sys.stderr)

    bitmap, dsc = [], []
    for cp, rows, ofs_x, ofs_y, adv in glyphs:
        dsc.append(f"    {{.bitmap_index = {len(bitmap)}, .adv_w = {adv * 16}, "
                   f".box_w = {len(rows[0]) if rows else 0}, .box_h = {len(rows)}, "
                   f".ofs_x = {ofs_x}, .ofs_y = {ofs_y}}}, /* U+{cp:04X} */")
        bitmap += pack(rows)

    n_text = len(TEXT)
    sparse = [cp for cp, *_ in glyphs[n_text:]]
    p = name  # prefix for the static tables
    lines = [f"/* ---- {name}: {size} px ---- */\n"]
    lines.append(f"static const uint8_t {p}_bitmap[] = {{")
    for i in range(0, len(bitmap), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in bitmap[i:i + 16]) + ",")
    lines.append("};\n")
    lines.append(f"static const lv_font_fmt_txt_glyph_dsc_t {p}_glyph_dsc[] = {{")
    lines.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, "
                 ".ofs_y = 0}, /* id 0: reserved */")
    lines += dsc
    lines.append("};\n")
    lines.append(f"static const uint16_t {p}_sparse[] = {{")
    lines.append("    " + ", ".join(f"0x{cp - sparse[0]:04x}" for cp in sparse) + ",")
    lines.append("};\n")
    lines.append(f"""static const lv_font_fmt_txt_cmap_t {p}_cmaps[] = {{
    {{.range_start = 0x{TEXT.start:04X}, .range_length = {n_text}, .glyph_id_start = 1,
     .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}},
    {{.range_start = 0x{sparse[0]:04X}, .range_length = {sparse[-1] - sparse[0] + 1},
     .glyph_id_start = {n_text + 1}, .unicode_list = {p}_sparse,
     .list_length = {len(sparse)}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}},
}};

static const lv_font_fmt_txt_dsc_t {p}_dsc = {{
    .glyph_bitmap = {p}_bitmap,
    .glyph_dsc = {p}_glyph_dsc,
    .cmaps = {p}_cmaps,
    .cmap_num = 2,
    .bpp = 1,
    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,
}};

const lv_font_t {name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_height},
    .base_line = {base_line},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -1,
    .underline_thickness = 1,
    .dsc = &{p}_dsc,
}};
""")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = os.path.join(sys.argv[1], "scripts", "built_in_font")
    text_ttf = os.path.join(src, "Montserrat-Medium.ttf")
    sym_ttf = os.path.join(src, "FontAwesome5-Solid+Brands+Regular.woff")

    for cp, _, _, _, rows in ICONS:
        assert all(len(r) == len(rows[0]) for r in rows), f"U+{cp:04X}: ragged rows"

    out = ["""/*
 * OLED status screen fonts: 1-bit Montserrat Medium + Font Awesome symbols + pixel-art
 * icons, at 12, 10 and 8 px. GENERATED by scripts/oled_fonts.py, do not edit; see that
 * script for why these exist.
 *
 * Montserrat: SIL Open Font License 1.1. Font Awesome Free: SIL OFL 1.1 (fonts).
 * SPDX-License-Identifier: MIT
 */

#include "oled_fonts.h"
"""]
    for name, size, line_height, base_line in FONTS:
        out.append(c_font(name, size, line_height, base_line,
                          ImageFont.truetype(text_ttf, size),
                          ImageFont.truetype(sym_ttf, size)))
    with open(OUT_C, "w") as f:
        f.write("\n".join(out))

    hdr = ["""/*
 * OLED status screen fonts (see oled_fonts.c). GENERATED by scripts/oled_fonts.py.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>

/* ASCII, the LV_SYMBOL_* glyphs listed in the script, and the OLED_ICON_* icons. */"""]
    hdr += [f"extern const lv_font_t {name};" for name, *_ in FONTS]
    hdr.append("")
    for cp, name, comment, _, _ in ICONS:
        utf8 = "".join(f"\\x{b:02X}" for b in chr(cp).encode())
        hdr.append(f'#define OLED_ICON_{name} "{utf8}" /* {comment} */')
    with open(OUT_H, "w") as f:
        f.write("\n".join(hdr) + "\n")


if __name__ == "__main__":
    main()
