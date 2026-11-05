/*
 * Battery runtime estimate (see battery_estimate.h).
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "battery_estimate.h"

/* Typical single cell Li-ion discharge curve (resting voltage, mV -> per mille). */
static const struct {
    int16_t mv;
    int16_t pm;
} curve[] = {
    {4200, 1000}, {4150, 950}, {4110, 900}, {4080, 850}, {4020, 800}, {3980, 750},
    {3950, 700},  {3910, 650}, {3870, 600}, {3850, 550}, {3840, 500}, {3820, 450},
    {3800, 400},  {3790, 350}, {3770, 300}, {3750, 250}, {3730, 200}, {3710, 150},
    {3690, 100},  {3610, 50},  {3270, 0},
};

#define CURVE_LEN (sizeof(curve) / sizeof(curve[0]))

int batt_est_charge_pm(int mv) {
    if (mv >= curve[0].mv) {
        return 1000;
    }
    for (unsigned i = 1; i < CURVE_LEN; i++) {
        if (mv >= curve[i].mv) {
            int dmv = curve[i - 1].mv - curve[i].mv;
            int dpm = curve[i - 1].pm - curve[i].pm;

            return curve[i].pm + (mv - curve[i].mv) * dpm / dmv;
        }
    }
    return 0;
}

void batt_est_reset(struct batt_est *e) { memset(e, 0, sizeof(*e)); }

static int bucket_mv(const struct batt_est_bucket *b) { return b->mv_sum / b->n; }

void batt_est_add(struct batt_est *e, uint32_t now_s, int mv) {
    struct batt_est_bucket *b = &e->b[e->head];

    if (e->used && mv - bucket_mv(b) >= BATT_EST_CHARGED_MV) {
        batt_est_reset(e); /* charged since: the old drain says nothing now */
    }

    if (!e->used || now_s - b->t0 >= BATT_EST_BUCKET_S) {
        if (e->used) {
            e->head = (e->head + 1) % BATT_EST_BUCKETS;
        }
        if (e->used < BATT_EST_BUCKETS) {
            e->used++;
        }
        b = &e->b[e->head];
        memset(b, 0, sizeof(*b));
        b->t0 = now_s;
    }
    b->t_sum += (int32_t)(now_s - b->t0);
    b->mv_sum += mv;
    b->n++;
}

int batt_est_minutes(const struct batt_est *e, uint32_t now_s) {
    /* least squares over the buckets in the window: x = mean time (s before now),
     * y = mean charge (per mille) */
    int64_t n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    int32_t oldest = 0;

    for (int i = 0; i < e->used; i++) {
        const struct batt_est_bucket *b =
            &e->b[(e->head + BATT_EST_BUCKETS - i) % BATT_EST_BUCKETS];
        int32_t x = (int32_t)(now_s - b->t0) - b->t_sum / b->n;
        int64_t y = batt_est_charge_pm(bucket_mv(b));

        if (x > BATT_EST_WINDOW_S) {
            break;
        }
        oldest = x;
        n++;
        sx += x;
        sy += y;
        sxx += (int64_t)x * x;
        sxy += x * y;
    }

    if (n < 3 || oldest < BATT_EST_MIN_SPAN_S) {
        return -1;
    }

    /* slope of charge against "seconds ago": positive when draining, in per mille/s */
    int64_t num = n * sxy - sx * sy;
    int64_t den = n * sxx - sx * sx;

    if (num <= 0 || den <= 0) {
        return -1;
    }

    /* charge now from the fit (less noisy than the newest bucket), clamped */
    int64_t fit_now = (sy * den - num * sx) / (n * den); /* intercept at x = 0 */
    int64_t pm = fit_now < 0 ? 0 : (fit_now > 1000 ? 1000 : fit_now);

    int64_t minutes = pm * den / (num * 60);

    return minutes > 1000000 ? 1000000 : (int)minutes;
}
