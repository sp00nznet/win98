# win98-recomp

Windows 98 SE's own userland, statically recompiled with
[pcrecomp](https://github.com/sp00nznet/pcrecomp) (branch `w98`) and run on
a modern Windows host, with no emulator. Sister of win2000-recomp.

**This repository contains no Windows code or data.** Everything from your
copy (extracted CAB files, catalogs, lifted C) goes into the gitignored
`work/` and `src/gen/`.

## Status

| Stage | What | State |
|---|---|---|
| 1 | Survey of the kernel boundary (`tools/survey.py`) | VxDCall gate, flat-thunk tables, 16-bit int 21h/2Fh/31h mapped |
| 2 | PE accessories on native32 against host DLLs | calc and notepad run (winmine/sol/freecell are Win16 on 98) |
| 3 | console program on Win98's own kernel32 + vwin32 shim | not started |
| 4 | GUI on lifted user32/gdi32 + USER.EXE/GDI.EXE via recomp16 | not started |

## Building

Visual Studio 2022 (x86 tools), CMake, Ninja, Python 3 with `capstone` and
`pefile`, 7-Zip, pcrecomp at `../pcrecomp-w98`.

```
7z x -owork\iso <your Win98 SE .iso>
for %c in (work\iso\WIN98\*.CAB) do 7z e -y -owork\sys %c
py -3 tools\lift.py calc work\sys\calc.exe
py -3 tools\lift.py notepad work\sys\notepad.exe
build.cmd
set PCRECOMP_MUTE=1
py -3 tools\hiddenrun.py --secs 7 --shot 5 --out shots\calc -- build\calc.exe --mute
```

The guest's registry is boxed (pcrecomp regbox32) under
`HKCU\Software\pcrecomp\w98\<app>`.
