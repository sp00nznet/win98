#!/bin/sh
# Milestone gate: boot Win98's own KRNL386/USER/GDI, MSGSRV32 starts calc,
# keys 1 2 + 3 = and the display region must match the reference "15.".
#   sh tools/check_calc.sh          (reference: work/calc_ref_15.png, cut from
#   out/s25key_15.png on first use; never committed, it is a Win98 render)
cd "$(dirname "$0")/.." || exit 1
export PCRECOMP_MUTE=1 HIDDENRUN_DESKTOP=pcrecomp_w98 W16_WINDOW=1 W16_LIBMAIN=KERNEL
export W16_INPUT='key 1@40;key 2@41;key +@42;key 3@43;key =@44'
py -3 tools/hiddenrun.py --secs 52 --shot 50 --out work/calc_gate -- 'build\w16test.exe' --mute > /dev/null 2>&1
py -3 - <<'P'
import os, sys
from PIL import Image, ImageChops
box = (72, 130, 310, 155)                       # calc's display, in the shot
if not os.path.exists('work/calc_ref_15.png'):
    Image.open('out/s25key_15.png').crop(box).save('work/calc_ref_15.png')
try: got = Image.open('work/calc_gate_50.png').convert('RGB').crop(box)
except OSError: print('calc: no screenshot'); sys.exit(1)
ok = ImageChops.difference(got, Image.open('work/calc_ref_15.png').convert('RGB')).getbbox() is None
print('calc 12+3= shows 15. (display identical)' if ok else 'calc display DIFFERS'); sys.exit(not ok)
P
