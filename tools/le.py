#!/usr/bin/env python3
"""Load a Windows 9x VxD (LE), apply its internal fixups, and show its Win32
services: the ones kernel32 reaches through the VMM gate as (VxD id << 16 |
n). Read a service's code here before answering it in vxd32.c.

    py -3 tools/le.py work/sys/vwin32.vxd               # the Win32 service table
    py -3 tools/le.py work/sys/vwin32.vxd 0C [count]    # disassemble service 0Ch
    py -3 tools/le.py work/sys/vwin32.vxd @1:2F40 [n]    # object 1, offset 2F40

Objects are placed at 0x10000000 * object number, so an address names its
object. Imports from other VxDs are VxDCall (int 20h + dword) in the code.
"""
import struct
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32


def load(path):
    d = open(path, 'rb').read()
    le = struct.unpack_from('<I', d, 0x3C)[0]
    assert d[le:le + 2] == b'LE', 'not an LE file'
    h = lambda o: struct.unpack_from('<I', d, le + o)[0]
    psize, otab, ocnt, optab = h(0x28), h(0x40), h(0x44), h(0x48)
    fpt, frt, pages, entry = h(0x68), h(0x6C), h(0x80), h(0x5C)
    objs, mem = [], {}
    for i in range(ocnt):
        vsize, base, flags, pidx, pcnt, _ = struct.unpack_from('<6I', d, le + otab + 24 * i)
        buf = bytearray(max(vsize, pcnt * psize))
        for p in range(pcnt):
            pn = struct.unpack_from('>I', b'\0' + d[le + optab + 4 * (pidx - 1 + p):][:3])[0]
            off = pages + (pn - 1) * psize
            buf[p * psize:(p + 1) * psize] = d[off:off + psize].ljust(psize, b'\0')
        objs.append((pidx, pcnt, flags))
        mem[i + 1] = buf
    # fixups: per page, records of (src, flags, srcoff, target)
    for oi, (pidx, pcnt, _) in enumerate(objs, 1):
        for p in range(pcnt):
            pg = pidx - 1 + p
            a, b = struct.unpack_from('<2I', d, le + fpt + 4 * pg)
            r = le + frt + a
            while r < le + frt + b:
                src, fl = d[r], d[r + 1]
                r += 2
                cnt = 1
                if src & 0x20:
                    cnt = d[r]; r += 1
                    offs = None
                else:
                    so = struct.unpack_from('<h', d, r)[0]; r += 2
                if fl & 3 != 0:      # only internal references in a VxD
                    raise SystemExit('fixup kind %d' % (fl & 3))
                tobj = d[r] if not fl & 0x40 else struct.unpack_from('<H', d, r)[0]
                r += 1 if not fl & 0x40 else 2
                if (src & 0xF) == 2:   # selector: nothing to patch
                    toff = 0
                elif fl & 0x10:
                    toff = struct.unpack_from('<I', d, r)[0]; r += 4
                else:
                    toff = struct.unpack_from('<H', d, r)[0]; r += 2
                if src & 0x20:
                    offs = [struct.unpack_from('<h', d, r + 2 * k)[0] for k in range(cnt)]
                    r += 2 * cnt
                else:
                    offs = [so]
                tgt = 0x10000000 * tobj + toff
                for so in offs:
                    at = p * psize + so
                    if at < 0 or at + 4 > len(mem[oi]):
                        continue
                    kind = src & 0xF
                    if kind == 7:
                        struct.pack_into('<I', mem[oi], at, tgt)
                    elif kind == 8:   # 32-bit self-relative
                        struct.pack_into('<I', mem[oi], at, (tgt - (0x10000000 * oi + at + 4)) & 0xFFFFFFFF)
    # entry 1 is the DDB
    e = le + entry
    cnt, typ = d[e], d[e + 1]
    obj = struct.unpack_from('<H', d, e + 2)[0]
    ddb_off = struct.unpack_from('<I', d, e + 5)[0]
    return mem, 0x10000000 * obj + ddb_off


def rd(mem, va, n=4):
    o, off = va >> 28, va & 0x0FFFFFFF
    if o not in mem:
        return 0
    return int.from_bytes(mem[o][off:off + n], 'little')


def main():
    mem, ddb = load(sys.argv[1])
    name = bytes(mem[ddb >> 28][(ddb & 0xFFFFFFF) + 0x0C:][:8]).decode().strip()
    dev = rd(mem, ddb + 6, 2)
    w32 = rd(mem, ddb + 0x38)   # DDB_Win32_Service_Table
    n = rd(mem, w32) if w32 else 0
    if len(sys.argv) == 2:
        print('%s (VxD %04X): DDB %08X, %d Win32 services at %08X' % (name, dev, ddb, n, w32))
        for i in range(n):
            print('  %04X%04X  %08X  %d args' % (dev, i, rd(mem, w32 + 4 + 8 * i), rd(mem, w32 + 8 + 8 * i)))
        return
    where = sys.argv[2]
    if where.startswith('@'):
        o, off = where[1:].split(':')
        va = 0x10000000 * int(o) + int(off, 16)
    else:
        va = rd(mem, w32 + 4 + 8 * int(where, 16))
    cnt = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    buf = bytes(mem[va >> 28][va & 0xFFFFFFF:])
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    pos, k = 0, 0
    while k < cnt and pos < len(buf):
        ins = next(md.disasm(buf[pos:pos + 16], va + pos), None)
        if ins is None:
            print('%08X  db %02X' % (va + pos, buf[pos])); pos += 1; k += 1; continue
        if ins.mnemonic == 'int' and ins.op_str == '0x20':   # VxDCall: int 20h + service dword
            svc = struct.unpack_from('<I', buf, pos + 2)[0]
            print('%08X  VxDCall  %04X:%04X' % (va + pos, svc >> 16, svc & 0xFFFF))
            pos += 6
        else:
            print('%08X  %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
            pos += ins.size
        k += 1


if __name__ == '__main__':
    main()
