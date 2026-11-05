#!/usr/bin/env python3
"""Host-side check of the indicators (src/prgb_ind.h) against the real keymap.

Runs the keymap through the C preprocessor with ZMK's headers (so &kp LSHFT, BT_SEL 0,
PRGB_TOG, ... become the numbers the firmware sees), compiles src/prgb_ind.h and the
firmware LED table (src/aurora_lily58_leds.h, once per half) into a host program and:

  - prints, for every layer and each half, which key LEDs light in which class in the
    layer key map (the class letter on a keyboard-shaped grid, plus the LED numbers
    per class), so the map can be eyeballed;
  - shows which keys light for held modifiers and for caps lock on every layer;
  - shows the Bluetooth/output key colours for a few connection states;
  - checks the relay: the central packs the right half's part into planes, the
    peripheral decodes them with only its own LED table, and every right LED must get
    exactly what the central computed for its key position (every scenario above, plus
    random data and the relay param1 encoding).

Usage: check_indicators.py [ZMK_DIR] [CC]   (default /tmp/zmk, cc; a sparse clone of
zmk with app/include and app/dts is enough)
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ZMK = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/zmk")
CC = sys.argv[2] if len(sys.argv) > 2 else "cc"
KEYMAP = REPO / "config/splitkb_aurora_lily58.keymap"

N_POS = 58
CLASS_LETTERS = {0: ".", 1: "K", 2: "L", 3: "C", 4: "S", 5: "T", 6: "E", 7: "V", 8: "B",
                 9: "H", 10: "A"}
CLASS_NAMES = {0: "none (dark, flashes)", 1: "key (white)", 2: "layer (orange)",
               3: "bt/out (blue)", 4: "system (red)", 5: "light toggle (pink)",
               6: "effect (green)", 7: "speed (cyan)", 8: "brightness (yellow)",
               9: "hue (rainbow)", 10: "saturation (purple)"}
CLASS_MASK, MOD, CAPS = 0x0F, 0x10, 0x20

# Lily58 key positions per keyboard row, (left part, right part)
ROWS = [
    (list(range(0, 6)), list(range(6, 12))),
    (list(range(12, 18)), list(range(18, 24))),
    (list(range(24, 30)), list(range(30, 36))),
    (list(range(36, 43)), list(range(43, 50))),
    (list(range(50, 54)), list(range(54, 58))),
]

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def preprocess_keymap(tmp):
    # ZMK's behaviors.dtsi pulls in a Zephyr header (mouse codes); use Zephyr's own if
    # there is a west workspace next to zmk, else an empty stand-in (not used by keymaps)
    stub = tmp / "stub"
    (stub / "zephyr/dt-bindings/input").mkdir(parents=True)
    (stub / "zephyr/dt-bindings/input/input-event-codes.h").write_text("")
    zephyr = [d for d in (ZMK.parent / "zephyr/include",) if d.is_dir()]
    cmd = [CC, "-E", "-P", "-x", "assembler-with-cpp", "-nostdinc", "-undef",
           "-I", str(ZMK / "app/include"), "-I", str(ZMK / "app/dts"),
           "-I", str(REPO / "include"), "-I", str(REPO / "dts")]
    for d in zephyr + [stub]:
        cmd += ["-I", str(d)]
    return subprocess.run(cmd + [str(KEYMAP)], check=True, capture_output=True, text=True).stdout


def split_cells(text):
    """Top-level whitespace split, keeping parenthesised expressions together."""
    cells, depth, cur = [], 0, ""
    for ch in text:
        if ch.isspace() and depth == 0:
            if cur:
                cells.append(cur)
            cur = ""
            continue
        depth += ch == "("
        depth -= ch == ")"
        cur += ch
    if cur:
        cells.append(cur)
    return cells


def source_labels():
    """The bindings as written in the keymap, per layer (for printing)."""
    text = re.sub(r"//[^\n]*", "", KEYMAP.read_text())
    return [[" ".join(b.split()) for b in body.split("&")[1:]]
            for body in re.findall(r"(?<![\w-])bindings\s*=\s*<(.*?)>;", text, re.S)]


def parse_keymap(text):
    names = dict(re.findall(r"\b(\w+)\s*:\s*([\w@-]+)\s*\{", text))  # label -> node name
    keymap = text[text.index('compatible = "zmk,keymap"'):]
    layers = []
    for m in re.finditer(r"(\w+)\s*\{\s*display-name\s*=\s*\"([^\"]*)\";(.*?)bindings\s*=\s*<(.*?)>;",
                         keymap, re.S):
        node, display, body = m.group(1), m.group(2), m.group(4)
        bindings = []
        for chunk in body.split("&")[1:]:
            cells = split_cells(chunk)
            label, params = cells[0], [int(eval(c, {})) for c in cells[1:]]  # noqa: S307
            params += [0, 0]
            bindings.append((names.get(label, label), params[0], params[1], chunk.strip()))
        layers.append((node, display, bindings))
    return layers


HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include "prgb_ind.h"

struct prgb_key_led { uint8_t position, x, y; };
extern const struct prgb_key_led *leds_left, *leds_right;
#define KEY_LEDS 29

#include "bindings.h"

static const struct prgb_ind_binding (*km)[N_POS] = keymap;

static bool get(void *ctx, uint8_t layer, uint8_t pos, struct prgb_ind_binding *out) {
    const struct prgb_ind_binding (*k)[N_POS] = km;
    if (ctx) k = (const struct prgb_ind_binding (*)[N_POS])ctx;
    if (layer >= N_LAYERS || pos >= N_POS) return false;
    *out = k[layer][pos];
    return out->dev != NULL;
}

static uint64_t mask_of(const struct prgb_key_led *leds) {
    uint64_t m = 0;
    for (int k = 0; k < KEY_LEDS; k++) m |= 1ULL << leds[k].position;
    return m;
}

/* As prgb_init(): an LED's plane bit is its position's rank among the half's positions */
static unsigned led_bit(const struct prgb_key_led *leds, uint64_t own, int k) {
    return __builtin_popcountll(own & ((1ULL << leds[k].position) - 1));
}

static int errors;

/* Central: planes for both halves; each half decodes its own with its LED table only. */
static void run(const char *name, void *ctx, const uint8_t *active, uint8_t n_active,
                bool map, uint8_t mods, bool caps) {
    uint8_t info[N_POS];
    struct prgb_ind_in in = {.get = get, .ctx = ctx, .n_pos = N_POS, .active = active,
                             .n_active = n_active, .n_layers = N_LAYERS,
                             .show_map = map && active[0] != 0, .mods = mods, .caps = caps,
                             .caps_fallback = 24};
    prgb_ind_compute(&in, info);
    uint32_t header = PRGB_IND_HEADER(in.show_map, active[0]);
    uint64_t lm = mask_of(leds_left), rm = mask_of(leds_right);
    uint64_t all = (1ULL << N_POS) - 1;
    uint32_t own[PRGB_IND_PLANES], rel[PRGB_IND_PLANES], got[PRGB_IND_PLANES] = {0};

    if ((lm | rm) != all || (lm & rm)) { printf("ERR halves don't split the positions\n"); errors++; }

    prgb_ind_pack(info, N_POS, lm, header, own);       /* central's own planes */
    prgb_ind_pack(info, N_POS, all & ~lm, header, rel); /* what the central relays */

    /* Relay: one &prgb message per plane, param1 encodes the plane */
    for (int p = 0; p < PRGB_IND_PLANES; p++) {
        uint32_t p1 = PRGB_IND_RELAY_PARAM1(p);
        int plane = prgb_ind_relay_plane(p1);
        if (plane != p) { printf("ERR param1 %08x -> plane %d\n", p1, plane); errors++; continue; }
        got[plane] = rel[p];
    }

    printf("S %s|%08x|%08x\n", name, own[PRGB_IND_PLANE_HEADER], got[PRGB_IND_PLANE_HEADER]);
    for (int k = 0; k < KEY_LEDS; k++) {
        uint8_t l = prgb_ind_unpack(own, led_bit(leds_left, lm, k));
        uint8_t r = prgb_ind_unpack(got, led_bit(leds_right, rm, k));
        if (l != info[leds_left[k].position] || r != info[leds_right[k].position]) {
            printf("ERR %s LED %d: left %x/%x right %x/%x\n", name, k + 5, l,
                   info[leds_left[k].position], r, info[leds_right[k].position]);
            errors++;
        }
        printf("L %d %d %d\nR %d %d %d\n", k + 5, leds_left[k].position, l, k + 5,
               leds_right[k].position, r);
    }
}

static void random_roundtrip(void) {
    srand(1);
    uint64_t rm = mask_of(leds_right);
    for (int t = 0; t < 20000; t++) {
        uint8_t info[N_POS];
        uint32_t planes[PRGB_IND_PLANES];
        for (int p = 0; p < N_POS; p++)
            info[p] = (rand() % PRGB_IND_CLASSES) | (rand() & 1 ? PRGB_IND_MOD : 0) |
                      (rand() & 1 ? PRGB_IND_CAPS : 0);
        uint32_t header = PRGB_IND_HEADER(rand() & 1, rand() & 0xFF);
        prgb_ind_pack(info, N_POS, rm, header, planes);
        if (planes[PRGB_IND_PLANE_HEADER] != header) errors++;
        for (int k = 0; k < KEY_LEDS; k++)
            if (prgb_ind_unpack(planes, led_bit(leds_right, rm, k)) != info[leds_right[k].position]) {
                printf("ERR random %d LED %d\n", t, k);
                errors++;
            }
    }
    /* param1s that are not relay messages, or out of range planes, are refused */
    uint32_t bad[] = {PRGB_RELAY_CMD + 1, PRGB_IND_RELAY_PARAM1(PRGB_IND_PLANES), 0,
                      PRGB_MAP_TOG_CMD, PRGB_IND_RELAY_PARAM1(255)};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++)
        if (prgb_ind_relay_plane(bad[i]) >= 0) { printf("ERR accepted %08x\n", bad[i]); errors++; }
    printf("RANDOM ok\n");
}

static void conn(const char *name, struct prgb_ind_conn c) {
    printf("C %s", name);
    for (int p = 0; p < N_POS; p++) {
        struct prgb_ind_binding b;
        if (!get(NULL, CONN_LAYER, p, &b)) continue;
        struct prgb_ind_color col = prgb_ind_conn_color(&b, &c);
        if (col.pct) printf("|%d:%d,%d,%d,%d", p, col.h, col.s, col.pct, col.pulse);
    }
    printf("\n");
}

int main(void) {
    uint8_t act[2];
    for (int l = 0; l < N_LAYERS; l++) {
        char name[64];
        act[0] = l, act[1] = 0;
        snprintf(name, sizeof name, "map %d", l);
        run(name, NULL, act, l ? 2 : 1, true, 0, false);
    }
    for (int l = 0; l < N_LAYERS; l++) {
        char name[64];
        act[0] = l, act[1] = 0;
        snprintf(name, sizeof name, "mods %d", l);
        run(name, NULL, act, l ? 2 : 1, false, 0xFF, false);
        snprintf(name, sizeof name, "caps %d", l);
        run(name, NULL, act, l ? 2 : 1, false, 0, true);
    }
    /* caps lock with no caps key anywhere: the fallback position */
    static struct prgb_ind_binding nocaps[N_LAYERS][N_POS];
    memcpy(nocaps, keymap, sizeof nocaps);
    for (int l = 0; l < N_LAYERS; l++)
        for (int p = 0; p < N_POS; p++)
            if (prgb_ind_binding_caps(nocaps[l][p].dev, nocaps[l][p].param1))
                nocaps[l][p] = (struct prgb_ind_binding){"none", 0, 0};
    act[0] = 0;
    run("caps fallback", nocaps, act, 1, false, 0, true);
    /* a right-half modifier (synthetic: right shift on position 35), shift held */
    static struct prgb_ind_binding rmod[N_LAYERS][N_POS];
    memcpy(rmod, keymap, sizeof rmod);
    rmod[0][35] = (struct prgb_ind_binding){"key_press", (0x07 << 16) | 0xE5, 0};
    act[0] = 0;
    run("right mod", rmod, act, 1, false, 0x22, false);
    /* everything at once: Control layer, shift held, caps on */
    act[0] = 1, act[1] = 0;
    run("all 1", NULL, act, 2, true, 0x02, true);

    random_roundtrip();

    conn("usb, profile 0 active+connected, 1 paired, 2-4 open",
         (struct prgb_ind_conn){.active = 0, .profiles = 5, .connected = 1, .open = 0x1C,
                                .transport = PRGB_IND_TRANSPORT_USB});
    conn("ble, profile 1 active advertising, 0 paired, 2-4 open",
         (struct prgb_ind_conn){.active = 1, .profiles = 5, .connected = 0, .open = 0x1C,
                                .transport = PRGB_IND_TRANSPORT_NONE});
    conn("ble, profile 2 active+connected, others open",
         (struct prgb_ind_conn){.active = 2, .profiles = 5, .connected = 4, .open = 0x1B,
                                .transport = PRGB_IND_TRANSPORT_BLE});
    printf("ERRORS %d\n", errors);
    return errors != 0;
}
"""

LEDS_TU = """
#define IS_ENABLED(x) x
#define CONFIG_SHIELD_SPLITKB_AURORA_LILY58_LEFT %d
#define CONFIG_SHIELD_SPLITKB_AURORA_LILY58_RIGHT %d
#include "aurora_lily58_leds.h"
const struct prgb_key_led *leds_%s = prgb_key_leds;
"""


def c_str(s):
    return '"' + s + '"'


def grid(values, title):
    """values: position -> short string. Prints the keyboard as two halves."""
    print(title)
    for left, right in ROWS:
        l = " ".join(f"{values.get(p, ''):>2}" for p in left)
        r = " ".join(f"{values.get(p, ''):>2}" for p in right)
        pad = "   " * (7 - len(left))
        print(f"    {pad}{l}   |   {r}")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        text = preprocess_keymap(tmp)
        layers = parse_keymap(text)
        labels = source_labels()
        check(len(labels) == len(layers), "keymap layers and source layers differ")
        layers = [(node, display, [(d, p1, p2, labels[i][j]) for j, (d, p1, p2, _) in enumerate(b)])
                  for i, (node, display, b) in enumerate(layers)]
        for node, display, b in layers:
            check(len(b) == N_POS, f"layer {node}: {len(b)} bindings, expected {N_POS}")
        conn_layer = next((i for i, (_, _, b) in enumerate(layers)
                           if any(d == "bluetooth" for d, *_ in b)), 0)
        rows = []
        for node, display, b in layers:
            rows.append("    {" + ", ".join(f"{{{c_str(d)}, {p1}u, {p2}u}}" for d, p1, p2, _ in b) + "},")
        (tmp / "bindings.h").write_text(
            f"#define N_LAYERS {len(layers)}\n#define N_POS {N_POS}\n#define CONN_LAYER {conn_layer}\n"
            "static const struct prgb_ind_binding keymap[N_LAYERS][N_POS] = {\n" + "\n".join(rows) + "\n};\n")
        (tmp / "main.c").write_text(HARNESS)
        (tmp / "leds_left.c").write_text(LEDS_TU % (1, 0, "left"))
        (tmp / "leds_right.c").write_text(LEDS_TU % (0, 1, "right"))
        exe = tmp / "check"
        subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-I", str(tmp), "-I", str(REPO / "src"), "-I", str(REPO / "include"),
                        "-o", str(exe), str(tmp / "main.c"), str(tmp / "leds_left.c"),
                        str(tmp / "leds_right.c")], check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        lines = out.stdout.splitlines()

    scenarios, cur = {}, None
    for line in lines:
        if line.startswith("S "):
            name, own_h, rel_h = line[2:].split("|")
            cur = scenarios[name] = {"left": {}, "right": {}, "hdr": (int(own_h, 16), int(rel_h, 16))}
        elif line[:2] in ("L ", "R "):
            led, pos, info = map(int, line[2:].split())
            cur["left" if line[0] == "L" else "right"][pos] = (led, info)
        elif line.startswith("ERR "):
            failures.append(line)
    check(out.returncode == 0 and "RANDOM ok" in lines, "harness reported errors")

    print("Layer key map: class per key (" + ", ".join(
        f"{CLASS_LETTERS[c]} {CLASS_NAMES[c]}" for c in CLASS_LETTERS) + ")")
    for i, (node, display, b) in enumerate(layers):
        sc = scenarios[f"map {i}"]
        both = {**sc["left"], **sc["right"]}
        shown = bool(sc["hdr"][0] & 1)
        check(sc["hdr"][0] == sc["hdr"][1], f"layer {display}: header differs between halves")
        print()
        if not shown:
            grid({p: "~" for p in both}, f"{display} (layer {i}): default layer, no map - the effect runs")
            check(all((v & CLASS_MASK) == 0 for _, v in both.values()), f"{display}: classes while hidden")
            continue
        grid({p: CLASS_LETTERS[v & CLASS_MASK] for p, (_, v) in both.items()},
             f"{display} (layer {i}): map shown, underglow tint of layer {sc['hdr'][0] >> 8}")
        for half in ("left", "right"):
            per = {}
            for p, (led, v) in sorted(sc[half].items(), key=lambda kv: kv[1][0]):
                per.setdefault(v & CLASS_MASK, []).append(led)
            desc = "; ".join(f"{CLASS_NAMES[c]}: LEDs {' '.join(map(str, per[c]))}" for c in sorted(per))
            print(f"  {half:5}: {desc}")
        # the map must agree with the bindings themselves
        for p, (led, v) in both.items():
            dev = b[p][0]
            want = 0 if dev in ("transparent", "none") else None
            if want is not None:
                check((v & CLASS_MASK) == want, f"{display} pos {p} ({b[p][3]}): class {v & CLASS_MASK}")
            else:
                check((v & CLASS_MASK) != 0, f"{display} pos {p} ({b[p][3]}): dark")

    print("\nHeld modifiers (all eight held at once): keys lit green, by layer")
    for i, (node, display, b) in enumerate(layers):
        sc = scenarios[f"mods {i}"]
        both = {**sc["left"], **sc["right"]}
        lit = sorted(p for p, (_, v) in both.items() if v & MOD)
        print(f"  {display:8}: positions {lit} = " + ", ".join(
            next(bb[3] for bb in [layers[j][2][p] for j in [i, 0]] if bb[0] != "transparent") for p in lit))
        right = [p for p, (_, v) in sc["right"].items() if v & MOD]
        print(f"            right half: {'positions ' + str(right) + ' (relayed)' if right else 'no modifier keys'}")

    print("\nCaps lock on: key lit cyan, by layer")
    for name in [f"caps {i}" for i in range(len(layers))] + ["caps fallback"]:
        sc = scenarios[name]
        both = {**sc["left"], **sc["right"]}
        lit = sorted(p for p, (_, v) in both.items() if v & CAPS)
        label = name if name == "caps fallback" else layers[int(name.split()[1])][1]
        print(f"  {label:13}: positions {lit}")
        check(len(lit) >= 1, f"{name}: caps lock lights nothing")
    check(sorted(p for p, (_, v) in {**scenarios['caps fallback']['left'],
                                     **scenarios['caps fallback']['right']}.items() if v & CAPS) == [24],
          "caps fallback is not position 24")

    sc = scenarios["right mod"]
    right = sorted(p for p, (_, v) in sc["right"].items() if v & MOD)
    left = sorted(p for p, (_, v) in sc["left"].items() if v & MOD)
    print(f"\nSynthetic &kp RSHFT on position 35, both shifts held: left {left}, right {right} (relayed)")
    check(left == [24] and right == [35], "right-half modifier relay")

    sc = scenarios["all 1"]
    both = {**sc["left"], **sc["right"]}
    print("\nControl layer + left shift held + caps lock (overlay letter: m mod, c caps):")
    grid({p: ("c" if v & CAPS else "m" if v & MOD else CLASS_LETTERS[v & CLASS_MASK]) for p, (_, v) in both.items()}, "")

    print(f"\nBluetooth/output keys on layer {conn_layer} (position: hue,sat,% brightness,pulsing):")
    for line in lines:
        if line.startswith("C "):
            name, *cols = line[2:].split("|")
            print(f"  {name}:")
            for c in cols:
                pos, vals = c.split(":")
                print(f"    pos {pos:>2} {layers[conn_layer][2][int(pos)][3]:12} -> {vals}")

    print(f"\nRelay round trip: {len(scenarios)} scenarios + 20000 random: "
          f"{'ok' if not failures else 'FAILED'}")
    if failures:
        print("\nFAILED:")
        for f in failures:
            print("  " + f)
        sys.exit(1)
    print("All checks passed.")


if __name__ == "__main__":
    main()
