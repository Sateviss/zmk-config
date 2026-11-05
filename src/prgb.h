/*
 * Per-key RGB lighting: API used by the &prgb behavior.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRGB_HUE_MAX 360
#define PRGB_SAT_MAX 100
#define PRGB_BRT_MAX 100
#define PRGB_SPD_MIN 1
#define PRGB_SPD_MAX 5
#define PRGB_BG_MAX 100

enum prgb_zone {
    PRGB_ZONE_UNDERGLOW,
    PRGB_ZONE_KEYS,
};

struct prgb_hs {
    uint16_t h; /* 0-359 */
    uint8_t s;  /* 0-100 */
};

bool prgb_is_on(void);
int prgb_set_on(bool on);

/* calc_* return the value a relative change would produce, without applying it. */
int prgb_calc_effect(int direction);
int prgb_set_effect(int effect);

int prgb_calc_brt(int direction);
int prgb_set_brt(int brt);

int prgb_calc_spd(int direction);
int prgb_set_spd(int spd);

struct prgb_hs prgb_calc_color(enum prgb_zone zone, int hue_dir, int sat_dir);
int prgb_set_color(enum prgb_zone zone, struct prgb_hs color);

/* Reactive/ripple background level, in % of the brightness (0 = keys dark at rest). */
int prgb_calc_bg_next(void);
int prgb_set_bg(int bg);

/* Layer key map (and the Bluetooth/output status shown in it) on/off. */
bool prgb_map_enabled(void);
int prgb_set_map(bool on);

/* Underglow pulse on key presses on/off. */
bool prgb_pulse_enabled(void);
int prgb_set_pulse(bool on);

/* Peripheral: one indicator plane relayed by the central (see prgb_ind.h). */
int prgb_relay_receive(uint32_t param1, uint32_t value);

/* Read-only snapshot of the lighting state, for the OLED status screen. */
struct prgb_status {
    bool on;
    uint8_t effect; /* PRGB_EFF_* */
    uint8_t brt;    /* 0-100, % of CONFIG_ZMK_PRGB_BRT_MAX */
    uint8_t spd;
};

struct prgb_status prgb_get_status(void);

/*
 * Display name of an effect (PRGB_EFF_*), taken from the effect list the &prgb behavior
 * gives ZMK Studio (behavior_prgb.c, needs CONFIG_ZMK_BEHAVIOR_METADATA). For an effect
 * without a name there, "FX <n>" is written to buf and buf is returned.
 */
const char *prgb_effect_name(uint8_t effect, char *buf, size_t len);
