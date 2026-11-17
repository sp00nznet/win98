#!/bin/sh
# Gate: Explorer's Start menu opens on a real click, and Start -> Run -> "calc" -> OK starts
# the lifted calc in the same process (kernel32's own CreateProcess).
#   sh tools/check_run.sh      (references: out/s33_startmenu.png, out/s33_run_calc.png, a run's own shots, never committed)
cd "$(dirname "$0")/.." || exit 1
export PCRECOMP_MUTE=1 HIDDENRUN_DESKTOP=pcrecomp_w98 W16_WINDOW=1 W16_LIBMAIN=KERNEL W16_SHELL=Explorer.exe
# screen coordinates (the shots have the 8,31 window frame on top): Start, Run..., the text, OK
export W16_INPUT="click 29,587@20;click 83,517@24;text calc@28;click 138,540@31"
rm -f work/run_gate_*.png
py -3 tools/hiddenrun.py --secs 46 --shot 23 --shot 44 --out work/run_gate -- 'build\w16test.exe' --mute > /dev/null 2>&1
py -3 - <<'P'
import sys
from PIL import Image, ImageChops
def same(got, ref, box):
    try: g = Image.open(got).convert('RGB')
    except OSError: return False
    return not ImageChops.difference(g.crop(box), Image.open(ref).convert('RGB').crop(box)).getbbox()
bad = [n for n, got, ref, box in (('start menu', 'work/run_gate_23.png', 'out/s33_startmenu.png', (10, 338, 168, 606)),
                                   ('calc', 'work/run_gate_44.png', 'out/s33_run_calc.png', (52, 74, 312, 326)))
       if not same(got, ref, box)]
print('run: Start menu opens, Run starts calc (identical)' if not bad else 'run DIFFERS: ' + ', '.join(bad))
sys.exit(bool(bad))
P
