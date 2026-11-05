/*
 * Battery runtime estimate for the OLED status screen: plain C (no Zephyr), so it can
 * be tested on the host (scripts/check_battery_estimate.py).
 *
 * Voltage readings are averaged into BATT_EST_BUCKET_S buckets covering the last
 * BATT_EST_WINDOW_S, and each average is turned into a Li-ion state of charge. The
 * drain rate is the least-squares slope through those buckets, and the estimate is
 * the charge left divided by that rate.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BATT_EST_WINDOW_S (2 * 60 * 60) /* usage this far back counts */
#define BATT_EST_BUCKET_S (5 * 60)
#define BATT_EST_BUCKETS (BATT_EST_WINDOW_S / BATT_EST_BUCKET_S)
#define BATT_EST_MIN_SPAN_S (30 * 60) /* no estimate from less data than this */
#define BATT_EST_CHARGED_MV 100       /* a rise this big means it was charged: start over */

struct batt_est_bucket {
    uint32_t t0;    /* time of the first reading in it, s */
    int32_t t_sum;  /* reading times, s after t0 */
    int32_t mv_sum; /* readings, mV */
    uint16_t n;
};

struct batt_est {
    struct batt_est_bucket b[BATT_EST_BUCKETS];
    uint8_t head; /* newest bucket */
    uint8_t used;
};

/* Li-ion state of charge in per mille for a voltage in mV (resting, rough). */
int batt_est_charge_pm(int mv);

void batt_est_reset(struct batt_est *e);

/* Add a voltage reading taken at now_s (a monotonic clock in seconds). */
void batt_est_add(struct batt_est *e, uint32_t now_s, int mv);

/* Minutes of battery left at the drain of the window, or -1 while there is not
 * enough data or no measurable drain. */
int batt_est_minutes(const struct batt_est *e, uint32_t now_s);
