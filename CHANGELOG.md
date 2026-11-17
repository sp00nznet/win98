# Changelog

## Unreleased

- Session 37: GDI's 32-bit (use32) code segments are now decoded as 32-bit code (pcrecomp edb2030), which fixes the crash when My Computer is double-clicked. The folder window opens with a taskbar button but does not paint yet. Added tools/check_mycomp.sh and a status summary at the top of docs/survey.md.
- Session 30: the host is now a small exe plus one DLL per lifted module
  (pcrecomp's pcrecomp_gen_parts); the single image of the whole shell lift
  no longer started. browseui is lifted. Explorer's tray window gets its
  messages through KRNL386's window-proc relay, and DIB sections work through
  a DIBENG stand-in, so comctl32's image lists and the tray's child windows
  are made and browseui's bands are created. Explorer still stops before its
  first paint (an ole32 call returns with a broken stack).
- Session 29: Explorer (W16_SHELL=Explorer.exe) runs its own lifted code on
  Win98's lifted shell32, shdocvw and ole32 up to building the taskbar's
  Start button; no desktop or taskbar paint yet. tools/reginst.py fills the
  registry box from Win98's own INFs (shell.inf, ieshell.inf, timezone.inf,
  and the REGINST resources of shell32/shdocvw/browseui), plus
  HKCC\Display\Settings for the stand-in display. tools/vtseeds.py adds seeds
  for vtable methods. USER's appearance keys are now marked whole in the box,
  so Windows 11's colours and metrics no longer leak through: windows have
  Win98's grey and the desktop is teal. Because USER now uses its own metrics,
  the calc and notepad gates search ±20 px for their reference. The winmine
  gate compares the clicked cell with its own earlier image, not with its
  neighbour, because flood fill could uncover both.
  A Win32 task's TDB+54 holds its thread from creation.

- Milestone 2, session 28: Notepad runs as the shell and types text
  (tools/check_notepad.sh). The fault was a far call made as `push cs; call
  near` whose callee returned into an address it had pushed: the lifter now
  checks for that case and runs the code the callee returned into. Timers
  work: once KRNL386 had chained every free LDT descriptor, the host's later
  selectors wrapped onto entry 0, so USER read the real-mode vector table as
  its tick count. The host now keeps its own pool of 256 descriptors.
  Solitaire's clock runs, its deal is seeded from the clock, and carets
  blink. MakeProcInstance thunks are recognised. hiddenrun.py now ends the
  processes it starts (taskkill /T timed out on them).
- Milestone 2, session 27: Win16 tasks wake on input. A task switch back to
  the boot thread used to start a fresh guest thread instead of waking it, and
  KRNL386's idle (int 2Fh 1689h, VWIN32 10h/11h) was missing. Solitaire deals
  on a click and a card drags to a foundation (tools/check_sol.sh). Minesweeper
  runs as the shell: a SOUND stand-in (no device), DISPLAY DIBtoDevice, and a
  seed for its window procedure; a click uncovers a cell and starts the timer
  (tools/check_winmine.sh). Notepad, rebased to 0x01100000, is lifted into
  w16test and its code runs; it faults in a nested 16/32 thunk before its
  window shows.
- Milestone 2, session 25: VTDAPI 9 now returns a real system-time selector
  (USER's DGROUP:0 tick pointer; every event time was 0). SYSTEM.DRV timers
  run from a host tick thread, and USER's static SYSTEM/KEYBOARD imports go to
  the stand-ins. W16_INPUT clicks (move, then press, then release) and keys
  reach calc: 12+3= shows 15. by mouse and by keyboard. check_m1's flake was
  the shared USER32 proxy in work/root, which is now rewritten only when its
  bytes change.
- Milestone 2, session 23: calc's process init failed because the root had
  setup's own commctrl.dll and shell.dll; Win98 installs KOMMCTRL.DLL and
  SHELL.NEW under those names (COMCTL32's and SHELL32's ThunkConnect32
  partners). With them, calc's DLLs initialize and its window procedure runs
  from 16-bit USER (FT_Prolog thunk stubs, the run-time wndproc thunk, and
  three code-map/jump-table fixes). A dying thread gives up the Win16Mutex.
  Stops in USER seg 52 0F49 after calc's first message.
- Milestone 2, session 21: a dying thread's teardown runs to the end (it frees
  its own fs; the reload keeps the cached base) and VWIN32 now runs kernel32's
  thread-termination routine (DLL detach, USER signal 20h) first; KRNL386's
  16-bit registry goes through regbox32. Still stops in MSGSRV32's
  BroadcastSystemMessage.
- Milestone 2, session 19: SHELL.DLL's LibMain returns (ne_lift resolves
  __WINFLAGS; LibEntry took the InitTask path); MSGSRV32 calls WNetInitialize
  through USER's flat thunk into lifted MPR (second stub form, lea without a
  segment base, ring-0 mutexes, int 21h 50h). Stops on an uninitialized
  critical section in MPR.
- Milestone 2, session 17: guest threads start on their own stack selector
  (the stray USER thread's Win16 stack was built from a 16-bit esp);
  VWIN32 Event Wait/Set/Destroy, VMM HeapReAllocate, int 21h 50h/55h for
  the task PSP; USER's flat-thunk targets seeded. MSGSRV32's task thread
  runs into KRNL386's Reschedule and loops (CurTDB never moves).
- Milestone 2, session 16: Win98's MSGSRV32.EXE (16-bit) lifted and loaded by
  KRNL386's LoadModule; BootStrap runs into Reschedule and makes the first
  task switch (VWIN32 01) to MSGSRV32's thread (pcrecomp: ret-jump calls,
  iret ret_ip, push cs:imm; jmp as a far call, int 21h 41/55h). Stops in a
  USER-made thread whose start runs on a 16-bit stack offset.
- Milestone 2, session 14: user32's LoadLibrary16("USER.EXE") succeeds
  (USER's DllEntryPoint ThunkConnect16s to MPR.DLL, now lifted into
  w16test; pcrecomp: retf-into-pushed-far-call, 67h [bx] in 32-bit code,
  any DLL's flat-thunk stub, o16-retf returns from flat code); USER.EXE's
  LibEntry runs on into the first window paint (DISPLAY ExtTextOut/BitBlt)
  and stops in GDI SelectVisRgn's Local32 lookup.
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
