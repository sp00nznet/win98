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
| 3 | console program on Win98's own kernel32 + vwin32 shim | milestone 1: `xcopy32 /?` prints its own help (exactly its string resources) and `xcopy32 C:\TEST\SRC C:\TEST\DST /S /I` copies a tree in the sandbox, byte-identical |
| 4 | GUI on lifted user32/gdi32 + USER.EXE/GDI.EXE via recomp16 | design written (docs/survey.md); lifted kernel32's QT_Thunk calls lifted USER.EXE (GetSystemMetrics) and comes back, in one address space (`tools/check_w16.sh`) |

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

xcopy32 on Win98's own lifted kernel32 (milestone 1):

```
py -3 tools\lift.py xcopy work\sys\xcopy32.mod work\sys\kernel32.dll
sh tools/run_xcopy.sh -- /?
sh tools/check_m1.sh        # /? against the string resources, then a /S /I copy
```

Milestone 2, chunk 1: the 16-bit modules in Win16 identity mode, called
through kernel32's own QT_Thunk:

```
set NE_IDENTITY_SEL=1
py -3 tools\w16_seeds.py work\sys > work\seeds\w16.json
py -3 ..\pcrecomp-w98\tools\ne\ne_link.py --work work\w16 --seeds work\seeds\w16.json --shims ..\pcrecomp-w98\runtime\win16\identity work\sys\KRNL386.EXE work\sys\GDI.EXE work\sys\USER.EXE
sh tools/check_w16.sh
```

The guest's C:\ is `work/root`. Its console is the host's stdout, and its
hard errors (kernel32's USER32!MessageBoxA) are logged, never shown.
