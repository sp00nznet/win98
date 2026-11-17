#!/bin/sh
# Gate: Win98's own Explorer as the shell: the taskbar (Start button, clock box) and the
# desktop's icons with their labels ("My Computer", not the host's name) paint.
#   sh tools/check_explorer.sh      (reference: out/s32ex_desktop.png, a run's own shot, never committed)
cd "$(dirname "$0")/.." || exit 1
export PCRECOMP_MUTE=1 HIDDENRUN_DESKTOP=pcrecomp_w98 W16_WINDOW=1 W16_LIBMAIN=KERNEL W16_SHELL=Explorer.exe
unset W16_INPUT
rm -f work/explorer_gate_*.png
py -3 tools/hiddenrun.py --secs 40 --shot 38 --out work/explorer_gate -- 'build\w16test.exe' --mute > /dev/null 2>&1
py -3 - <<'P'
import sys
from PIL import Image, ImageChops
try: got = Image.open('work/explorer_gate_38.png').convert('RGB')
except OSError: print('explorer: no screenshot'); sys.exit(1)
ref = Image.open('out/s32ex_desktop.png').convert('RGB')
# the Start button and the icon column with labels; the clock's text changes
boxes = {'start button': (8, 604, 66, 632), 'desktop icons': (8, 31, 90, 235)}
bad = [n for n, b in boxes.items() if ImageChops.difference(got.crop(b), ref.crop(b)).getbbox()]
print('explorer: taskbar and desktop icons paint (identical)' if not bad else 'explorer DIFFERS: ' + ', '.join(bad))
sys.exit(bool(bad))
P
