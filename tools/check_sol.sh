#!/bin/sh
# Gate: boot with Solitaire as the shell (its deal is seeded from the clock, so
# it differs every run). A click on the deck turns a card onto the waste pile,
# and the status bar's clock runs (the two shots' "Time:" fields differ).
cd "$(dirname "$0")/.." || exit 1
export PCRECOMP_MUTE=1 HIDDENRUN_DESKTOP=pcrecomp_w98 W16_WINDOW=1 W16_LIBMAIN=KERNEL W16_SHELL=sol.exe
export W16_INPUT='click 82,129@40'
rm -f work/sol_gate_*.png
py -3 tools/hiddenrun.py --secs 58 --shot 48 --shot 56 --out work/sol_gate -- 'build\w16test.exe' --mute > /dev/null 2>&1
py -3 - <<'P'
import sys
from PIL import Image, ImageChops
try: a, b = (Image.open(f'work/sol_gate_{t}.png').convert('RGB') for t in (48, 56))
except OSError: print('sol: no screenshot'); sys.exit(1)
waste = a.crop((135, 115, 205, 205))           # the waste pile, in the shot
dealt = sum(1 for p in waste.getdata() if p == (255, 255, 255)) > 1000   # a card face, not the green felt
clock = (580, 474, 625, 490)                    # the seconds after "Time:"
ticks = ImageChops.difference(a.crop(clock), b.crop(clock)).getbbox() is not None
print(f'sol: deck dealt {dealt}, clock running {ticks}'); sys.exit(not (dealt and ticks))
P
