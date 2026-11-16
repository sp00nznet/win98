"""Rebuild work/seeds/w16.json: the 32->16 thunk targets of USER.EXE and GDI.EXE.

Each *THKCONNECTIONDATALS export is {dword checksum, 16:16 dispatcher}.
user32/gdi32 get that 16:16 through kernel32 ordinal 43 and enter it via
ordinal 45 with di = thunk index * 2; the dispatcher is
`mov bp,sp; mov ax,DGROUP; mov ds,ax; jmp cs:[di+table]`, and the table of
near targets runs from `table` up to the dispatcher. Those targets are not
reachable by the code map, so they become ne_link --seeds.

    py -3 tools/w16_seeds.py work/sys > work/seeds/w16.json
"""
import json, os, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'pcrecomp-w98', 'tools', 'ne'))
from ne_parse import parse_ne


def seeds(path):
    ne = parse_ne(path)
    names = dict(ne.resident_names + ne.nonresident_names)
    names = {o: n for n, o in names.items()} if isinstance(next(iter(names)), str) else names
    out = {}
    for e in ne.entries:
        nm = names.get(e.ordinal, ('', 0))
        nm = nm[0] if isinstance(nm, tuple) else nm
        if not nm.upper().endswith('THKCONNECTIONDATALS'):
            continue
        d = ne.segments[e.segment - 1].data
        disp = struct.unpack_from('<H', d, e.offset + 4)[0]
        code = d[disp:disp + 12]
        if code[:3] != b'\x8b\xec\xb8' or code[5:10] != b'\x8e\xd8\x2e\xff\xa5':
            sys.exit(f'{nm}: no dispatcher at {e.segment}:{disp:04X}: {code.hex()}')
        base = struct.unpack_from('<H', code, 10)[0]
        offs = [struct.unpack_from('<H', d, o)[0] for o in range(base, disp, 2)]
        if any(o >= len(d) for o in offs):
            sys.exit(f'{nm}: target past segment {e.segment}')
        out.setdefault(str(e.segment), []).extend(hex(o) for o in offs + [disp])
        print(f'{nm}: seg {e.segment} dispatcher {disp:04X}, {len(offs)} targets', file=sys.stderr)
    return ne.module_name, out


if __name__ == '__main__':
    root = sys.argv[1] if len(sys.argv) > 1 else 'work/sys'
    res = dict(seeds(os.path.join(root, f)) for f in ('USER.EXE', 'GDI.EXE'))
    # KRNL386 call targets that ne_codemap's descent lost to data decoded as
    # code over them (seg 1 83EA runs through 852A, the int 21h handler it
    # calls as `pushf; push cs; call 852A`): seeds decode first and win.
    # 1:B711: BootStrap's `push [259A]; push B711h; retf` into segment 1 as
    # LoadSegment reloaded it (the selector is data, so the code map cannot
    # follow it). 1:B9E8: where its #UD handler (B9BD) resumes after its ud0.
    res['KERNEL'] = {'1': ['0x852a', '0xab30', '0xb711', '0xb9e8'], '3': ['0x3165', '0x3168']}
    json.dump(res, sys.stdout)
