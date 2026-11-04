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
 * SPDX-License-Identifier: MIT
 */

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
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/workqueue.h>

#include <dt-bindings/prgb.h>

#include "prgb.h"
#include "aurora_lily58_leds.h"

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

#define STATE_VERSION 1

struct prgb_state {
    uint8_t version;
    bool on;
    uint8_t effect;
    uint8_t brt;
    uint8_t spd;
    struct prgb_hs ug;
    struct prgb_hs key;
};

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

static struct k_work_delayable render_work;

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

/* level 0-256: 0 = a, 256 = b */
static struct led_rgb blend(struct led_rgb a, struct led_rgb b, uint32_t level) {
    struct led_rgb out = a;

    out.r = a.r + (((int)b.r - a.r) * (int)level) / 256;
    out.g = a.g + (((int)b.g - a.g) * (int)level) / 256;
    out.b = a.b + (((int)b.b - a.b) * (int)level) / 256;
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

/* Keys at a dim base; a pressed key shows the accent (underglow) colour while held,
 * then fades back. With ripples, each press also sends a ring across this half. */
static bool render_reactive(uint32_t now, uint8_t val, bool with_ripples) {
    struct led_rgb accent = hsv_to_rgb(state.ug.h, state.ug.s, val);
    struct led_rgb base =
        hsv_to_rgb(state.key.h, state.key.s, val * CONFIG_ZMK_PRGB_REACTIVE_BASE / 100);
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

        pixels[UG_LEDS + k] = blend(base, accent, MIN(level, 256));
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

static void render_handler(struct k_work *work) {
    uint32_t now = k_uptime_get_32();
    bool animating = false;

    if (!state.on || !awake) {
        memset(pixels, 0, sizeof(pixels));
    } else {
        uint8_t val = state.brt * CONFIG_ZMK_PRGB_BRT_MAX * 255 / (PRGB_BRT_MAX * 100);

        /* LEDs past the underglow + key zones (if the chain is longer) stay dark */
        memset(pixels, 0, sizeof(pixels));

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
        case PRGB_EFF_STATIC:
        default:
            fill(0, UG_LEDS, hsv_to_rgb(state.ug.h, state.ug.s, val));
            fill(UG_LEDS, PRGB_KEY_LEDS, hsv_to_rgb(state.key.h, state.key.s, val));
            break;
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
           s->key.s <= PRGB_SAT_MAX;
}

static int prgb_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                             void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "state", &next) && !next) {
        struct prgb_state loaded;

        if (len != sizeof(loaded)) {
            return -EINVAL;
        }

        int rc = read_cb(cb_arg, &loaded, sizeof(loaded));
        if (rc < 0) {
            return rc;
        }

        if (!state_valid(&loaded)) {
            LOG_WRN("Ignoring saved lighting state (old format or out of range)");
            return 0;
        }

        state = loaded;
        anim_start_ms = k_uptime_get_32();
        kick();
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(prgb, "prgb", NULL, prgb_settings_set, NULL, NULL);
#endif /* IS_ENABLED(CONFIG_SETTINGS) */

/* ---- API (used by the &prgb behavior) ---------------------------------------------- */

static int changed(void) {
    kick();
    return save_state();
}

bool prgb_is_on(void) { return state.on; }

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
    return changed();
}

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
        ripples[next_ripple].key = k;
        ripples[next_ripple].start_ms = now;
        next_ripple = (next_ripple + 1) % RIPPLES;
        atomic_set_bit(&pressed, k);
    } else {
        released_ms[k] = now;
        atomic_clear_bit(&pressed, k);
    }

    if (state.on && awake &&
        (state.effect == PRGB_EFF_REACTIVE || state.effect == PRGB_EFF_RIPPLE)) {
        kick();
    }
}

static int prgb_event_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos) {
        on_position(pos);
        return ZMK_EV_EVENT_BUBBLE;
    }

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
    };

    uint32_t now = k_uptime_get_32();
    anim_start_ms = now;

    for (size_t p = 0; p < ARRAY_SIZE(key_for_position); p++) {
        key_for_position[p] = -1;
    }
    for (int k = 0; k < PRGB_KEY_LEDS; k++) {
        key_for_position[prgb_key_leds[k].position] = k;
        released_ms[k] = now - LONG_AGO_MS;
    }
    for (int r = 0; r < RIPPLES; r++) {
        ripples[r].start_ms = now - LONG_AGO_MS;
    }

    k_work_init_delayable(&render_work, render_handler);
#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&save_work, save_work_handler);
#endif

    ready = true;
    kick();
    return 0;
}

SYS_INIT(prgb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
