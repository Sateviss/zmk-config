---
name: zmk-build
description: Build the ZMK keyboard firmware locally in the cloud container (native west + Zephyr SDK, quiet output, logs to files). Use to narrow down a CI failure or iterate fast on one target; CI (zmk-ci skill) stays the default build and the source of flashable firmware.
---

# Local build (narrowing down / fast iteration)

Mirrors the CI: same pinned ZMK (`config/west.yml`), same `build.yaml` matrix,
snippets, cmake-args, and the repo root as `ZMK_EXTRA_MODULES` when
`zephyr/module.yml` exists.

## Commands (all print ~1 line per target)

    S=.claude/skills/zmk-build/scripts
    $S/build.sh            # every build.yaml target, incremental
    $S/build.sh left       # filter = substring of the artifact name (left/right/reset)
    $S/build.sh -p right   # pristine (after Kconfig/DT/CMake/module.yml changes that misbehave)

- First call in a container runs `setup.sh` automatically (~2.5 min, 4 GB: venv+west,
  Zephyr SDK arm toolchain in `~/zephyr-sdk-*`, protoc for ZMK Studio, workspace
  `~/zmk-ws`). Run that first call with `run_in_background: true`. It re-runs
  `west update` only when `config/west.yml` changed.
- Output: `OK <artifact> <secs> FLASH: x% RAM: y%` or `FAIL <artifact> ... log: <path>`
  followed by up to 25 matching error lines (`ERR_LINES=60` for more).
- Timing: left ~30 s clean / ~8 s incremental, right ~27 s, settings_reset ~12 s.
- Artifacts: `~/zmk-ws/out/<artifact>.uf2` (the user flashes from CI; only hand these
  over if they ask).

## Reading results without dumping files

    L=~/zmk-ws/logs/<artifact>.log; B=~/zmk-ws/build/<artifact>/zephyr
    grep -n -B3 -A3 'error' $L | head -60        # context around an error
    grep -E '^CONFIG_(ZMK_SLEEP|ZMK_STUDIO)=' $B/.config   # resolved Kconfig
    grep -n -A8 'left_encoder' $B/zephyr.dts      # final devicetree node
    grep -E 'warning:' $L | sort -u | head        # warnings (not shown by build.sh)
    # biggest RAM users (b/d = bss/data; use ' [tr] ' for flash):
    ~/zephyr-sdk-*/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm --size-sort -S -t d $B/zmk.elf | grep -E ' [bBdD] ' | tail -15

Never `Read`/`cat` whole logs, `.config` (~1500 lines) or `zephyr.dts`.

## Gotchas

- Docker image `zmkfirmware/zmk-build-arm` is rate-limited (429) from this container;
  that's why the build is native. If native setup itself breaks (SDK download,
  apt, pip) and one retry doesn't fix it, stop and ask the user to fix the environment.
- `ModuleNotFoundError: google` / `protoc` missing => Studio deps: rerun setup.sh.
- Host check scripts (`scripts/check_*.py`) take a ZMK dir:
  pass `~/zmk-ws/zmk` instead of the default `/tmp/zmk`.
- OLED preview (`scripts/oled_preview/run.sh`): LVGL is at `~/zmk-ws/modules/lib/gui/lvgl`.
