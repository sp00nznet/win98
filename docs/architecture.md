# How win98-recomp works

Windows 98 SE is not one program. It is a stack of 32-bit and 16-bit code that
was designed to run on top of a ring-0 kernel (VMM and its VxDs) on a real PC.
This project turns that stack into C, links it into **one Windows 11 process**,
and replaces only the bottom layer: the kernel and the hardware. Everything
above that line is Windows 98's own code, statically recompiled. Nothing is
emulated or interpreted, and no original instruction ever runs.

    ┌──────────────────────────────────────────────────────────────────────────┐
    │                         one host process (x86)                           │
    │                                                                          │
    │   Win98's own code, lifted to C by pcrecomp                              │
    │  ┌────────────────────────────────────────────────────────────────────┐  │
    │  │  32-bit (PE)    Explorer  calc  notepad  shell32  shdocvw  browseui │  │
    │  │                 comctl32  ole32  mpr  advapi32  user32  gdi32       │  │
    │  │                                  kernel32                           │  │
    │  ├───────────────────── flat thunks (QT_Thunk) ───────────────────────┤  │
    │  │  16-bit (NE)    USER.EXE   GDI.EXE   SHELL.DLL   MSGSRV32  sol/mine │  │
    │  │                            KRNL386.EXE                              │  │
    │  └────────────────────────────────────────────────────────────────────┘  │
    │        │ VxD calls            │ int 21h/2Fh/31h       │ driver calls      │
    │        ▼                      ▼                       ▼                   │
    │  ┌──────────────┐   ┌────────────────────┐   ┌──────────────────────┐    │
    │  │   vxd32.c    │   │  identity.c        │   │  drv16.c             │    │
    │  │  VMM, VWIN32,│   │  DOS/DPMI/BIOS     │   │  DISPLAY, KEYBOARD,  │    │
    │  │  VCOND, REG… │   │  answers           │   │  MOUSE, SYSTEM, SOUND│    │
    │  └──────┬───────┘   └─────────┬──────────┘   └──────────┬───────────┘    │
    │         └─────── pcrecomp runtime (what used to be ring 0) ┘              │
    └─────────┬─────────────────────┬──────────────────────────┬──────────────┘
              ▼                     ▼                          ▼
       host memory/threads     sandbox file root         a host window
       (VirtualAlloc, events)  (work/root = C:\)         (DIB framebuffer)

## 1. Static recompilation, in one paragraph

pcrecomp's disassembler walks each Win98 binary from its entry points and
exports, finds every function, and the lifter writes each one out as a C
function that operates on a CPU-state struct (`eax`, `ebx`, flags, segment
bases…). Memory reads and writes become reads and writes of the guest's flat
address space, which lives at the same addresses Win98 used. Calls between
functions become C calls; indirect calls go through a dispatch table that maps
a guest address to its lifted function. Each lifted module is built as its own
host DLL at a fixed base, all loaded into the one process.

        Win98 binary (bytes)                         host
     ┌────────────────────────┐    disassemble   ┌────────────────────────┐
     │ 55 8B EC 83 EC 0C ...  │ ───────────────▶ │ void f_BFF7123A(cpu*c) │
     │ (kernel32 .text)       │    + lift        │ { push32(c,c->ebp);    │
     └────────────────────────┘                  │   c->ebp = c->esp; ... │
                                                 └────────────────────────┘
     guest memory keeps the original bytes as DATA ONLY: it is mapped
     read/write, never executable. A jump into unlifted code faults instead
     of running.

## 2. Two worlds in one address space

Win98 is a hybrid. user32 and gdi32 are thin 32-bit layers that hand most of
their work to the 16-bit USER.EXE and GDI.EXE, through **flat thunks**.
user32 also reads USER's 16-bit data segment directly, through flat pointers.
So the 16-bit segments must live at real flat addresses in the same process
as the 32-bit code. pcrecomp's **identity mode** does that:

      flat address space (one process)
      0x00010000 ┌──────────────────────┐  real-mode megabyte: PSPs, BIOS data
                 │ low memory           │  area, KRNL386 loaded "high" as DOS
                 ├──────────────────────┤  would load it
                 │ 16-bit segments      │  each has an LDT descriptor whose
                 │ (KRNL386, USER, GDI) │  base is its flat address
                 ├──────────────────────┤
      0x00400000 │ calc / xcopy32 …     │  32-bit programs
                 │ shell32, ole32 …     │  32-bit DLLs at Win98's own bases
      0x80000000 │ shared arena         │  shared heaps (comctl32's), as on 9x
      0xBFF20000 │ gdi32 user32 kernel32│  Win98's system DLLs, unrelocated
                 └──────────────────────┘

      the LDT is owned by the host. KRNL386 and kernel32 write their own
      descriptors into it, exactly as on Win98, and lifted 16-bit code
      resolves seg:off through it:

        16-bit code ── mov ax,[es:bx] ──▶ es=0x1477 ─▶ LDT[0x1477].base
                                                          + bx = flat addr

## 3. Crossing between 32 and 16 bits

Every call from 32-bit into 16-bit code goes through one place, kernel32's
**QT_Thunk**: take the Win16Mutex, switch to the thread's 16-bit stack, far
call the 16-bit target. On real Win98 that's a few instructions of
segment-register juggling. Here the lifted QT_Thunk runs as-is, and the
runtime makes the far call land in the lifted 16-bit function.

     user32!GetSystemMetrics(1)               (32-bit, lifted)
        │  push args as words; call [slot]
        ▼
     slot stub ──▶ kernel32 QT_Thunk          (32-bit, lifted)
        │  take Win16Mutex, ss:sp = thread's 16-bit stack
        │  "retf" into 16:16 target  ───────────────┐
        ▼                                           │ runtime: w16_enter
     USER.EXE GETSYSTEMMETRICS                 (16-bit, lifted, identity mode)
        │  reads USER's DGROUP through LDT
        │  retf ─────────────────────────────────────┘ back to the 32-bit frame
        ▼
     kernel32 QT_Thunk tail ─▶ release mutex ─▶ return to user32

     the other direction (16 → 32), for window procedures and callbacks:

     USER.EXE (16-bit) ── SendMessage ──▶ KRNL386 relay ──▶ kernel32 stub
          ──▶ Explorer's 32-bit WndProc ──▶ … returns through the relay

Some thunk stubs are rewritten at run time (kernel32 ordinal 90 patches them
on first use). Those bytes are never run either: the runtime recognises the
patched stub by its byte pattern and does what it would do.

## 4. Where the kernel used to be

Win98's kernel32 leaves the process in exactly one way: the VMM call gate
(`VxDCall`), with a service ID such as `0x002A0010` (VWIN32 Int21Dispatch).
The 16-bit side leaves through interrupts (int 21h DOS, int 2Fh multiplex,
int 31h DPMI) and through far calls into drivers. pcrecomp answers all three:

     kernel32 ── VxDCall(service, args) ───▶ vxd32.c
                                              ├─ VMM    _PageReserve/Commit → VirtualAlloc
                                              │         _HeapAllocate        → host heap
                                              │         registry 100h+       → regbox32
                                              ├─ VWIN32 threads, events, mutexes,
                                              │         semaphores, waits    → host threads/events
                                              │         Int21Dispatch        → file I/O in work/root
                                              ├─ VCOND  console              → stdout/stdin
                                              └─ anything unknown            → STOP with its id
     KRNL386 ── int 21h / 2Fh / 31h ───────▶ identity.c (DOS, DPMI, BIOS, VxD-entry lookup)
     GDI.EXE ── far call DISPLAY.Output ───▶ drv16.c (stand-in drivers)

Every service is named from how Win98 calls it, never guessed from a
DDK list: an unknown ID stops the run and prints itself, so nothing is
silently wrong. Guesses that had to be made are marked `ponytail:` in the
source and listed in survey.md.

## 5. Booting

Real Win98 boots DOS, then VMM, then the 16-bit kernel, then kernel32. Here
the host plays DOS and VMM, and lets Win98's own code do the rest in its own
order:

     host (k32host / w16test)
       │ 1. reserve low memory, build an LDT, a PSP and an environment
       │ 2. load KRNL386 "high", as DOS would, and enter BootStrap
       ▼
     KRNL386 BootStrap (16-bit, Win98's own)
       │ GlobalInit: builds its heap and selector tables
       │ builds kernel32's boot block and the first thread's TIB
       │ calls kernel32's boot entry (0xBFFC7AE4)
       ▼
     kernel32 _INIT (32-bit, Win98's own)
       │ first process, first thread, NLS, registry
       ▼
     KRNL386 SlowBoot
       │ reads system.ini [boot], LoadModule:
       │   SYSTEM.DRV KEYBOARD.DRV MOUSE.DRV DISPLAY.DRV SOUND.DRV ← stand-ins
       │   GDI.EXE  (→ gdi32.dll)   USER.EXE (→ user32.dll, mpr.dll)
       ▼
     USER's LibEntry paints the desktop; KRNL386 starts MSGSRV32
       │ MSGSRV32 starts the shell from system.ini (shell=Explorer.exe)
       ▼
     Explorer: desktop, taskbar, Start menu

The stand-in drivers are small NE files that pcrecomp generates at start-up
(no Microsoft bytes) whose exports land in host functions. KRNL386 loads them
through its own LoadModule, like any driver.

## 6. Drawing and input

     GDI.EXE ──▶ DISPLAY.DRV stand-in ──▶ 800×600×16 DIB framebuffer ──▶ host window
                 Enable/BitBlt/Output/          (drv16.c draws into it)   (W16_WINDOW=1)
                 ExtTextOut/RealizeObject…

     host input thread ──▶ MOUSE/KEYBOARD stand-in event procs ──▶ USER's queue
       (scripted with W16_INPUT, or from the window)

     host tick thread  ──▶ USER's time page (VTDAPI) + SYSTEM.DRV timers

Fonts are Win98's own raster fonts; Marlett (the caption buttons) is loaded
privately into the process from Win98's marlett.ttf.

## 7. Threads and the Win16 lock

Each guest thread runs on its own host thread. A single "machine lock" stands
in for Win98's Win16Mutex plus the CPU: whoever holds it runs guest code.
Waits (events, messages, Sleep) release it, and so does KERNEL.447, the spot
where Win98 itself briefly gives up the Win16 lock inside modal loops. That's
what lets a held mouse button be released while USER tracks a caption button.

     thread A (Explorer tray) ──── holds machine ─── waits ──▶ releases
     thread B (calc)          ─ wait ─────────────── acquires ─ runs ─▶
     input thread             ─ wait ─────────────── acquires ─ posts click ─▶

## 8. The walls

     ┌──────────────────────── guest ────────────────────────┐
     │ registry  ─▶ regbox32: HKCU\Software\pcrecomp\w98\…   │ seeded from Win98's INFs;
     │                        Win98-owned keys are "whole"   │ no host values leak in
     │ files     ─▶ every path boxed under work/root (C:\)   │
     │ code      ─▶ guest memory never executable            │
     │ devices   ─▶ none: no sound, network, printer, ports  │
     │ shell     ─▶ its own taskbar, never the host's        │
     │ errors    ─▶ hard-error boxes logged, never shown     │
     └───────────────────────────────────────────────────────┘

## 9. What lives where

| Piece | Where |
|---|---|
| Disassembler, lifter, NE/PE tools | pcrecomp `tools/` |
| VxD answers (VMM, VWIN32, VCOND, registry) | pcrecomp `runtime/native32/vxd32.c` |
| 16-bit identity runtime (LDT, thunks, DOS/DPMI) | pcrecomp `runtime/win16/identity/identity.c` |
| Stand-in drivers (DISPLAY, KEYBOARD, MOUSE, SYSTEM, SOUND, DIBENG) | pcrecomp `runtime/win16/identity/drv16.c` |
| Registry box | pcrecomp `runtime/native32/regbox32.c` |
| Host programs (xcopy runner, boot host) | this repo, `src/k32host.c` |
| Registry seeding, Start menu, seeds | this repo, `tools/reginst.py`, `startmenu.py`, `w16_seeds.py`, `vtseeds.py` |
| Gates | this repo, `tools/check_*.sh` |
| The long story, session by session | `docs/survey.md` |
