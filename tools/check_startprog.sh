#!/bin/sh
# Gate: programs start from Explorer's Start menu (Programs > Accessories > ...): Notepad
# (shell32's context menu on the shortcut, then ShellExecute) and Solitaire (Games, a Win16 app).
#   sh tools/check_startprog.sh      (reference: out/s36_start_notepad.png, a run's own shot, never committed)
cd "$(dirname "$0")/.." || exit 1
export PCRECOMP_MUTE=1 HIDDENRUN_DESKTOP=pcrecomp_w98 W16_WINDOW=1 W16_LIBMAIN=KERNEL W16_SHELL=Explorer.exe
MENU="click 29,587@20;click 92,326@23;click 212,320@26"
rm -f work/startprog_*.png
W16_INPUT="$MENU;click 312,359@29" py -3 tools/hiddenrun.py --secs 40 --shot 38 --out work/startprog_np -- 'build\w16test.exe' --mute > /dev/null 2>&1
W16_INPUT="$MENU;click 308,320@29;click 408,340@32" py -3 tools/hiddenrun.py --secs 57 --shot 55 --out work/startprog_sol -- 'build\w16test.exe' --mute > /dev/null 2>&1
py -3 - <<'P'
import sys
from PIL import Image, ImageChops
def shot(f):
    try: return Image.open(f).convert('RGB')
    except OSError: return None
np, sol = shot('work/startprog_np_38.png'), shot('work/startprog_sol_55.png')
ref = Image.open('out/s36_start_notepad.png').convert('RGB')
box = (36, 54, 636, 95)                          # Notepad's caption and menu bar (the caret blinks below)
np_ok = np is not None and not ImageChops.difference(np.crop(box), ref.crop(box)).getbbox()
# Solitaire's deal is seeded from the clock: count its felt (0,130,0) and the caption's blue
sol_ok = sol is not None and sum(1 for p in sol.crop((40, 320, 620, 460)).get_flattened_data() if p == (0, 130, 0)) > 50000 \
    and sol.getpixel((300, 65)) != (0, 128, 128)
print(f'startprog: notepad {np_ok}, solitaire {sol_ok}'); sys.exit(not (np_ok and sol_ok))
P
