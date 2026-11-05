---
name: host-checks
description: Run this repo's host-side check scripts (scripts/check_*.py) and/or the OLED preview renderer on the current branch and report pass/fail concisely.
tools: Bash, Grep, Read
model: haiku
---

Work in /home/user/zmk-config on the branch you were given (do not switch branches
unless told). If `scripts/` is missing on that branch, say so and stop.

- ZMK dir argument: use ~/zmk-ws/zmk if it exists, else make a sparse clone
  `git clone -q --depth 1 --filter=blob:none --sparse https://github.com/zmkfirmware/zmk /tmp/zmk && git -C /tmp/zmk sparse-checkout set app`.
- check_led_map.py also needs QMK: sparse clone qmk/qmk_firmware into /tmp/qmk with
  `sparse-checkout set keyboards/splitkb/aurora/lily58` (plus anything its docstring names).
- Read each script's docstring (`head -30`) for its arguments before running it.
- Run each script with output to a file: `python3 scripts/X.py ... > /tmp/claude-checks/X.log 2>&1; echo $?`.
- OLED preview: `scripts/oled_preview/run.sh ~/zmk-ws/modules/lib/gui/lvgl` (needs a
  local build workspace and Pillow); it writes docs/oled-preview.png. Don't commit it.

Return at most 15 lines: per script PASS/FAIL (exit code), and for failures only the
assertion/mismatch lines (grep them from the log). Mention log paths. Do not edit code.
