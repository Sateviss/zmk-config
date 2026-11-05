/*
 * Custom OLED status screen (CONFIG_ZMK_OLED_STATUS): ZMK side.
 *
 * Collects the state from ZMK (connection, battery voltage, layer, per-key RGB) and
 * hands it to oled_layout.c, which draws it in portrait on the panel turned by 90
 * degrees. Follows ZMK's widget pattern: event listeners copy the state in the context
 * that raised the event and queue a work item on the display queue, which is the only
 * place LVGL is touched. Only labels whose text changed get redrawn.
 *
 * Battery voltage is read from the zmk,battery sensor (SENSOR_CHAN_GAUGE_VOLTAGE) on
 * ZMK's low priority work queue, the queue ZMK's own battery reporting uses, so the two
 * never read the ADC at the same time and input handling is never blocked by it.
 *
 * Rotation: LVGL is told the display is rotated, so it lays the screen out at 32x128;
 * the flush callback below turns each rendered area back into the panel's 128x32
 * orientation before handing it to Zephyr's SSD1306 flush. (The SSD1306 itself can
 * only mirror/flip, and Zephyr's LVGL glue does not rotate.)
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>
#include <display/lv_display_private.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/workqueue.h>

#include "oled_layout.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define IS_CENTRAL (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

/* ---- connection ---------------------------------------------------------------------- */

#if IS_CENTRAL

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>

static struct oled_conn conn_get_state(const zmk_event_t *eh) {
    struct zmk_endpoint_instance ep = zmk_endpoint_get_selected();
    enum zmk_transport transport = ep.transport;
    struct oled_conn c = {.connected = transport != ZMK_TRANSPORT_NONE};

    /* not connected: show what we are trying to connect to */
    if (!c.connected) {
        transport = zmk_endpoint_get_preferred_transport();
    }

    switch (transport) {
    case ZMK_TRANSPORT_USB:
        c.kind = OLED_CONN_USB;
        break;
#if IS_ENABLED(CONFIG_ZMK_BLE)
    case ZMK_TRANSPORT_BLE:
        c.kind = OLED_CONN_BLE;
        c.profile = zmk_ble_active_profile_index();
        c.bonded = !zmk_ble_active_profile_is_open();
        c.connected = zmk_ble_active_profile_is_connected();
        break;
#endif
    default:
        c.kind = OLED_CONN_NONE;
        break;
    }
    return c;
}

#else /* peripheral */

#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>

static struct oled_conn conn_get_state(const zmk_event_t *eh) {
    return (struct oled_conn){.kind = OLED_CONN_SPLIT,
                              .connected = zmk_split_bt_peripheral_is_connected()};
}

#endif

static void conn_update(struct oled_conn c) { oled_layout_set_conn(&c); }

ZMK_DISPLAY_WIDGET_LISTENER(oled_conn, struct oled_conn, conn_update, conn_get_state)
#if IS_CENTRAL
ZMK_SUBSCRIPTION(oled_conn, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(oled_conn, zmk_ble_active_profile_changed);
#endif
#else
ZMK_SUBSCRIPTION(oled_conn, zmk_split_peripheral_status_changed);
#endif

/* ---- layer (central only) ------------------------------------------------------------ */

#if IS_CENTRAL

#include <zmk/keymap.h>
#include <zmk/events/layer_state_changed.h>

struct layer_state {
    uint8_t index;
    const char *name;
};

static struct layer_state layer_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();

    return (struct layer_state){
        .index = index, .name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index))};
}

static void layer_update(struct layer_state s) { oled_layout_set_layer(s.name, s.index); }

ZMK_DISPLAY_WIDGET_LISTENER(oled_layer, struct layer_state, layer_update, layer_get_state)
ZMK_SUBSCRIPTION(oled_layer, zmk_layer_state_changed);

#endif

/* ---- lighting ------------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_ZMK_PRGB)

#include "../prgb.h"
#include "../events/prgb_state_changed.h"

struct lighting_state {
    struct prgb_status status;
    char name[16];
};

static struct lighting_state lighting_get_state(const zmk_event_t *eh) {
    struct lighting_state l = {.status = prgb_get_status()};
    const char *name = prgb_effect_name(l.status.effect, l.name, sizeof(l.name));

    if (name != l.name) {
        strncpy(l.name, name, sizeof(l.name) - 1);
    }
    return l;
}

static void lighting_update(struct lighting_state l) {
    struct oled_lighting shown = {
        .present = true,
        .on = l.status.on,
        .brt = l.status.brt,
        .effect = l.status.effect,
        .name = l.name,
    };

    oled_layout_set_lighting(&shown);
}

ZMK_DISPLAY_WIDGET_LISTENER(oled_lighting, struct lighting_state, lighting_update,
                            lighting_get_state)
ZMK_SUBSCRIPTION(oled_lighting, zmk_prgb_state_changed);

#endif

/* ---- battery ------------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

#if DT_HAS_CHOSEN(zmk_battery)
static const struct device *const battery = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
#else
static const struct device *const battery = NULL;
#endif

static atomic_t battery_mv = ATOMIC_INIT(-1);

static bool usb_powered(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

static void battery_show(struct k_work *work) {
    oled_layout_set_battery(atomic_get(&battery_mv), usb_powered());
}

static K_WORK_DEFINE(battery_show_work, battery_show);

/* Runs on the low priority work queue. */
static void battery_read(struct k_work *work) {
    struct sensor_value v;

    if (battery == NULL || !device_is_ready(battery)) {
        return;
    }
    if (sensor_sample_fetch_chan(battery, SENSOR_CHAN_GAUGE_VOLTAGE) != 0 ||
        sensor_channel_get(battery, SENSOR_CHAN_GAUGE_VOLTAGE, &v) != 0) {
        LOG_DBG("Battery voltage read failed");
        return;
    }

    atomic_set(&battery_mv, v.val1 * 1000 + v.val2 / 1000);
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &battery_show_work);
    }
}

static K_WORK_DELAYABLE_DEFINE(battery_read_work, battery_read);

static void battery_read_after(k_timeout_t delay) {
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &battery_read_work, delay);
}

static void battery_timer_expired(struct k_timer *timer) { battery_read_after(K_NO_WAIT); }

static K_TIMER_DEFINE(battery_timer, battery_timer_expired, NULL);

static void battery_timer_start(void) {
    k_timer_start(&battery_timer, K_NO_WAIT, K_SECONDS(CONFIG_ZMK_OLED_STATUS_BATTERY_INTERVAL));
}

static int battery_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *act = as_zmk_activity_state_changed(eh);

    if (act != NULL) {
        /* the screen is blanked while idle: no need to keep reading */
        if (act->state == ZMK_ACTIVITY_ACTIVE) {
            battery_timer_start();
        } else {
            k_timer_stop(&battery_timer);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_USB)
    if (as_zmk_usb_conn_state_changed(eh) != NULL) {
        /* show the charge bolt now, read the voltage once it has settled */
        if (zmk_display_is_initialized()) {
            k_work_submit_to_queue(zmk_display_work_q(), &battery_show_work);
        }
        battery_read_after(K_SECONDS(2));
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    /* ZMK read the battery and the level changed: show the new voltage too */
    if (as_zmk_battery_state_changed(eh) != NULL) {
        battery_read_after(K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(oled_battery, battery_listener);
ZMK_SUBSCRIPTION(oled_battery, zmk_activity_state_changed);
ZMK_SUBSCRIPTION(oled_battery, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(oled_battery, zmk_usb_conn_state_changed);
#endif

/* ---- rotation ------------------------------------------------------------------------ */

#define PANEL_NODE DT_CHOSEN(zephyr_display)
#define PANEL_W ROUND_UP(DT_PROP(PANEL_NODE, width), 8)
#define PANEL_H ROUND_UP(DT_PROP(PANEL_NODE, height), 8)

BUILD_ASSERT(DT_PROP(PANEL_NODE, width) == OLED_LAYOUT_H &&
                 DT_PROP(PANEL_NODE, height) == OLED_LAYOUT_W,
             "The status screen layout is made for a 128x32 panel");

/* I1 palette (8 bytes, which Zephyr's mono flush skips) + a whole panel of pixels: at
 * least as big as Zephyr's conversion buffer, which its flush copies back in full. */
static uint8_t rotated[8 + PANEL_W * PANEL_H / 8] __aligned(4);
static lv_display_flush_cb_t panel_flush;

static void rotated_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    lv_area_t panel_area = *area;

    /* Zephyr's mono rounder aligns both axes of the area to 8 pixels (SSD1306 pages are
     * 8 rows), so the rotated area is page aligned too. */
    lv_display_rotate_area(disp, &panel_area);
    memcpy(rotated, px_map, 8);
    oled_rotate_i1(px_map + 8, rotated + 8, lv_area_get_width(area), lv_area_get_height(area),
                   IS_ENABLED(CONFIG_ZMK_OLED_STATUS_ROTATE_CW));
    panel_flush(disp, &panel_area, rotated);
}

static void rotate_display(lv_display_t *disp) {
    panel_flush = disp->flush_cb;
    if (panel_flush == NULL) {
        LOG_ERR("Display has no flush callback, not rotating");
        return;
    }
    /* LVGL's rotation constants are counter-clockwise */
    lv_display_set_rotation(disp, IS_ENABLED(CONFIG_ZMK_OLED_STATUS_ROTATE_CW)
                                      ? LV_DISPLAY_ROTATION_270
                                      : LV_DISPLAY_ROTATION_90);
    lv_display_set_flush_cb(disp, rotated_flush);
}

/* ---- screen -------------------------------------------------------------------------- */

/* Called by ZMK on the display queue, once, after the display is initialised. */
lv_obj_t *zmk_display_status_screen(void) {
    lv_display_t *disp = lv_display_get_default();

    rotate_display(disp);

    lv_obj_t *screen = lv_obj_create(NULL);
    oled_layout_create(screen, IS_CENTRAL);

    oled_conn_init();
#if IS_CENTRAL
    oled_layer_init();
#endif
#if IS_ENABLED(CONFIG_ZMK_PRGB)
    oled_lighting_init();
#endif

    battery_show(NULL);
    battery_timer_start();

    return screen;
}
