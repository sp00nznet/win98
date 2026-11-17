#!/usr/bin/env python3
"""Seed a module's catalog with its vtables' methods (and other code pointers in data).

    py -3 tools/vtseeds.py work/rebased/ole32.dll > work/seeds/ole32.dll.json

COM servers (ole32, shdocvw, ...) reach most of their code only through
vtables, which recursive descent never follows: a run found them one fault at
a time. Every relocated dword that points into the code section is taken as a
function, when it sits in a run of two or more such pointers (or outside the
code section), except runs that a `jmp [reg*4+table]` names (switch tables:
labels, not functions). An existing seed file's entries are kept.
A lone one counts when something points at its slot (a one-method vtable).
ponytail: other lone code pointers inside .text are skipped (could be immediates).
"""
import json, os, re, struct, sys
import pefile

p = pefile.PE(sys.argv[1])
b = p.OPTIONAL_HEADER.ImageBase
img = p.get_memory_mapped_image()
text = next(s for s in p.sections if s.Characteristics & 0x20000000)
lo, hi = b + text.VirtualAddress, b + text.VirtualAddress + text.Misc_VirtualSize
in_text = lambda va: lo <= va < hi
sites = sorted(r.rva for blk in p.DIRECTORY_ENTRY_BASERELOC for r in blk.entries if r.type == 3)
ptr = {s: struct.unpack_from('<I', img, s)[0] for s in sites}
code = {s for s, v in ptr.items() if in_text(v)}
# switch tables: FF 24 (85|8D|95|9D|AD|B5|BD) imm32
tables = set()
for m in re.finditer(rb'\xff\x24[\x85\x8d\x95\x9d\xad\xb5\xbd]', img[text.VirtualAddress:text.VirtualAddress + text.Misc_VirtualSize]):
    tables.add(struct.unpack_from('<I', img, text.VirtualAddress + m.start() + 3)[0] - b)
skip = set()
for t in tables:   # the whole run of a switch table
    s = t
    while s in code: skip.add(s); s += 4
# a one-slot vtable in .text (ole32 7F42A670, a destructor only) is a lone pointer,
# but its slot is named by a relocated `mov [ecx], 7F42A670`
named = set(ptr.values())
seeds = set()
for s in code - skip:
    if not in_text(b + s) or (s - 4) in code or (s + 4) in code or b + s in named:
        seeds.add(ptr[s])
out = sys.argv[2] if len(sys.argv) > 2 else None
old = set()
if out and os.path.exists(out): old = {int(x, 16) for x in json.load(open(out))}
res = json.dumps(['0x%08X' % v for v in sorted(seeds | old)])
(open(out, 'w').write(res) if out else print(res))
print('%d seeds' % len(seeds | old), file=sys.stderr)
