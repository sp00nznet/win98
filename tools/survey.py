#!/usr/bin/env python3
"""Stage 1 survey: the kernel boundary of Windows 98's 32-bit system DLLs.

    py -3 tools/survey.py work/sys/kernel32.dll work/sys/user32.dll work/sys/gdi32.dll

Linear-sweeps every executable section with capstone and counts the
instructions that leave ring 3 code (int N, far call/jmp, lcall through a
gate), and names the export each site falls in (nearest export below).
"""
import collections
import sys

import capstone
import pefile


def survey(path):
    pe = pefile.PE(path)
    base = pe.OPTIONAL_HEADER.ImageBase
    exps = sorted((base + s.address, (s.name or b'#%d' % s.ordinal).decode())
                  for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if not s.forwarder) if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT') else []
    import bisect
    eaddr = [a for a, _ in exps]

    def owner(va):
        i = bisect.bisect_right(eaddr, va) - 1
        return exps[i][1] if i >= 0 else '?'
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    sites = collections.defaultdict(list)
    services = collections.Counter()
    prev = []
    # VxDCall0..8 (kernel32 ordinals 1-9): the export addresses, or the import slots' targets elsewhere
    vxd = {base + x.address for x in getattr(pe, 'DIRECTORY_ENTRY_EXPORT', None).symbols if x.name is None and x.ordinal <= 9}         if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT') else set()
    for s in pe.sections:
        if not s.Characteristics & 0x20000000:
            continue
        data, va = s.get_data(), base + s.VirtualAddress
        off = 0
        while off < len(data):
            insn = next(md.disasm(data[off:off + 16], va + off), None)
            if insn is None:
                off += 1
                continue
            m = insn.mnemonic
            if m == 'int' and insn.op_str in ('0x21', '0x2f', '0x31', '0x30', '0x20') or                     (m in ('call', 'jmp') and 'ptr cs:' in insn.op_str):
                sites['%s %s' % (m, insn.op_str)].append((insn.address, owner(insn.address)))
            if m == 'call' and insn.op_str.startswith('0x') and int(insn.op_str, 16) in vxd:
                svc = prev[-1].op_str if prev and prev[-1].mnemonic == 'push' else '?'
                services[svc] += 1
                sites['call VxDCall'].append((insn.address, owner(insn.address)))
            prev = (prev + [insn])[-4:]
            off += insn.size
    print('== %s  base %08X, %d exports' % (path, base, len(exps)))
    for k, v in sorted(sites.items(), key=lambda kv: -len(kv[1])):
        own = collections.Counter(o for _, o in v)
        print('  %-28s %4d  in %s' % (k, len(v), ', '.join('%s(%d)' % x for x in own.most_common(6))))
    for svc, n in sorted(services.items()):
        print('  VxD service %-12s %4d  (VxD id %s)' % (svc, n, ('0x%04X' % (int(svc, 16) >> 16)) if svc.startswith('0x') else '?'))



def thunks(path):
    """Flat-thunk stubs: `mov cl, N; push ebp; mov ebp, esp; push ecx; sub esp, 3Ch; <push args>; call [slot]`."""
    import re
    pe = pefile.PE(path)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    slots = collections.Counter()
    ids = collections.defaultdict(set)
    n = 0
    for m in re.finditer(rb'\xb1(.)\x55\x8b\xec\x51\x83\xec\x3c', img, re.S):
        tail = img[m.end():m.end() + 200]
        c = tail.find(b'\xff\x15')
        if c < 0:
            continue
        slot = int.from_bytes(tail[c + 2:c + 6], 'little')
        slots[slot] += 1
        ids[slot].add(m.group(1)[0])
        n += 1
    print('== %s: %d flat-thunk stubs' % (path, n))
    for s, k in slots.most_common():
        print('  call [%08X]  %4d stubs, thunk index %d..%d' % (s, k, min(ids[s]), max(ids[s])))


if __name__ == '__main__' and '--thunks' in sys.argv:
    for p in sys.argv[1:]:
        if p != '--thunks':
            thunks(p)

if __name__ == '__main__' and '--thunks' not in sys.argv:
    for p in sys.argv[1:]:
        survey(p)
