#!/bin/sh
# Gate: Win98's own KRNL386/USER/GDI with notepad (Win32, rebased) as the shell;
# keys H i ! and the edit control's first line must match the reference "Hi!".
#   sh tools/check_notepad.sh       (reference: work/notepad_ref_hi.png, cut from
#   out/s28np_hi.png on first use; never committed, it is a Win98 render)
cd "$(dirname "$0")/.." || exit 1
export PCRECOMP_MUTE=1 HIDDENRUN_DESKTOP=pcrecomp_w98 W16_WINDOW=1 W16_LIBMAIN=KERNEL W16_SHELL=notepad.exe
export W16_INPUT='key H@40;key i@41;key !@42'
rm -f work/notepad_gate_*.png
py -3 tools/hiddenrun.py --secs 47 --shot 45 --out work/notepad_gate -- 'build\w16test.exe' --mute > /dev/null 2>&1
py -3 - <<'P'
import os, sys
from PIL import Image, ImageChops
box = (40, 60, 330, 125)                        # caption and the first text line, in the shot
if not os.path.exists('work/notepad_ref_hi.png'):
    Image.open('out/s28np_hi.png').crop(box).save('work/notepad_ref_hi.png')
try: got = Image.open('work/notepad_gate_45.png').convert('RGB').crop(box)
except OSError: print('notepad: no screenshot'); sys.exit(1)
ok = ImageChops.difference(got, Image.open('work/notepad_ref_hi.png').convert('RGB')).getbbox() is None
print('notepad typed Hi! (identical)' if ok else 'notepad DIFFERS'); sys.exit(not ok)
P
