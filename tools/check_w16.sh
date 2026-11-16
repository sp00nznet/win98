#!/bin/sh
# Milestone 2, chunk 1: lifted kernel32's QT_Thunk into lifted USER.EXE.
#   py -3 tools/w16_seeds.py work/sys > work/seeds/w16.json
#   NE_IDENTITY_SEL=1 py -3 ../pcrecomp-w98/tools/ne/ne_link.py --work work/w16 --seeds work/seeds/w16.json \
#       work/sys/KRNL386.EXE work/sys/GDI.EXE work/sys/USER.EXE
#   sh tools/check_w16.sh            (W16_TRACE=1 logs every 32->16 entry)
cd "$(dirname "$0")/.." || exit 1
cmd //d //c "$(cygpath -w "$PWD")\build.cmd" --target w16test 2>&1 | grep -E "error C|FAILED"
PCRECOMP_MUTE=1 timeout 30 build/w16test.exe --mute --watchdog 10 > work/w16run.txt 2>&1
echo "rc=$?"
grep -a '^USER\|^\[w16\]' work/w16run.txt
grep -aq '^USER.GetSystemMetrics(0) = 640' work/w16run.txt && grep -aq '^USER.GetSystemMetrics(1) = 480' work/w16run.txt &&
  echo "QT_Thunk -> lifted USER.EXE -> back: 16-bit code reads the bytes the 32-bit side wrote"
