#!/usr/bin/env bash
# Renders Tyrian's music to the WAV files the port streams (see
# ../README.md): builds tools/musrender (OpenTyrian's LDS player and OPL
# emulator, for the host) and runs it on music.mus. Optionally copies the
# files to the SD card in the board (TYRIAN/MUSIC/) with
# ../../core/tools/sdput/sdput.py (about 64 KB/s).
#
# Usage: make_music.sh DATADIR OUTDIR [--upload | --to CARDDIR]
set -euo pipefail

if [ $# -lt 2 ]; then
    sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
case "${3-}" in   # --upload: to the board's SD card; --to: into a card's file tree
"" | --upload) PUT=(put) ;;
--to) PUT=(put --to "${4:?--to needs a directory}") ;;
*) echo "unknown option $3" >&2; exit 1 ;;
esac
DATA=$1
OUT=$(realpath -m "$2")
HERE=$(dirname "$(realpath "$0")")
SRC=$HERE/../opentyrian/src
mkdir -p "$OUT"

cc -O2 -I"$HERE/../port" -I"$SRC" "$HERE/musrender.c" "$SRC/lds_play.c" "$SRC/opl.c" "$SRC/memreader.c" \
   -lm -o "$OUT/musrender"
"$OUT/musrender" "$DATA/music.mus" "$OUT"

if [ -n "${3-}" ]; then
    args=()
    for f in "$OUT"/SONG*.WAV; do args+=("$f" "TYRIAN/MUSIC/$(basename "$f")"); done
    python3 "$HERE/../../core/tools/sdput/sdput.py" "${PUT[@]}" "${args[@]}"
fi
