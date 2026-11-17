#!/bin/sh
# Gate: double-clicking My Computer opens its folder window without a fault (GDI's use32 segments,
# s37) and the taskbar gets its "My Computer" button. ponytail: the window itself does not paint
# yet (survey Session 37); tighten this to the window's contents when it does.
cd "$(dirname "$0")/.." || exit 1
export PCRECOMP_MUTE=1 HIDDENRUN_DESKTOP=pcrecomp_w98 W16_WINDOW=1 W16_LIBMAIN=KERNEL W16_SHELL=Explorer.exe
rm -f work/mycomp_*.png
W16_INPUT="click 40,50@22;click 40,50@22.15" py -3 tools/hiddenrun.py --secs 42 --shot 40 --out work/mycomp -- 'build\w16test.exe' --mute > /dev/null 2>&1
py -3 - <<'P'
import sys
from PIL import Image
log = open('work/mycomp.log', errors='replace').read()
try: got = Image.open('work/mycomp_40.png').convert('RGB')
except OSError: print('mycomp: no screenshot'); sys.exit(1)
fault = '=== fault' in log
# the "My Computer" taskbar button: its black text and edges at x 165..320 (none without it)
button = sum(1 for x in range(165, 320) for y in range(606, 630) if got.getpixel((x, y)) == (0, 0, 0)) > 200
print(f'mycomp: no fault {not fault}, taskbar button {button}'); sys.exit(fault or not button)
P
