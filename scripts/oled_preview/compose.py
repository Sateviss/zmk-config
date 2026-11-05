"""Compose the PBM frames written by preview.c into one PNG.

Each state is shown twice: the 128x32 panel as it is mounted (landscape, so the
rotated content is visible), and the same picture turned upright (portrait).
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

SCALE = 4
LIT = (120, 200, 255)
DARK = (8, 10, 14)
ORDER = [
    "left-ble", "left-usb", "left-open", "left-static", "right-on", "right-off", "right-long",
]


def load(path):
    lines = [l for l in Path(path).read_text().split("\n") if l and not l.startswith("#")]
    w, h = map(int, lines[1].split())
    img = Image.new("RGB", (w, h), DARK)
    for y, row in enumerate(lines[2 : 2 + h]):
        for x, c in enumerate(row):
            # ZMK's mono theme draws dark text on a light background and the panel
            # runs in inverted mode, so a dark pixel ("1" in PBM) is a lit one.
            if c == "1":
                img.putpixel((x, y), LIT)
    return img


def main(src, dst, upright):
    frames = [(n, load(Path(src) / f"{n}.pbm")) for n in ORDER]
    pad = 12
    label_h = 14
    pw, ph = 128 * SCALE, 32 * SCALE
    col_w = max(pw, 32 * SCALE) + pad
    # top row: panels as mounted; bottom row: upright portrait
    width = pad + len(frames) * (32 * SCALE + pad)
    width = max(width, pad + 3 * (pw + pad))
    rows_panel = (len(frames) + 2) // 3
    height = pad + rows_panel * (ph + label_h + pad) + label_h + 128 * SCALE + pad
    out = Image.new("RGB", (width, height), (40, 40, 40))
    d = ImageDraw.Draw(out)
    for i, (name, img) in enumerate(frames):
        x = pad + (i % 3) * (pw + pad)
        y = pad + (i // 3) * (ph + label_h + pad)
        d.text((x, y), f"{name} (panel as mounted)", fill=(220, 220, 220))
        out.paste(img.resize((pw, ph), Image.NEAREST), (x, y + label_h))
    y0 = pad + rows_panel * (ph + label_h + pad)
    for i, (name, img) in enumerate(frames):
        x = pad + i * (32 * SCALE + pad)
        d.text((x, y0), name, fill=(220, 220, 220))
        up = img.transpose(upright)
        out.paste(up.resize((32 * SCALE, 128 * SCALE), Image.NEAREST), (x, y0 + label_h))
    out.save(dst, optimize=True)
    print(f"wrote {dst}")


if __name__ == "__main__":
    # turning the panel picture back upright undoes the firmware's rotation
    ccw = len(sys.argv) > 3 and sys.argv[3] == "ccw"
    main(sys.argv[1], sys.argv[2], Image.ROTATE_270 if ccw else Image.ROTATE_90)
