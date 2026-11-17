#!/bin/sh
# Gate: boot with Solitaire as the shell, drag the dealt ace of hearts to the
# first foundation; that pile must match the reference (work/sol_ref_ace.png,
# cut from out/s27sol_drag.png on first use; a Win98 render, never committed).
cd "$(dirname "$0")/.." || exit 1
export PCRECOMP_MUTE=1 HIDDENRUN_DESKTOP=pcrecomp_w98 W16_WINDOW=1 W16_LIBMAIN=KERNEL W16_SHELL=sol.exe
export W16_INPUT='drag 570,209,324,119@40'
py -3 tools/hiddenrun.py --secs 50 --shot 49 --out work/sol_gate -- 'build\w16test.exe' --mute > /dev/null 2>&1
py -3 - <<'P'
import os, sys
from PIL import Image, ImageChops
box = (290, 105, 625, 330)                      # foundations and the right tableau, in the shot
if not os.path.exists('work/sol_ref_ace.png'):
    Image.open('out/s27sol_drag.png').crop(box).save('work/sol_ref_ace.png')
try: got = Image.open('work/sol_gate_49.png').convert('RGB').crop(box)
except OSError: print('sol: no screenshot'); sys.exit(1)
ok = ImageChops.difference(got, Image.open('work/sol_ref_ace.png').convert('RGB')).getbbox() is None
print('sol: ace dragged to the foundation (identical)' if ok else 'sol DIFFERS'); sys.exit(not ok)
P
