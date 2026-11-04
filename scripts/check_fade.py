#!/usr/bin/env python3
"""Host-side check of the reactive/ripple fade colour math in src/prgb.c.

Pulls hsv_to_rgb() and fade_accent() out of src/prgb.c as they are, compiles them with
the host C compiler and runs fades from the accent colour down to every background
level, for a spread of hues, saturations and brightnesses. Checks that:

  - level 256 is exactly the accent and level 0 is the background (all off at 0%),
  - no channel ever gets brighter while the fade runs down,
  - every step keeps the accent's channel ratios (to within 1 LSB of rounding), i.e.
    the same hue and saturation: only the brightness changes, no white or hue shift.

Usage: check_fade.py [CC]   (default cc)
"""

import colorsys
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "src/prgb.c"
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"

HUES = range(0, 360, 15)
SATS = (100, 80, 50, 25)
VALS = (127, 64, 25)  # full (50% cap), half, low brightness
BGS = (0, 5, 10, 15, 25)

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
struct led_rgb { uint8_t r, g, b; };
%s
%s
int main(void) {
    static const int hues[] = {%s}, sats[] = {%s}, vals[] = {%s}, bgs[] = {%s};
    for (unsigned hi = 0; hi < sizeof(hues) / sizeof(*hues); hi++)
    for (unsigned si = 0; si < sizeof(sats) / sizeof(*sats); si++)
    for (unsigned vi = 0; vi < sizeof(vals) / sizeof(*vals); vi++)
    for (unsigned bi = 0; bi < sizeof(bgs) / sizeof(*bgs); bi++) {
        struct led_rgb a = hsv_to_rgb(hues[hi], sats[si], vals[vi]);
        printf("A %%d %%d %%d %%d %%d %%d %%d\n", hues[hi], sats[si], vals[vi], bgs[bi],
               a.r, a.g, a.b);
        for (int level = 256; level >= 0; level--) {
            struct led_rgb o = fade_accent(a, bgs[bi], level);
            printf("%%d %%d %%d %%d\n", level, o.r, o.g, o.b);
        }
    }
    return 0;
}
"""


def extract(src, signature):
    start = src.index(signature)
    end = src.index("\n}\n", start) + 3
    return src[start:end]


def hue_sat(rgb):
    h, s, _ = colorsys.rgb_to_hsv(*(c / 255 for c in rgb))
    return h * 360, s


def main():
    src = SRC.read_text()
    funcs = [extract(src, "static struct led_rgb hsv_to_rgb("),
             extract(src, "static struct led_rgb fade_accent(")]
    join = lambda xs: ", ".join(map(str, xs))
    code = HARNESS % (*funcs, join(HUES), join(SATS), join(VALS), join(BGS))

    with tempfile.TemporaryDirectory() as tmp:
        c_file, exe = Path(tmp) / "fade.c", Path(tmp) / "fade"
        c_file.write_text(code)
        subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-o", exe, c_file],
                       check=True)
        out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout

    errors, fades, steps = [], 0, 0
    max_dh = max_ds = 0.0
    fade = None

    def finish(f):
        if f is None:
            return
        (h, s, v, bg, accent), rows = f
        name = f"hue {h} sat {s} val {v} bg {bg}%"
        if rows[0][1] != accent:
            errors.append(f"{name}: level 256 is {rows[0][1]}, not the accent {accent}")
        end = rows[-1][1]
        if bg == 0 and end != (0, 0, 0):
            errors.append(f"{name}: background 0% ends at {end}, not off")
        if bg and max(end) == 0 and max(accent) * bg // 100 > 1:
            errors.append(f"{name}: background {bg}% ends dark")
        for (_, prev), (lvl, cur) in zip(rows, rows[1:]):
            if any(c > p for c, p in zip(cur, prev)):
                errors.append(f"{name}: brighter at level {lvl}: {prev} -> {cur}")
                break

    for line in out.splitlines():
        f = line.split()
        if f[0] == "A":
            finish(fade)
            h, s, v, bg, r, g, b = map(int, f[1:])
            fade = ((h, s, v, bg, (r, g, b)), [])
            fades += 1
            continue
        lvl, *rgb = map(int, f)
        rgb = tuple(rgb)
        accent = fade[0][4]
        fade[1].append((lvl, rgb))
        steps += 1
        # Same colour = channels proportional to the accent's. Integer scaling can be
        # off by 1 LSB per channel: |c * A - a * C| <= A, with A/C the max channels.
        big_a, big_c = max(accent), max(rgb)
        for c, a in zip(rgb, accent):
            if abs(c * big_a - a * big_c) > big_a:
                errors.append(f"{fade[0][:4]} level {lvl}: {rgb} not a dimmed {accent}")
                break
        if big_c >= 32:  # where 8-bit rounding is small enough to measure hue/sat
            ha, sa = hue_sat(accent)
            hc, sc = hue_sat(rgb)
            dh = abs(ha - hc)
            max_dh = max(max_dh, min(dh, 360 - dh))
            max_ds = max(max_ds, abs(sa - sc) * 100)
    finish(fade)

    print(f"{fades} fades, {steps} steps (hues {HUES.start}-{HUES.stop - HUES.step} "
          f"step {HUES.step}, sat {SATS}, val {VALS}, bg {BGS}%)")
    print(f"max drift from the accent where the brightest channel >= 32: "
          f"hue {max_dh:.2f} deg, saturation {max_ds:.2f} points")
    if errors:
        print(f"{len(errors)} problem(s):")
        for e in errors[:20]:
            print("  " + e)
        return 1
    print("OK: fades only change brightness, end at the background, never brighten")
    return 0


if __name__ == "__main__":
    sys.exit(main())
