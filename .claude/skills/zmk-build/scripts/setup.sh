#!/usr/bin/env bash
# One-time (per container) native ZMK toolchain + west workspace setup. Idempotent.
# Quiet: full output goes to $LOG; prints one line per stage.
set -euo pipefail
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
WS=${ZMK_WS:-$HOME/zmk-ws}
SDK_VER=${ZMK_SDK_VER:-0.17.0}
SDK=$HOME/zephyr-sdk-$SDK_VER
LOG=${LOG:-$WS/setup.log}
mkdir -p "$WS/config"; : > "$LOG"
step() { printf '%-28s' "$1"; shift; if "$@" >>"$LOG" 2>&1; then echo ok; else echo "FAIL (see $LOG)"; tail -20 "$LOG"; exit 1; fi; }

[ -x "$WS/.venv/bin/west" ] || step "venv + west" sh -c "python3 -m venv $WS/.venv && $WS/.venv/bin/pip install -q -U pip setuptools wheel west"
export PATH="$WS/.venv/bin:$PATH"
if [ ! -x "$SDK/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" ]; then
  U=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v$SDK_VER
  step "sdk minimal" sh -c "curl -sSfL $U/zephyr-sdk-${SDK_VER}_linux-x86_64_minimal.tar.xz | tar -xJ -C $HOME"
  step "sdk arm toolchain" sh -c "curl -sSfL $U/toolchain_linux-x86_64_arm-zephyr-eabi.tar.xz | tar -xJ -C $SDK"
  step "sdk cmake pkg" "$SDK/setup.sh" -c
fi
command -v protoc >/dev/null || step "apt protoc (ZMK Studio)" sh -c "sudo -n apt-get -qq update && sudo -n apt-get -qq install -y protobuf-compiler"
# west.yml decides the ZMK/Zephyr revisions; re-run update only when it changes.
if ! cmp -s "$REPO/config/west.yml" "$WS/config/west.yml"; then
  cp "$REPO/config/west.yml" "$WS/config/west.yml"
  [ -d "$WS/.west" ] || step "west init" west init -l "$WS/config"
  step "west update" sh -c "cd $WS && west update --narrow -o=--depth=1"
  step "west zephyr-export" sh -c "cd $WS && west zephyr-export"
  step "pip zephyr reqs" "$WS/.venv/bin/pip" install -q -r "$WS/zephyr/scripts/requirements-base.txt" protobuf grpcio-tools
fi
echo "workspace ready: $WS"
