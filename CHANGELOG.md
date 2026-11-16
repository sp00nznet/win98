# Changelog

## Unreleased

- Milestone 2, session 13: GDI's Local32 heap works (pcrecomp lift16: a
  67h [disp32] past 64K stays 32-bit); DISPLAY.DRV stand-in gains BitBlt,
  Output, Pixel, ExtTextOut, DeviceBitmapBits and the cursor entries, and
  a host window (W16_WINDOW=1); USER.EXE's LibEntry gets past the keyboard,
  cursor and event setup and loads Win98's own lifted USER32.DLL.
- Milestone 2, session 12: KRNL386's parameter-error returns unwind
  correctly (pcrecomp w16_farcall/w16_jmpf); USER.EXE's LibEntry loads
  DISPLAY.DRV, a host stand-in (800x600x16 framebuffer): Enable, Control,
  ColorInfo, RealizeObject.
- Milestone 2, sessions 10-11: KRNL386's SlowBoot loads every boot driver
  (host stand-ins SYSTEM, KEYBOARD, MOUSE, MMSOUND, COMM), GDI.EXE (whose
  LibEntry loads lifted GDI32.DLL through kernel32, and its raster fonts),
  DDEML.DLL and USER.EXE through its own LoadModule; USER.EXE's LibEntry runs.
- Milestone 2, session 9: with W16_LIBMAIN=KERNEL, KRNL386's own BootStrap
  boots kernel32 (its own boot block and TIB), KRNL386's run-time generated
  16->32 thunks run kernel32's real dispatcher, and SlowBoot reaches the
  load of SYSTEM.DRV.
- Stage 1 survey tool (tools/survey.py): VxDCall sites and service ids, flat-thunk stubs.
- Stage 2: Windows 98 calc and notepad lifted on native32 (host DLLs).
- Launcher and host set SetErrorMode so no loader box reaches the real desktop.
- Milestone 1, session 2: Windows 98 kernel32 boots to the end (first thread,
  its service thread on a second host thread, NLS tables, registry through
  regbox32) and starts xcopy32 through its own CreateProcessA; guest threads
  run as host threads under the machine lock. The guest's C:\ is a sandbox
  (work/root), with a Windows 98 environment and a host-made USER32 proxy.
- tools/run_xcopy.sh (was the ignored work/go.sh), tools/le.py (VxD reader).
- Milestone 1, session 3: `xcopy32 /?` prints Windows 98's own help text
  (string resources 4 and 27, exact) on the host's stdout, and
  `xcopy32 C:\TEST\SRC C:\TEST\DST /S /I` copies a small tree inside the
  sandbox byte-identically; the process ends through kernel32's own
  ExitProcess (code 0). The console object comes from kernel32's own
  constructor (what CONAGENT calls on Win98); VCOND 1 (teletype text) goes
  to stdout. Int21 3Ch/3Dh, 1Ah/2Fh/4Eh/4Fh (8.3 find into the DTA), 69h,
  71A7h. Guest hard errors (MessageBoxA) are answered in vxd32 and logged.
  tools/check_m1.sh checks both; run_xcopy.sh keeps `/S` from MSYS path
  conversion.
- Console input (session 4): VCOND F/10 on stdin; `xcopy32 /P` takes
  scripted answers (tools/check_m1.sh checks y/n/y).
- Milestone 2 design in docs/survey.md: Win16 identity mode (16-bit
  selectors in native32's LDT, cpu->mem 0), the crossings, who loads the
  16-bit modules.
- Milestone 2, chunk 1, first half: KRNL386 + GDI + USER lifted by ne_link
  in identity mode (CMake target w16); build/w16test.exe calls lifted
  kernel32's QT_Thunk into lifted USER.EXE's GetSystemMetrics and back
  (tools/check_w16.sh).
