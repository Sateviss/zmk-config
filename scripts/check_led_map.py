#!/usr/bin/env python3
"""Check the per-key LED -> ZMK key position table in src/aurora_lily58_leds.h.

Derives the mapping two independent ways and checks the firmware table against both:

  A. By wiring: QMK's rgb_matrix gives each LED a QMK matrix [row, col]. QMK and ZMK
     both name the matrix pins of each half (QMK by AVR pin, ZMK by Pro Micro pin
     number), so a QMK [row, col] is mapped to the ZMK kscan row/col of the same
     wires, then through ZMK's matrix transform (with the right half's col-offset) to
     a key position.
  B. By layout order: QMK's LAYOUT lists the keys in the same order as ZMK's keymap
     (checked against ZMK's physical layout coordinates), so the index of an LED's
     matrix [row, col] in LAYOUT is its ZMK key position.

It also checks the x/y stored for each LED against QMK's rgb_matrix layout, and prints
the table per half with the base-layer binding of each key.

Usage: check_led_map.py [QMK_DIR] [ZMK_DIR]   (default /tmp/qmk /tmp/zmk; sparse
clones of qmk_firmware and zmk are enough)
"""

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
QMK = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/qmk")
ZMK = Path(sys.argv[2] if len(sys.argv) > 2 else "/tmp/zmk")

QMK_JSON = QMK / "keyboards/splitkb/aurora/lily58/rev1/keyboard.json"
SHIELD = ZMK / "app/boards/shields/splitkb_aurora_lily58"
ZMK_LAYOUT = ZMK / "app/dts/layouts/kata0510/lily58.dtsi"
FW_TABLE = REPO / "src/aurora_lily58_leds.h"
KEYMAP = REPO / "config/splitkb_aurora_lily58.keymap"

UNDERGLOW_LEDS = 5
LEDS_PER_HALF = 34

# Pro Micro (ATmega32U4) pin number -> AVR port pin, as used by QMK
PRO_MICRO = {
    0: "D2", 1: "D3", 2: "D1", 3: "D0", 4: "D4", 5: "C6", 6: "D7", 7: "E6", 8: "B4",
    9: "B5", 10: "B6", 14: "B3", 15: "B1", 16: "B2", 18: "F7", 19: "F6", 20: "F5",
    21: "F4",
}

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def load_qmk():
    text = re.sub(r"//[^\n]*", "", QMK_JSON.read_text())
    return json.loads(text)


def zmk_transform():
    """List of (row, col) per ZMK key position."""
    text = (SHIELD / "splitkb_aurora_lily58.dtsi").read_text()
    body = re.search(r"map\s*=\s*<(.*?)>;", text, re.S).group(1)
    body = re.sub(r"//[^\n]*", "", body)
    return [(int(r), int(c)) for r, c in re.findall(r"RC\((\d+),\s*(\d+)\)", body)]


def zmk_kscan(half):
    text = (SHIELD / f"splitkb_aurora_lily58_{half}.overlay").read_text()

    def pins(prop):
        body = re.search(prop + r"\s*=(.*?);", text, re.S).group(1)
        return [PRO_MICRO[int(p)] for p in re.findall(r"&pro_micro\s+(\d+)", body)]

    m = re.search(r"col-offset\s*=\s*<(\d+)>", text)
    return pins("row-gpios"), pins("col-gpios"), int(m.group(1)) if m else 0


def zmk_physical():
    text = ZMK_LAYOUT.read_text()
    rows = re.findall(r"&key_physical_attrs\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)", text)
    return [(int(x), int(y), r != "0") for _w, _h, x, y, r in rows]


def fw_tables():
    text = FW_TABLE.read_text()
    out = {}
    for half, sym in (("left", "LEFT"), ("right", "RIGHT")):
        block = re.search(
            r"SHIELD_SPLITKB_AURORA_LILY58_" + sym + r"\)\s*\n\s*static const[^{]*\{(.*?)\n\};",
            text,
            re.S,
        ).group(1)
        block = re.sub(r"/\*.*?\*/", "", block, flags=re.S)
        out[half] = [tuple(map(int, t)) for t in re.findall(r"\{\s*(\d+),\s*(\d+),\s*(\d+)\}", block)]
    return out


def base_layer_labels():
    text = KEYMAP.read_text()
    layer = re.search(r"default_layer\s*\{.*?bindings\s*=\s*<(.*?)>;", text, re.S).group(1)
    layer = re.sub(r"//[^\n]*", "", layer)
    return [b.strip().replace("&kp ", "") for b in layer.split("&")[1:]]


def main():
    qmk = load_qmk()
    layout = [k["matrix"] for k in qmk["layouts"]["LAYOUT"]["layout"]]
    qmk_layout_xy = [(k["x"], k["y"]) for k in qmk["layouts"]["LAYOUT"]["layout"]]
    rgb = qmk["rgb_matrix"]["layout"]
    transform = zmk_transform()
    phys = zmk_physical()
    labels = base_layer_labels()
    fw = fw_tables()

    check(len(layout) == len(transform) == len(phys) == len(labels) == 58,
          f"key counts differ: QMK {len(layout)}, ZMK transform {len(transform)}, "
          f"ZMK physical {len(phys)}, keymap {len(labels)}")

    # B relies on QMK LAYOUT order == ZMK key position order: compare coordinates
    for i, ((qx, qy), (zx, zy, rotated)) in enumerate(zip(qmk_layout_xy, phys)):
        tol = 100 if rotated else 15  # the two rotated thumb keys are placed differently
        check(abs(qx * 100 - zx) <= tol and abs(qy * 100 - zy) <= tol,
              f"position {i}: QMK LAYOUT ({qx},{qy}) vs ZMK physical ({zx},{zy})")

    qmk_pins = {
        "left": (qmk["matrix_pins"]["rows"], qmk["matrix_pins"]["cols"], 0),
        "right": (qmk["split"]["matrix_pins"]["right"]["rows"],
                  qmk["split"]["matrix_pins"]["right"]["cols"], 5),
    }

    for h, half in enumerate(("left", "right")):
        z_rows, z_cols, col_offset = zmk_kscan(half)
        q_rows, q_cols, q_row_base = qmk_pins[half]
        leds = rgb[h * LEDS_PER_HALF:(h + 1) * LEDS_PER_HALF]
        check(len(fw[half]) == LEDS_PER_HALF - UNDERGLOW_LEDS,
              f"{half}: firmware table has {len(fw[half])} entries")

        print(f"\n{half.upper()} half ({'central' if half == 'left' else 'peripheral'})")
        print(" LED | QMK matrix | QMK x,y  | ZMK RC    | pos (wiring) | pos (layout) | firmware | base key")
        print("-----+------------+----------+-----------+--------------+--------------+----------+---------")
        for i, led in enumerate(leds):
            if "matrix" not in led:
                check(i < UNDERGLOW_LEDS, f"{half} LED {i}: no matrix but not underglow")
                print(f" {i:3d} | underglow  | {led['x']:3d},{led['y']:3d}  |")
                continue
            check(i >= UNDERGLOW_LEDS, f"{half} LED {i}: per-key LED inside underglow range")
            qr, qc = led["matrix"]
            # A: by wiring
            row_pin, col_pin = q_rows[qr - q_row_base], q_cols[qc]
            zr, zc = z_rows.index(row_pin), z_cols.index(col_pin) + col_offset
            pos_a = transform.index((zr, zc))
            # B: by LAYOUT order
            pos_b = layout.index([qr, qc])
            # firmware
            fpos, fx, fy = fw[half][i - UNDERGLOW_LEDS]
            ok = pos_a == pos_b == fpos and (fx, fy) == (led["x"], led["y"])
            check(ok, f"{half} LED {i}: wiring {pos_a}, layout {pos_b}, firmware {fpos} "
                      f"({fx},{fy}) vs QMK ({led['x']},{led['y']})")
            print(f" {i:3d} | [{qr},{qc}]      | {led['x']:3d},{led['y']:3d}  | RC({zr},{zc:2d}) "
                  f"| {pos_a:12d} | {pos_b:12d} | {fpos:4d} {'ok' if ok else 'BAD'} | {labels[pos_a]}")

        owned = sorted(p for p, _, _ in fw[half])
        check(len(set(owned)) == len(owned), f"{half}: duplicate positions in firmware table")
        expected = sorted(i for i, (r, c) in enumerate(transform) if (c >= 6) == (half == "right"))
        check(owned == expected, f"{half}: firmware covers {owned}, half has {expected}")
        print(f" -> {len(owned)} key LEDs, cover exactly this half's positions: {owned == expected}")

    if failures:
        print("\nFAILED:")
        for f in failures:
            print("  " + f)
        sys.exit(1)
    print("\nAll checks passed: wiring, layout order and firmware table agree.")


if __name__ == "__main__":
    main()
