#!/usr/bin/env bash
# Loads a Tyrian build (default build-prof/) and starts a game: 1 Player
# Arcade, episode 1, Normal, then Play Next Level. For tests.
set -euo pipefail
cd "$(dirname "$0")/.."
B=${1:-build-prof}
python3 ../core/tools/run.py "$B/tyrian.bin" --listen 14 > /dev/null 2>&1
python3 ../core/tools/m9bctl.py keys "<enter><wait:1500><enter><wait:2500><down><wait:500><enter><wait:2500><enter><wait:2000><enter><wait:3000><enter>"
