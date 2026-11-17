#!/bin/sh
# Gate: double-clicking My Computer opens and paints its folder window (s38: USER's thunk entries
# far-jump by retf; GetWindowPlacement had never run, so the frame was sized from stack garbage),
# then double-clicking C: lists work/root in it (s38: int 21h 7303).
cd "$(dirname "$0")/.." || exit 1
export PCRECOMP_MUTE=1 HIDDENRUN_DESKTOP=pcrecomp_w98 W16_WINDOW=1 W16_LIBMAIN=KERNEL W16_SHELL=Explorer.exe
rm -f work/mycomp_*.png
W16_INPUT="click 40,50@22;click 40,50@22.15;click 64,127@32;click 64,127@32.15" py -3 tools/hiddenrun.py --secs 46 --shot 30 --shot 45 --out work/mycomp -- 'build\w16test.exe' --mute > /dev/null 2>&1
py -3 - <<'P'
import sys
from PIL import Image
log = open('work/mycomp.log', errors='replace').read()
def look(t):
    try: im = Image.open(f'work/mycomp_{t}.png').convert('RGB')
    except OSError: return None
    white = sum(1 for x in range(40, 430, 2) for y in range(200, 400, 2) if im.getpixel((x, y)) == (255, 255, 255))
    yellow = sum(1 for x in range(40, 430) for y in range(135, 240) if im.getpixel((x, y)) == (255, 255, 0))
    text = sum(1 for x in range(40, 430) for y in range(170, 190) if im.getpixel((x, y)) == (0, 0, 0))
    button = sum(1 for x in range(165, 320) for y in range(606, 630) if im.getpixel((x, y)) == (0, 0, 0)) > 200
    return white, yellow, text, button
fault = '=== fault' in log or 'unknown AX' in log
a, b = look(30), look(45)
if not a or not b: print('mycomp: no screenshot'); sys.exit(1)
# My Computer: a white client area, (C:)/Printers/Control Panel (one yellow icon) with labels; C:: its yellow folders
mc = a[0] > 15000 and 200 < a[1] < 1000 and a[2] > 150 and a[3]
c = b[0] > 15000 and b[1] > 1000 and b[2] > 300 and b[3]
print(f'mycomp: no fault {not fault}, My Computer painted {mc} {a}, C: listed {c} {b}')
sys.exit(fault or not mc or not c)
P
