#!/bin/sh
# Gate: windows close by their X button, move by their caption and minimize/restore from the
# taskbar (s41: USER's modal loops poll PeekMessage, whose KERNEL.447 now hands the machine over
# so the input thread can post the release; a dead thread's Win16Mutex is released the right way;
# PR_SHARED heaps sit in the shared arena). Where a window was, the desktop repaints: the icon
# labels must be whole again (1051 white pixels in the icon column, as before anything opened).
cd "$(dirname "$0")/.." || exit 1
export PCRECOMP_MUTE=1 HIDDENRUN_DESKTOP=pcrecomp_w98 W16_WINDOW=1 W16_LIBMAIN=KERNEL W16_SHELL=Explorer.exe
rm -f work/close_*.png
# Explorer keeps a folder's place in its box (Streams\N): start and end from the default place
STREAMS='HKCU\Software\pcrecomp\w98\xcopy\HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Streams'
forget() { for n in 1 2 3 4 5; do MSYS2_ARG_CONV_EXCL='*' reg delete "$STREAMS\\$n" /f > /dev/null 2>&1; done; }
forget
# Notepad (Start > Programs > Accessories > Notepad), then its X
W16_INPUT="click 29,587@20;click 92,326@23;click 212,320@26;click 312,359@29;click 614,35@36" py -3 tools/hiddenrun.py --secs 44 --shot 34 --shot 42 --out work/close_np -- 'build\w16test.exe' --mute > /dev/null 2>&1
# calc (Start > Run > calc), then its X
W16_INPUT="click 29,587@20;click 83,517@24;text calc@28;click 138,540@31;click 291,56@42" py -3 tools/hiddenrun.py --secs 50 --shot 40 --shot 48 --out work/close_calc -- 'build\w16test.exe' --mute > /dev/null 2>&1
# My Computer: open, drag the caption 200,200 down-right, minimize, restore from the taskbar, X
W16_INPUT="click 40,50@22;click 40,50@22.15;drag 200,35,400,235@30;click 580,235@34;click 232,587@38;click 615,235@42" py -3 tools/hiddenrun.py --secs 48 --shot 29 --shot 33 --shot 37 --shot 41 --shot 46 --out work/close_mc -- 'build\w16test.exe' --mute > /dev/null 2>&1
forget
py -3 - <<'P'
import sys
from PIL import Image
TEAL, WHITE, GREY = (0, 130, 132), (255, 255, 255), (198, 195, 198)
def shot(n):
    try: return Image.open(f'work/close_{n}.png').convert('RGB')
    except OSError: return None
def labels(im): return sum(1 for x in range(10, 82) for y in range(66, 234) if im.getpixel((x, y)) == WHITE)
def button(im): return sum(1 for x in range(165, 320) for y in range(606, 630) if im.getpixel((x, y)) == (0, 0, 0)) > 200
def teal(im, box): return all(im.getpixel((x, y)) == TEAL for x in range(box[0], box[2], 7) for y in range(box[1], box[3], 7))
s = {n: shot(n) for n in ('np_34', 'np_42', 'calc_40', 'calc_48', 'mc_29', 'mc_33', 'mc_37', 'mc_41', 'mc_46')}
if any(v is None for v in s.values()): print('close: missing screenshot', [k for k, v in s.items() if v is None]); sys.exit(1)
ok = {
    'notepad open': s['np_34'].getpixel((300, 300)) == WHITE and button(s['np_34']),
    'notepad closed': teal(s['np_42'], (90, 60, 630, 460)) and labels(s['np_42']) >= 1000 and not button(s['np_42']),
    'calc open': s['calc_40'].getpixel((100, 300)) == GREY and button(s['calc_40']),
    'calc closed': teal(s['calc_48'], (90, 80, 310, 320)) and labels(s['calc_48']) >= 1000 and not button(s['calc_48']),
    'folder open': s['mc_29'].getpixel((100, 250)) == WHITE,
    'folder moved': s['mc_33'].getpixel((300, 450)) == WHITE and teal(s['mc_33'], (90, 60, 220, 240)) and labels(s['mc_33']) >= 1000,
    'folder minimized': teal(s['mc_37'], (90, 60, 800, 590)) and button(s['mc_37']),
    'folder restored': s['mc_41'].getpixel((300, 450)) == WHITE,
    'folder closed': teal(s['mc_46'], (90, 60, 800, 590)) and labels(s['mc_46']) >= 1000 and not button(s['mc_46']),
}
print('close: ' + ', '.join(f'{k} {v}' for k, v in ok.items()))
sys.exit(not all(ok.values()))
P
