#!/usr/bin/env python3
"""Host-side check of the OLED battery runtime estimate (src/display/battery_estimate.c).

Compiles battery_estimate.c with the host C compiler and feeds it simulated Li-ion
discharges (a reading a minute, with ADC noise, idle gaps, charging). Checks that:

  - the voltage -> charge curve is monotonic and spans 0-1000 per mille,
  - there is no estimate until BATT_EST_MIN_SPAN_S of data is in,
  - a steady drain gives an estimate within 25 % of the true time left, also in the
    steep parts of the curve and with +-10 mV noise,
  - a flat battery (no drain) and a rising one (charging) give no estimate,
  - charging resets the history, and data older than the 2 h window is dropped.

Usage: check_battery_estimate.py [CC]   (default cc)
"""

import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "src/display"
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"

HARNESS = r"""
#include <stdio.h>
#include "battery_estimate.h"

/* Inverse of the charge curve, by search (mV for a per mille level). */
static int mv_for(int pm) {
    int mv = 3270;
    while (mv < 4200 && batt_est_charge_pm(mv) < pm) mv++;
    return mv;
}

static int noise(int i) { return (int)((i * 7919u) % 21u) - 10; }

/* Drain at pm_per_h from pm0, a reading a minute for minutes; print estimate vs truth. */
static void drain(const char *name, int pm0, int pm_per_h, int minutes) {
    static struct batt_est e;
    batt_est_reset(&e);
    for (int m = 0; m <= minutes; m++) {
        double pm = pm0 - pm_per_h * m / 60.0;
        batt_est_add(&e, 1000 + m * 60, mv_for((int)pm) + noise(m));
        int truth = pm_per_h ? (int)(pm * 60 / pm_per_h) : -1;
        printf("D %s %d %d %d\n", name, m, batt_est_minutes(&e, 1000 + m * 60), truth);
    }
}

int main(void) {
    for (int mv = 3200; mv <= 4250; mv++) printf("C %d %d\n", mv, batt_est_charge_pm(mv));

    drain("full-10h", 1000, 100, 150);
    drain("mid-5h", 650, 200, 90);
    drain("low-2h", 300, 150, 90);
    drain("flat", 600, 0, 150);

    static struct batt_est e;
    batt_est_reset(&e);
    /* charging: rising voltage, then unplugged */
    for (int m = 0; m < 60; m++) batt_est_add(&e, m * 60, 3700 + m * 5);
    printf("R rising %d\n", batt_est_minutes(&e, 59 * 60));
    batt_est_reset(&e);
    for (int m = 0; m < 60; m++) batt_est_add(&e, m * 60, 3950 - m);
    int before = batt_est_minutes(&e, 59 * 60);
    batt_est_add(&e, 60 * 60, 4150); /* charged */
    printf("R charged %d %d %d\n", before, batt_est_minutes(&e, 60 * 60), e.used);
    /* everything older than the window */
    batt_est_reset(&e);
    for (int m = 0; m < 60; m++) batt_est_add(&e, m * 60, 3950 - m);
    batt_est_add(&e, 60 * 60 + 3 * 3600, 3880);
    printf("R stale %d\n", batt_est_minutes(&e, 60 * 60 + 3 * 3600));
    return 0;
}
"""

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)


with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / "harness.c"
    exe = Path(tmp) / "harness"
    c.write_text(HARNESS)
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{SRC}", str(c),
                    str(SRC / "battery_estimate.c"), "-o", str(exe)], check=True)
    lines = subprocess.run([str(exe)], check=True, capture_output=True,
                           text=True).stdout.splitlines()

curve = [tuple(map(int, l.split()[1:])) for l in lines if l.startswith("C ")]
check(curve[0][1] == 0 and curve[-1][1] == 1000, "curve does not span 0-1000")
check(all(b[1] >= a[1] for a, b in zip(curve, curve[1:])), "curve not monotonic")

runs = {}
for l in lines:
    if l.startswith("D "):
        _, name, m, est, truth = l.split()
        runs.setdefault(name, []).append((int(m), int(est), int(truth)))

for name, rows in runs.items():
    worst = 0.0
    for m, est, truth in rows:
        if m < 30:
            check(est == -1, f"{name}: estimate at {m} min, before 30 min of data")
        elif name == "flat":
            check(est == -1 or est > 100 * 60, f"{name}: {est} min with no drain at {m} min")
        elif est != -1 or m >= 40:
            check(est > 0, f"{name}: no estimate at {m} min")
            err = abs(est - truth) / truth
            worst = max(worst, err)
            check(err <= 0.25, f"{name}: {est} min vs {truth} min at {m} min")
    print(f"{name:9} worst error {worst * 100:4.1f} %  (last: {rows[-1][1]} min est, "
          f"{rows[-1][2]} min true)")

res = {l.split()[1]: list(map(int, l.split()[2:])) for l in lines if l.startswith("R ")}
check(res["rising"][0] == -1, "estimate while charging")
check(res["charged"][0] > 0 and res["charged"][1] == -1 and res["charged"][2] == 1,
      f"charging did not reset the history: {res['charged']}")
check(res["stale"][0] == -1, "data older than the window still used")

for f in failures[:20]:
    print("FAIL", f)
print("FAIL" if failures else "OK", f"({len(failures)} failures)")
sys.exit(1 if failures else 0)
