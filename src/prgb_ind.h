/*
 * Per-key RGB lighting: indicator logic (layer key map, modifiers, caps lock, Bluetooth
 * profiles) and the packing used to relay it to the other half.
 *
 * Plain C with no Zephyr dependencies, so scripts/check_indicators.py can compile it on
 * the host and run it against the real keymap. prgb.c gathers the inputs (keymap, layer
 * state, HID state, BLE state) and renders the result.
 *
 * Key positions are numbered as in the keymap. Each half lists its own positions in
 * ascending order; the n-th of them is bit n of every 32-bit indicator "plane". The
 * central computes planes for both halves: its own are rendered locally, the other
 * half's are sent to the peripheral, which only needs to know its own positions to
 * decode them (no keymap, layer or HID state on the peripheral).
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <dt-bindings/prgb.h>

/* ---- binding classes (layer key map) ---------------------------------------------- */

enum prgb_ind_class {
    PRGB_IND_NONE = 0, /* &none, &trans: dark */
    PRGB_IND_KEY,      /* key presses, consumer keys, mod-taps, macros, anything else */
    PRGB_IND_LAYER,    /* &mo, &lt, &tog, &to, &sl */
    PRGB_IND_CONN,     /* &bt, &out */
    PRGB_IND_SYSTEM,   /* &ext_power, &studio_unlock, &sys_reset, &bootloader, &soft_off */
    /* Lighting keys, grouped by what they change (&prgb by command, see
     * prgb_ind_prgb_class(); &rgb_ug and &bl are all PRGB_IND_LIGHT) */
    PRGB_IND_LIGHT,        /* on/off, layer key map and underglow pulse toggles */
    PRGB_IND_LIGHT_EFFECT, /* next/previous/set effect */
    PRGB_IND_LIGHT_SPEED,  /* effect speed */
    PRGB_IND_LIGHT_BRT,    /* brightness, reactive/ripple background level */
    PRGB_IND_LIGHT_HUE,    /* underglow/key hue (and absolute colours) */
    PRGB_IND_LIGHT_SAT,    /* underglow/key saturation */
    PRGB_IND_CLASSES,
};

#define PRGB_IND_CLASS_BITS 4 /* enough for PRGB_IND_CLASSES */

/* Per-position info computed by prgb_ind_compute(): class in bits 0-3, plus flags. */
#define PRGB_IND_CLASS_MASK 0x0F
#define PRGB_IND_MOD 0x10  /* bound to a modifier that is currently held */
#define PRGB_IND_CAPS 0x20 /* the caps lock key, while caps lock is on */

_Static_assert(PRGB_IND_CLASSES <= PRGB_IND_CLASS_MASK + 1, "class bits");

/* Device names (DEVICE_DT_NAME = devicetree node name) of ZMK's behaviors. */
struct prgb_ind_name_class {
    const char *name;
    uint8_t cls;
};

static const struct prgb_ind_name_class prgb_ind_names[] = {
    {"transparent", PRGB_IND_NONE},  {"none", PRGB_IND_NONE},
    {"momentary_layer", PRGB_IND_LAYER}, {"layer_tap", PRGB_IND_LAYER},
    {"toggle_layer", PRGB_IND_LAYER},    {"to_layer", PRGB_IND_LAYER},
    {"sticky_layer", PRGB_IND_LAYER},    {"bluetooth", PRGB_IND_CONN},
    {"outputs", PRGB_IND_CONN},          {"prgb", PRGB_IND_LIGHT},
    {"rgb_ug", PRGB_IND_LIGHT},          {"bcklight", PRGB_IND_LIGHT},
    {"extpower", PRGB_IND_SYSTEM},       {"studio_unlock", PRGB_IND_SYSTEM},
    {"sysreset", PRGB_IND_SYSTEM},       {"bootload", PRGB_IND_SYSTEM},
    {"z_so_off", PRGB_IND_SYSTEM},
};

static inline bool prgb_ind_is(const char *dev, const char *name) {
    return dev && strcmp(dev, name) == 0;
}

/* The group of an &prgb binding, by its command (param1). */
static inline uint8_t prgb_ind_prgb_class(uint32_t param1) {
    switch (param1) {
    case PRGB_EFF_NEXT_CMD:
    case PRGB_EFF_PREV_CMD:
    case PRGB_EFF_SET_CMD:
        return PRGB_IND_LIGHT_EFFECT;
    case PRGB_SPD_UP_CMD:
    case PRGB_SPD_DN_CMD:
    case PRGB_SPD_SET_CMD:
        return PRGB_IND_LIGHT_SPEED;
    case PRGB_BRI_UP_CMD:
    case PRGB_BRI_DN_CMD:
    case PRGB_BRT_SET_CMD:
    case PRGB_BG_NEXT_CMD:
    case PRGB_BG_SET_CMD:
        return PRGB_IND_LIGHT_BRT;
    case PRGB_UG_HUE_UP_CMD:
    case PRGB_UG_HUE_DN_CMD:
    case PRGB_KEY_HUE_UP_CMD:
    case PRGB_KEY_HUE_DN_CMD:
    case PRGB_UG_COLOR_CMD:
    case PRGB_KEY_COLOR_CMD:
        return PRGB_IND_LIGHT_HUE;
    case PRGB_UG_SAT_UP_CMD:
    case PRGB_UG_SAT_DN_CMD:
    case PRGB_KEY_SAT_UP_CMD:
    case PRGB_KEY_SAT_DN_CMD:
        return PRGB_IND_LIGHT_SAT;
    default: /* on/off/toggle, map, pulse */
        return PRGB_IND_LIGHT;
    }
}

static inline uint8_t prgb_ind_classify(const char *dev, uint32_t param1) {
    if (!dev || !dev[0]) {
        return PRGB_IND_NONE;
    }
    if (prgb_ind_is(dev, "prgb")) {
        return prgb_ind_prgb_class(param1);
    }
    for (unsigned i = 0; i < sizeof(prgb_ind_names) / sizeof(prgb_ind_names[0]); i++) {
        if (strcmp(dev, prgb_ind_names[i].name) == 0) {
            return prgb_ind_names[i].cls;
        }
    }
    return PRGB_IND_KEY;
}

/* HID keyboard usages (page 7; page 0 in a binding means the keyboard page too). */
#define PRGB_IND_USAGE_KEY_PAGE 0x07
#define PRGB_IND_USAGE_CAPS 0x39
#define PRGB_IND_USAGE_LOCKING_CAPS 0x82
#define PRGB_IND_USAGE_LCTRL 0xE0 /* LCTRL .. RGUI = 0xE0 .. 0xE7, mod bits 0-7 */

static inline bool prgb_ind_keyboard_usage(uint32_t usage, uint16_t *id) {
    uint8_t page = (usage >> 16) & 0xFF;

    *id = usage & 0xFFFF;
    return page == 0 || page == PRGB_IND_USAGE_KEY_PAGE;
}

/* Behaviors whose first parameter is a keycode that is pressed (held: &mt's hold). */
static inline bool prgb_ind_keycode_behavior(const char *dev) {
    return prgb_ind_is(dev, "key_press") || prgb_ind_is(dev, "sticky_key") ||
           prgb_ind_is(dev, "key_toggle") || prgb_ind_is(dev, "mod_tap");
}

/* The explicit modifier bits (ZMK/HID order: LCTRL bit 0 .. RGUI bit 7) a binding
 * presses: &kp LSHFT, &sk LCTRL, &kt LALT, &mt LGUI X (hold). Not implicit mods of
 * things like &kp LS(A). */
static inline uint8_t prgb_ind_binding_mods(const char *dev, uint32_t param1) {
    uint16_t id;

    if (!prgb_ind_keycode_behavior(dev) || !prgb_ind_keyboard_usage(param1, &id)) {
        return 0;
    }
    if (id >= PRGB_IND_USAGE_LCTRL && id <= PRGB_IND_USAGE_LCTRL + 7) {
        return 1U << (id - PRGB_IND_USAGE_LCTRL);
    }
    return 0;
}

static inline bool prgb_ind_binding_caps(const char *dev, uint32_t param1) {
    uint16_t id;

    if (!(prgb_ind_is(dev, "key_press") || prgb_ind_is(dev, "key_toggle")) ||
        !prgb_ind_keyboard_usage(param1, &id)) {
        return false;
    }
    return id == PRGB_IND_USAGE_CAPS || id == PRGB_IND_USAGE_LOCKING_CAPS;
}

/* ---- computing the indicators ----------------------------------------------------- */

struct prgb_ind_binding {
    const char *dev;
    uint32_t param1;
    uint32_t param2;
};

/* Fill *out with the binding at (layer id, position); false if there is none. */
typedef bool (*prgb_ind_get_fn)(void *ctx, uint8_t layer, uint8_t position,
                                struct prgb_ind_binding *out);

struct prgb_ind_in {
    prgb_ind_get_fn get;
    void *ctx;
    uint8_t n_pos;         /* key positions in the keymap */
    const uint8_t *active; /* active layer ids, highest first (the default layer last) */
    uint8_t n_active;
    uint8_t n_layers;    /* layer ids 0 .. n_layers - 1 exist (caps key search) */
    bool show_map;       /* show the key map of active[0] */
    uint8_t mods;        /* explicit modifiers currently held */
    bool caps;           /* caps lock on */
    uint8_t caps_fallback; /* position to light for caps lock if no key is bound to it */
};

/*
 * info[p] for every position p < n_pos:
 *  - class of the binding on the top active layer itself (&trans stays dark), if
 *    show_map; otherwise PRGB_IND_NONE;
 *  - PRGB_IND_MOD if the binding in effect (top-most active, non-&trans) presses a
 *    modifier that is held now;
 *  - PRGB_IND_CAPS, while caps lock is on, on the keys whose binding in effect is caps
 *    lock; if there are none, on the keys bound to caps lock on any layer; if there are
 *    none either, on caps_fallback.
 */
static inline void prgb_ind_compute(const struct prgb_ind_in *in, uint8_t *info) {
    bool caps_shown = false;

    for (uint8_t p = 0; p < in->n_pos; p++) {
        struct prgb_ind_binding b;
        uint8_t v = 0;

        if (in->show_map && in->n_active && in->get(in->ctx, in->active[0], p, &b)) {
            v = prgb_ind_classify(b.dev, b.param1);
        }

        if (in->mods || in->caps) {
            for (uint8_t l = 0; l < in->n_active; l++) {
                if (!in->get(in->ctx, in->active[l], p, &b) ||
                    prgb_ind_is(b.dev, "transparent")) {
                    continue;
                }
                if (prgb_ind_binding_mods(b.dev, b.param1) & in->mods) {
                    v |= PRGB_IND_MOD;
                }
                if (in->caps && prgb_ind_binding_caps(b.dev, b.param1)) {
                    v |= PRGB_IND_CAPS;
                    caps_shown = true;
                }
                break;
            }
        }
        info[p] = v;
    }

    if (!in->caps || caps_shown) {
        return;
    }
    for (uint8_t p = 0; p < in->n_pos; p++) {
        for (uint8_t l = 0; l < in->n_layers; l++) {
            struct prgb_ind_binding b;

            if (in->get(in->ctx, l, p, &b) && prgb_ind_binding_caps(b.dev, b.param1)) {
                info[p] |= PRGB_IND_CAPS;
                caps_shown = true;
                break;
            }
        }
    }
    if (!caps_shown && in->caps_fallback < in->n_pos) {
        info[in->caps_fallback] |= PRGB_IND_CAPS;
    }
}

/* ---- planes (relay format) ---------------------------------------------------------- */

/* Planes 0-3: class bits 0-3; 4: modifier; 5: caps lock; 6: header. */
#define PRGB_IND_PLANE_CLASS 0
#define PRGB_IND_PLANE_MOD (PRGB_IND_PLANE_CLASS + PRGB_IND_CLASS_BITS)
#define PRGB_IND_PLANE_CAPS (PRGB_IND_PLANE_MOD + 1)
#define PRGB_IND_PLANE_HEADER (PRGB_IND_PLANE_CAPS + 1)
#define PRGB_IND_PLANES (PRGB_IND_PLANE_HEADER + 1)

/* Header: bit 0 = key map shown, bits 8-15 = id of the layer shown (underglow tint). */
#define PRGB_IND_HEADER(shown, layer) (((shown) ? 1U : 0U) | ((uint32_t)(layer) << 8))
#define PRGB_IND_HEADER_SHOWN(h) (((h) & 1U) != 0)
#define PRGB_IND_HEADER_LAYER(h) (((h) >> 8) & 0xFF)

/* A relay message: &prgb with param1 = PRGB_RELAY_CMD | plane << 8, param2 = plane. */
#define PRGB_IND_RELAY_PARAM1(plane) (PRGB_RELAY_CMD | ((uint32_t)(plane) << 8))

/* Plane number of a relay message's param1, or -1 if it is not a valid one. */
static inline int prgb_ind_relay_plane(uint32_t param1) {
    uint32_t plane = param1 >> 8;

    if ((param1 & 0xFF) != PRGB_RELAY_CMD || plane >= PRGB_IND_PLANES) {
        return -1;
    }
    return (int)plane;
}

/*
 * Pack the info of one half's positions (the set bits of half, ascending) into planes;
 * bit n of each plane belongs to the n-th position of the half. At most 32 positions.
 */
static inline void prgb_ind_pack(const uint8_t *info, uint8_t n_pos, uint64_t half,
                                 uint32_t header, uint32_t planes[PRGB_IND_PLANES]) {
    unsigned bit = 0;

    memset(planes, 0, sizeof(uint32_t) * PRGB_IND_PLANES);
    for (uint8_t p = 0; p < n_pos && p < 64 && bit < 32; p++) {
        if (!(half & (1ULL << p))) {
            continue;
        }
        for (int c = 0; c < PRGB_IND_CLASS_BITS; c++) {
            if (info[p] & (1U << c)) {
                planes[PRGB_IND_PLANE_CLASS + c] |= 1UL << bit;
            }
        }
        if (info[p] & PRGB_IND_MOD) {
            planes[PRGB_IND_PLANE_MOD] |= 1UL << bit;
        }
        if (info[p] & PRGB_IND_CAPS) {
            planes[PRGB_IND_PLANE_CAPS] |= 1UL << bit;
        }
        bit++;
    }
    planes[PRGB_IND_PLANE_HEADER] = header;
}

/* The info of bit n of the planes (inverse of prgb_ind_pack). */
static inline uint8_t prgb_ind_unpack(const uint32_t planes[PRGB_IND_PLANES], unsigned bit) {
    uint8_t v = 0;

    for (int c = 0; c < PRGB_IND_CLASS_BITS; c++) {
        if (planes[PRGB_IND_PLANE_CLASS + c] & (1UL << bit)) {
            v |= 1U << c;
        }
    }
    if (planes[PRGB_IND_PLANE_MOD] & (1UL << bit)) {
        v |= PRGB_IND_MOD;
    }
    if (planes[PRGB_IND_PLANE_CAPS] & (1UL << bit)) {
        v |= PRGB_IND_CAPS;
    }
    return v;
}

/* ---- colours ------------------------------------------------------------------------- */

/* pct: % of the brightness (0 = off / no override); pulse: breathes (25-100% of pct). */
struct prgb_ind_color {
    uint16_t h;
    uint8_t s;
    uint8_t pct;
    bool pulse;
};

#define PRGB_IND_HUE_BLUE 220
#define PRGB_IND_HUE_CYAN 180
#define PRGB_IND_HUE_AMBER 35
#define PRGB_IND_HUE_RED 0
#define PRGB_IND_HUE_GREEN 120

/* PRGB_IND_LIGHT_HUE keys run round the colour wheel (see prgb.c), starting at h. */
static const struct prgb_ind_color prgb_ind_class_colors[PRGB_IND_CLASSES] = {
    [PRGB_IND_NONE] = {0, 0, 0, false},
    [PRGB_IND_KEY] = {0, 0, 60, false},                         /* white, a bit dimmer */
    [PRGB_IND_LAYER] = {20, 100, 100, false},                   /* orange */
    [PRGB_IND_CONN] = {PRGB_IND_HUE_BLUE, 100, 100, false},     /* blue (BT_CLR) */
    [PRGB_IND_SYSTEM] = {PRGB_IND_HUE_RED, 100, 100, false},    /* red */
    [PRGB_IND_LIGHT] = {330, 100, 100, false},                  /* pink: toggles */
    [PRGB_IND_LIGHT_EFFECT] = {140, 100, 100, false},           /* green */
    [PRGB_IND_LIGHT_SPEED] = {PRGB_IND_HUE_CYAN, 100, 100, false}, /* cyan */
    [PRGB_IND_LIGHT_BRT] = {55, 100, 100, false},               /* yellow */
    [PRGB_IND_LIGHT_HUE] = {0, 100, 100, false},                /* rainbow */
    [PRGB_IND_LIGHT_SAT] = {285, 100, 100, false},              /* purple */
};

static const struct prgb_ind_color prgb_ind_mod_color = {PRGB_IND_HUE_GREEN, 100, 100, false};
static const struct prgb_ind_color prgb_ind_caps_color = {PRGB_IND_HUE_CYAN, 100, 100, false};

/* Whether a layer's key map leaves its unbound (&none) keys dark, flashing a colour
 * that was just changed. Otherwise they keep the effect, as on the default layer, and
 * only the bound keys show their class colours. */
static inline bool prgb_ind_layer_map_unbound(uint8_t layer) {
    return layer != 2; /* Extra: unbound keys keep the effect */
}

/* Underglow tint while a layer's key map is shown. */
static inline struct prgb_ind_color prgb_ind_layer_tint(uint8_t layer) {
    switch (layer) {
    case 1: /* Control */
        return (struct prgb_ind_color){PRGB_IND_HUE_BLUE, 100, 100, false};
    case 2: /* Extra */
        return (struct prgb_ind_color){285, 100, 100, false}; /* purple */
    default:
        return (struct prgb_ind_color){(uint16_t)((layer * 67 + 40) % 360), 100, 100, false};
    }
}

/* Bluetooth / output state, from the central. */
struct prgb_ind_conn {
    int active;            /* active BLE profile */
    uint8_t profiles;      /* number of BLE profiles */
    uint8_t connected;     /* bit n: profile n connected */
    uint8_t open;          /* bit n: profile n unpaired (open) */
    uint8_t transport;     /* PRGB_IND_TRANSPORT_* of the endpoint in use */
};

#define PRGB_IND_TRANSPORT_NONE 0
#define PRGB_IND_TRANSPORT_USB 1
#define PRGB_IND_TRANSPORT_BLE 2

/* &bt / &out command numbers (<dt-bindings/zmk/bt.h>, <dt-bindings/zmk/outputs.h>) */
#define PRGB_IND_BT_SEL_CMD 3
#define PRGB_IND_OUT_TOG 0
#define PRGB_IND_OUT_USB 1
#define PRGB_IND_OUT_BLE 2
#define PRGB_IND_OUT_NONE 3

/*
 * Status colour of a &bt BT_SEL n or &out key (pct 0 for any other binding):
 *  BT_SEL n: active + connected = blue; active, not connected (advertising) = amber,
 *            pulsing; other profiles dim: connected = blue, paired = red, unpaired =
 *            white.
 *  &out:     OUT_TOG shows the endpoint in use: USB = cyan, BLE = blue, none = dim
 *            white; OUT_USB / OUT_BLE / OUT_NONE are bright when theirs is in use.
 */
static inline struct prgb_ind_color prgb_ind_conn_color(const struct prgb_ind_binding *b,
                                                        const struct prgb_ind_conn *c) {
    struct prgb_ind_color none = {0, 0, 0, false};

    if (prgb_ind_is(b->dev, "bluetooth")) {
        uint32_t n = b->param2;

        if (b->param1 != PRGB_IND_BT_SEL_CMD || n >= c->profiles || n >= 8) {
            return none;
        }
        bool active = (int)n == c->active;
        bool connected = c->connected & (1U << n);
        bool open = c->open & (1U << n);

        if (active) {
            return connected
                       ? (struct prgb_ind_color){PRGB_IND_HUE_BLUE, 100, 100, false}
                       : (struct prgb_ind_color){PRGB_IND_HUE_AMBER, 100, 100, true};
        }
        if (connected) {
            return (struct prgb_ind_color){PRGB_IND_HUE_BLUE, 100, 30, false};
        }
        return open ? (struct prgb_ind_color){0, 0, 20, false}
                    : (struct prgb_ind_color){PRGB_IND_HUE_RED, 100, 30, false};
    }

    if (prgb_ind_is(b->dev, "outputs")) {
        switch (b->param1) {
        case PRGB_IND_OUT_TOG:
            if (c->transport == PRGB_IND_TRANSPORT_USB) {
                return (struct prgb_ind_color){PRGB_IND_HUE_CYAN, 100, 100, false};
            }
            if (c->transport == PRGB_IND_TRANSPORT_BLE) {
                return (struct prgb_ind_color){PRGB_IND_HUE_BLUE, 100, 100, false};
            }
            return (struct prgb_ind_color){0, 0, 20, false};
        case PRGB_IND_OUT_USB:
            return (struct prgb_ind_color){
                PRGB_IND_HUE_CYAN, 100, c->transport == PRGB_IND_TRANSPORT_USB ? 100 : 25, false};
        case PRGB_IND_OUT_BLE:
            return (struct prgb_ind_color){
                PRGB_IND_HUE_BLUE, 100, c->transport == PRGB_IND_TRANSPORT_BLE ? 100 : 25, false};
        case PRGB_IND_OUT_NONE:
            return (struct prgb_ind_color){
                0, 0, c->transport == PRGB_IND_TRANSPORT_NONE ? 100 : 20, false};
        }
    }

    return none;
}
