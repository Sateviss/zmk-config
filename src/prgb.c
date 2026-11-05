/*
 * Per-key RGB lighting for the splitkb Aurora Lily58.
 *
 * Replaces ZMK's RGB underglow and drives the zmk,underglow LED strip directly, as
 * two zones: the underglow LEDs (0-4) and the per-key LEDs (5-33), each with its own
 * hue/saturation, under one brightness. Effects are listed in <dt-bindings/prgb.h>.
 *
 * Each half runs its own copy and only lights the LEDs on its own chain: key presses
 * are taken from zmk_position_state_changed events that originate on this half
 * (source == LOCAL); the central also sees the peripheral's presses, with the
 * peripheral as source, and ignores them.
 *
 * Frames are rendered on ZMK's low priority work queue by a delayable work item that
 * only re-arms itself while something is animating; static output is written once.
 *
 * Indicators (see prgb_ind.h) are drawn over the effect: the layer key map replaces it
 * while a layer above the default one is active, held modifiers and caps lock light
 * their keys on any layer, and the underglow pulses on key presses. The central works
 * them out from the keymap, layer, HID and BLE state on the same work queue and relays
 * the peripheral's part as &prgb commands; the peripheral only decodes and draws them.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/workqueue.h>

#include <dt-bindings/prgb.h>

#include "prgb.h"
#include "prgb_ind.h"
#include "aurora_lily58_leds.h"
#include "events/prgb_state_changed.h"

/* The half that has the keymap, layers, HID and BLE state (or a unibody board). */
#define IND_CENTRAL (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
/* The central relays the peripheral's indicators to it through &prgb. */
#define IND_RELAY                                                                                  \
    (IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&                  \
     DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_prgb))

#if IND_CENTRAL
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#endif
#endif /* IND_CENTRAL */

#if IND_RELAY
#include <zmk/split/central.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IND_RELAY
#include <zephyr/bluetooth/conn.h>
#define IND_RESYNC_ON_CONNECT 1
#else
#define IND_RESYNC_ON_CONNECT 0
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/events/split_peripheral_status_changed.h>
#endif

LOG_MODULE_REGISTER(prgb, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_underglow)
#error "Per-key RGB needs a zmk,underglow chosen LED strip"
#endif

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define STRIP_LEDS DT_PROP(STRIP_NODE, chain_length)
#define UG_LEDS CONFIG_ZMK_PRGB_UNDERGLOW_LEDS

BUILD_ASSERT(STRIP_LEDS >= UG_LEDS + PRGB_KEY_LEDS,
             "LED strip chain-length is shorter than underglow + per-key LEDs");
BUILD_ASSERT(PRGB_KEY_LEDS <= 32, "Key state is kept in 32-bit masks");

#define FRAME_MS (1000 / CONFIG_ZMK_PRGB_FPS)
#define QMK_GRID_W 224

/* Long enough ago that no fade or ripple is still running (max fade is 3 x 10 s). */
#define LONG_AGO_MS 60000

#define RIPPLES 4
#define RIPPLE_WIDTH 18 /* ring thickness, in QMK grid units (a key is ~14-15) */

#define HEAT_MAX 4096           /* fixed-point heat of a key: 0 cold .. HEAT_MAX red hot */
#define HEAT_HIT (HEAT_MAX / 3) /* a press: three quick presses make a key red */
#define HEAT_SPREAD 24          /* neighbours closer than this (grid units) warm a little */

/* Underglow pulse: level the underglow rests at between presses, in % of its own */
#define PULSE_REST_PCT 50
/* Bluetooth "advertising" keys breathe between 25% and 100% once per this many ms */
#define IND_BLINK_MS 1200
/* Hue keys in the layer key map run once round the colour wheel per this many ms */
#define IND_HUE_CYCLE_MS 3000
/* Changing a colour, saturation, brightness or background level while the key map is
 * shown flashes the keys bound to nothing in the result for this long */
#define IND_FLASH_MS 900
#define IND_FLASH_HOLD_MS 300 /* at full before it starts fading */

#define STATE_VERSION 3

struct prgb_state {
    uint8_t version;
    bool on;
    uint8_t effect;
    uint8_t brt;
    uint8_t spd;
    struct prgb_hs ug;
    struct prgb_hs key;
    uint8_t bg;  /* reactive/ripple background, % of the brightness (0 = keys dark) */
    uint8_t map;   /* layer key map on (v3; bytes, so a bad saved value can be checked) */
    uint8_t pulse; /* underglow pulse on key presses on (v3) */
};

/* Saved by the second version (no indicator toggles); migrated on load. */
#define STATE_VERSION_2 2

struct prgb_state_v2 {
    uint8_t version;
    bool on;
    uint8_t effect;
    uint8_t brt;
    uint8_t spd;
    struct prgb_hs ug;
    struct prgb_hs key;
    uint8_t bg;
};

/* Saved by the first version (no background level); migrated on load. */
#define STATE_VERSION_1 1

struct prgb_state_v1 {
    uint8_t version;
    bool on;
    uint8_t effect;
    uint8_t brt;
    uint8_t spd;
    struct prgb_hs ug;
    struct prgb_hs key;
};

BUILD_ASSERT(sizeof(struct prgb_state) != sizeof(struct prgb_state_v1) &&
                 sizeof(struct prgb_state) != sizeof(struct prgb_state_v2) &&
                 sizeof(struct prgb_state_v2) != sizeof(struct prgb_state_v1),
             "Saved state formats must be told apart by size");

/* Background levels PRGB_BG_NEXT steps through, in % of the brightness. */
static const uint8_t bg_levels[] = {0, 5, 10, 15, 25};

struct ripple {
    uint32_t start_ms;
    uint8_t key;
};

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[STRIP_LEDS];

static struct prgb_state state;
static bool ready;
static bool awake = true;
static uint32_t anim_start_ms;

/* Key position -> index into prgb_key_leds, or -1 if the key is not on this half. */
static int8_t key_for_position[64];

static atomic_t pressed;
static uint32_t released_ms[PRGB_KEY_LEDS];
static struct ripple ripples[RIPPLES];
static uint8_t next_ripple;

/* Heatmap: presses since the last frame (set by the event, taken by the renderer). */
static atomic_t heat_hits;
static uint16_t heat[PRGB_KEY_LEDS];
static uint32_t heat_ms;

/* Underglow pulse: when this half's last key press was. */
static uint32_t pulse_ms;

/* Key map flash: when a colour setting last changed (on this half) and what to show. */
static bool flash_on;
static uint32_t flash_ms;
static struct prgb_hs flash_color;
static uint8_t flash_pct; /* % of the brightness */

/*
 * This half's indicator planes (prgb_ind.h): bit n is this half's n-th key position in
 * ascending order. Written by the central's indicator work or, on the peripheral, by
 * the relay; read by the renderer. Each plane is one word, written whole.
 */
static uint32_t ind_planes[PRGB_IND_PLANES];
/* Per-key LED: its bit in the planes. */
static uint8_t led_bit[PRGB_KEY_LEDS];
/* Key positions on this half (bit per position). */
static uint64_t own_positions;

#if IND_CENTRAL
static struct k_work ind_work;
/* Bluetooth/output status colour per key LED while the key map is shown (pct 0 = none) */
static struct prgb_ind_color conn_colors[PRGB_KEY_LEDS];
#endif

static struct k_work_delayable render_work;

/* Work the indicators out again (central) after something they depend on changed. */
static void ind_update(void);

#if IS_ENABLED(CONFIG_SETTINGS)
static struct k_work_delayable save_work;
#endif

/* ---- colour helpers ---------------------------------------------------------------- */

/* h 0-359, s 0-100, v 0-255 */
static struct led_rgb hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v) {
    struct led_rgb rgb = {0};
    uint32_t f = (h % 60) * 256 / 60; /* 0-255 position within the 60 degree sector */
    uint8_t p = v * (100 - s) / 100;
    uint8_t q = v * (25600 - s * f) / 25600;
    uint8_t t = v * (25600 - s * (256 - f)) / 25600;

    switch ((h / 60) % 6) {
    case 0:
        rgb.r = v, rgb.g = t, rgb.b = p;
        break;
    case 1:
        rgb.r = q, rgb.g = v, rgb.b = p;
        break;
    case 2:
        rgb.r = p, rgb.g = v, rgb.b = t;
        break;
    case 3:
        rgb.r = p, rgb.g = q, rgb.b = v;
        break;
    case 4:
        rgb.r = t, rgb.g = p, rgb.b = v;
        break;
    default:
        rgb.r = v, rgb.g = p, rgb.b = q;
        break;
    }
    return rgb;
}

/*
 * The accent colour dimmed for a fade: level 0-256 goes from the background (bg_pct %
 * of the accent's brightness, 0 = off) to the full accent. All channels are scaled by
 * the same factor, so hue and saturation stay the accent's the whole way: only the
 * brightness changes.
 */
static struct led_rgb fade_accent(struct led_rgb accent, uint8_t bg_pct, uint32_t level) {
    uint32_t bg = MIN(bg_pct, 100) * 256 / 100;
    uint32_t scale = bg + (256 - bg) * MIN(level, 256) / 256; /* 0-256 */
    struct led_rgb out = accent;

    out.r = accent.r * scale / 256;
    out.g = accent.g * scale / 256;
    out.b = accent.b * scale / 256;
    return out;
}

static uint32_t isqrt(uint32_t n) {
    uint32_t root = 0, bit = 1UL << 30;

    while (bit > n) {
        bit >>= 2;
    }
    while (bit) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

/* ---- rendering --------------------------------------------------------------------- */

static void fill(int from, int count, struct led_rgb c) {
    for (int i = from; i < from + count; i++) {
        pixels[i] = c;
    }
}

static uint32_t fade_ms(void) { return CONFIG_ZMK_PRGB_FADE_MS * 3 / state.spd; }

/* Underglow breathes from off to full and back; one cycle is 12 s / speed. */
static bool render_breathe(uint32_t now, uint8_t val) {
    uint32_t period = 12000 / state.spd;
    uint32_t phase = ((now - anim_start_ms) % period) * 512 / period; /* 0-511 */
    uint32_t tri = phase < 256 ? phase : 511 - phase;                  /* 0-255-0 */
    uint32_t smooth = tri * tri * (765 - 2 * tri) / (255 * 255);       /* smoothstep */

    fill(0, UG_LEDS, hsv_to_rgb(state.ug.h, state.ug.s, val * smooth / 255));
    fill(UG_LEDS, PRGB_KEY_LEDS, hsv_to_rgb(state.key.h, state.key.s, val));
    return true;
}

/* Keys rest at the background: a dim tint of the accent (underglow) colour, off by
 * default. A pressed key shows the full accent while held, then its brightness fades
 * back down to the background, staying in the accent colour. With ripples, each press
 * also sends a ring across this half. The key colour is not used here. */
static bool render_reactive(uint32_t now, uint8_t val, bool with_ripples) {
    struct led_rgb accent = hsv_to_rgb(state.ug.h, state.ug.s, val);
    uint32_t fade = fade_ms();
    uint32_t held = atomic_get(&pressed);
    bool animating = false;

    struct ripple active[RIPPLES];
    int n_active = 0;
    if (with_ripples) {
        for (int r = 0; r < RIPPLES; r++) {
            struct ripple rp = ripples[r];
            if (now - rp.start_ms < fade && rp.key < PRGB_KEY_LEDS) {
                active[n_active++] = rp;
            }
        }
        animating = n_active > 0;
    }

    fill(0, UG_LEDS, accent);

    for (int k = 0; k < PRGB_KEY_LEDS; k++) {
        uint32_t level = 0;

        if (held & BIT(k)) {
            level = 256;
        } else {
            uint32_t since = now - released_ms[k];
            if (since < fade) {
                level = 256 - since * 256 / fade;
                animating = true;
            }
        }

        for (int r = 0; r < n_active && level < 256; r++) {
            const struct prgb_key_led *o = &prgb_key_leds[active[r].key];
            int dx = (int)prgb_key_leds[k].x - o->x;
            int dy = (int)prgb_key_leds[k].y - o->y;
            uint32_t dist = isqrt(dx * dx + dy * dy);
            uint32_t age = now - active[r].start_ms;
            /* ring radius grows ~160 grid units per second at speed 3 */
            uint32_t radius = age * 160 * state.spd / 3 / 1000;
            uint32_t off = dist > radius ? dist - radius : radius - dist;

            if (off < RIPPLE_WIDTH) {
                uint32_t ring = (RIPPLE_WIDTH - off) * 256 / RIPPLE_WIDTH;
                uint32_t lvl = ring * (fade - age) / fade;
                level = MAX(level, lvl);
            }
        }

        pixels[UG_LEDS + k] = fade_accent(accent, state.bg, level);
    }

    return animating;
}

/* Keys: one full rainbow across the whole board by x, scrolling outward-right. */
static bool render_rainbow(uint32_t now, uint8_t val) {
    uint32_t period = 12000 / state.spd;
    uint32_t shift = ((now - anim_start_ms) % period) * PRGB_HUE_MAX / period;

    fill(0, UG_LEDS, hsv_to_rgb(state.ug.h, state.ug.s, val));
    for (int k = 0; k < PRGB_KEY_LEDS; k++) {
        uint32_t h = prgb_key_leds[k].x * PRGB_HUE_MAX / QMK_GRID_W;
        h = (h + PRGB_HUE_MAX - shift) % PRGB_HUE_MAX;
        pixels[UG_LEDS + k] = hsv_to_rgb(h, PRGB_SAT_MAX, val);
    }
    return true;
}

/* Keys: each press warms the key by a third (and keys within HEAT_SPREAD a little), and
 * heat runs from blue through green to red. Full heat cools off in 6 x the fade time (a
 * single press in 2 x). Cold keys show the background, as in Reactive. */
static bool render_heatmap(uint32_t now, uint8_t val) {
    struct led_rgb accent = hsv_to_rgb(state.ug.h, state.ug.s, val);
    uint32_t cool = 6 * fade_ms();
    uint32_t elapsed = MIN(now - heat_ms, cool);
    uint32_t dec = elapsed * HEAT_MAX / cool;
    uint32_t hits = atomic_set(&heat_hits, 0);
    uint32_t bg = MIN(state.bg, 100) * 256 / 100;
    bool animating = false;

    /* advance the clock only by the time used, so short frames don't lose cooling */
    heat_ms = elapsed == cool ? now : heat_ms + dec * cool / HEAT_MAX;

    for (int k = 0; k < PRGB_KEY_LEDS; k++) {
        heat[k] = heat[k] > dec ? heat[k] - dec : 0;
    }

    for (int j = 0; hits; j++, hits >>= 1) {
        if (!(hits & 1) || j >= PRGB_KEY_LEDS) {
            continue;
        }
        for (int k = 0; k < PRGB_KEY_LEDS; k++) {
            int dx = (int)prgb_key_leds[k].x - prgb_key_leds[j].x;
            int dy = (int)prgb_key_leds[k].y - prgb_key_leds[j].y;
            uint32_t d = isqrt(dx * dx + dy * dy);
            uint32_t add = 0;

            if (k == j) {
                add = HEAT_HIT;
            } else if (d < HEAT_SPREAD) {
                add = HEAT_HIT / 4 * (HEAT_SPREAD - d) / HEAT_SPREAD;
            }
            heat[k] = MIN(heat[k] + add, HEAT_MAX);
        }
    }

    fill(0, UG_LEDS, accent);
    for (int k = 0; k < PRGB_KEY_LEDS; k++) {
        if (!heat[k]) {
            pixels[UG_LEDS + k] = fade_accent(accent, state.bg, 0);
            continue;
        }
        animating = true;
        /* hue 240 (blue) -> 0 (red); brightness rises fast from the background */
        uint32_t h = 240 - heat[k] * 240 / HEAT_MAX;
        uint32_t ramp = MIN(heat[k] * 8, HEAT_MAX);
        uint32_t v = val * (bg + (256 - bg) * ramp / HEAT_MAX) / 256;
        pixels[UG_LEDS + k] = hsv_to_rgb(h, PRGB_SAT_MAX, v);
    }
    return animating;
}

/* ---- indicators ---------------------------------------------------------------------- */

/* An indicator colour at pct % of val; a pulsing one breathes between 25% and 100%. */
static struct led_rgb ind_rgb(struct prgb_ind_color c, uint8_t val, uint32_t now,
                              bool *animating) {
    uint32_t v = val * c.pct / 100;

    if (c.pulse) {
        uint32_t phase = (now % IND_BLINK_MS) * 512 / IND_BLINK_MS; /* 0-511 */
        uint32_t tri = phase < 256 ? phase : 511 - phase;          /* 0-255-0 */

        v = v * (64 + tri * 192 / 255) / 256;
        *animating = true;
    }
    return hsv_to_rgb(c.h, c.s, v);
}

/* The info (prgb_ind.h) of key LED k. */
static uint8_t ind_info(int k) { return prgb_ind_unpack(ind_planes, led_bit[k]); }

/* Flash level (0-256) of the keys bound to nothing: full for IND_FLASH_HOLD_MS after a
 * colour setting changed, then easing out by IND_FLASH_MS. */
static uint32_t flash_level(uint32_t now, bool *animating) {
    uint32_t age = now - flash_ms;

    if (!flash_on || age >= IND_FLASH_MS) {
        flash_on = false;
        return 0;
    }
    *animating = true;
    if (age < IND_FLASH_HOLD_MS) {
        return 256;
    }
    uint32_t r = 256 - (age - IND_FLASH_HOLD_MS) * 256 / (IND_FLASH_MS - IND_FLASH_HOLD_MS);
    return r * r / 256; /* ease out */
}

/* Layer key map: keys by binding class (Bluetooth/output keys by status, hue keys round
 * the colour wheel, unbound keys flashing a colour that was just changed), underglow in
 * the layer's tint. Replaces the effect; with unbound false the unbound keys keep what
 * the effect drew. Returns the underglow's full colour. */
static struct led_rgb render_map(uint32_t now, uint8_t val, uint32_t header, bool unbound,
                                 bool *animating) {
    struct led_rgb tint =
        ind_rgb(prgb_ind_layer_tint(PRGB_IND_HEADER_LAYER(header)), val, now, animating);

    fill(0, UG_LEDS, tint);

    uint32_t flash = flash_level(now, animating);
    struct led_rgb flash_rgb =
        hsv_to_rgb(flash_color.h, flash_color.s, val * flash_pct / 100 * flash / 256);

    for (int k = 0; k < PRGB_KEY_LEDS; k++) {
        uint8_t cls = ind_info(k) & PRGB_IND_CLASS_MASK;
        struct prgb_ind_color c =
            prgb_ind_class_colors[cls < PRGB_IND_CLASSES ? cls : PRGB_IND_KEY];

        if (cls == PRGB_IND_LIGHT_HUE) {
            c.h = (c.h + (now % IND_HUE_CYCLE_MS) * PRGB_HUE_MAX / IND_HUE_CYCLE_MS) %
                  PRGB_HUE_MAX;
            *animating = true;
        }
#if IND_CENTRAL
        if (conn_colors[k].pct) {
            c = conn_colors[k];
        }
#endif
        if (cls == PRGB_IND_NONE && !c.pct) {
            if (unbound) {
                pixels[UG_LEDS + k] = flash_rgb;
            }
            continue;
        }
        pixels[UG_LEDS + k] = ind_rgb(c, val, now, animating);
    }
    return tint;
}

/* Held modifiers and caps lock light their keys over whatever is drawn. */
static void render_mods_caps(uint32_t now, uint8_t val) {
    bool unused;

    if (!ind_planes[PRGB_IND_PLANE_MOD] && !ind_planes[PRGB_IND_PLANE_CAPS]) {
        return;
    }
    for (int k = 0; k < PRGB_KEY_LEDS; k++) {
        uint8_t info = ind_info(k);

        if (info & PRGB_IND_CAPS) {
            pixels[UG_LEDS + k] = ind_rgb(prgb_ind_caps_color, val, now, &unused);
        } else if (info & PRGB_IND_MOD) {
            pixels[UG_LEDS + k] = ind_rgb(prgb_ind_mod_color, val, now, &unused);
        }
    }
}

static uint32_t pulse_len_ms(void) { return CONFIG_ZMK_PRGB_PULSE_MS * 8 / (state.spd + 5); }

/*
 * Underglow pulse: the underglow rests at PULSE_REST_PCT of what the effect drew and
 * jumps to its full colour (`full`) on a key press, easing back out over pulse_len_ms()
 * (400 ms at speed 3, 530-320 ms over speeds 1-5). Stays within the brightness: full is
 * the colour at the current brightness.
 */
static bool render_pulse(uint32_t now, struct led_rgb full) {
    uint32_t len = pulse_len_ms();
    uint32_t age = now - pulse_ms;
    uint32_t p = 0; /* 0-256 */

    if (age < len) {
        uint32_t r = 256 - age * 256 / len;
        p = r * r / 256; /* ease out */
    }

    for (int i = 0; i < UG_LEDS; i++) {
        uint8_t *px[3] = {&pixels[i].r, &pixels[i].g, &pixels[i].b};
        uint8_t to[3] = {full.r, full.g, full.b};

        for (int c = 0; c < 3; c++) {
            int32_t rest = *px[c] * PULSE_REST_PCT / 100;
            *px[c] = rest + ((int32_t)to[c] - rest) * (int32_t)p / 256;
        }
    }
    return age < len;
}

static void render_handler(struct k_work *work) {
    uint32_t now = k_uptime_get_32();
    bool animating = false;

    if (!state.on || !awake) {
        memset(pixels, 0, sizeof(pixels));
    } else {
        uint8_t val = state.brt * CONFIG_ZMK_PRGB_BRT_MAX * 255 / (PRGB_BRT_MAX * 100);
        uint32_t header = ind_planes[PRGB_IND_PLANE_HEADER];
        struct led_rgb ug_full = hsv_to_rgb(state.ug.h, state.ug.s, val);

        /* LEDs past the underglow + key zones (if the chain is longer) stay dark */
        memset(pixels, 0, sizeof(pixels));

        bool map = PRGB_IND_HEADER_SHOWN(header);
        bool map_unbound = map && prgb_ind_layer_map_unbound(PRGB_IND_HEADER_LAYER(header));

        /* the effect runs unless the key map covers every key */
        if (!map_unbound) {
            switch (state.effect) {
            case PRGB_EFF_BREATHE:
                animating = render_breathe(now, val);
                break;
            case PRGB_EFF_REACTIVE:
                animating = render_reactive(now, val, false);
                break;
            case PRGB_EFF_RIPPLE:
                animating = render_reactive(now, val, true);
                break;
            case PRGB_EFF_RAINBOW:
                animating = render_rainbow(now, val);
                break;
            case PRGB_EFF_HEATMAP:
                animating = render_heatmap(now, val);
                break;
            case PRGB_EFF_STATIC:
            default:
                fill(0, UG_LEDS, hsv_to_rgb(state.ug.h, state.ug.s, val));
                fill(UG_LEDS, PRGB_KEY_LEDS, hsv_to_rgb(state.key.h, state.key.s, val));
                break;
            }
        }
        if (map) {
            ug_full = render_map(now, val, header, map_unbound, &animating);
        }

        render_mods_caps(now, val);

        if (state.pulse && render_pulse(now, ug_full)) {
            animating = true;
        }
    }

    int err = led_strip_update_rgb(strip, pixels, STRIP_LEDS);
    if (err < 0) {
        LOG_ERR("Failed to update the LED strip (%d)", err);
    }

    if (animating) {
        k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &render_work,
                                  K_MSEC(FRAME_MS));
    }
}

/* Render now (and keep going if the effect animates). */
static void kick(void) {
    if (!ready) {
        return;
    }
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &render_work, K_NO_WAIT);
}

ZMK_EVENT_IMPL(zmk_prgb_state_changed);

/* Tell listeners (the OLED status screen) about a new state. */
static void notify(void) {
    raise_zmk_prgb_state_changed((struct zmk_prgb_state_changed){.status = prgb_get_status()});
}

/* ---- persistence ------------------------------------------------------------------- */

static int save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

#if IS_ENABLED(CONFIG_SETTINGS)
static void save_work_handler(struct k_work *work) {
    int err = settings_save_one("prgb/state", &state, sizeof(state));
    if (err < 0) {
        LOG_ERR("Failed to save lighting state (%d)", err);
    }
}

static bool state_valid(const struct prgb_state *s) {
    return s->version == STATE_VERSION && s->effect < PRGB_EFF_COUNT &&
           s->brt <= PRGB_BRT_MAX && s->spd >= PRGB_SPD_MIN && s->spd <= PRGB_SPD_MAX &&
           s->ug.h < PRGB_HUE_MAX && s->ug.s <= PRGB_SAT_MAX && s->key.h < PRGB_HUE_MAX &&
           s->key.s <= PRGB_SAT_MAX && s->bg <= PRGB_BG_MAX && s->map <= 1 && s->pulse <= 1;
}

/*
 * Read the saved state, upgrading older versions: v1 (no bg) and v2 (no indicator
 * toggles) get the new fields' initial values. Formats are told apart by size.
 */
static int read_state(size_t len, settings_read_cb read_cb, void *cb_arg,
                      struct prgb_state *out) {
    int rc;

    if (len == sizeof(*out)) {
        rc = read_cb(cb_arg, out, sizeof(*out));
        return rc < 0 ? rc : 0;
    }

    if (len == sizeof(struct prgb_state_v2)) {
        struct prgb_state_v2 v2;

        rc = read_cb(cb_arg, &v2, sizeof(v2));
        if (rc < 0) {
            return rc;
        }
        if (v2.version != STATE_VERSION_2) {
            return -EINVAL;
        }
        *out = (struct prgb_state){
            .version = STATE_VERSION,
            .on = v2.on,
            .effect = v2.effect,
            .brt = v2.brt,
            .spd = v2.spd,
            .ug = v2.ug,
            .key = v2.key,
            .bg = v2.bg,
            .map = IS_ENABLED(CONFIG_ZMK_PRGB_MAP_START),
            .pulse = IS_ENABLED(CONFIG_ZMK_PRGB_PULSE_START),
        };
        return 0;
    }

    if (len == sizeof(struct prgb_state_v1)) {
        struct prgb_state_v1 v1;

        rc = read_cb(cb_arg, &v1, sizeof(v1));
        if (rc < 0) {
            return rc;
        }
        if (v1.version != STATE_VERSION_1) {
            return -EINVAL;
        }
        *out = (struct prgb_state){
            .version = STATE_VERSION,
            .on = v1.on,
            .effect = v1.effect,
            .brt = v1.brt,
            .spd = v1.spd,
            .ug = v1.ug,
            .key = v1.key,
            .bg = CONFIG_ZMK_PRGB_BG_START,
            .map = IS_ENABLED(CONFIG_ZMK_PRGB_MAP_START),
            .pulse = IS_ENABLED(CONFIG_ZMK_PRGB_PULSE_START),
        };
        return 0;
    }

    return -EINVAL;
}

static int prgb_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                             void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "state", &next) && !next) {
        struct prgb_state loaded;

        if (read_state(len, read_cb, cb_arg, &loaded) < 0) {
            LOG_WRN("Ignoring saved lighting state (unknown format)");
            return 0;
        }
        /* An effect this firmware doesn't have (saved by a newer one): keep the rest. */
        if (loaded.effect >= PRGB_EFF_COUNT) {
            LOG_WRN("Saved effect %d unknown, using static", loaded.effect);
            loaded.effect = PRGB_EFF_STATIC;
        }
        if (!state_valid(&loaded)) {
            LOG_WRN("Ignoring saved lighting state (old format or out of range)");
            return 0;
        }

        state = loaded;
        anim_start_ms = k_uptime_get_32();
        ind_update();
        kick();
        notify();
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(prgb, "prgb", NULL, prgb_settings_set, NULL, NULL);
#endif /* IS_ENABLED(CONFIG_SETTINGS) */

/* ---- API (used by the &prgb behavior) ---------------------------------------------- */

static int changed(void) {
    kick();
    notify();
    return save_state();
}

/* Show a colour that was just set on the keys bound to nothing in the key map (if it is
 * shown: otherwise the effect itself shows the change). Runs on both halves, as the
 * setting does. */
static void flash(struct prgb_hs color, uint8_t pct) {
    flash_color = color;
    flash_pct = pct;
    flash_ms = k_uptime_get_32();
    flash_on = true;
}

bool prgb_is_on(void) { return state.on; }

struct prgb_status prgb_get_status(void) {
    return (struct prgb_status){
        .on = state.on, .effect = state.effect, .brt = state.brt, .spd = state.spd};
}

/* Used when the &prgb behavior (which has the effect names) is not built. */
__weak const char *prgb_effect_name(uint8_t effect, char *buf, size_t len) {
    snprintf(buf, len, "FX %u", effect);
    return buf;
}

int prgb_set_on(bool on) {
    state.on = on;
    anim_start_ms = k_uptime_get_32();
    return changed();
}

int prgb_calc_effect(int direction) {
    return (state.effect + PRGB_EFF_COUNT + direction % PRGB_EFF_COUNT) % PRGB_EFF_COUNT;
}

int prgb_set_effect(int effect) {
    if (effect < 0 || effect >= PRGB_EFF_COUNT) {
        return -EINVAL;
    }
    state.effect = effect;
    anim_start_ms = k_uptime_get_32();
    return changed();
}

int prgb_calc_brt(int direction) {
    return CLAMP(state.brt + direction * CONFIG_ZMK_PRGB_BRT_STEP, 0, PRGB_BRT_MAX);
}

int prgb_set_brt(int brt) {
    if (brt < 0 || brt > PRGB_BRT_MAX) {
        return -EINVAL;
    }
    state.brt = brt;
    flash(state.key, 100);
    return changed();
}

int prgb_calc_spd(int direction) {
    return CLAMP(state.spd + direction, PRGB_SPD_MIN, PRGB_SPD_MAX);
}

int prgb_set_spd(int spd) {
    if (spd < PRGB_SPD_MIN || spd > PRGB_SPD_MAX) {
        return -EINVAL;
    }
    state.spd = spd;
    return changed();
}

struct prgb_hs prgb_calc_color(enum prgb_zone zone, int hue_dir, int sat_dir) {
    struct prgb_hs c = zone == PRGB_ZONE_UNDERGLOW ? state.ug : state.key;
    int h = c.h + hue_dir * CONFIG_ZMK_PRGB_HUE_STEP;

    c.h = ((h % PRGB_HUE_MAX) + PRGB_HUE_MAX) % PRGB_HUE_MAX;
    c.s = CLAMP(c.s + sat_dir * CONFIG_ZMK_PRGB_SAT_STEP, 0, PRGB_SAT_MAX);
    return c;
}

int prgb_set_color(enum prgb_zone zone, struct prgb_hs color) {
    if (color.h >= PRGB_HUE_MAX || color.s > PRGB_SAT_MAX) {
        return -EINVAL;
    }
    if (zone == PRGB_ZONE_UNDERGLOW) {
        state.ug = color;
    } else {
        state.key = color;
    }
    flash(color, 100);
    return changed();
}

int prgb_calc_bg_next(void) {
    for (size_t i = 0; i < ARRAY_SIZE(bg_levels); i++) {
        if (bg_levels[i] > state.bg) {
            return bg_levels[i];
        }
    }
    return bg_levels[0];
}

int prgb_set_bg(int bg) {
    if (bg < 0 || bg > PRGB_BG_MAX) {
        return -EINVAL;
    }
    state.bg = bg;
    /* The background is a dim tint of the accent (underglow) colour: show it at its level,
     * but visibly (a 0% background flashes the accent faintly) */
    flash(state.ug, MAX(bg, 5));
    return changed();
}

bool prgb_map_enabled(void) { return state.map != 0; }

int prgb_set_map(bool on) {
    state.map = on;
    ind_update();
    return changed();
}

bool prgb_pulse_enabled(void) { return state.pulse != 0; }

int prgb_set_pulse(bool on) {
    state.pulse = on;
    return changed();
}

int prgb_relay_receive(uint32_t param1, uint32_t value) {
#if IND_CENTRAL
    /* Only the central sends these; it works its own indicators out itself. */
    return 0;
#else
    int plane = prgb_ind_relay_plane(param1);

    if (plane < 0) {
        return -EINVAL;
    }
    /* Called from the Bluetooth thread: one word store, the renderer runs on its queue */
    ind_planes[plane] = value;
    kick();
    return 0;
#endif
}

/* ---- indicators: central --------------------------------------------------------------- */

#if IND_CENTRAL

/* HID LED report bit of caps lock (usage 0x02 of the LED page, bit 1). */
#define HID_LED_CAPS_LOCK BIT(1)

static bool keymap_binding(void *ctx, uint8_t layer, uint8_t position,
                           struct prgb_ind_binding *out) {
    const struct zmk_behavior_binding *b = zmk_keymap_get_layer_binding_at_idx(layer, position);

    if (!b || !b->behavior_dev) {
        return false;
    }
    *out = (struct prgb_ind_binding){
        .dev = b->behavior_dev, .param1 = b->param1, .param2 = b->param2};
    return true;
}

static bool caps_lock_on(void) {
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    return zmk_hid_indicators_get_current_profile() & HID_LED_CAPS_LOCK;
#else
    return false;
#endif
}

static struct prgb_ind_conn conn_state(void) {
    struct prgb_ind_conn c = {.active = -1};

#if IS_ENABLED(CONFIG_ZMK_BLE)
    c.active = zmk_ble_active_profile_index();
    c.profiles = MIN(ZMK_BLE_PROFILE_COUNT, 8);
    for (uint8_t n = 0; n < c.profiles; n++) {
        if (zmk_ble_profile_is_connected(n)) {
            c.connected |= BIT(n);
        }
        if (zmk_ble_profile_is_open(n)) {
            c.open |= BIT(n);
        }
    }
#endif
    switch (zmk_endpoint_get_selected().transport) {
    case ZMK_TRANSPORT_USB:
        c.transport = PRGB_IND_TRANSPORT_USB;
        break;
    case ZMK_TRANSPORT_BLE:
        c.transport = PRGB_IND_TRANSPORT_BLE;
        break;
    default:
        c.transport = PRGB_IND_TRANSPORT_NONE;
        break;
    }
    return c;
}

#if IND_RELAY

BUILD_ASSERT(ZMK_KEYMAP_LEN <= 64, "Key positions are kept in a 64-bit mask");

#define PRGB_DEV_NAME DEVICE_DT_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_behavior_prgb))

/* What the peripheral was last sent (it starts from all zero after connecting). */
static uint32_t relay_sent[PRGB_IND_PLANES];
/* Bit 0: the peripheral disconnected, so it has cleared its planes. */
static atomic_t relay_reset;
/* Send every non-zero plane again (in case it was lost while the link came up). */
static bool relay_force;

static void relay_send(int plane, uint32_t value) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = PRGB_DEV_NAME,
        .param1 = PRGB_IND_RELAY_PARAM1(plane),
        .param2 = value,
    };
    struct zmk_behavior_binding_event event = {
        .position = 0,
        .timestamp = k_uptime_get(),
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
    };

    /* The same path global &prgb commands take to the peripheral; only "pressed". */
    for (int i = 0; i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; i++) {
        int err = zmk_split_central_invoke_behavior(i, &binding, event, true);
        if (err < 0) {
            LOG_DBG("Indicator relay to peripheral %d failed (%d)", i, err);
        }
    }
    relay_sent[plane] = value;
}

/* Send the peripheral the planes that changed: class planes first, the header last, so
 * the key map is complete when the header turns it on. The class planes don't matter
 * while it is hidden and are not sent then. */
static void relay(const uint8_t *info, uint32_t header) {
    uint64_t all = ZMK_KEYMAP_LEN >= 64 ? UINT64_MAX : (1ULL << ZMK_KEYMAP_LEN) - 1;
    uint32_t planes[PRGB_IND_PLANES];

    if (atomic_test_and_clear_bit(&relay_reset, 0)) {
        memset(relay_sent, 0, sizeof(relay_sent));
    }

    prgb_ind_pack(info, ZMK_KEYMAP_LEN, all & ~own_positions, header, planes);

    for (int p = 0; p < PRGB_IND_PLANES; p++) {
        bool forced = relay_force && planes[p];

        if (p < PRGB_IND_PLANE_MOD && !PRGB_IND_HEADER_SHOWN(header)) {
            continue;
        }
        if (forced || planes[p] != relay_sent[p]) {
            relay_send(p, planes[p]);
        }
    }
    relay_force = false;
}

#endif /* IND_RELAY */

/*
 * Work out the indicators of both halves from the keymap (as it is now, so ZMK Studio
 * edits count), the layer, modifier, caps lock and connection state. Runs on the
 * renderer's work queue, so the planes never change under a frame.
 */
static void ind_work_handler(struct k_work *work) {
    static uint8_t info[ZMK_KEYMAP_LEN]; /* off the work queue's stack */
    uint8_t active[ZMK_KEYMAP_LAYERS_LEN];
    uint8_t n_active = 0;
    zmk_keymap_layer_id_t def = zmk_keymap_layer_default();

    /* Active layers from the top down to the default one, as the keymap resolves keys */
    for (int idx = ZMK_KEYMAP_LAYERS_LEN - 1; idx >= 0; idx--) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(idx);

        if (id == ZMK_KEYMAP_LAYER_ID_INVAL || !zmk_keymap_layer_active(id)) {
            continue;
        }
        active[n_active++] = id;
        if (id == def) {
            break;
        }
    }

    bool show_map = state.map && n_active && active[0] != def;
    struct prgb_ind_in in = {
        .get = keymap_binding,
        .n_pos = ZMK_KEYMAP_LEN,
        .active = active,
        .n_active = n_active,
        .n_layers = ZMK_KEYMAP_LAYERS_LEN,
        .show_map = show_map,
        .mods = zmk_hid_get_explicit_mods(),
        .caps = caps_lock_on(),
        .caps_fallback = CONFIG_ZMK_PRGB_CAPS_FALLBACK_POS,
    };
    uint32_t header = PRGB_IND_HEADER(show_map, show_map ? active[0] : 0);
    uint32_t planes[PRGB_IND_PLANES];

    prgb_ind_compute(&in, info);
    prgb_ind_pack(info, ZMK_KEYMAP_LEN, own_positions, header, planes);

    struct prgb_ind_conn conn = conn_state();
    for (int k = 0; k < PRGB_KEY_LEDS; k++) {
        struct prgb_ind_binding b;

        conn_colors[k] = (struct prgb_ind_color){0};
        if (show_map && keymap_binding(NULL, active[0], prgb_key_leds[k].position, &b)) {
            conn_colors[k] = prgb_ind_conn_color(&b, &conn);
        }
    }
    memcpy(ind_planes, planes, sizeof(ind_planes));

#if IND_RELAY
    relay(info, header);
#endif

    kick();
}

static void ind_update(void) {
    if (ready) {
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &ind_work);
    }
}

#if IND_RESYNC_ON_CONNECT

/*
 * The peripheral clears its indicators when the link drops and starts from zero when
 * it comes back, but messages sent before the link has found the peripheral's
 * behavior characteristic are dropped. So after the peripheral connects, send it
 * everything that isn't zero a few times (2, 5 and 10 s after connecting).
 */
static struct k_work_delayable resync_work;
static uint8_t resync_step;
static const uint16_t resync_delay_ms[] = {2000, 3000, 5000};

static void resync_work_handler(struct k_work *work) {
    relay_force = true;
    ind_work_handler(NULL);
    if (++resync_step < ARRAY_SIZE(resync_delay_ms)) {
        k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &resync_work,
                                  K_MSEC(resync_delay_ms[resync_step]));
    }
}

/* Connections in the central role are the split peripherals (hosts connect to us). */
static bool is_split_conn(struct bt_conn *conn) {
    struct bt_conn_info info;

    return bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_CENTRAL;
}

static void split_connected(struct bt_conn *conn, uint8_t err) {
    if (err || !ready || !is_split_conn(conn)) {
        return;
    }
    atomic_set_bit(&relay_reset, 0);
    resync_step = 0;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &resync_work,
                                K_MSEC(resync_delay_ms[0]));
}

static void split_disconnected(struct bt_conn *conn, uint8_t reason) {
    if (!ready || !is_split_conn(conn)) {
        return;
    }
    atomic_set_bit(&relay_reset, 0);
}

BT_CONN_CB_DEFINE(prgb_split_conn_cb) = {
    .connected = split_connected,
    .disconnected = split_disconnected,
};

#endif /* IND_RESYNC_ON_CONNECT */

#else /* !IND_CENTRAL */

static void ind_update(void) {}

#endif /* IND_CENTRAL */

/* ---- events ------------------------------------------------------------------------ */

static void on_position(const struct zmk_position_state_changed *ev) {
    /* On the central, presses relayed from the peripheral have the peripheral as
     * source: those keys are lit by the peripheral itself. */
    if (ev->source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL ||
        ev->position >= ARRAY_SIZE(key_for_position)) {
        return;
    }

    int k = key_for_position[ev->position];
    if (k < 0) {
        return; /* combo / virtual position, or not a key with an LED on this half */
    }

    uint32_t now = k_uptime_get_32();

    if (ev->state) {
        pulse_ms = now;
        ripples[next_ripple].key = k;
        ripples[next_ripple].start_ms = now;
        next_ripple = (next_ripple + 1) % RIPPLES;
        atomic_set_bit(&pressed, k);
    } else {
        released_ms[k] = now;
        atomic_clear_bit(&pressed, k);
    }

    if (ev->state && state.effect == PRGB_EFF_HEATMAP) {
        atomic_set_bit(&heat_hits, k);
    }

    if (state.on && awake &&
        (state.effect == PRGB_EFF_REACTIVE || state.effect == PRGB_EFF_RIPPLE ||
         state.effect == PRGB_EFF_HEATMAP ||
         (ev->state && state.pulse))) {
        kick();
    }
}

static int prgb_event_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos) {
        on_position(pos);
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IND_CENTRAL
    /* Modifiers change with modifier key codes (the HID state is updated by the time the
     * indicator work runs); everything else here may change what the indicators show. */
    const struct zmk_keycode_state_changed *kc = as_zmk_keycode_state_changed(eh);
    if (kc) {
        if (kc->usage_page == PRGB_IND_USAGE_KEY_PAGE &&
            kc->keycode >= PRGB_IND_USAGE_LCTRL && kc->keycode <= PRGB_IND_USAGE_LCTRL + 7) {
            ind_update();
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (as_zmk_layer_state_changed(eh) || as_zmk_endpoint_changed(eh)
#if IS_ENABLED(CONFIG_ZMK_BLE)
        || as_zmk_ble_active_profile_changed(eh)
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        || as_zmk_hid_indicators_changed(eh)
#endif
    ) {
        ind_update();
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif /* IND_CENTRAL */

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* Lost the central: drop what it relayed (it resends after reconnecting). */
    const struct zmk_split_peripheral_status_changed *st =
        as_zmk_split_peripheral_status_changed(eh);
    if (st) {
        if (!st->connected) {
            memset(ind_planes, 0, sizeof(ind_planes));
            kick();
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_PRGB_AUTO_OFF_IDLE)
    const struct zmk_activity_state_changed *act = as_zmk_activity_state_changed(eh);
    if (act) {
        bool now_awake = act->state == ZMK_ACTIVITY_ACTIVE;
        if (now_awake != awake) {
            awake = now_awake;
            anim_start_ms = k_uptime_get_32();
            kick();
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(prgb, prgb_event_listener);
ZMK_SUBSCRIPTION(prgb, zmk_position_state_changed);
#if IND_CENTRAL
ZMK_SUBSCRIPTION(prgb, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(prgb, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(prgb, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(prgb, zmk_ble_active_profile_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(prgb, zmk_hid_indicators_changed);
#endif
#endif /* IND_CENTRAL */
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(prgb, zmk_split_peripheral_status_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_PRGB_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(prgb, zmk_activity_state_changed);
#endif

/* ---- init -------------------------------------------------------------------------- */

static int prgb_init(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip %s is not ready", strip->name);
        return -ENODEV;
    }

    state = (struct prgb_state){
        .version = STATE_VERSION,
        .on = IS_ENABLED(CONFIG_ZMK_PRGB_ON_START),
        .effect = CONFIG_ZMK_PRGB_EFF_START,
        .brt = CONFIG_ZMK_PRGB_BRT_START,
        .spd = CONFIG_ZMK_PRGB_SPD_START,
        .ug = {.h = CONFIG_ZMK_PRGB_UG_HUE_START, .s = CONFIG_ZMK_PRGB_UG_SAT_START},
        .key = {.h = CONFIG_ZMK_PRGB_KEY_HUE_START, .s = CONFIG_ZMK_PRGB_KEY_SAT_START},
        .bg = CONFIG_ZMK_PRGB_BG_START,
        .map = IS_ENABLED(CONFIG_ZMK_PRGB_MAP_START),
        .pulse = IS_ENABLED(CONFIG_ZMK_PRGB_PULSE_START),
    };

    uint32_t now = k_uptime_get_32();
    anim_start_ms = now;

    for (size_t p = 0; p < ARRAY_SIZE(key_for_position); p++) {
        key_for_position[p] = -1;
    }
    for (int k = 0; k < PRGB_KEY_LEDS; k++) {
        key_for_position[prgb_key_leds[k].position] = k;
        own_positions |= 1ULL << prgb_key_leds[k].position;
        released_ms[k] = now - LONG_AGO_MS;
    }
    /* Indicator plane bit of each LED: its position's rank among this half's positions */
    for (int k = 0; k < PRGB_KEY_LEDS; k++) {
        uint64_t below = own_positions & ((1ULL << prgb_key_leds[k].position) - 1);

        led_bit[k] = __builtin_popcountll(below);
    }
    pulse_ms = now - LONG_AGO_MS;
    heat_ms = now;
    for (int r = 0; r < RIPPLES; r++) {
        ripples[r].start_ms = now - LONG_AGO_MS;
    }

    k_work_init_delayable(&render_work, render_handler);
#if IND_CENTRAL
    k_work_init(&ind_work, ind_work_handler);
#endif
#if IND_RESYNC_ON_CONNECT
    k_work_init_delayable(&resync_work, resync_work_handler);
#endif
#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&save_work, save_work_handler);
#endif

    ready = true;
    ind_update();
    kick();
    return 0;
}

SYS_INIT(prgb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
