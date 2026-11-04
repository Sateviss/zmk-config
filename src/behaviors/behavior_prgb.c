/*
 * &prgb: controls the per-key RGB lighting (see <dt-bindings/prgb.h>).
 *
 * Global locality: the central runs it locally and sends it to the peripheral, so
 * both halves change together. Relative commands (toggle, next effect, hue up, ...)
 * are first turned into absolute ones from the central's state, so the halves end
 * up with the same values even if they had drifted apart.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_prgb

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/keymap.h>

#include <dt-bindings/prgb.h>

#include "../prgb.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

#define CMD(name, cmd)                                                                             \
    {.display_name = name, .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = cmd}

static const struct behavior_parameter_value_metadata no_arg_values[] = {
    CMD("Toggle On/Off", PRGB_TOG_CMD),
    CMD("Turn On", PRGB_ON_CMD),
    CMD("Turn Off", PRGB_OFF_CMD),
    CMD("Next Effect", PRGB_EFF_NEXT_CMD),
    CMD("Previous Effect", PRGB_EFF_PREV_CMD),
    CMD("Brightness Up", PRGB_BRI_UP_CMD),
    CMD("Brightness Down", PRGB_BRI_DN_CMD),
    CMD("Speed Up", PRGB_SPD_UP_CMD),
    CMD("Speed Down", PRGB_SPD_DN_CMD),
    CMD("Underglow Hue Up", PRGB_UG_HUE_UP_CMD),
    CMD("Underglow Hue Down", PRGB_UG_HUE_DN_CMD),
    CMD("Underglow Saturation Up", PRGB_UG_SAT_UP_CMD),
    CMD("Underglow Saturation Down", PRGB_UG_SAT_DN_CMD),
    CMD("Key Hue Up", PRGB_KEY_HUE_UP_CMD),
    CMD("Key Hue Down", PRGB_KEY_HUE_DN_CMD),
    CMD("Key Saturation Up", PRGB_KEY_SAT_UP_CMD),
    CMD("Key Saturation Down", PRGB_KEY_SAT_DN_CMD),
};

static const struct behavior_parameter_value_metadata effect_cmd_values[] = {
    CMD("Set Effect", PRGB_EFF_SET_CMD),
};

static const struct behavior_parameter_value_metadata effect_values[] = {
    CMD("Static", PRGB_EFF_STATIC),   CMD("Breathe", PRGB_EFF_BREATHE),
    CMD("Reactive", PRGB_EFF_REACTIVE), CMD("Ripple", PRGB_EFF_RIPPLE),
    CMD("Rainbow", PRGB_EFF_RAINBOW),
};

static const struct behavior_parameter_metadata_set sets[] = {
    {
        .param1_values = no_arg_values,
        .param1_values_len = ARRAY_SIZE(no_arg_values),
    },
    {
        .param1_values = effect_cmd_values,
        .param1_values_len = ARRAY_SIZE(effect_cmd_values),
        .param2_values = effect_values,
        .param2_values_len = ARRAY_SIZE(effect_values),
    },
};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(sets),
    .sets = sets,
};

#endif /* IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA) */

#if IS_ENABLED(CONFIG_ZMK_PRGB)

static void to_color(struct zmk_behavior_binding *binding, enum prgb_zone zone, int hue_dir,
                     int sat_dir) {
    struct prgb_hs c = prgb_calc_color(zone, hue_dir, sat_dir);

    binding->param1 = zone == PRGB_ZONE_UNDERGLOW ? PRGB_UG_COLOR_CMD : PRGB_KEY_COLOR_CMD;
    binding->param2 = PRGB_HS(c.h, c.s);
}

static int prgb_convert_params(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case PRGB_TOG_CMD:
        binding->param1 = prgb_is_on() ? PRGB_OFF_CMD : PRGB_ON_CMD;
        break;
    case PRGB_EFF_NEXT_CMD:
    case PRGB_EFF_PREV_CMD:
        binding->param2 = prgb_calc_effect(binding->param1 == PRGB_EFF_NEXT_CMD ? 1 : -1);
        binding->param1 = PRGB_EFF_SET_CMD;
        break;
    case PRGB_BRI_UP_CMD:
    case PRGB_BRI_DN_CMD:
        binding->param2 = prgb_calc_brt(binding->param1 == PRGB_BRI_UP_CMD ? 1 : -1);
        binding->param1 = PRGB_BRT_SET_CMD;
        break;
    case PRGB_SPD_UP_CMD:
    case PRGB_SPD_DN_CMD:
        binding->param2 = prgb_calc_spd(binding->param1 == PRGB_SPD_UP_CMD ? 1 : -1);
        binding->param1 = PRGB_SPD_SET_CMD;
        break;
    case PRGB_UG_HUE_UP_CMD:
        to_color(binding, PRGB_ZONE_UNDERGLOW, 1, 0);
        break;
    case PRGB_UG_HUE_DN_CMD:
        to_color(binding, PRGB_ZONE_UNDERGLOW, -1, 0);
        break;
    case PRGB_UG_SAT_UP_CMD:
        to_color(binding, PRGB_ZONE_UNDERGLOW, 0, 1);
        break;
    case PRGB_UG_SAT_DN_CMD:
        to_color(binding, PRGB_ZONE_UNDERGLOW, 0, -1);
        break;
    case PRGB_KEY_HUE_UP_CMD:
        to_color(binding, PRGB_ZONE_KEYS, 1, 0);
        break;
    case PRGB_KEY_HUE_DN_CMD:
        to_color(binding, PRGB_ZONE_KEYS, -1, 0);
        break;
    case PRGB_KEY_SAT_UP_CMD:
        to_color(binding, PRGB_ZONE_KEYS, 0, 1);
        break;
    case PRGB_KEY_SAT_DN_CMD:
        to_color(binding, PRGB_ZONE_KEYS, 0, -1);
        break;
    default:
        return 0;
    }

    LOG_DBG("prgb: relative -> absolute (%d/%d)", binding->param1, binding->param2);
    return 0;
}

static int set_color(enum prgb_zone zone, uint32_t hs) {
    return prgb_set_color(zone, (struct prgb_hs){.h = (hs >> 8) & 0xFFFF, .s = hs & 0xFF});
}

static int prgb_binding_pressed(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case PRGB_TOG_CMD:
        return prgb_set_on(!prgb_is_on());
    case PRGB_ON_CMD:
        return prgb_set_on(true);
    case PRGB_OFF_CMD:
        return prgb_set_on(false);
    case PRGB_EFF_NEXT_CMD:
        return prgb_set_effect(prgb_calc_effect(1));
    case PRGB_EFF_PREV_CMD:
        return prgb_set_effect(prgb_calc_effect(-1));
    case PRGB_BRI_UP_CMD:
        return prgb_set_brt(prgb_calc_brt(1));
    case PRGB_BRI_DN_CMD:
        return prgb_set_brt(prgb_calc_brt(-1));
    case PRGB_SPD_UP_CMD:
        return prgb_set_spd(prgb_calc_spd(1));
    case PRGB_SPD_DN_CMD:
        return prgb_set_spd(prgb_calc_spd(-1));
    case PRGB_UG_HUE_UP_CMD:
        return prgb_set_color(PRGB_ZONE_UNDERGLOW, prgb_calc_color(PRGB_ZONE_UNDERGLOW, 1, 0));
    case PRGB_UG_HUE_DN_CMD:
        return prgb_set_color(PRGB_ZONE_UNDERGLOW, prgb_calc_color(PRGB_ZONE_UNDERGLOW, -1, 0));
    case PRGB_UG_SAT_UP_CMD:
        return prgb_set_color(PRGB_ZONE_UNDERGLOW, prgb_calc_color(PRGB_ZONE_UNDERGLOW, 0, 1));
    case PRGB_UG_SAT_DN_CMD:
        return prgb_set_color(PRGB_ZONE_UNDERGLOW, prgb_calc_color(PRGB_ZONE_UNDERGLOW, 0, -1));
    case PRGB_KEY_HUE_UP_CMD:
        return prgb_set_color(PRGB_ZONE_KEYS, prgb_calc_color(PRGB_ZONE_KEYS, 1, 0));
    case PRGB_KEY_HUE_DN_CMD:
        return prgb_set_color(PRGB_ZONE_KEYS, prgb_calc_color(PRGB_ZONE_KEYS, -1, 0));
    case PRGB_KEY_SAT_UP_CMD:
        return prgb_set_color(PRGB_ZONE_KEYS, prgb_calc_color(PRGB_ZONE_KEYS, 0, 1));
    case PRGB_KEY_SAT_DN_CMD:
        return prgb_set_color(PRGB_ZONE_KEYS, prgb_calc_color(PRGB_ZONE_KEYS, 0, -1));
    case PRGB_EFF_SET_CMD:
        return prgb_set_effect(binding->param2);
    case PRGB_BRT_SET_CMD:
        return prgb_set_brt(binding->param2);
    case PRGB_SPD_SET_CMD:
        return prgb_set_spd(binding->param2);
    case PRGB_UG_COLOR_CMD:
        return set_color(PRGB_ZONE_UNDERGLOW, binding->param2);
    case PRGB_KEY_COLOR_CMD:
        return set_color(PRGB_ZONE_KEYS, binding->param2);
    }

    return -ENOTSUP;
}

#else /* !IS_ENABLED(CONFIG_ZMK_PRGB) */

static int prgb_convert_params(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    return 0;
}

static int prgb_binding_pressed(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#endif /* IS_ENABLED(CONFIG_ZMK_PRGB) */

static int prgb_binding_released(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_prgb_driver_api = {
    .binding_convert_central_state_dependent_params = prgb_convert_params,
    .binding_pressed = prgb_binding_pressed,
    .binding_released = prgb_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_prgb_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
