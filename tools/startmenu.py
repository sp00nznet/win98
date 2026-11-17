#!/usr/bin/env python3
"""Write the Start Menu shortcuts Win98 setup makes, from its own INFs, into work/root.

    py -3 tools/startmenu.py

Setup's UpdateInis sections write setup.ini [progman.groups] / [groupN] lines and grpconv
turns them into Start Menu\\Programs\\<group>\\<name>.lnk. This does the same for every
"creates link" line whose target is in work/root\\WINDOWS (notepad, calc, sol, winmine...).
The .lnk is the Win98 format: an ID list (My Computer\\C:\\...) plus a LinkInfo local path.
ponytail: only C:\\WINDOWS targets, no icon/args/hotkey fields; add them when an INF needs one.
"""
import csv, os, re, struct, time
from reginst import sections, strings, SYS

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'work', 'root')


def dosdt(t):
    t = time.localtime(t)
    return ((t.tm_year - 1980) << 9 | t.tm_mon << 5 | t.tm_mday,
            t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec // 2)


def item(kind, attr, size, mtime, name):
    d, t = dosdt(mtime)
    b = struct.pack('<BBIHHH', kind, 0, size, d, t, attr) + name.encode('ascii') + b'\0'
    if len(b) % 2: b += b'\0'
    return struct.pack('<H', len(b) + 2) + b


def lnk(target):
    host = os.path.join(ROOT, *target[3:].split('\\'))
    st = os.stat(host)
    clsid_mc = bytes.fromhex('e04fd020ea3a6910a2d808002b30309d')
    ids = struct.pack('<HBB', 0x14, 0x1F, 0x50) + clsid_mc
    ids += struct.pack('<HB', 0x19, 0x2F) + b'C:\\'.ljust(0x16, b'\0')
    parts = target[3:].split('\\')
    for i, p in enumerate(parts):
        hp = os.path.join(ROOT, *parts[:i + 1])
        f = i == len(parts) - 1
        ids += item(0x32 if f else 0x31, 0x20 if f else 0x10, os.stat(hp).st_size if f else 0,
                    os.stat(hp).st_mtime, p.upper())
    ids = struct.pack('<H', len(ids) + 2) + ids + b'\0\0'
    vol = struct.pack('<IIII', 0x11, 3, 0x12345678, 0x10) + b'\0'
    base = target.encode('ascii') + b'\0'
    li = struct.pack('<IIIIII', 0x1C, 1, 0x1C, 0x1C + len(vol), 0, 0x1C + len(vol) + len(base)) + vol + base + b'\0'
    li = struct.pack('<I', len(li) + 4) + li
    wd = target.rsplit('\\', 1)[0]
    wds = struct.pack('<H', len(wd)) + wd.encode('ascii')
    ft = int((st.st_mtime + 11644473600) * 1e7)
    hdr = struct.pack('<I16sIIQQQIIIHH8s', 0x4C, bytes.fromhex('0114020000000000c000000000000046'),
                      0x1 | 0x2 | 0x10, 0x20, ft, ft, ft, st.st_size, 0, 1, 0, 0, b'\0' * 8)
    return hdr + ids + li + wds + b'\0\0\0\0'


def main():
    made = 0
    for fn in sorted(os.listdir(SYS)):
        if not fn.lower().endswith('.inf'): continue
        sec = sections(open(os.path.join(SYS, fn), encoding='latin-1').read())
        sub = strings(sec, {})
        for name, ls in sec.items():
            groups = {}
            for l in ls:
                f = [x.strip() for x in next(csv.reader([sub(l)], skipinitialspace=True))]
                if len(f) < 4 or f[0].lower() != 'setup.ini': continue
                if f[1].lower() == 'progman.groups':
                    k, _, v = f[3].partition('=')
                    groups[k.lower()] = re.sub(r'\s*;.*$', '', v).strip('" ')
                elif f[1].lower() in groups:
                    v = next(csv.reader([f[3]], skipinitialspace=True))
                    if len(v) < 2: continue          # a "deletes link" line
                    exe = v[1].strip().strip('"')
                    target = exe if ':' in exe else 'C:\\WINDOWS\\' + exe.upper()
                    if not target.upper().startswith('C:\\WINDOWS\\') or not os.path.isfile(
                            os.path.join(ROOT, *target[3:].split('\\'))): continue
                    d = os.path.join(ROOT, 'WINDOWS', 'Start Menu', 'Programs', *groups[f[1].lower()].split('\\'))
                    os.makedirs(d, exist_ok=True)
                    open(os.path.join(d, v[0].strip() + '.lnk'), 'wb').write(lnk(target))
                    print(fn, groups[f[1].lower()], v[0].strip(), target)
                    made += 1
    print(made, 'shortcuts')


if __name__ == '__main__':
    main()
