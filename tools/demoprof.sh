#!/usr/bin/env bash
# Loads a Tyrian build (default build-prof/, made with EXTRA=-DM9B_PROFILE),
# starts the title screen's demo and records the reports for SECONDS
# (default 40): demoprof.sh [BUILD] [SECONDS] > log
set -euo pipefail
cd "$(dirname "$0")/.."
B=${1:-build-prof}
T=${2:-40}
python3 ../core/tools/run.py "$B/tyrian.bin" --listen 12 > /dev/null 2>&1
python3 ../core/tools/m9bctl.py keys "<down><down><down><down><down><enter>"
python3 - "$T" <<'PY'
import os, sys
sys.path.insert(0, "../core/tools")
import run
fd = os.open(run.default_port(), os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
run.set_raw(fd, 115200)
_, b = run.read_until(fd, b"\x00NEVER", float(sys.argv[1]))
sys.stdout.write(b.decode(errors="replace"))
PY
