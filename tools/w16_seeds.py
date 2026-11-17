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
    # FT_*THKCONNECTIONDATA (flat thunks: user32's second stub, 0xBFF5D474):
    # a table of 16:16 targets after the export, each selector word on an NE
    # selector fixup chain (src type 2) of the segment it names.
    ft = sorted((e.segment, e.offset) for e in ne.entries
                if (lambda n: (n[0] if isinstance(n, tuple) else n).upper())(names.get(e.ordinal, ('', 0))).startswith('FT_'))
    for i, (sg, start) in enumerate(ft):
        seg = ne.segments[sg - 1]
        d = seg.data
        end = ft[i + 1][1] if i + 1 < len(ft) and ft[i + 1][0] == sg else len(d)
        n = 0
        for r in seg.relocations:
            if r.src_type != 2 or r.flags & 3 or not r.target_seg:
                continue
            o = r.offset
            while o != 0xFFFF and o + 2 <= len(d):
                if start < o - 2 < end:
                    out.setdefault(str(r.target_seg), []).append(hex(struct.unpack_from('<H', d, o - 2)[0]))
                    n += 1
                o = struct.unpack_from('<H', d, o)[0]
        print(f'FT seg {sg}:{start:04X}: {n} targets', file=sys.stderr)
    return ne.module_name, out


if __name__ == '__main__':
    root = sys.argv[1] if len(sys.argv) > 1 else 'work/sys'
    res = dict(seeds(os.path.join(root, f)) for f in ('USER.EXE', 'GDI.EXE', 'shell.new'))
    # KRNL386 call targets that ne_codemap's descent lost to data decoded as
    # code over them (seg 1 83EA runs through 852A, the int 21h handler it
    # calls as `pushf; push cs; call 852A`): seeds decode first and win.
    # (1:B711, BootStrap's `push [259A]; push B711h; retf`, is now found by
    # ne_codemap.) 1:B9E8: where its #UD handler (B9BD) resumes after its
    # ud0. 1:6582: KRNL386's 32->16 entry, which kernel32 reaches through
    # the table at KEXP+4 (DGROUP:1AC7, filled at run time).
    res['KERNEL'] = {'1': ['0x852a', '0xab30', '0xb9e8', '0x6582'], '3': ['0x3165', '0x3168']}
    # GDI 11:2B73 `call cs:[si+2CADh]`: three near stubs (`jmp far` to imports)
    # its init steps through, counted by the words at 2C96h (1, 6, 4, 0).
    res['GDI'].setdefault('11', []).extend(['0x2c9e', '0x2ca3', '0x2ca8'])
    # USER 30:3D12: the 32-bit window procedure's 16-bit entry. A KERNEL-owned
    # run-time thunk (`push proc32; push 3; jmp far` KRNL386 2:7616) reaches
    # it through the table at KRNL386 4:2214 (filled at run time).
    res['USER'].setdefault('30', []).append('0x3d12')
    json.dump(res, sys.stdout)
