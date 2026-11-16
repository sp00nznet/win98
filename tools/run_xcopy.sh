#!/bin/sh
# Build and run xcopy32 on Windows 98's own lifted kernel32 (milestone 1).
# Relift first when the lifter or the modules change:
#   py -3 tools/lift.py xcopy work/sys/xcopy32.mod work/sys/kernel32.dll
#
#   sh tools/run_xcopy.sh [host options] [-- xcopy args]
#   sh tools/run_xcopy.sh -- /?
#   VXD=1 sh tools/run_xcopy.sh --calltrace work/ct.txt -- /?   # trace every VxD service
#
# Muted, headless, a 10 s watchdog. The guest's C:\ is work/root (vxd32
# boxes every path under it). The full log is work/run.txt.
cd "$(dirname "$0")/.." || exit 1
cmd //d //c "$(cygpath -w "$PWD")\\build.cmd" --target xcopy 2>&1 | grep -E "error C|FAILED"
[ -n "$VXD" ] && export NATIVE32_VXD=1
PCRECOMP_MUTE=1 timeout 30 build/xcopy.exe --mute --watchdog 10 "$@" > work/run.txt 2>&1
rc=$?
grep -v '^\[native32\]\|^\[bind\]' work/run.txt | tail -${TAIL:-30}
echo "rc=$rc"
