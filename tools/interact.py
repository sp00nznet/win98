#!/usr/bin/env python3
"""Drive an accessory the same way on its stage 1 build (host user32/gdi32) and
on build/nt<app>.exe (Windows 2000's own user32/gdi32, lifted), and compare a
screenshot after each step pixel by pixel.

    py -3 tools/interact.py [winmine|notepad|calc ...]   (all three by default)
winmine: start, a click on a cell, Game > Intermediate, Game > Custom (dialog);
then, in runs of their own, the Custom dialog driven, the Game menu open
(WM_SYSCOMMAND SC_KEYMENU 'g'), at start and after Game > Intermediate.
notepad: start, text typed into its edit control (WM_CHAR).
calc: start, 12+3= pressed (WM_COMMAND with the buttons' IDs).
The mines are the same in both (W2K_FIXED_TICK: winmine seeds with
GetTickCount). Shots: shots/<run>_<build>_<t>.png.
"""
import os, subprocess, sys
from PIL import Image, ImageChops
Image.MAX_IMAGE_PIXELS = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CALC = {c: 0x7C + int(c) for c in '0123456789'}
CALC.update({'+': 0x5C, '-': 0x5D, '*': 0x5B, '/': 0x5A, '=': 0x70})
APPS = {
    'winmine': [('int', [('start', 4, []), ('click', 5.4, ['--click', '30,80@5']), ('intermediate', 8, ['--command', '522@7']),
                         ('custom', 10, ['--command', '524@9'])]),
                ('dlg', [('custom 20x30', 8, ['--command', '524@3', '--type', '0x8D=20@4', '--type', '0x8E=30@5',
                                             '--dlgcommand', '1@6'])]),
                ('menu', [('menu open', 5, ['--post', '0x112,0xF100,0x67@3'])]),
                ('menu2', [('menu, Interm.', 7, ['--command', '522@3', '--post', '0x112,0xF100,0x67@5'])])],
    # notepad's stage 1 build takes ~6 s to show its window (host comdlg32, shell32)
    'notepad': [('np', [('start', 9, []), ('typed', 11, ['--type', '15=Hello, Windows 2000@9.5'])])],
    'sol': [('sol', [('deal', 8, []), ('deck click', 10, ['--click', '40,60@9'])])],
    'freecell': [('fc', [('deal', 8, ['--command', '102@3']), ('card to cell', 11, ['--click', '40,300@9', '--click', '40,50@10'])])],
    'packager': [('pk', [('start', 6, [])])],
    'mspaint': [('paint', [('start', 9, []), ('line', 11, ['--drag', '100,100,250,180@9.5'])])],
    'wordpad': [('wp', [('start', 9, []), ('typed', 11, ['--type', '0xE900=Hello, Windows 2000@9.5'])])],
    'calc': [('calc', [('start', 5, []),
                       ('12+3=', 8, sum((['--command', '%d@%.1f' % (CALC[c], 5.5 + 0.3 * i)] for i, c in enumerate('12+3=')), []))])],
}
import time
T0 = time.time()
env = dict(os.environ, PCRECOMP_MUTE='1', MSYS_NO_PATHCONV='1', W2K_FIXED_TICK='12345', W2K_FIXED_TIME='1000000000')   # (sol's and freecell's deals)
RUNS = [(app, run, steps) for app in (sys.argv[1:] or APPS) for run, steps in APPS[app]]
for app, run, steps in RUNS:
    shots = [t for _, t, _ in steps]
    extra = sum((s for _, _, s in steps), [])
    for build in (app, 'nt' + app):
        cmd = [sys.executable, os.path.join(ROOT, 'tools', 'hiddenrun.py'), '--secs', str(int(max(shots)) + 2)]
        for t in shots:
            cmd += ['--shot', str(t)]
        cmd += extra + ['--out', os.path.join(ROOT, 'shots', '%s_%s' % (run, build)), '--',
                        os.path.join(ROOT, 'build', build + '.exe'), '--mute', '--watchdog', str(int(max(shots)) + 1)]
        subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True)
for app, name, t, run in ((app, n, t, r) for app, r, steps in RUNS for n, t, _ in steps):
    a, b = (os.path.join(ROOT, 'shots', '%s_%s_%d.png' % (run, k, int(t))) for k in (app, 'nt' + app))
    for f in (a, b):
        if os.path.exists(f) and os.path.getmtime(f) < T0: os.remove(f)   # a stale shot is no shot
    if not (os.path.exists(a) and os.path.exists(b)):
        print('%-8s %-13s missing shot (%s)' % (app, name, 'host' if not os.path.exists(a) else 'lifted'))
        continue
    A, B = Image.open(a).convert('RGB'), Image.open(b).convert('RGB')
    if A.size != B.size:
        print('%-8s %-13s sizes differ: %s vs %s' % (app, name, A.size, B.size))
        continue
    n = sum(1 for p in ImageChops.difference(A, B).get_flattened_data() if p != (0, 0, 0))
    print('%-8s %-13s %dx%d, %d pixels differ' % (app, name, A.size[0], A.size[1], n))
