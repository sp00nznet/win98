#!/usr/bin/env python3
"""Write every work/seeds/*.json the lift needs, from work/sys and work/rebased.

    py -3 tools/seeds.py

- vtseeds.py for the COM servers and Explorer (their vtables' methods);
- w16_seeds.py for ne_link (the 32->16 thunk targets: w16.json);
- kernel32: the byte after each `xchg [esp],eax; 66 CB` (o16 retf), where
  the host's ring-0 entries come back (survey.md, milestone 1);
- the seeds a run found by hand (HAND), added to the files above.
"""
import json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W = os.path.join(ROOT, 'work')
SEEDS = os.path.join(W, 'seeds')
VT = ['rebased/ole32.dll', 'sys/shdocvw.dll', 'sys/shell32.dll', 'rebased/explorer_1400.exe',
      'sys/browseui.dll', 'sys/shdoc401.dll']
HAND = {'browseui.dll': [0x7F334D00]}   # a window procedure (survey.md, session 40)


def main():
    os.makedirs(SEEDS, exist_ok=True)
    tools = os.path.join(ROOT, 'tools')
    for m in VT:
        out = os.path.join(SEEDS, os.path.basename(m) + '.json')
        subprocess.run([sys.executable, os.path.join(tools, 'vtseeds.py'), os.path.join(W, m), out], check=True)
    for name, extra in HAND.items():
        p = os.path.join(SEEDS, name + '.json')
        have = {int(x, 16) for x in json.load(open(p))} if os.path.exists(p) else set()
        open(p, 'w').write(json.dumps(['0x%08X' % v for v in sorted(have | set(extra))]))
    with open(os.path.join(SEEDS, 'w16.json'), 'w') as f:
        subprocess.run([sys.executable, os.path.join(tools, 'w16_seeds.py'), os.path.join(W, 'sys')], stdout=f, check=True)
    import pefile
    p = pefile.PE(os.path.join(W, 'sys', 'kernel32.dll'))
    b, img = p.OPTIONAL_HEADER.ImageBase, p.get_memory_mapped_image()
    k = ['0x%08X' % (b + m.start() + 5) for m in re.finditer(rb'\x87\x04\x24\x66\xcb', img)]
    open(os.path.join(SEEDS, 'kernel32.dll.json'), 'w').write(json.dumps(k))
    print('kernel32: %d retf returns' % len(k), file=sys.stderr)


if __name__ == '__main__':
    main()
