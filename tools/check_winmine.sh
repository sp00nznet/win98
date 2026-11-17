#!/bin/sh
# Gate: boot with Minesweeper as the shell, click one cell of the field; that
# cell must be uncovered (no longer look like its unclicked neighbour) and the
# timer must have started (its digits change from the boot shot's).
# Mines are placed at random, so it compares against the run's own cells.
cd "$(dirname "$0")/.." || exit 1
export PCRECOMP_MUTE=1 HIDDENRUN_DESKTOP=pcrecomp_w98 W16_WINDOW=1 W16_LIBMAIN=KERNEL W16_SHELL=winmine.exe
export W16_INPUT='click 146,171@40'
rm -f work/winmine_gate_*.png
py -3 tools/hiddenrun.py --secs 47 --shot 39 --shot 46 --out work/winmine_gate -- 'build\w16test.exe' --mute > /dev/null 2>&1
py -3 - <<'P'
import sys
from PIL import Image, ImageChops
try: a, b = (Image.open(f'work/winmine_gate_{t}.png').convert('RGB') for t in (39, 46))
except OSError: print('winmine: no screenshot'); sys.exit(1)
cell, nb, timer = (147, 195, 162, 210), (163, 195, 178, 210), (180, 123, 220, 147)
uncovered = ImageChops.difference(b.crop(cell), b.crop(nb)).getbbox() is not None
was_covered = ImageChops.difference(a.crop(cell), a.crop(nb)).getbbox() is None
ticking = ImageChops.difference(a.crop(timer), b.crop(timer)).getbbox() is not None
ok = uncovered and was_covered and ticking
print('winmine: cell uncovered, timer running' if ok else f'winmine FAILS (uncovered {uncovered}, covered before {was_covered}, timer {ticking})'); sys.exit(not ok)
P
