#!/bin/sh
# Milestone 1 check: `xcopy32 /?` prints xcopy32.mod's own strings (4 + 27,
# then xcopy's CRLF), and a /S /I copy inside the sandbox is byte-identical.
#   sh tools/check_m1.sh        (after tools/lift.py xcopy ...)
cd "$(dirname "$0")/.." || exit 1
sh tools/run_xcopy.sh -- /? > /dev/null 2>&1
grep -av '^\[' work/run.txt | tail -n +2 > work/help.txt
py -3 - <<'E' || exit 1
import pefile, struct, sys
p = pefile.PE('work/sys/xcopy32.mod'); S = {}
for t in p.DIRECTORY_ENTRY_RESOURCE.entries:
    if t.id != 6: continue
    for n in t.directory.entries:
        for l in n.directory.entries:
            d = l.data.struct; data = p.get_data(d.OffsetToData, d.Size); off = 0
            for i in range(16):
                ln = struct.unpack_from('<H', data, off)[0]; off += 2
                if ln: S[(n.id - 1) * 16 + i] = data[off:off + 2 * ln].decode('utf-16le')
                off += 2 * ln
want = (S[4] + S[27]).replace('%n', '\n') + '\n'
got = open('work/help.txt', encoding='cp437', newline='').read().replace('\r\n', '\n')
print('/? matches the string resources' if got == want else '/? DIFFERS'); sys.exit(got != want)
E
R=work/root/TEST
rm -rf $R && mkdir -p $R/SRC/SUB && printf 'hello win98\r\n' > $R/SRC/A.TXT &&
  head -c 70000 /dev/urandom > $R/SRC/B.BIN && printf 'nested\r\n' > $R/SRC/SUB/C.TXT
sh tools/run_xcopy.sh -- 'C:\TEST\SRC' 'C:\TEST\DST' /S /I | grep "copied"
diff -r $R/SRC $R/DST && echo "copy identical"
