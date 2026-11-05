#!/usr/bin/env bash
# Quiet local build of the build.yaml matrix, mirroring ZMK's build-user-config CI.
# usage: build.sh [-p] [filter...]   -p = pristine; filter = substring of artifact name
#   build.sh            # all targets, incremental
#   build.sh left       # only splitkb_aurora_lily58_left
# Prints one line per target. Full logs: $WS/logs/<artifact>.log; on failure prints
# only the error lines. Firmware: $WS/out/<artifact>.uf2
set -uo pipefail
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
WS=${ZMK_WS:-$HOME/zmk-ws}
[ -x "$WS/.venv/bin/west" ] && cmp -s "$REPO/config/west.yml" "$WS/config/west.yml" \
  || "$(dirname "$0")/setup.sh" >&2 || exit 1
export PATH="$WS/.venv/bin:$PATH" ZEPHYR_SDK_INSTALL_DIR=$(ls -d "$HOME"/zephyr-sdk-* | tail -1)
PRISTINE=auto; [ "${1:-}" = -p ] && { PRISTINE=always; shift; }
mkdir -p "$WS/logs" "$WS/out"
EXTRA_MOD=; [ -f "$REPO/zephyr/module.yml" ] && EXTRA_MOD="-DZMK_EXTRA_MODULES=$REPO"
rc=0
# board|shield|snippet|cmake-args|artifact, one per line, from build.yaml
while IFS='|' read -r board shield snippet cargs name; do
  if [ $# -gt 0 ]; then m=0; for f in "$@"; do [[ $name == *$f* ]] && m=1; done; [ $m = 1 ] || continue; fi
  log=$WS/logs/$name.log; t0=$SECONDS; p=$PRISTINE
  # CMake caches module paths/args: go pristine when branch, module or target args change
  sig="$(git -C "$REPO" branch --show-current) $EXTRA_MOD $board $shield $snippet $cargs"
  [ "$(cat "$WS/build/$name/.zmk-sig" 2>/dev/null)" = "$sig" ] || p=always
  (cd "$WS" && eval west build -p "$p" -s zmk/app -d "build/$name" -b "$board" \
     ${snippet:+-S "$snippet"} -- -DZMK_CONFIG="$REPO/config" ${shield:+-DSHIELD="$shield"} $EXTRA_MOD $cargs) >"$log" 2>&1
  if [ $? -eq 0 ]; then
    echo "$sig" > "$WS/build/$name/.zmk-sig"
    cp "$WS/build/$name/zephyr/zmk.uf2" "$WS/out/$name.uf2"
    mem=$(grep -E '^ +(FLASH|RAM):' "$log" | awk '{printf "%s %s ", $1, $NF}')
    printf 'OK   %-45s %3ss  %s\n' "$name" $((SECONDS-t0)) "$mem"
  else
    rc=1; printf 'FAIL %-45s %3ss  log: %s\n' "$name" $((SECONDS-t0)) "$log"
    grep -nE 'error:|Error:|error [0-9]|undefined reference|warning: .*(undefined|implicit)|^CMake Error|devicetree error|FATAL' "$log" \
      | grep -v 'ninja: build stopped' | head -${ERR_LINES:-25}
  fi
done < <("$WS/.venv/bin/python" - "$REPO/build.yaml" <<'PY'
import sys, yaml
d = yaml.safe_load(open(sys.argv[1])) or {}
rows = list(d.get("include", []))
for b in d.get("board", []):
    for s in d.get("shield", [None]) or [None]:
        rows.append({"board": b, "shield": s})
for r in rows:
    b, s = r["board"], r.get("shield") or ""
    name = r.get("artifact-name") or (f"{s}-" if s else "") + b.replace("/", "_") + "-zmk"
    print("|".join([b, s, r.get("snippet") or "", r.get("cmake-args") or "", name]))
PY
)
exit $rc
