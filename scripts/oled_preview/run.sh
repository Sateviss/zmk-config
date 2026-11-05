#!/bin/sh
# Render a preview of the OLED status screen (src/display/oled_layout.c) with the LVGL
# that ZMK builds with, on the PC.
#
# usage: run.sh <lvgl source dir> [cw|ccw]
#   e.g. run.sh <west workspace>/modules/lib/gui/lvgl
# Writes docs/oled-preview.png (needs gcc and Python with Pillow).
set -e
LVGL=${1:?usage: run.sh <lvgl source dir> [cw|ccw]}
DIR=${2:-cw}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

mkdir -p "$OUT/obj"
find "$LVGL/src" -name '*.c' | while read -r f; do
    echo "$f"
done | xargs -P"$(nproc)" -I{} sh -c \
    'gcc -c -O1 -w -DLV_CONF_INCLUDE_SIMPLE -I"'"$HERE"'" -I"'"$LVGL"'" "{}" -o "'"$OUT"'/obj/$(echo "{}" | tr / _).o"'
gcc -O1 -Wall -DLV_CONF_INCLUDE_SIMPLE -I"$HERE" -I"$LVGL" -I"$REPO/src/display" \
    "$HERE/preview.c" "$REPO/src/display/oled_layout.c" "$OUT"/obj/*.o -lm -o "$OUT/preview"
"$OUT/preview" "$OUT" "$DIR"
python3 "$HERE/compose.py" "$OUT" "$REPO/docs/oled-preview.png" "$DIR"
