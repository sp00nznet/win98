# Windows 98 SE survey

Stage 1 survey of the kernel boundary. `tools/survey.py work/sys/kernel32.dll` (and `--thunks`) reproduces it.

## Load addresses
kernel32 0xBFF70000, user32 0xBFF50000, gdi32 0xBFF20000. All three carry relocations, so they can be rebased like Win2000's.

## kernel32
- Imports nothing. Its only exit is ordinals 1-9 (VxDCall0..8, one shared body): `mov eax,[esp+4]; pop [esp]; call fword cs:[0xBFFC9734]`, the VMM Win32 call gate.
- 521 VxDCall sites, plus 6 inline gate calls. 503 push a constant service ID, so the table is known up front.
- By VxD: VMM 192 (memory, selectors), VWIN32 72 (threads, events; 0x2A0010 = Int21Dispatch), VCOND 47 (console), VCOMM 25, REBOOT 17, DEBUG 2, unknown IDs 0x4B/0x4C 11, computed at run time 18.
- No int 21h/2Fh/31h of its own: DOS calls go through Int21Dispatch.
- 48 flat-thunk stubs down into KRNL386.
- On Win98, kernel32 is set up at boot by VMM and KRNL386, not by a loader, so its per-process structures have to be built by the host (as `native32_nt_boot` does for Win2000).

## user32 / gdi32 thunks
- No QT_Thunk call sites. Thunked exports (user32 114, gdi32 66) all look like `mov cl,N; push ebp; mov ebp,esp; push ecx; sub esp,3Ch; push 16-bit args as words; call [slot]`.
- The slot points at a stub in writable `.data`. On first call, kernel32 ordinal 90 rewrites it into `mov cl,[ebp-4]; mov edx,[table+ecx*4]; jmp QT_Thunk` (QT_Thunk = 0xBFF727D9).
- `table` is the dword before the stub, 0 on disk, next to `UsrThkConnectionDataLS` / `ThkConnectionDataLS`. USER.EXE and GDI.EXE export the matching `*THKCONNECTIONDATA*`.
- Each thunk is therefore a (module, index) pair naming a 16:16 entry in the 16-bit module. QT_Thunk is the single place 32-bit meets 16-bit.
- user32 also reads USER.EXE's data segment directly through a flat pointer (GetSystemMetrics, IsWindow), under the Win16 lock (kernel32 ordinals 97/98). So the lifted 16-bit segments must sit at flat addresses in the same process. pcrecomp's Win16 runtime keeps 16-bit memory in a separate `cpu->mem` buffer today, so this needs base 0 (identity) in a 32-bit host.

## 16-bit modules
| Module | int 21h | int 2Fh | int 31h (DPMI) | Imports |
|---|---|---|---|---|
| KRNL386.EXE | 81 | 15 | 38 | none |
| USER.EXE | 12 | 17 | 13 | KERNEL, GDI, SYSTEM, KEYBOARD, COMM, DDEML, MOUSE |
| GDI.EXE | 0 | 3 | 2 | KERNEL |

- int 2Fh is almost always AX=1684h (get a VxD entry point), a second, 16-bit VxD boundary: VDD, CONFIGMG, VMD, VWIN32, SHELL, VPOWERD, VTDAPI.
- GDI.EXE reaches the display driver through 199 indirect far calls. A DISPLAY.DRV stand-in that draws with the host plugs in there.

## Stage 2
calc (307 functions) and notepad (107) are lifted onto native32 against the host DLLs, with no gaps. winmine, sol, freecell and cards.dll are Win16 programs on Win98, so they go on pcrecomp's Win16 host.

## Next: milestone 1
Rebase and lift kernel32 with a console tool that needs only kernel32. Answer the gate at 0xBFFC9734 in a new pcrecomp vwin32 shim, keyed by service ID: VMM memory to VirtualAlloc, VWIN32 to host objects, VCOND to the host console, Int21Dispatch to the dos32 runtime's table. Answer guest hard errors locally.

## Milestone 1: progress and handoff

`build/xcopy.exe` (src/k32host.c) maps xcopy32.mod and Windows 98's own
kernel32 (lifted, at its fixed 0xBFF70000: kernel32 has no relocations, so it
is not rebased) and boots kernel32 the way VWIN32 does. kernel32's whole boot
now runs (first thread, its service thread on a second host thread, NLS
tables, registry) and returns; the host then starts xcopy32 through
kernel32's own `CreateProcessA`; xcopy32 runs to its own ExitProcess (milestone
1, "Session 3" below).

    py -3 tools/lift.py xcopy work/sys/xcopy32.mod work/sys/kernel32.dll
    sh tools/run_xcopy.sh [host options] [-- xcopy args]    # log: work/run.txt
    VXD=1 sh tools/run_xcopy.sh --calltrace work/ct.txt -- /?

`VXD=1` (NATIVE32_VXD=1) traces every service. An unknown service, Int21
function, ring-0 entry or KRNL386 thunk stops the run (exit 5) with its id
and arguments. A fault prints the lifted C file and line (DbgHelp).

### How kernel32 is booted (learnt from the code)

- `_FREQASM` (image+0x1000) starts with a **KEXP header**: `'KEXP'`, a
  pointer to the slot for VWIN32's ring-0 entry table (0xBFFC974A), 0xBFFC9CE0,
  the **boot entry 0xBFFC7AE4**, 0, 0xBFF994BC. The boot entry takes one
  argument, VWIN32's boot block. The real work is in `_INIT` (0xBFFC6000+).
- **Boot block** (0x9C bytes; +0 must be 0x9C). The fields known so far:
  +08 the PSP selector; +0C the VMM call gate (off16:sel16); +10 the LDT
  alias selector; +14 the LDT's flat address (MapSL reads descriptors
  there; low 3 bits 0); +18 a selector; +28/+2A words; +3C a 16:16 pointer to
  the KRNL386 handshake block; +44/+54/+58 16:16 pointers; +50 the selector of
  KRNL386's global arena (+54 -> the selector table's offset in it, +58 -> its
  limit); +60 a byte flag; +78 a size in KB; +7A a selector read as gs:[0].
  Everything else is a pointer to a VWIN32-side block, a zeroed page each.
- **KRNL386 handshake block** (0x54 bytes, built by KRNL386 at run time): +0
  0x54, +30 magic 0x35AA69, +34 = kernel32's dword at 0xBFFC9EC8; +10/+48/+4C
  16:16 data; +14..+28, +3C..+44, +50 KRNL386 entry points (16:16; the host
  tags them FEEE:<field> so a QT_Thunk down to one will name itself). kernel32
  writes its own entries back into +04/+08/+0C/+38: that happening is how to
  tell the handshake worked.
- The PSP selector's PSP:2C is the environment ("VAR=val\0..\0", word 1,
  program path) and PSP:80 the command tail. kernel32's own code copies them
  into the first process.
- The first process database (PDB): +1C the memory context (VMM 6), +24 the
  PSP's linear address, +28 the PSP selector, +21 bit 2 = registered with
  VWIN32 (4F/33h).

### Services answered (pcrecomp runtime/native32/vxd32.c)

| id | name (from use) | args | answer |
|---|---|---|---|
| 00010000 | VMM _PageReserve(page/PR_*, n, flags) | 3 | VirtualAlloc reserve |
| 00010001 | VMM _PageCommit(page, n, hpd, pagerdata, flags) (also its own stub at 0xBFF713E2) | 5 | VirtualAlloc commit |
| 00010002 | VMM _PageDecommit | 3 | VirtualFree decommit |
| 00010005 | VMM _HeapAllocate(n, flags) | 2 | private host heap |
| 00010006 | VMM context create (no args, result -> PDB+1C) | 0 | a cookie |
| 0001000B | VMM context switch(ctx) | 1 | no-op |
| 002A0003 | VWIN32 boot: kernel32's cs/ds | 8 | no-op |
| 002A0006 | VWIN32 register kernel32 callbacks (block, 2 entries) | 3 | kept |
| 002A0032 | VWIN32 release an object's ring-0 part | 1 | no-op |
| 002A0047 | VWIN32 environment moved | 1 | no-op |
| 002A004A | VWIN32 critical section's ring-0 half (crit, level) | 2 | a zeroed block |
| 002A004F | VWIN32 object fn: 33h register, 34h drop | 2 | success |
| ring-0 table +28 | allocate LDT selectors (ecx) -> ax | | host LDT counter |

A wrong guess, kept as a warning: I took VMM 6-8 to be _HeapFree/
_HeapReAllocate/_HeapGetSize (DDK order). Service 6 takes no arguments and
creates a memory context, and the wrong answer sent the boot down its
cleanup path quietly. Name a service from its call sites, never from the DDK
order.

### Toolkit pieces this needed (pcrecomp w98, `LIFT32_W9X=1`)

- `call fword [m]` (the VMM gate) -> `recomp_farcall32()`; the gate returns to
  VxDCall's caller and pops the service's arguments.
- `66 CB` (o16 retf after `push cs; push ret; push target`) -> a far jump:
  `recomp_retf32()` runs the host's ring-0 entries (tagged FEED:off), then
  comes back to the pushed return. The return points are seeded
  (`work/seeds/kernel32.dll.json`: the byte after each `xchg [esp],eax;
  66 CB`).
- A host-owned **LDT** (`g_ldt`): kernel32 writes descriptors itself through
  the alias selector; `recomp_sel_base`, `lsl`, `lar` read them back.
- **Segmented functions**: a function that loads ds/es (about 20 in
  kernel32: the DOS-environment copy, the PSP) addresses implicit-ds operands
  through DS_BASE and string ops through ES_BASE/DS_BASE. `mov es/ds/gs` ->
  `recomp_set_seg`. Every other function stays flat.

### Session 2 (pcrecomp w98 cd4d654+, win98 5184a15+): boot done, CreateProcess runs

**What runs.** `sh tools/run_xcopy.sh -- /?` boots kernel32 to the end of
`_INIT` (`[boot] kernel32 boot returned ...`), releases the Win16Mutex, calls
`CreateProcessA("C:\WINDOWS\COMMAND\XCOPY32.MOD /?")`, and the boot thread
waits (VWIN32 5D) while xcopy's main thread (start 0xBFF8B15D) runs on its own
host thread: kernel32's loader maps XCOPY32.MOD over native32's copy at
0x400000, loads the USER32 proxy at 0x30000000, signals USER, starts the
console agent (answered) and gets the new console's ring-0 object (VCOND 1F).
It then faults (exit 3) in 0xBFF8DFB2 reading `[PDB+40h]` of the new process
(0), from 0xBFFA2110. xcopy's own code has not run yet; no output.

**Next step.** Find who fills a child PDB's +40 (the parent's at 0xBFF8A08D
reads the current one; the boot process got it in `_INIT` at 0xBFFC6AC6 from
a 0xB4 block) - probably the parent after its 5D wait, i.e. the child ran
before the parent finished: check whether CreateProcess expects the child to
stay unscheduled until a resume (vxd32 starts a thread at VWIN32 A, "ready").
Then the console's VCOND services (38xxxx, the table below), on the host
console, and xcopy's file calls through Int21Dispatch, to milestone 1.

**Threads (the model).** Every guest thread is a host thread under native32's
machine lock; `native32_wait()` (new, native32.c) blocks one inside a host
answer: its registers kept, the lock released however deep, then both back.
vxd32 keeps a ring-0 twin per thread (thread, process, TDBX, entry, esp, wake
and parked events, the wait it is in) and switches VWIN32's "current" triple
(0xBFFC9CE0/4/8 -> {thread, process, TDBX}) with the thread. A created thread
(VWIN32 8) starts at its frame (0xBFF72170 pops ds es fs gs, popad, popfd;
its `ret` lands in C, and the frame's next word, 0xBFF868EC, is entered as a
jump). Blocking: 2D (Sleep), 5D (wait on wait blocks; woken by index), 1D/1E
(critical-section contention, with the hand-over of ownership done by 1E).

**KRNL386 and USER.** kernel32's 32->16 thunks all go through one slot
(`call [0xBFF9AD46]`); the host points it at vxd32's KRNL386 stand-in, keyed
by the stub's `mov cl, N`. USER32!UserSignalProc (0xBFFC95C0, called
unchecked) is a vxd32 stand-in too. xcopy32 imports USER32: the host writes a
proxy USER32.DLL into the sandbox, an export table only, each RVA wrapping to
the host user32's function (no code, nothing of Microsoft's).

**The sandbox.** Guest C:\ is work/root (`--root`); vxd32's `host_path` boxes
every path (".." resolved, then the prefix checked). The host lays out
WINDOWS, WINDOWS\SYSTEM (KERNEL32.DLL, the *.nls tables), WINDOWS\COMMAND
(XCOPY32.MOD), WINDOWS\TEMP from work/sys. The guest's environment is a Win98
one (PATH=C:\WINDOWS;C:\WINDOWS\COMMAND ...), never the host's. Registry calls
(VMM 11-1B) go through regbox32.

**Lessons.** (1) Argument counts: count the pushes back to the previous call
and subtract what that call pops. VWIN32 4F looked like 2 args (`push eax;
push 33h`) but the `push ebx; push ebx` before `call 0xBFF80E6E` (a `ret 4`)
are its last two: 4 args. The 8-byte leak only showed as esi = 0 one function
up. (2) `push L; ... ret` is a jump (generate.py, W9X), and the catalog may
make L its own function. (3) An inline gate call pushes its own continuation
(lift32: continue when the return address is the next instruction).
(4) VWIN32's Win32 services are not in vwin32.vxd's DDB (its Win32 table
pointer is 0): they live in VMM32.VXD, which is compressed. tools/le.py reads
a VxD's objects with fixups applied, for when that is unpacked.

| id | name (from use) | args | answer |
|---|---|---|---|
| 00010003/4 | VMM pager register (pd) / query (n, &pd) | 1/2 | host table; page-in run at commit |
| 00010001 | PageCommit: a pager's page-in runs per page, pagerdata + i | 5 | eager |
| 0001000A | PageFree (base, flags) | 2 | VirtualFree |
| 0001000D | PageModifyPermissions (page, n, and, or) | 4 | 20060000h |
| 00010010 | HeapFree (block, flags) | 2 | |
| 00010011-1B | registry: open, create, close, delete key, enum key/value, query/set value | 3,3,1,2,4,8,6,6 | regbox32 |
| 00010028 | (inline gate, process start) | 0 | 0 |
| 002A0001 | ring-0 file entries (10 out pointers) -> the pager's read | 10 | tagged host entries (mutex, paging open/read/close) |
| 002A0008 | new thread's twin (9 args, frame at a[7]) | 9 | r0 block |
| 002A0009 | thread starting (-1) | 1 | 0 |
| 002A000A | thread ready | 2 | host thread started |
| 002A000C | first thread: {thread, process, TDBX} triple, NLS block (+44), PSP selector | 1 | see vxd32 |
| 002A0010 | Int21Dispatch (eax, ecx; rest live) | 2 | host files, boxed; 0E 19 2A 2C 30 3E 3F 40 42 44(00,08,09) 57 59 EA 71(39 3A 3B 41 43 47 4E 4F 56 60 6C A0 A1) |
| 002A001D/1E | crst wait / wake (hand-over) | 1/1 | |
| 002A0020 | coprocessor (&type) | 1 | 3 |
| 002A0021 | thread priority (r0, prio) -> offset | 2 | 10h |
| 002A0026 | link thread twins | 2 | 0 |
| 002A002D | block (ms, alertable) | 2 | C0h / 102h |
| 002A0030 | destroy wait block (blk, 1) | 2 | 0 |
| 002A0036 | run thread until it blocks | 1 | |
| 002A005D | wait on wait blocks (n, objs, ms, all, alertable) | 5 | index / 102h / C0h |
| 002A0060 | (wait block, on a new file object) | 1 | 0 |
| 002A0064 | thread priorities (pairs, n) | 2 | 0 |
| 0038001F | VCOND new console's ring-0 object | 0 | zeroed block |
| 00380023 | VCOND init (6, &info, sysdir) | 3 | 0 |
| KRNL386 0D/12/13/2A/2D/30/32 | GetDriveType, GetSystemDirectory, GetWindowsDirectory, WinExec (conagent only), GetWinFlags, task name, new task | | vxd32 |

Not done (asked for): guest hard errors (none raised yet); Int21 3Ch/3Dh
(plain create/open) and 4Eh/4Fh (8.3 find) are not answered, only the LFN
71xx ones kernel32 has used so far.

### Session 3 (pcrecomp w98 aaf7dc0+, win98 cef2fe5+): milestone 1

**What runs.** `sh tools/run_xcopy.sh -- /?` prints xcopy's help on the
host's stdout, exactly string resources 4 + 27 of xcopy32.mod (and its final
CRLF), then `[boot] xcopy32.mod exited with 0`. `sh tools/run_xcopy.sh --
C:\TEST\SRC C:\TEST\DST /S /I` prints `A.TXT B.BIN SUB\C.TXT  3 File(s)
copied`, exit 0, and the copy is byte-identical (DOS times: 2 s steps).
`sh tools/check_m1.sh` checks both. No windows: the console is stdout.

**The PDB+40 fault was not the PDB.** 0xBFFA2100 (attach a process to its
console) gets the console *object*, VCOND 1F's result, and reads its +40.
On Win98 that object is built by kernel32 itself when CONAGENT calls back in
(16-bit entry 0xBFF941D9 -> 0xBFF8E9E1(info, word)): 470h bytes, crst at
+20, two buffers at +40/+44, +150 the VCOND handle (VCOND 2), +46C an event;
the constructor also sets the current process's +B0. vxd32's VCOND 1F now
runs that constructor on the calling thread (registers saved, +B0 put
back). So the scheduling question did not matter here: a thread still
starts when it is made ready (VWIN32 A), and nothing so far needed it to
wait for a resume.

**Threads, continued.** (1) VWIN32's current triple now follows the machine
(native32_on_enter, called on the outermost mach_enter): a back-edge
hand-over switched threads without vxd32 knowing, and kernel32 ran a thread
under another's TDBX. (2) A TDBX is {+0 thread, +4 process}. (3) Waits list
kernel32 objects or wait blocks; an object's +4 is its block, and 60/61/62
(SetEvent, PulseEvent, ResetEvent: 0xBFF92D66..88) set +4 of the block and
wake its waiters. That is how the host's WaitForSingleObject(process) ends.
(4) Thread ends: B (terminate: a call for the current thread, otherwise
queued and run at that thread's next wake), 2B (done: parked for good), D
(queue fn(arg) to the service thread: run now, on the caller).

**Win9x's low 64 KB.** A dying last thread frees its own context (+58 = 0)
and its process teardown then reads [0+38h] (0xBFF7D65E): Win9x maps the
first 64 KB readable. vxd32's first-chance handler emulates lifted loads
from there as 0 (stores dropped): mov/movzx/movsx forms only, logged under
VXD=1 (six per run). Ceiling: it would also hide a real null read.
The same handler commits reserved pages on first touch (thread stacks
grow that way: kernel32 commits the top two pages only).

| id | name (from use) | args | answer |
|---|---|---|---|
| 00010007 | VMM context destroy (PDB+1C) | 1 | 1 |
| 00010020 | (block's last hand-over to ring 0: mapped-file record, dying process) | 1 | 0 |
| 002A000B | terminate thread (r0, blk {thread, -, 0xBFF89302}) | 2 | call / queue |
| 002A000D | queue fn(arg) to a thread (the service thread) | 3 | run now |
| 002A002B | thread done (never returns) | 0 | parked |
| 002A0040 | VirtualQuery (r0, addr, &mbi, 1Ch) | 4 | host VirtualQuery |
| 002A0041 | value into a context (ExitProcess) | 2 | the value |
| 002A0058 | dying thread's +58 handed back | 1 | 0 |
| 002A005C | wait on one block/object (blk, ms, alertable) | 3 | as 5D |
| 002A0060/61/62 | SetEvent / PulseEvent / ResetEvent (blk) | 1 | +4, wake |
| 00380001 | VCOND teletype write (h, buf, n) | 3 | host stdout |
| 00380002 | VCOND register console (info, obj) -> handle | 2 | obj |
| 0038000A/0C/0D/0E/13/14/17/18/1B/1E | VCOND console state, agent posts, attach/detach | 1-3 | 0 |
| KRNL386 31/33/34/43/55 | delete task, new process's KRNL386 part, FreeSelector, process gone, thread selector gone | | 0 (33: 1) |
| Int21 1A 2F 3C 3D 4E 4F 69 71A7 | DTA, create/open, 8.3 find, volume serial (fixed 1998-0505, "NO NAME", FAT32), time conversion | | boxed host files |

**Hard errors.** kernel32 shows them with USER32!MessageBoxA, bound lazily
through its own table (0xBFFCB2C4; FatalAppExit, load/teardown failures).
The USER32 proxy's MessageBoxA is a vxd32 stand-in: `[hard error] "cap":
"text"` on stderr, answer OK/Cancel/Abort/No. Not triggered yet by xcopy.
VWIN32's own Int24-style errors cannot happen: Int21 failures come back
as codes.

**Next step.** Milestone 2 candidates: more console programs on the same
kernel32 (the VCOND input side, 03-09/24-2C, for `/P` and the overwrite
prompt: map onto stdin), then a GUI guest (stage 4). Open ends: VWIN32 D
runs on the caller, not the service thread; B to a thread that is not in a
w9x_wait waits for its next one; FreeSelector does not reuse selectors.

### Session 1's list (1 is done; 2 and 3 are open)


1. **VWIN32 0C** (at 0xBFFC69D4, `_INIT`): the first thread. Its argument is
   a struct on the stack with kernel32 entries (0xBFF99B91, 0xBFFC7F00,
   0xBFFC9E1C); VWIN32 fills [ebp-68h] and the word at [ebp-58h]. This is
   where the threading model starts: TDB/TIB (Win9x fs layout, fs:[1Ch]
   flags, fs:[1Eh] checked by MapSL), the thread's ring-0 twin, then running
   it. That is the next session's work. It needs native32's fs to point at a
   Win9x TIB.
2. The xcopy path after boot: GetStdHandle/WriteFile/FindFirstFile go
   through VWIN32 Int21Dispatch (2A0010) and VCOND (38xxxx). Map them to
   the host's console and file APIs (dos32's int 21h table is a model).
3. Guest hard errors: kernel32 raises them through VWIN32 too. Answer them in
   vxd32 (logged, never shown), as the coordinator asked.
4. Side quest, not done: winmine/sol/freecell (Win16) on runtime/win16. It
   is not cheap: each johnny-style title needs its own relift and a
   `runtime/game.c`.

## Milestone 2 design: lifted 32-bit and lifted 16-bit in one process

Goal: calc (or notepad) on Win98's own lifted user32 + gdi32 + kernel32,
with USER.EXE, GDI.EXE and KRNL386.EXE lifted through recomp16 and the
flat thunks bridged. Written before building (session 4).

### Why identity

user32 reads USER.EXE's data segment through flat pointers (GetSystemMetrics,
IsWindow), and QT_Thunk's 16-bit stack is an *alias* of the 32-bit one
(0xBFF72819: `sp = esp - base`, ss = the thread's stack selector from
TIB+7C, base -TIB+78). So a 16-bit address sel:off and a flat address must
name the same byte. pcrecomp's Win16 runtime computes every access as
`cpu->mem[cpu->sel_base[sel] + off]` (runtime/win16/cpu.h `seg_off`), so:

- **Base 0.** `cpu->mem = 0` in a 32-bit host: `mem[addr]` is the byte at
  flat `addr`. No copy, no second address space. (`RECOMP_W16_IDENTITY`.)
- **One descriptor table.** `seg_off` asks native32's LDT (`g_ldt`, the
  one kernel32 already writes through its alias selector and reads in
  MapSL/MapLS) for the selector's base instead of `cpu->sel_base[]`, so a
  selector kernel32 allocates is valid in 16-bit code at once and a 16-bit
  segment is valid in MapSL. lar/lsl read the same descriptors.
- **Real selectors.** ne_link numbers every segment of every module
  globally (SEG_1..SEG_N); in identity mode SEG_n is the selector
  `(n << 3) | 7` (LDT index n), and gen_image writes that value into
  relocated selector fields. The host places each segment at a flat
  address (VirtualAlloc, 64 KB each) and writes descriptor n for it before
  kernel32 boots; vxd32's LDT allocator starts above N.
- **Lifted 16-bit registers.** The Win16 runtime's `CPU` keeps its own
  register file. The bridge copies native32's g_eax..g_edi and the segment
  registers in and out at each crossing; nothing else is shared.

### The crossings

1. **32 -> 16 (QT_Thunk).** A user32/gdi32 thunk pushes word args and calls
   QT_Thunk with edx = the 16:16 target from `table[cl]`. QT_Thunk takes
   the Win16Mutex, switches ss:esp to the 16-bit alias, pushes the return
   `[0xBFFC979C]` (a 16:16 stub) and does `retf` to the target. The lifted
   `retf` (recomp_retf32, W9X) sees a 16-bit code selector: the bridge
   loads `CPU` (ss:sp from ss/esp, ds/es, ax..di), calls the lifted
   16-bit function from recomp16's dispatch table (sel:off), and when it
   returns (its `retf N` popped the return and the args) continues at the
   32-bit code that stub would jump to (0xBFF7286D: back to the 32-bit
   stack, release the mutex) with dx:ax as the result.
2. **Connection tables.** The dword before each patched stub (user32
   0xBFF512D6 / 0xBFF527B3, gdi32's) is filled by ThunkConnect32 with a
   flat pointer to the 16-bit module's `THKCONNECTIONDATALS` array of
   16:16 targets. Chunk 1 fills it from the host (the export's flat
   address, identity) instead of running ThunkConnect32/16; the real
   handshake comes when LoadLibrary16 does.
3. **16 -> 32** (USER calling user32 callbacks, window procs): later,
   through the `*SL` tables (QT_Thunk's mirror, FT_Thunk / the 16-bit
   `call far` into a 32-bit selector); the lifted 16-bit far call to a
   32-bit selector ends in native32's dispatch.
4. **16-bit system calls.** KRNL386's int 21h/2Fh/31h and USER/GDI's
   int 2Fh AX=1684h (VxD entry points) go to vxd32's existing tables:
   int 21h -> the Int21Dispatch handler, int 31h (DPMI) -> the LDT
   (allocate/set base/limit), 1684h -> tagged entries per VxD.
5. **DISPLAY.DRV.** GDI.EXE calls the driver through far pointers it
   reads from the driver's entry table (199 sites). A host module with
   DISPLAY's exports (Enable, BitBlt, Output, StrBlt, RealizeObject,
   ColorInfo, Control, ...) answers them on a 32 bpp DIB the host owns;
   the DIB is blitted to a host window on hiddenrun's private desktop and
   screenshot with PrintWindow.

### Who loads the 16-bit modules

On Win98 KRNL386 boots first (from WIN.COM/VMM), builds the global heap
and loads USER/GDI with its own NE loader. Lifted code, though, has its
segments at fixed places (gen_image). So the host does what VMM does for
kernel32: it maps the lifted image, writes the descriptors, and fills the
few KRNL386 structures the 32-bit side reads (the handshake block, the
global arena kernel32's boot block points at, module database entries for
USER/GDI so GetModuleHandle16 finds them), then runs USER's and GDI's own
LibMain. KRNL386's own boot code is not run in chunk 1.

### Chunks

1. A thunked user32 export that does not draw (GetKBCodePage, index 51 of
   0xBFF527B3, or GetMenuCheckMarkDimensions, 98 of 0xBFF512D6) returns
   USER.EXE's own answer through QT_Thunk and lifted 16-bit code.
2. Window creation (CreateWindowEx through USER's own window manager).
3. First paint through GDI.EXE and the DISPLAY stand-in; PrintWindow
   shot compared with stage 2's native32 calc (tools/gdicmp.py).

### Session 4 (pcrecomp w98 d89c3d8, faefa53): console input; milestone 2 chunk 1, first half

**Console input.** VCOND F (h) is the input mode (7: processed, line,
echo); VCOND 10 (h, buf, n, 1) reads one byte (kernel32 does the line
editing and echo, 0xBFFA5C5E). Both on the host's stdin; LF and CRLF are
Enter, EOF gives CR then ^Z. ReadFile on a console handle first waits on
the console object itself (VWIN32 5D with VCOND 2's object): vxd32 treats
that as ready. `xcopy /P` with `y n y` on stdin copies A.TXT and
SUB\C.TXT, not B.BIN (tools/check_m1.sh). Each answer prompts twice more,
for the CR and LF kernel32's line mode hands xcopy after the letter:
xcopy reads one byte at a time and re-asks on anything but Y/N. Whether
real Win98 does the same in a DOS box is unchecked. Win98's xcopy32 has
no overwrite prompt (no /-Y): it overwrites.

**Identity mode, built.** As designed above:
- `cpu.h` `RECOMP_W16_IDENTITY`: `seg_off`/lar/lsl read native32's LDT
  (`recomp_sel_base`, `recomp_lar32/lsl32`), `cpu->mem` is 0.
- `ne_link` with `NE_IDENTITY_SEL=1`: SEG_n = (n << 3) | 7 in segments.h
  and in every relocated selector of the image (gen_image `sel_value`).
  KRNL386 + GDI + USER: 85 segments (KERNEL 1-4, GDI 5-51, USER 52-85),
  6000 code-map functions, 18 unresolved stubs, 65 imports of the five
  modules not lifted (COMM, DDEML, KEYBOARD, MOUSE, SYSTEM) as stubs.
  The whole tree builds with MSVC x86 (`w16.lib`, CMake target w16).
- `runtime/win16/identity.c`: dispatch (far: selector >> 3), the trace
  ring, fatal stops, and `w16_enter` (the 32 -> 16 bridge).
- vxd32: `native32_w9x_set_sel` (fixed descriptors, allocator moves past);
  `recomp_retf32` hands an o16 retf into an LDT code selector to
  `native32_w16_enter`; `recomp_set_seg(2)` tracks ss (`g_ss_base`).
- lift32 (W9X): a function that loads ss addresses esp/ebp operands and
  the retf's target through `SS_BASE` (QT_Thunk runs on the 16-bit alias
  between `mov ss, edx` and `mov ss, edi`; the read of [ebp-20h] in its
  tail is ss-relative). generate.py sets it too (it builds Lifter state
  itself, so a flag set only in lift_function never reached kernel32).

**Chunk 1, first half: runs.** `sh tools/check_w16.sh`: after kernel32's
boot, build/w16test.exe calls kernel32's own lifted QT_Thunk
(0xBFF727D9) with edx = USER.GETSYSTEMMETRICS (global seg 55:0FDD), a
user32-style frame and the index pushed as a word. QT_Thunk takes the
Win16Mutex, switches to the 16-bit stack (the task-thread path,
0xBFF728BA: ss = TIB+0E), retf's into lifted USER.EXE, which reads its
metrics table (DGROUP:02D8) and `retf 2`s to the return stub; kernel32's
tail rebuilds the flat stack and returns. Five calls in a row. USER's
LibMain has not run, so the table is zeros: the test plants 640/480 in
SM_CXSCREEN/SM_CYSCREEN from the 32-bit side and gets them back.

Learnt: (1) QT_Thunk has two paths: a Win32 thread (TIB+1C bit 0) uses
TIB+78/7C and returns through [0xBFFC979C]; the KRNL386 task's thread
(the boot thread) uses TIB+0E and [0xBFFC97A4]. Both returns come from
KRNL386's handshake block (+1C, +24): 16-bit stubs that jump far into
kernel32 (0xBFF7286D, 0xBFF72913). The host builds them (66 EA off32
sel16) since KRNL386's boot does not run. (2) The host sets TIB+0E to a
selector over the boot thread's stack (ponytail: one, 48 KB below; the
TIB+78 path builds its own). (3) A thunk target's DS: USER's exports
expect ds = DGROUP, which its thunk dispatchers set (USR seg 32:0048,
`mov ax, DGROUP; mov ds, ax; jmp cs:[di+0Ah]`); the test sets it.

**Next (chunk 1, second half).**
1. USER's thunk targets are not in the code map: the dispatcher's
   `jmp cs:[di+0Ah]` table (USRTHKCONNECTIONDATALS +0Ah: word offsets
   0078, 00C5, ...; MSG's at seg 30:2D6E) has to seed ne_link
   (`--seeds`). Read kernel32 ordinal 90 / ThunkConnect32 to learn what
   the 32-bit `table` dword holds (16:16 per index, built from these).
2. KRNL386's boot data: SM_CYCAPTION (4) already goes into KRNL386
   (seg001_5B32: `es = cs:[4]`, a flat selector filled at boot, then
   VMM's dword at 0x1008). Then USER's and GDI's LibMain (RECOMP_MODULES
   cs:ip), with the 16-bit int 21h/31h/2Fh answered from vxd32.
3. Load the real user32/gdi32 with calc (lift.py calc + user32 + gdi32 +
   kernel32), connect the thunks, call a thunked export (GetKBCodePage
   51 of 0xBFF527B3). Then window creation, DISPLAY stand-in, first paint.

### Session 4, handoff (after the coordinator's "continue")

Done: step 1's seeds. `work/seeds/w16.json` (made from USER.EXE, not
committed) seeds USER seg 32 with the 31 word offsets of the
USRTHKCONNECTIONDATALS dispatcher table (+0Ah up to the dispatcher at
0048) and the dispatcher itself: seg 83 now has 218 lifted functions (was
40); check_w16 still passes. Recipe: read seg 32's words from +0Ah while
the offset < 48h, write `{"USER": {"32": [...]}}`, pass `--seeds
work/seeds/w16.json` to ne_link. Not done: MSG's table (seg 30:2D6E,
layout unread: `f0 54 01 00 | 8c 2d 90 21 | bc 2d ...`) and the other
connection tables (LSR, LSS, U, V, SLW, GDI's).

Context ran heavy; the next session picks up at "Next (chunk 1, second
half)" items 1 (ThunkConnect32 / kernel32 ordinal 90: what user32's
`table` dword holds), 2 (KRNL386 boot data, then USER/GDI LibMain with
16-bit int 21h/31h/2Fh onto vxd32) and 3 (a real user32 thunked export,
then window creation).

### Session 5: what user32's thunk words hold; seeds for MSG and GDI

**Not ThunkConnect32.** user32 has no LS01 block. Its entry stubs
(0xBFF51134 etc.) call kernel32 **ordinal 43** (0xBFF7155D) with
(&target, "UsrThkConnectionDataLS", checksum 0x9198C): LoadLibrary16 +
GetProcAddress16 of the 16-bit export, compare its first dword with the
checksum, and store its second dword, a **16:16 pointer to the 16-bit
dispatcher**, as is (flag 0; ordinal 41 passes 1 and stores MapSL of it).
So 0xBFF5D3E0 = USER seg 32:0048. The thunks themselves are
`mov cl,2n; mov edx,argsize; mov ch,0; mov eax,[0xBFF5D3E0]; jmp ord 45`;
ordinal 45 (0xBFF71414) takes the Win16Mutex, switches stacks, sets
di = cx and far-calls eax. The dispatcher (`mov ax,DGROUP; mov ds,ax;
jmp cs:[di+table]`) indexes a near-word table that runs up to the
dispatcher. ThunkConnect32 (0xBFF72EBE) and the LS01/SL01 path at
0xBFF72F0A belong to thunk-compiler DLLs (winmm, version, shell32...),
not user32/gdi32.

Three LS exports: USRTHKCONNECTIONDATALS (seg 32, 31 targets),
MSGTHKCONNECTIONDATALS (seg 30, 11), GDITHKCONNECTIONDATALS (GDI seg 43,
33). `tools/w16_seeds.py` reads them (checks the dispatcher bytes, reads
the table base from its jmp) and writes work/seeds/w16.json; check_w16
still passes with all three seeded. Unread: the FT_ exports
(FT_USRFTHK 853, FT_USRF2THK 854, FT_GDIFTHK 702: {checksum, 16:16, then
word pairs}) and the `mov cl,N; push ebp` stubs whose patched
`mov edx,[ecx*4+X]` uses X = 0xBFF5D400 (the dword at 0xBFF512D6).

### Session 5, step 2: KRNL386's own BootStrap runs, on answered 16-bit interrupts

**GDI's LibMain first.** `W16_LIBMAIN=GDI` runs GDI's entry (15:2795) on
the host: GlobalSize(DGROUP), then GlobalReAlloc(DGROUP, 0FFFFh, 2), which
returns 0 because KRNL386 has no global heap: its arena code
(seg001_3B5C...) reads the Burgermaster selector at KRNL386 DGROUP:121E,
gi_lrulock at +1E, and arenas ({+0 next, +4 prev, +8 base, +0C size,
+10 handle, +12 owner, +14 sig 'M'/'Z', ...}, written by GlobalInit at
seg001_C850), with SelTableLen/SelTableStart at DGROUP:1232/1234. So
"KRNL386's startup data" is its whole global heap: the plan is to let
KRNL386 build it.

**KRNL386 BootStrap (1:B47F)**, `W16_LIBMAIN=KERNEL`: the host gives it
what a DOS exec in the System VM would: ax = 4B4Fh ("KO"), es = the PSP
paragraph 1000h, the load module (KRNL386.EXE past its 512-byte MZ
header, which covers the whole file) at PSP+10h, in a 1 MB real-mode
buffer (identity.c `w16_real_base`). Its 16-bit interrupts are answered
in `runtime/win16/identity/identity.c`, which is now ne_link's `--shims`
directory for identity mode (so the generated print-and-continue
int_handler is not made):
- int 2Fh 1687h (DPMI present; the mode-switch entry is a host far
  entry that only sets es = the PSP's selector), 168Ah "MS-DOS" vendor
  API (ax=100h: the LDT alias, vxd32 `native32_w9x_ldt_alias`), 1684h
  VxD entries from a named table (PAGEFILE 21h and VWIN32 2Ah answered
  absent from their call sites; any other id stops).
- int 31h 0000-000C (on vxd32's LDT; 0002 maps paragraphs of the
  real-mode buffer), 0500/0501/0502 (VirtualAlloc), 0600-0603, 0702/0703.
- int 21h: 25h/35h (vectors; an unset one is a host entry that runs our
  handler, so KRNL386's `pushf; call far [old int 21h]` works), 33h, 34h,
  50h/51h/62h here; the rest goes to vxd32's Int21Dispatch table
  (`native32_w9x_int21`) with ds:dx, ds:si, es:di made flat.
- int 11h/12h (BIOS), int 41h (no kernel debugger).

It runs ~16,400 functions: DPMI handshake, CS alias, LDT alias, DOS
state (break flag, InDOS, drive, 14 vectors), the paging-file probe, a
557 KB DPMI block for the global heap, and stops in seg001_4AFF/4E52: a
selector array for a block of 0FBC3xxxxh bytes (int 31h 0000 cx=FBC3 is
refused; it then tiles descriptors anyway and the lifted `loop` as
tail-call recursion overflows the C stack). That size is computed from
something we answered: suspect 0500's buffer (linear-space pages, +0Ch)
or the 0501 block's base (VirtualAlloc gave ~043Dxxxx; 4 GB minus that
is FBC3xxxx). Next: read the caller of 4AFF (the FN before 4E8C in a
W16_FN run) to see which, then continue to GlobalInit (C850) and on.

pcrecomp findings: (1) ne_link's lifter leaves `addr32 rep stosd`
UNHANDLED (seg001_C850, GlobalInit zeroing the selector table); (2) a
`loop` back-edge lifted as a tail call recurses on the C stack: 64K
iterations overflow it. Both before GlobalInit can work.

**Not done this session**: steps 3 (calc on lifted user32/gdi32/kernel32,
GetKBCodePage through user32's own ord-43/45 path) and 4 (window, DISPLAY
stand-in, first paint).

### Session 5, step 2b: where 0FBC3xxxxh comes from; lifter fixes (handoff)

**Lifter fixes (pcrecomp w98 380920f).** (1) A 32-bit memory operand
with a displacement (`[eax+8]`, KRNL386's arena walks) was wrapped to 16
bits: `ADDR32((uint16_t)(eax + 8))`; now a 32-bit sum. (2) `xchg` used a
16-bit temp for every width (`xchg dword es:[1310], eax` lost the high
word). (3) addr32 movs/stos/lods (b/w/d) are lifted on esi/edi, and a rep
on them counts ecx (GlobalInit's selector-table clear was UNHANDLED).
(4) ne_lift: a backward branch to an earlier function (up to 256 bytes
back) lifts the function from that target with a goto to its own entry,
so the loop is a C loop, not a tail call per iteration (seg001_4E55 is
now one loop over 4E52..4E5x). Still UNHANDLED in this tree: addr32
scas/cmps (6), addr32 movsx (12). johnny (SCRANTIC.SCR) relifts and
builds with MinGW on the new lifter (into a scratch tree, not johnny's
work/). civnet not checked.

**The size, found.** GlobalInit (seg001_C850, C8C3..C908) makes the
first arena span from KRNL386's own code up to a real-mode paragraph:
`movzx eax,dx; shl eax,4; and al,0E0h` (a paragraph -> linear), minus
3554's answer (the linear base of KRNL386's own segment), and asks 4AFF
for selectors over that size. On a real machine KRNL386 is DOS-loaded
below 640 KB, so paragraph*16 is a linear address above it. Here
KRNL386's segments are in the lifted image at ~04A0xxxxh and
paragraph*16 is < 1 MB, so the size is negative (0FBxxxxxxh) and the
tiling loop writes descriptors from 02E8h round the whole LDT
(overwriting SEG_1's: the later fault reads cs:[6] through garbage).
Moving the DPMI block top-down (done) did not change it: it is not the
block.

**Design decision for next session.** KRNL386 treats paragraph*16 as a
linear address, so in identity mode real-mode memory must sit at host
linear = paragraph*16. In a 32-bit Win11 process 0x10000-0xFFFFF can be
VirtualAlloc'd at fixed addresses (only the first 64 KB is off-limits):
put `g_rm` at host 0 + 64 KB (paragraphs 1000h+, PSP at 1000h is
already there), and KRNL386's four segments inside it (ne_link/gen_image
placing KERNEL's segments first and low, or a second image base for
them). Then GlobalInit's conventional arena is right by construction.
Then: GlobalInit on, GDI/USER LibMain, GetKBCodePage (task 3).

### Session 6: the low layout; GlobalInit completes; BootStrap reaches LoadSegment (handoff)

pcrecomp w98 a547da2, 76d2aa6; win98 14bdc0f, 149697c.

**Low layout (identity only).** Real-mode memory is host linear =
paragraph*16, 0x10000-0x10FFFF (identity.c `w16_real_base` returns 0). A
32-bit process already has its activation contexts, process parameters
and heaps bottom-up in that megabyte by `main()`, and with
`/DYNAMICBASE:NO` even the *suspended* child of native32's relaunch does.
So: `native32_hold_range(0x10000, 0x100000)` (new, native32.c) makes
`native32_hold_image`'s relaunch reserve it too, w16test links
`/DYNAMICBASE` (then the suspended child's low megabyte is free), and the
relaunch retries up to 8 times when ASLR happens to land the child's first
allocations on a held span (it did on 1 run in 6 at 0x400000). KERNEL's
selectors 1-4 point at its segments where DOS put them (load module at
PSP+10h, PSP at 1000h), with the *file's* bytes: KRNL386 applies its own
fixups (seg001_0DED walks each chain from the raw bytes; relocated bytes
broke the chain). BootStrap is entered with what the MZ stub (load module
+0) leaves: ss = the load module (1010h), sp = si = 200h (the NE header;
w16test passes 204h because w16_call pushes a far return), ds = DGROUP,
cx = 400h, bx = 0, es = PSP; PSP+2Ch an environment at A000h with
`C:\WINDOWS\SYSTEM\KRNL386.EXE` after it (the DPMI switch makes it a
selector). The 16-bit modules are copied into the guest's SYSTEM dir.

**GlobalInit completes** (args A000h end, start 1025Ch), then its caller
needs VWIN32's PM API (int 2Fh 1684h bx=2Ah, now present): ah=0 version
(>= 100h; 040Ah), ah=8 a flag (0), and ax=030xh VMM page services with
cdecl args on the 16-bit stack that **the entry removes** (ecx bytes;
1FF7 pops what it pushed before them): 0 _PageReserve (committed at once:
KRNL386 commits only whole pages inside a block and relies on the partial
end pages), 1 _PageCommit, 2 _PageDecommit (kept), 3 _PageFree. Also
answered: int 21h 02/09 (to stderr), 47h, int 10h, DPMI 0004/0005.
Then it opens its own EXE, builds its module database from the in-memory
NE header, and runs ~1360 functions to **LoadSegment(KERNEL, 1)**
(seg001_B635 -> 1135), which relocates segment 1 and then fails:
`3BA3(handle)` -> `559C` finds no arena for selector **000Fh** in
KRNL386's selector table (DGROUP:1234). That is SEG_1: the lifted code's
`push cs` is the fixed identity selector, but KRNL386's tables only know
selectors it allocated itself (its arenas over its DOS-loaded segments).
B635 then jumps to the fatal path (BC7E: "KERNEL" + ExitKernel).

**Next.** Pick one: (a) give KRNL386 SEG_1..4 as *its* selectors: find
where BootStrap allocates its segment selectors (4AFA/4B36 tiling in B4F1,
the module DB segment table's handle words) and make those calls return
SEG_n for KERNEL's own segments, so arenas are made for them; or (b) after
GlobalInit, register SEG_1..4 in the selector table by hand (the arena for
the DOS-loaded block is there; entry = arena pointer). (a) is the faithful
one. Expect LoadSegment of seg 1 to copy the segment to new memory only if
it is not "already loaded"; identity.c's `seg_of` (dispatch of an alias
selector by equal base) handles aliases, not copies. Then BootStrap's far
jump into the reloaded copy (`push cs; push [259A]; push B711h; retf`,
lifted as a far jump in identity mode now) has to land in lifted seg 1.
Tasks 2-4 of this session (GDI/USER startup with heap entries, calc on
lifted user32/gdi32/kernel32 with GetKBCodePage via ords 43/45, window
creation) were not reached.

**Lifter (shared).** lift16: addr32 scas/cmps (b/w/d) lifted on esi/edi,
rep/repnz forms count ecx; addr32 `movsx` was flagged UNHANDLED only
because `'movsx'[:4] == 'movs'` matched the string-op filter (fixed).
w16 tree: no addr32 UNHANDLED left (was 19). ne_lift, identity mode only:
`push x; retf` is a far jump (dispatch_far), not a C return. ne_codemap
lost call targets to data decoded as code over them (seg 1 83EA runs
through 852A, the int 21h handler); tools/w16_seeds.py seeds them for
KERNEL. pcrecomp gap: a near-call target that lands inside an already
claimed instruction should win over the linear decode that claimed it.
Regression: johnny and civnet relift and build in scratch trees (MinGW);
civnet's lifted C is byte-identical to 380920f's, johnny has no addr32
sites; check_m1 and check_w16 pass. ne_link exits 1 on civnet on main as
well (pre-existing).

### Session 7: BootStrap reloads itself and runs on in its own segments (handoff)

pcrecomp w98 d176051; win98 c4097c7.

**What session 6 misread.** The failure was not "000F missing from the
selector table" but cs:[2] read through a segment LoadSegment had just
overwritten. Two real causes:
1. *DOS load high.* KRNL386's MZ header has e_minalloc = e_maxalloc = 0, which
   DOS EXEC treats as "load high": the image goes at the **top** of the block
   (PSP:2 = A000h) and the PSP stays at the bottom. w16test put the image at
   PSP+10h, so GlobalInit's first arena (the image) covered all free memory,
   the first GlobalAlloc moved it to extended memory, and BootStrap's 16-bit
   paragraph deltas (B880 -> C37E's slide) wrapped. Now the load paragraph
   is A000h minus the image size, and seg 1/4 are allocated low with no move.
2. *Lifted cs is a constant.* LoadSegment(KERNEL, n) copies segment n from
   cs into a fresh block; the lifted code kept reading cs:[n] through the old
   selector. Done the faithful way (task 1, option a): in identity mode
   `SEG_n` is `g_w16_segsel[n]` (gen_segments_h). On the first far call into
   a selector the runtime does not know, identity.c `w16_rebind` reads
   KRNL386's own data (selector table DGROUP:1234 -> arena -> owner = module
   DB, its segment table +22 handles and resident name) and binds that
   segment, plus the module's DGROUP, to KRNL386's selectors. Other segments
   with a handle but no bytes yet (C597 allocates all four of KERNEL's at
   once; the flags do not say "loaded") wait for their own first call. Code
   segments 1-3 start on a private copy of the file bytes (w16test) so the
   copy source survives the move; DGROUP stays where DOS put it.

**Also fixed.** lar/lsl accept not-present descriptors (a discarded handle is
read with lar before GlobalReAlloc reuses it; it was taking the "new handle"
path) and return 32-bit rights/limits (lift16 temp was 16-bit). `0F FF`
(ud0) / `0F 0B` (ud2) decode as `ud`; in identity mode they lift to
`w16_raise(cpu, 6, cs, ip)`, which runs the guest's DPMI 0203 handler on a
host stack with a 16-bit exception frame and continues at the frame's cs:ip
(BootStrap's #UD probe at 1:B9BB -> handler B9BD -> B9E8). DPMI 0200-0205,
0283 (KRNL386's form of 0203), int 2Fh 1684h bx=1 (VMM, optional: absent).
Seeds 1:B711 (the retf into reloaded seg 1) and 1:B9E8.

**Where it stands (task 2 done).** BootStrap: GlobalInit, module DB,
LoadSegment 1 and 4, far jump 0347:B711 into lifted seg 1 (rebound), frees
the DOS-loaded DGROUP, LoadSegment 2 and 3 from the sliding image, sets its
exception handlers, the #UD probe, then int 31h 0002 for BIOS/video
paragraphs (0, 40h, F000h, A000h, B000h, B800h, C000h, D000h, E000h) and
then **spins**: watchdog at 50 s (`W16_LIBMAIN=KERNEL W16_TRACE=1`, last
call int 31h 0002 bx=E000). Next: run with W16_FN=1 for a few seconds and
find the loop (likely a ROM/BIOS-area scan through those selectors that
expects real BIOS bytes, e.g. a ROM signature 55AA at C000:0 or the BIOS
data area at 40:xx; give the real-mode megabyte plausible BIOS data area and
empty ROM areas). Then SlowBoot: KRNL386 LoadModule of SYSTEM/KEYBOARD/
MOUSE/DISPLAY/GDI/USER (task 3; drivers need stand-ins), then tasks 4-5.

**Debug recipe used.** Temporary prints in the generated work/w16/src
(restored from a copy afterwards) and a swappable `recomp_fn_hit` in
identity.c to dump KRNL386's arenas (DGROUP:121E Burgermaster, 1232 table
length, 1234 table; arena +8 base, +0C size, +10 handle, +12 owner) at chosen
functions. Worth making a W16_ARENAS=fn option.

**pcrecomp gaps.** A `push [mem]; push imm; retf` target and an exception
frame's rewritten ip are not followed by the code map (seeded by hand). The
rebind trigger is the first far call; data-only use of a not-yet-called
segment of another module would still see the old selector.

### Session 8: BootStrap past the BIOS setup, into kernel32's own boot entry (handoff)

pcrecomp w98 b473aa3; win98 (this commit).

**The "spin" was not a BIOS scan.** After int 31h 0002, seg 3 (15E7) walks
a list at DGROUP:1BD2 through `cs:[2]`; seg 3 had never been far-called, so
SEG_3 was still its placeholder and cs:[2] read the private file copy
(FFFF -> ds 0, a list of zeros). Fix: identity mode emits `W16_SEGIN(n)`
before every direct far call into another segment; on a placeholder it
rebinds n from the module database (through any bound segment of the same
module). Second bug behind it: KRNL386 writes the LDT itself (alias from
int 2Fh 168Ah), so vxd32's bump allocator handed int 31h 0002 the
selector of KERNEL's module database (033F -> base C0000h). alloc_sel now
skips non-empty descriptors.

**Low megabyte.** Linear 0-FFFFh cannot be mapped on Win11; cpu.h
`w16_lin` sends those addresses to a host block (identity only). It holds
an IVT (all F000:FF53, an iret), a BIOS data area (VGA colour 80x25,
640 KB, LPT1, empty keyboard buffer), C0000-EFFFF = FFh (no option ROMs),
the F000 reset jump/date/model FCh.

**kernel32 boot handshake (BootStrap's seg001_6A3F).** VWIN32 ah=0 must
give edx != 0 (kernel32's base, a guess); KRNL386 then reads kernel32's
export block 'KEXP' (kernel32 0xBFF71000, its first section) at **flat
1000h** and stores DGROUP:1AC7's linear address through KEXP+4 (into
kernel32, 0xBFFC974A): the host copies the block to the low block on
ah=0. Then int 2Fh 168D (es:di, unchecked: 0:0), VTDAPI (0442) ax=9, VWIN32
ah=4, and seg001_6500 calls KEXP entry 3 (kernel32 0xBFFC7AE4) by an o32
retf on a flat stack. The lifter now emits `w16_retf32` for 66 CB; the
runtime calls the lifted 32-bit function and does the work of KRNL386's
flat return stub (1:656C: ss=di, sp=esp-esi, push ss, FFFF, ebx; retf to
1:7A58). push/pop use esp on a flat stack (B bit and base 0; the host
sets B on every data selector it allocates, QT_Thunk's alias stack too).

**Where it stands.** kernel32's 0xBFFC7AE4 runs and calls back into
KRNL386's own 32->16 entry 1:6582 (through DGROUP:1AC7+28h; seeded),
w16_enter's flat-stack branch. 6582 reads the Win98 TIB through fs:
+0E 16-bit stack selector, +18 self, +1C flags (bit 0 picks the path).
The host thread's TIB (fs 00FF) has +0E 02BF, +18 8B002F5C, +1C 48D8: not
what KRNL386 expects, and it faults. Next: give the boot thread a TIB
KRNL386 can read (ptibSelf at +18, TIBFlags, ss16), or decide that this
entry is kernel32 re-initialising what the host already built and answer
0xBFFC7AE4 at the host. That is the design fork for SlowBoot: until now
kernel32's 32->16 thunks go to vxd32's KRNL386 stand-in
(`call [0xBFF9AD46]`); with real KRNL386 booting, kernel32 should reach
KRNL386 through KEXP's table instead.

Not reached: SlowBoot (drivers, GDI, USER), task 3 (calc), the display
stand-in.

**pcrecomp gaps.** ne_codemap now follows `push sel; push imm; retf` and
`mov r,imm; push r; retf` (1:B711 is no longer a seed). Still seeded:
exception resume addresses (1:B9E8) and code addresses KRNL386 stores at
run time (1:6582). W16_SEGIN covers direct calls; data use of another
module's unbound segment still sees the placeholder.

**Regression.** johnny and civnet relift in scratch trees with d176051 and
b473aa3: lifted C identical (civnet's ne_link rc=1 is pre-existing).
check_m1 and check_w16 pass. Not rebuilt with MinGW (C identical; cpu.h
changes are identity-only macros).

### Session 9: KRNL386 boots kernel32; real 16->32 thunks; SlowBoot reaches SYSTEM.DRV (handoff)

pcrecomp w98 3ae8730; win98 (this commit). Run: `W16_LIBMAIN=KERNEL
build/w16test.exe --mute` (W16_TRACE=1, W16_FN=1 for detail).

**The faulting TIB was a selector collision, not a missing TIB.** kernel32's
own _INIT TIB (Win9x layout) got selector 00FF from the host allocator, which
is segment 31's identity selector: map_image then overwrote it. The host now
stamps every SEL(n) before kernel32 boots (w16test.c `w16_reserve_sels`), and
the LDT alias moves past them too (0030 was segment 6's 0037).

**Faithful boot order.** With W16_LIBMAIN=KERNEL the host no longer calls
kernel32's boot entry: KRNL386's BootStrap builds the 9Ch boot block itself
(on its stack, seg001_6B2A..6EC3) and calls KEXP entry 3 (0xBFFC7AE4), as on
Win98. Read off KRNL386:
- the boot thread's TIB is KRNL386's own (seg001_7240: on its stack, a
  descriptor over it, +0E an alias of ss, +04/+08 the stack); fs is shared
  between the two sides now (identity.c w16_retf32);
- block +14 (LDT base) comes from the LDT alias's own descriptor, which the
  host LDT did not hold (vxd32 native32_w9x_ldt writes it);
- 6582's `lss esp, [bp]` is m16:32 (lifter fix).
kernel32's _INIT then runs to the end on KRNL386's block (MapSL of +3C,
crsts, its service thread, registry, APPLOG) and returns 1.

**16->32 thunks into kernel32 are the real ones.** KRNL386's seg 3 entries
(+40C6, +4102, +4114, ...: `mov cx, n; jmp 41F6`) reach seg001_76BA, which
writes a stub over seg 3 +41F6 at run time (B8 sel / 8E C0 / 66 0F B7 C9 /
67 66 26 8B 91 tab / 66 EA 0xBFF71903) and retfs into it. Static code cannot
hold it: identity.c `w16_retf` (emitted for every plain retf in identity
mode) recognises the stub, takes edx = table[cx], runs kernel32's dispatcher
0xBFF71903 as lifted code on the 16-bit stack and unwinds (setjmp) when the
dispatcher's o16 retf comes back to the 16-bit caller. For that: every
LIFT32_W9X function now pushes/pops through SS_BASE (the dispatcher pushes on
the caller's 16-bit stack before its `mov ss`), lss/lds/... load the
selector's base and take m16:16 for a 16-bit register, and identity far
calls push the caller's SEG_n rather than the stale cpu->cs. Seen working:
cx 78h, 50h, 84h, 00h, DCh (entries 0xBFF93F90, 93E95, 93FCF, 93BFD, 9427F).

**kernel32 -> KRNL386 (32->16).** kernel32's own callback into KRNL386's
entry 1:6582 (table at KEXP+4 / DGROUP:1AC7) now works on the real TIB.
The vxd32 stand-in (`call [0xBFF9AD46]`) is still installed in both paths:
in the KERNEL path it still answered thunks 12/13/30 (GetSystemDirectory,
GetWindowsDirectory, task name) during _INIT, some with null buffers
(it reads its args off the wrong frame there). Next: leave 0xBFF9AD46 as
kernel32 has it when W16_LIBMAIN=KERNEL, so those thunks reach KRNL386's
real exports (the stub patches itself into QT_Thunk).

**Where it stands.** After kernel32's boot: VWIN32 0Ch (KRNL386's entry
registration, answered eax 0), KRNL386 rereads its own file (int 21h now
passes cx and value-dx), VDD absent, then SlowBoot asks to load SYSTEM.DRV,
fails, and prints its fatal "SYSTEM.DRV" (int 10h mode 3 + int 21h). Task 2
starts here: a SYSTEM.DRV stand-in KRNL386's LoadModule can load (an NE in
work/root/WINDOWS/SYSTEM whose entries are host functions, from pcrecomp),
then KEYBOARD, MOUSE, DISPLAY (DIB framebuffer on the private desktop),
GDI.EXE and USER.EXE. Not reached: tasks 3 (calc) and 4.

ponytail (this session):
- VWIN32 0Ch: entries not kept, eax 0 (stored at [13BE]:36h by seg001_53FF).
- VDD (0Ah) answered absent.
- `lss sp` zero-extends esp (a B=0 stack ignores the top).
- every W9X function addresses its stack through SS_BASE (one add per access).

**Regression.** check_m1 and check_w16 pass. johnny and civnet relifted
into scratch trees with b473aa3 and 3ae8730: lifted C identical.

### Session 10: kernel32's own KRNL386 thunks; host drivers load through LoadModule (handoff)

pcrecomp w98 (this session's commits); win98 (this commit). Run: `W16_LIBMAIN=KERNEL
build/w16test.exe --mute` (W16_TRACE=1; W16_FN=2 prints ss:sp and bp per function).

**Task 1 done: 32->16 thunks reach KRNL386's real exports.** In the KERNEL
path k32host.c no longer writes 0xBFF9AD46. kernel32's slot points at a stub
in its .data (0xBFFC9ED0) that rewrites itself on first call (patcher
0xBFF7273F) into `xor ecx,ecx; mov cl,[ebp-4]; mov edx,[ecx*4+table];
mov eax,0xBFF727D9; jmp eax` (table = the dword before the stub, filled at
run time: 0001B418). Static code can't run rewritten bytes, so vxd32.c
`qt_stub` is that stub (answers lookups of 0xBFFC9ED0) and enters lifted
QT_Thunk. Seen: thunks 12h (seg 3:017F, GetSystemDirectory, copies DGROUP
12BC), 13h (3:0155), 2Dh (3:0552), 30h (1:6411), 0Dh (1:7AD6, drive type:
int 2Fh 150Bh MSCDEX check, answered "none").

**Two runtime bugs it exposed (fixed):**
- native32 saved only fs's *base* per thread, not the selectors or the other
  Win9x bases: a nested mach_enter at depth 0 reloaded the host TIB's base
  under fs=041F (QT_Thunk read fs:[0Eh] = 0 and ran on ss 0). regs_t now
  carries g_seg_* and the es/cs/ds/ss/gs bases (the definitions moved from
  vxd32.c to native32.c, so hosts without vxd32 link); a new thread starts
  from the current selectors as before.
- w16test ran KRNL386's BootStrap outside the machine lock: w16_call is now
  inside mach_enter/mach_leave.
- identity.c: g_w16, g_w16_ready, g_w16_ret and the 16->32 unwind stack are
  per thread (kernel32's dispatcher leaves the Win16Mutex, so its service
  thread can run 16-bit code meanwhile; seen: thunk 12 from fs 043F on ss
  0457). segsel_init runs once per process.

**Task 2 started: host drivers.** runtime/win16/identity/drv16.c writes
SYSTEM/KEYBOARD/MOUSE/DISPLAY.DRV into work/root/WINDOWS/SYSTEM at start (our
own NE: MZ stub, NE header, one code segment tagged "PCRD"+index, ordinals
1-64 as fixed exported entries at +10h+2(n-1), each a `retf`; no Microsoft
bytes). dispatch_far checks the tag before the lifted lookup and runs the
entry's host function; LibEntry (+08) returns ax=1; an entry with no host
function stops with driver and ordinal. KRNL386's own LoadModule loads
SYSTEM.DRV (handle 045F, segment 0467 rebound) and calls its LibEntry.

**Where it stands.** SlowBoot's driver loop (seg001_BDB8: di 26F0..26F8, a
GetPrivateProfileString(system.ini [boot], key, default) + LoadModule per
entry, seg001_BFC3) loads SYSTEM, then on the second entry (26F4) the
GetPrivateProfileString epilogue (seg002_520D: pop ds; pop di; leave; retf
16h) runs with bp = 0: the frame at 032F:1EA0 holds a saved bp of 0 (the
loop's own bp is 0, BE01). sp becomes 000A, di 0004, and the loop runs 16K
times (LoadModule returns 045F each time) before the fatal "SYSTEM.DRV". The
first GetPrivateProfileString returns via seg001_A942, the second via
seg002_5DCA (both through thunk DCh, entry 0xBFF9427F, kernel32 calls back
into KRNL386 seg001_3B5C.. during it). Next: find which frame loses its bp
between seg002_58EE/5954 and 520D in the second call (W16_FN=2), compare
with the first; suspect the far-prolog `inc bp` frames (3B6B shows bp odd)
or a callback's bp restore in w16_enter. No host driver entry has been
called yet beyond LibEntry, so none are named.

Not reached: KEYBOARD/MOUSE/DISPLAY entries, GDI/USER through LoadModule
(task 3), calc (task 4).

ponytail (this session):
- qt_stub hardcodes Win98 SE kernel32's 0xBFFC9ECC/0xBFF727D9 (read them off
  the patcher if another build turns up).
- drv16.c exports ordinals 1-64 for every driver (no names yet).
- int 2Fh 150Bh: no MSCDEX.

**Regression.** check_m1 and check_w16 pass. johnny and civnet build from
runtime/win16/host and the lifter, neither touched this session (identity/
and native32 are not in their builds), so not relifted.

### Session 11: the bp=0 fault was a lifter bug; GDI.EXE, DDEML and USER.EXE load (handoff)

pcrecomp w98 37f1d99, 4313d0c, 733789d, 41326c2; win98 9d0ae82, 981d107,
fb17e4e and this commit. Run: `W16_LIBMAIN=KERNEL build/w16test.exe --mute`
(after the two lifts in README: ne_link now takes DDEML.DLL too, and
w16test has its own lift, `tools/lift.py w16test` = xcopy32 + kernel32 +
gdi32 + advapi32 + user32).

**The bp=0 fault (task 1).** Not the `inc bp` frames: KRNL386's
case-insensitive compare (seg1 AAF6) is `call AAE5`, and AAE5 is
`pop dx; ...; jmp dx` -- it pops its own return address and jumps to it.
ne_lift emitted the call as a C call followed by the fall-through, so the
continuation (AAF9..AA0D, ending in `retf 8`) ran once inside AAE5 (via
dispatch_near) and again after it returned, on a stack already unwound:
sp ended 12h high, and the next push in SlowBoot's profile read (seg2 573E)
overwrote the saved bp at 1EAC. The first profile read survived only
because its `leave` happened to restore sp from an intact bp. Fix in
ne_lift: a near call to a function whose first instruction pops a register
that the function later `jmp`s through is a tail call. 33 sites in
KRNL386/GDI/USER (KRNL 1:AAE5, USER 63:535A x30, 54:9BE0 x2).

**Other lifter/runtime bugs found on the way (all generic):**
- ne_lift: an exported entry hidden inside a skip instruction (KRNL386
  3:3162 `mov cl,2` / 3164 `mov bx,01B1` covers AddAtom's 3165 `mov cl,1`,
  and 3167 `mov bx,00B1` FindAtom's 3168 `mov cl,0`) -- the function before
  fell into the hidden entry, so DeleteAtom and AddAtom both ran as
  FindAtom. The function now decodes on along its own stream until it lands
  on a function start. Only for exports/far-call targets and the 3-byte
  `mov r16,imm16`/`cmp ax`/`test ax` idiom (padding before a prologue
  straddles entries in civnet too). Also hit 1:AB2E.
- ne_parse: a fixup past file_size into the alloc tail (GDI seg 43 ends in
  `jmp far KERNEL.630` with its selector bytes beyond the file): the data is
  padded so the instruction decodes.
- lift32 (W9X): `mov sp, r16` drops esp's top when ss is 16-bit
  (`recomp_ss_big`); kernel32's 16->32 return (0xBFF714AC `mov ss,dx;
  mov sp,si`) returns to both kinds.
- KRNL386's stack 032F is DPMI's boot ss: it is 16-bit (B=0) now
  (w16test), as are DPMI 0000 descriptors.
- identity w16_enter: kernel32's Call16 (0xBFF71414, gdi32's thunks) puts
  an o32 flat return frame on the 16-bit stack: unwound like the big-stack
  case.

johnny and civnet relift (scratch trees) to byte-identical C and
mem_image.bin with every lifter change above.

**What runs now.** SlowBoot loads SYSTEM, KEYBOARD, MOUSE, MMSOUND, COMM
(drv16 stand-ins), then GDI.EXE: its LibEntry LoadLibrary32s GDI32.DLL,
which kernel32 maps itself from the guest's SYSTEM and runs as lifted
code (DllMain, thunks both ways), loads vgasys/vgafix/vgaoem.fon from
WINDOWS\FONTS (copied from work/sys), builds stock objects and connects
GDI's thunks (KERNEL.630). Then DDEML.DLL and USER.EXE load and USER's
LibEntry runs (VMM/VMD/ENABLE/REBOOT/SHELL asked for).

**Driver entries named (task 2).** KEYBOARD.5 AnsiToOem / .6 OemToAnsi
(KRNL386 1:A5C2, OpenFile's path conversion through DGROUP:1250).
Nothing else has been called yet; DISPLAY.DRV is not loaded yet (USER
loads it).

**Where it stops.** In USER's LibEntry the 16-bit sp drops from 1DF6 to
079D on ss 0347 (after a run of kernel32 thunks; look at the last stub
returns before the drop), then a read of 0 bytes and kernel32's
Int21Dispatch gets AX=1C03 (garbage). Next: W16_FN=2 around the drop;
then USER's DISPLAY.DRV load (DIB framebuffer on hiddenrun's desktop),
GetKBCodePage, calc (tasks 3-4 not reached).

ponytail (this session):
- KEYBOARD.5/.6: ANSI 1252 / OEM 437 fixed (Win98 SE US).
- VTDAPI 0Ah: no-op (per-segment notify, result unread).
- VMM present; 26Dh answers ax 0 (the no-VMM path); registry 100h-11Fh
  answer 3F3h (ERROR_REGISTRY_IO_FAILED, what KRNL386 returns with no VMM)
  -- route to regbox32 when a 16-bit caller needs a key.
- VMD, ENABLE and any other VxD id: absent (logged under W16_TRACE).
- MMSOUND/COMM stand-ins: LibEntry only.
- only *.fon copied into WINDOWS\FONTS (no TrueType, no WIN.INI).
- ne_lift hidden-entry decode skips _fpu_fixup.

**Regression.** check_m1 and check_w16 pass (one check_m1 failure this
session was a k32host loop bound I broke, fixed). johnny and civnet: see
above.

### Session 12: the sp drop was a non-local error return; USER loads DISPLAY.DRV (handoff)

pcrecomp w98 6d51a20, 3962194; win98 499206b and this commit. Run:
`W16_LIBMAIN=KERNEL build/w16test.exe --mute` (relink work/w16 first:
ne_lift changed; README's ne_link line).

**Root cause (task 1).** USER's LibEntry (seg67 31AE) calls
GetModuleFileName(GetExePtr(x), ...) with x = 0. KRNL386's parameter
validation (088D/08C1 -> seg1 9D47 -> HandleParamError) fails the handle
and does what Win16 does: unwinds to the API's frame (`sub bp,2; mov sp,bp;
pop bx; pop bp`), then `push ax; push bx; retf` into the API's error exit
(2:00C4 for GetModuleFileName, 2:0093 for GetModuleName), whose `retf N`
returns to the API's caller. Lifted, that retf (9D76, also 9D47's normal
return) was a C return: the API body ran on after its validator call with
the frame already popped -- bp became the pushed return offset (3211), and
the sp of 079D followed. Generic fix:
- ne_lift (identity): every direct far call is `w16_farcall(cpu, f)`, an
  SEH frame keyed by the 16-bit sp at entry (the far return's slot);
  host entries (w16_enter, w16_call) are frames too (an error return out of
  QT_Thunk's GetSystemMetrics(82) lands there: it now answers 0).
- a pushed retf, and a fall-through into a bare retf right after a push
  (only 9D47 has that shape), is `w16_jmpf`: to a lifted function, run it
  and `w16_unwind` (throw to the innermost far call frame the 16-bit sp has
  passed); to a call site's middle (GDI's driver wrapper 5FB0 retfs to
  5:2941), a return (unwind, else C return up tail calls).
- gen_unresolved_stubs sees w16_farcall's prototypes.
johnny and civnet relift (scratch, non-identity) byte-identical C and
mem_image.bin.

**Then.** Int21 AX=1C03 was not garbage: kernel32's profile code asks
drive C's allocation (AH=1Ch, DL=3) -- vxd32 answers a fixed 2 GB FAT
drive. int 2Fh 4A32h (USER seg72 3742, bl=5) no-op. USER's LibEntry
LoadLibrarys SYSTEM.INI [boot] display.drv= (empty -> it wrote
`*DisplayFallback=1` and failed): w16test now writes SYSTEM.INI [boot]
naming the stand-ins each run.

**DISPLAY.DRV (task 2), named from GDI's call sites** (all go through GDI
seg5 5E6C/5E75/5E87/5EB4 -> 5FB0, its driver-call wrapper):
- 5 Enable (seg5 26A4 style 1 into a 7Eh GDIINFO block, 289F style 0 for
  the PDEVICE GDI GlobalAllocs): GDIINFO 3.10, 800x600x16, 208x156 mm,
  96 dpi, no palette, LC_POLYLINE, PC_SCANLINE, CP_RECTANGLE, TC_RA_ABLE,
  dpRaster 699h; PDEVICE a BITMAP header (bmType 2000h).
- 3 Control (seg5 2AB6, QUERYESCSUPPORT 8): 0.
- 2 ColorInfo (seg5 1B99): RGB <-> 5-6-5.
- 10 RealizeObject (seg5 5263): pens/brushes realized, fonts left to GDI.
Not reached: BitBlt 1, Disable 4, Output 8, Pixel 9, StrBlt 11,
ExtTextOut 14, SetPalette 22, cursor 100-103; the framebuffer (g_fb in
drv16.c) has no host window yet.

**Where it stops.** Right after RealizeObject's size query for a brush
(136 bytes): GDI 52B4 Local32Allocs it (KERNEL.209 -> kernel32 BFF94106 ->
BFF744EC) and 52CD translates the handle through `ds:[ebx+10000h]` and
the table at ds:[60h]; the read of `ds:[ebx+edx*2]` faults (addr
E66315BC). Next: check what Local32Alloc returned (the handle form GDI
expects for type 0, flags 1) and whether GDI's 32-bit DGROUP selector
(ds) has its 32-bit limit/base; then continue DISPLAY (Output, BitBlt,
StrBlt/ExtTextOut, cursor) and the host window on hiddenrun's desktop.
Tasks 3-4 (16-bit registry via regbox32, GetKBCodePage, calc) not reached.

ponytail (this session):
- int 21h 1Ch: fixed 2 GB FAT drive C: (AL 64, CX 512, DX FFFFh), AL FFh otherwise.
- int 2Fh 4A32h: no-op.
- w16farcall covers direct far calls only (not dispatch_far's indirect ones
  or `push cs; call near`); w16_jmpf's C-return case assumes tail calls.
- DISPLAY: 800x600x16, no palette; Control answers 0 for every escape;
  hatched/pattern brushes realized solid.
- SYSTEM.INI rewritten each run with the stand-in driver names.

**Regression.** check_m1 and check_w16 pass after each runtime change.

### Session 13: the Local32 fault was a truncated disp32; DISPLAY draws; USER32 is Win98's own (handoff)

pcrecomp w98 9f7c227 and this session's commit; win98 this commit. Run:
`W16_LIBMAIN=KERNEL build/w16test.exe --mute` (relink work/w16 first:
ne_parse and ne_lift changed). `W16_WINDOW=1` opens the framebuffer window
(on hiddenrun's desktop), `W16_HOLD=n` keeps the process n seconds after a
stop so a shot can be taken.

**Root cause (task 1).** Local32Alloc and GDI's Local32 heap were right:
Local32Init gives GDI's DGROUP (064F) a 32-bit limit (21FFFFh, G) with the
heap header at ds:10000h (magic LH32, selector table offset 20078h at
header+60h). GDI reads that header as `mov ebx, ds:[10060h]` (67h, disp32,
no base). lift16 printed and computed the offset as `& 0xFFFF`, so it read
ds:[60h], a DGROUP word pair (DF8315B4), and indexed off into the void.
Fixed in lift16 `_mem_addr`: a [disp32] past 64K is ADDR32 (32-bit). Two
more found on the way, same family:
- `btr word ds:[edi], dx` (bt-string, 32-bit base) computed its address in
  a uint16_t and used the 16-bit accessors: now 32-bit when the base is.
- ne_parse dropped a code segment's zero tail: GDI seg 11's last `ret 2`
  ends one byte past file_size (2EEEh of alloc 2EEFh), so it decoded as
  `leave` and fell off the C function; each call leaked 4 bytes of 16-bit
  stack, and a later `pop ds` loaded a stack word (1BF6). Code segments are
  padded to alloc_size.
johnny and civnet relift identical (C and image) with all three.

**Then (in order).**
- DISPLAY.14 ExtTextOut (GDI's raster fonts as GDI hands them: FONTINFO is
  the .FNT header from dfType; 3.0 char table at +52h; glyph offsets from
  the file start, 66 bytes before FONTINFO; extents for count < 0).
- DISPLAY cursor ordinals are 101 Inquire (USER seg072_3714 wants 4),
  102 SetCursor, 103 MoveCursor, 104 CheckCursor (not 100-103).
- DISPLAY.19 DeviceBitmapBits (icons: 32x32 1 and 4 bpp DIBs set).
- KERNEL.442 VWIN32_EventCreate (VWIN32 PM API ah=04): a host event (USER's
  LibEntry failed on 0 and KRNL386 FatalExit'd "USER.EXE").
- identity rebind: a module's loaded data segments (flags 1|4) are bound
  too, not only DGROUP and code. USER seg 33 (lifted 84, the per-monitor
  cursor table at 8Eh) was filled through DGROUP:[508] = its real selector
  0A17 and read as SEG_84 = 02A7 -> a far call to 0:0.
- Guest threads get 16 MB stack reservations (vxd32 start_thread): GDI seg
  5 2DBE/2DE9 walks a table one C call per slot.
- GDI seg 5 AC05/AC24 pop their own return address and return to their
  caller's caller (AC05 `call ax`s the continuation first). In C the caller
  (AACB) went on and ran the continuation twice on an unwound stack (sp
  1A80 -> 0355). ne_lift (identity): near ret/retf record the offset they
  pop (cpu->ret_ip); a near call to a function that pops its return address
  before pushing anything (straight line, or a branch/fall into such a
  function: `popsret_offsets`) returns too when ret_ip is not the offset it
  pushed (W16_RETPAST, logged under W16_TRACE). By value, not slot: KRNL386
  seg1 3B5C pops its return, builds a frame and pushes it back. A global
  check (every near call) had false positives through the 16->32 stubs
  (ret popped 41F6), hence the static set. NE_NO_RETPAST=1 turns it off.
- DISPLAY.1 BitBlt (ROP3 over P/S/D, mono<->color by the DRAWMODE colors),
  8 Output (OS_POLYLINE, OS_SCANLINES), 9 Pixel. So far every BitBlt lands
  in GDI memory bitmaps (365 PATCOPY, 8 SRCCOPY): nothing on the screen yet,
  so no desktop screenshot (the window works: out/w16disp_25.png is black).
- USER's LibEntry then wanted user32's InitSharedTable (KERNEL.453
  GetProcAddress32 on the USER32 hModule): the guest's USER32.DLL was the
  host proxy (30000000h) a plain xcopy run writes. k32host copies Win98's
  own USER32.DLL over it when W16_LIBMAIN is set; USER.EXE now loads the
  lifted user32, whose init thunks back into 16-bit code (thunk 38).
- int 21h from 16-bit code saves/restores g_ds_base/g_es_base around
  kernel32's handler (only the selectors were).

**Where it stops.** user32 init (BFF53FF0 -> BFF51134 -> BFF58072) QT_Thunks
(thunk 38) into KRNL386 seg3 0668, a LoadLibrary16 wrapper. LoadLibrary
(seg2 0DA4 -> 0102 LoadModule -> 0EA0) fails and on its error path a frame
comes back with bp wrong: at seg002_0FE9 bp is 193A where 0EA0's frame is
1A98 (look between seg002_0D3A, 2973, 2991 and seg001_3B93 in a W16_FN=2
trace: one of them leaves bp changed). 18E6's `leave; retf 8` then returns
with sp 1948, 0668 returns with ds 090F and esi/edi garbage, and kernel32's
QT_Thunk tail (BFF72913) faults on `mov ss, edi`/fs. Also check what module
user32 asks for (its name is [bp+6] of 0668): it may simply be missing from
work/root (then the fix is the file, and the bp bug is a second one).

Not reached: the desktop paint (task 3), GetKBCodePage/calc (task 4),
16-bit registry (task 5). DISPLAY entries still unnamed when called stop the
run with their ordinal (StrBlt 11, ScanLR 12, Disable 4, SetPalette 22...).

ponytail (this session):
- KERNEL.442 VWIN32_EventCreate: auto-reset, unsignaled host event; Set/Wait
  not wired yet.
- DISPLAY cursor: shape and position kept, not drawn (the host arrow shows).
- DISPLAY Output: 1-pixel pens, no dash styles, styles other than polyline/
  scanlines answer -1 (GDI simulates); DeviceBitmapBits: BI_RGB only, Get
  writes 1 or 24 bpp; no huge (segmented) bitmaps.
- The framebuffer window only with W16_WINDOW (check_w16 stays headless).
- popsret: a function is marked when it pops before any push on its
  straight line; a pop after a push (or reached through an indirect jump)
  is not seen.
- Guest thread stacks 16 MB reserved (a lifted loop across C functions
  still recurses; a deeper table would need a loop-aware lifter).

**Regression.** check_m1 and check_w16 pass. johnny and civnet relift in
scratch trees identical to 3962194's (C and mem_image.bin).

### Session 14: USER.EXE's own LoadLibrary16 succeeds (MPR.DLL, four lifter/runtime fixes); handoff

pcrecomp w98 9786305; win98 this commit. Relink work/w16 (ne_lift,
ne_codemap changed) and relift w16test with mpr.dll (README).

**Task 1: the module.** user32's thunk 38 (KRNL386 seg3 0668) asks
LoadLibrary16 for `USER.EXE` itself. It is there; LoadModule calls each
module's `DLLENTRYPOINT` (seg2 0BA5 looks the name up, 0D3A calls it), and
USER's (seg 16 311C) is `ThunkConnect16("USER.EXE", "MPR.DLL", ...)`: the
32-bit side is MPR.DLL, missing from work/root and from the lift. Now
lifted into w16test (283 functions) and copied into work/root by k32host.
kernel32's ThunkConnect16 helper (BFF73057) still failed the 'SL01' check:
see lift32 below. After both, LoadLibrary16 returns 0A1E.

**Task 2: the bp cause.** seg2 0D3A calls DllEntryPoint by
`push cs; push 0D83; ...; push [bp-2]; push [bp-4]; mov ds, si; retf`.
ne_lift took that retf for a C return (the push;retf far-jump rule needs the
push right before), so the entry and the continuation at 0D83 never ran,
0CCD's `leave` never ran, and its caller went on with bp = 193A. Fixed
generically: ne_lift (identity) counts the words a straight run pushed
before a retf (pushes and sp-neutral instructions back to the last call/
pop/label); 4 or more means the retf's own target and a continuation are
both on the stack: `w16_retf_call` runs the target with dispatch_far and,
if its retf popped the continuation, runs that. 4 sites (KRNL386 seg2
0D3A and seg3 2779, USER seg 3 twice). johnny and civnet relift (scratch, both trees)
identical C and image.

**Then, in order (all generic).**
- lift32: a 16-bit base/index in 32-bit code (67h) is a 16-bit address:
  `lgs bx, [ebp+10h]; mov eax, gs:[bx]` read with ebx's high word set
  (ThunkConnect16 'SL01' check). Only kernel32's thunk-connect code
  changed (78 lines).
- identity w16_retf (16->32 stubs) and w16_retf32 restore g_ds_base/
  g_es_base, not only the selectors (the QT_Thunk tail read
  [0xBFFC9470] through a 16-bit ds base after a nested ThunkConnect).
- vxd32: the flat thunk 32->16 stub is recognized by its bytes in any DLL
  (`EB 1E`, then `call $+5; pop eax; sub eax, 25h; mov edx, imm`): user32's
  at BFF5D400 (into USER.EXE) as well as kernel32's BFFC9ED0. Before, user32
  ran it natively and lost eax through the VEH bridge.
- identity w16_retf32: an o32 far call into flat code whose return is a
  16:16 address (USER seg 21 A7A7: push cs:A7E9, __FLATCS:user32 BFF540DA,
  which ends `66 CB`). The o16 retf reaches w16_enter, which longjmps back
  (g_thk); the 16-bit code goes on at the return.
- ne_codemap jump tables: `xchg ax, bx` (93h) names bx second, so USER seg 3
  48F4's bound was missed and the table ran over the code at 498A
  (unlifted target 3:498A); a bound on an index that is never scaled is in
  bytes (USER seg 4 4789: `shr ax, 3; cmp ax, 32h; xchg bx, ax`).

**Where it stops.** USER's LibEntry runs on through its MPR thunks and
user32 (DISPLAY ExtTextOut "2","1","0" and BitBlts, still into GDI memory
bitmaps), then USER seg 6 (lifted 57) 81E8 calls GDI SelectVisRgn (GDI.105,
seg1 137F) -> 63B9 -> 13AA -> 972D -> KRNL386 seg1 AFC8 (a Local32 lookup):
`mov eax, ds:[edi+esi]` with ds = [13A6] faults at E001FEA4. Check the hdc/
hrgn SelectVisRgn gets (and whether USER made that region through GDI or
got it from user32), then the Local32 handle it maps.

Not reached: the desktop paint into the DISPLAY framebuffer (task 3; StrBlt,
ScanLR, SetPalette, Disable not yet called; EventCreate Set/Wait not
wired), GetKBCodePage/calc (task 4).

ponytail (this session):
- w16_retf_call: 4+ pushed words means "target + continuation"; a run that
  pushes args and a target with no continuation would be misread (none
  seen: the 4 sites all push cs:cont).
- jump-table "unscaled" means no shl/sal/add idx,idx between the bound and
  the jmp; a scale by mov/lea is not seen.
- lift32 67h: a bare 67h [disp16] (no register) is not narrowed.
- qt_stub keeps the stub's address in a thread-local set by the lookup just
  before the call; the 0xBFF727D9 target is Win98 SE kernel32's.
- w16_retf32's 16:16 return path: the flat callee must end in a retf that
  reaches w16_enter with exactly that address.

**Regression.** check_m1 (xcopy relifted with the lift32 change) and
check_w16 pass. johnny and civnet relift identical to ceb26ea's (C and
mem_image.bin).

### Session 15: SelectVisRgn's fault was an unpatched export prologue; Win98's desktop paints; KRNL386 asks for MSGSRV32 (handoff)

pcrecomp w98 f08aa41; win98 this commit. Relink work/w16 (ne_lift and
gen_segments_h changed; `NE_IDENTITY_SEL=1` on the ne_link line).

**Root cause (task 1).** USER seg 6 81E8 calls GDI.105 SelectVisRgn (seg1
137F), which starts `mov ax, ds; nop`. The NE loader patches that prologue
of a library's exported shared-data entry (flags 3) to `mov ax, DGROUP`
when it loads the segment (KRNL386 did: the bytes in memory were B8 4F 06),
but the C was lifted from the file bytes, so GDI ran on USER's ds. 972D's
`push dword ds:[422]` (GDI's Local32 heap) pushed 0 and the handle 0, and
Local32Size (KRNL386 seg1 AFC8) read [0+0] -> E001FEA4. The hdc/hrgn were
fine and GDI's own. Fixed in ne_lift: such entries (`8C D8 90` /
`1E 58 90`) lift as `mov ax, SEG_dgroup` (28 in GDI). Identity mode only.

**Then, in order (all generic).**
- popsret by stack depth: GDI seg1 9658 `push edi ... pop esi; pop bx;
  jmp 964E` drops its return and leaves through CombineRgn's epilogue.
  ne_lift walks each function's paths (jumps followed, calls end a path)
  counting pushes; a pop past them marks it (1140 -> 1336 checked sites).
- pushed cs:continuation calls: KRNL386 seg1 CD6A `mov bx, CD7D; push cs;
  push bx; ...; call far GetProcAddress; push dx; push ax; retf` calls
  SYSTEM.7. Counted words after the pair, minus what each far call into
  the module pops (its `retf n`), must be exactly the 2-word target:
  w16_retf_call. A `push cs; push imm; call far` (a far pointer argument)
  is popped by its call and is not one. 7 sites.
- drv16 ret_far sets ret_ip (w16_retf_call compares it).
- identity iret pops ip, cs, flags (lift16 iret_frame): KRNL386's int 21h
  hook (seg1 8818) returned leaving 6 bytes, and C116's popa then loaded
  ds from them.
- w16_farcall: a callee that near-`ret`s with its far return still on the
  stack jumped to a pushed address (852A's handler offset in a saved-bx
  slot, 8624 `ret`): run it in the callee's segment (W16_SEGIN now records
  g_w16_callseg) until the far return is popped.
- int 21h AH=06 DL=FF from 16-bit code: no key, ZF set (KRNL386 seg1 80D9
  drains the console at boot).
- drv16: DISPLAY 4 Disable, 15 GetCharWidth; SYSTEM 1-6 (InquireSystem,
  timers, GetSystemMSecCount), 7-9 (x87 save size 94, save/restore no-op);
  KEYBOARD 1 Inquire, 2 Enable, 3 Disable, 7 SetSpeed, 132 GetKBCodePage
  (437), 134/135 AnsiToOemBuff/OemToAnsiBuff, 136 EnableKBSysReq; MOUSE 1-3.

**What runs.** USER's LibEntry completes; KRNL386 boots on: USER's desktop
window fills the screen (DISPLAY.BitBlt 0,0 800x600 PATCOPY, the only
screen blit so far): `out/w16desk15_20.png` (PrintWindow on hiddenrun's
desktop, `W16_WINDOW=1 W16_HOLD=30`, shot at 20 s) is Win98's teal
desktop, 0080 80h. Then KRNL386 seg1 BEC7 LoadModules its first program,
`MSGSRV32.EXE` (DGROUP:2778), which work/root does not hold: error 2,
"KERNEL" fatal message, USER disables the drivers (KEYBOARD.3 import stub,
SYSTEM.5, DISPLAY.4) and KRNL386 calls VWIN32 0901 (ExitKernel), where the
run stops (tagged VxD entry).

Not reached: StrBlt, ScanLR, SetPalette (never called: dpText TC_RA_ABLE
goes through ExtTextOut, RC_PALETTE off), EventCreate Set/Wait (not
called yet), GetKBCodePage through user32 and calc (task 3).

**Next.** MSGSRV32.EXE (Win98's, Win32): copy it into work/root\WINDOWS\
SYSTEM like USER32 (k32host) and lift it into w16test (tools/lift.py
w16test ... work/sys/msgsrv32.exe); kernel32's CreateProcess from
KRNL386's LoadModule (thunk cx DCh, BFF9427F) then runs it, and it starts
the shell (system.ini shell=Explorer.exe; for calc, shell=calc.exe in the
guest's own system.ini). Answer VWIN32 0901 when it comes (exit).

ponytail (this session):
- Loader prologue patch: identity only. civnet's SMEDSNET/TIMERDLL have 2
  such exports (host runtime): left as the file bytes until a civnet run
  checks them.
- _callee_args: the first retf on a straight decode from the callee names
  its n; unknown callees (imports, near calls) reject the site.
- popsret walk: `add/sub/mov sp`, pusha and calls end a path; 400 steps.
- w16_farcall's near-ret continuation: direct far calls only (W16_SEGIN);
  dispatch_far's indirect ones are not covered.
- SYSTEM timers: CreateSystemTimer returns 1 and never calls back; x87
  save/restore do nothing; InquireSystem: 1 ms timer, every drive fixed.
- KEYBOARD/MOUSE Enable keep no event procs (no input events yet).
- int 21h 06: no console input ever.

**Regression.** check_m1 and check_w16 pass. johnny and civnet relift in
scratch trees with 9786305 and f08aa41: identical C and mem_image.bin;
identity mode (NE_IDENTITY_SEL=1) differs only by the intended lines (one
iret in johnny, civnet's 2 DLL prologues, segments.h's g_w16_callseg).

### Session 16: MSGSRV32 loads, BootStrap reaches the scheduler, the first task switch (handoff)

pcrecomp w98 26f04d2; win98 this commit. Relink work/w16 (ne_lift, lift16
changed; README's ne_link line now ends in MSGSRV32.EXE).

**MSGSRV32.EXE is 16-bit (NE, 47 functions)**, not Win32: it is lifted by
ne_link with the other modules (lifted segs 89-90) and k32host copies it
into work/root\WINDOWS\SYSTEM. KRNL386's LoadModule now opens, reads and
loads it; the guest's SYSTEM.INI (w16test writes it each run) says
shell=calc.exe.

**Then, in order (all generic).**
- ret-jump calls: KRNL386 seg1 852A (its int 21h hook, called as
  `pushf; push cs; call 852A`) `xchg [bp-4], bx; ...; ret`s into the
  handler, whose iret/`retf 2` comes back to the call. In C the call
  returned at the ret and the handler never ran (_lread's 9FC3 then
  surfaced as "near ret under a far call"). ne_lift (identity) marks
  functions whose near ret is reached with their own words still pushed
  (same path walk as popsret); their call sites run w16_retjump(seg, ip):
  dispatch what ret_ip names until something returns to ip.
- the identity iret records ret_ip (johnny's one iret line differs).
- `push cs; push imm; jmp near f` (or `mov ax, imm; push ax`): a far call
  to f, back at imm. BootStrap ends `push cs; push 5E13; jmp 3A0E` into
  Reschedule; as a tail call 3A0E's retf left KRNL386 (LibMain returned).
- vxd32 int 21h 55h (create child PSP at DX: copy of the current, SI top,
  parent = current, becomes current), 41h (delete, boxed: KRNL386 deletes
  WINDOWS\WNBOOTNG.STS), VWIN32 13 (after kernel32 creates its service
  thread, 0xBFF91A8E), REBOOT 0 (that thread's Ctrl+Alt+Del callback).
- VWIN32 16-bit API 01 (KRNL386 seg1 73BA; Reschedule 5F54, the boot
  thread's idle loop 7404): ebx = the next task's r0 thread. It is a task
  switch: native32_w9x_switch_to starts the thread if new (kernel32 made
  MSGSRV32's thread 0588270C suspended, C0DE0002) or sets its wake event,
  and the caller sleeps on its own wake event until switched back.
- k32host's watchdog dumps the main thread's C stack (dbghelp), so a hang
  names its lifted function (that is how the ret-jump loop was found).

**Where it stops.** After the boot thread switches to MSGSRV32's thread,
the machine goes first to a thread readied long before and never run
(05DA22B0, made by 16-bit code through kernel32 BFF93F47 during USER's
init, entry 0xBFF712C6 with a 16:16 target). It enters KRNL386 at 687A
with ss:esp 0000:0000FFBC: its 32-bit esp is already a 16-bit offset when
BFF712C6 runs (low-memory reads at FFBC), so something in its start
(0xBFF74B0D / 0xBFF8CAEC / 0xBFF88BF2, before the `retf` into 16-bit)
moved esp onto a stack whose ss base the lifted code takes as 0, or the
thread proc's parameter is a 16-bit pointer. USER's seg 1 0740 then calls
kernel32 BFF93F90 (cx 78h) and the 16->32 stub builds its flat esp from
ss 116F's LDT base, also 0 -> fault in recomp_farcall32 (esp FED0).
Look at: what 116F's descriptor holds when the stub runs (g_ldt), and who
sets TIB+E = 116F for that thread (kernel32 0xBFF8CAEC?). MSGSRV32's own
thread (0588270C, entry BFF72170) has not run yet.

Not reached: calc (task 2), input (task 3), SYSTEM timer callbacks,
EventCreate Set/Wait, VWIN32 0901.

ponytail (this session):
- w16_retjump: 256 hops, then a stop.
- ret-jump detection: same limits as popsret (enter, calls, sp writes end
  a path); 40-odd functions marked across KRNL386/GDI/USER.
- int 21h 55h: the job file table is shared, not duplicated.
- VWIN32 13: meaning unknown, answered 0 (result not read).
- REBOOT 0: callback kept nowhere.
- VWIN32 01: ecx (0 or F01h) not read; every switch blocks the caller
  until woken (the callers loop and re-check).

**Regression.** check_m1 and check_w16 pass. johnny and civnet relift in
scratch trees (s16-*, f08aa41 vs 26f04d2): normal mode identical C and
mem_image.bin; identity mode differs only by johnny's iret line.

### Session 17: the stray thread's stack, MSGSRV32's task thread runs; stops in KRNL386's scheduler loop (handoff)

pcrecomp w98 2f6d052; win98 this commit. Rerun w16_seeds.py and ne_link
(new seeds), then lift.py w16test.

**Root cause of session 16's fault (task 1).** Not 116F's allocation and not
kernel32 0xBFF8CAEC: every guest thread after the boot one started on a host
thread whose ss was 0 (native32's per-thread state). The thread (USER's,
entry 0xBFF712C6) retf's into KRNL386's 32->16 entry 1:687A; w16_enter saw
ss 0, not a flat (big) stack, so it took the QT alias-stack path and cut esp
to its low 16 bits (FFBC). 687A then builds the thread's Win16 stack
selector (TDB +84h, here 116F) as base = (esp - 8000h) & ~FFFh: 7000h
instead of 0851_7000h, which the 16->32 stub (0xBFF71903) later read back
(flat esp FF3A). Found with W16_WSEL=116F (new: names the 16-bit function
that first sees a selector's descriptor change). Fix (generic, vxd32): VWIN32
8's a[6] is the stack selector of a[7]; the twin keeps it (R0T_SS) and
guest_thread loads it before the first instruction. Per-thread 16-bit stacks
then come out right with no other change (687A makes them itself).

**Then, in order.**
- VWIN32 16-bit API 05/06/07 (KERNEL.443 EventDestroy, 450 EventWait, 451
  and 479 EventSet; seg1 78AB/78C7/78E3/7903): host events; Wait gives up
  the machine (native32_w9x_wait). USER's init thread now waits there.
- REBOOT 1 (kernel32's service thread 0xBFF91B44): no-op.
- VMM 0C = _HeapReAllocate(block, n, flags) (a process's handle table,
  0xBFF7D7B0).
- USER's FT_USRFTHK flat-thunk targets were not lifted (user32's second
  stub 0xBFF5D474 -> USER 31:01F5): w16_seeds.py now also seeds every
  FT_*THKCONNECTIONDATA table (16:16 slots on NE selector fixup chains):
  80 USER targets, 1 GDI.
- VMM 1F (thread, 35h): the service thread sleeps on itself each turn.
- MSGSRV32's thread died at once: 0xBFF8CAEC (Win16 process start) reads
  its PSP's environment (PSP:2C) and fails on 0. KRNL386 seg2 4956 makes the
  PSP with `SetPSP(parent); int 21h 55h`; the identity int 21h path took
  55h's dx as a ds:dx pointer and made it flat, and 50h was dropped. Now 50h
  sets vxd32's current PSP and 55h's dx goes through as a selector: PSP 117F
  copies 036F's environment (0437 = kernel32's [BFFC9D24]).

**Where it stops.** MSGSRV32's thread runs: BFF712C6 -> KRNL386 1:687A ->
its 16:16 start 1:72D6 -> 1:5F57 (the code after the task switch) ->
Reschedule (5E16) picks MSGSRV32's TDB, which is not CurTDB (DGROUP:122C,
still the boot task's), so it switches (VWIN32 01) to its own thread, a
no-op, and loops. Each turn of that 16-bit loop is a nested C call (the
loop crosses function blocks: 5F21 -> 73B4 -> 5F57), and the host stack
overflows (C00000FD). Two things to settle:
1. who sets CurTDB on Win98 when a task's thread takes over. Only seg1 7930
   (called from 630D when DGROUP:1417 != 0, and 8FDA) and the boot code
   write 122C. 1417 is VWIN32 08's answer (ponytail 0 since session 9): try
   1 there first; the VWIN32 0C entries KRNL386 registers (8D71, 0ACE,
   0C2B, 7C44, 7CAA) are the other candidate (VWIN32 calls one on a switch?).
2. a loop across lifted function blocks must not grow the C stack
   (lift16: tail calls to a function that is a jump target are already
   `f(cpu); return;`, but MSVC does not turn them into jumps; a trampoline
   or /O2 tail-call check).

Not reached: MSGSRV32's own code (seg 89/90), calc (task 2), input (task
3), SYSTEM timer callbacks (task 4; VWIN32 0901 not called yet).

ponytail (this session):
- VWIN32 06 (EventWait): al (27h) not read; INFINITE.
- VMM 1F: blocks on the thread's wake event; 35h not read; nothing wakes it.
- REBOOT 1: no-op.
- int 21h 51h/62h still answer KRNL386's fixed PSP (W16_PSP_PARA), not
  the one 50h set.

**Regression.** check_m1 and check_w16 pass. No lifter change this session
(runtime and w16_seeds.py only), so no johnny/civnet relift.

(Tried after the handoff: VWIN32 08 answering 1 takes an early boot path
at once (KRNL386 asks int 21h 4300h, get attributes, before LibMain's
drivers); reverted. If 1 is right, 4300h is the next service to write.)

### Session 18: trampolined lifted jumps, CurTDB on a switch, MSGSRV32 runs and loads SHELL.DLL (handoff)

pcrecomp w98 e19b797 (513bced, e19b797); win98 this commit. Rerun
w16_seeds.py (now also shell.dll), copy work/sys/shell.dll to
work/root/WINDOWS/SYSTEM/SHELL.DLL, and ne_link with `work/sys/shell.dll`
appended to the module list.

**Stack growth (task 1, generic, ne_lift identity mode only).** Every lifted
function is a public wrapper `NAME(cpu) { recomp_tramp(cpu, NAME__b); }`
around its body `NAME__b`. A jump into another lifted function (jmp, jcc,
loop/jcxz, far jmp, fall-through) is `RECOMP_TAIL(f)` = `cpu->tail = f__b;
return;`, and the wrapper's loop runs the next body. Every caller (lifted
calls, dispatch, the runtime) still calls the wrapper, which drains the chain
before it returns, so nothing else changed. segments.h declares the bodies;
_unresolved_stubs.c gives a stubbed tail target a body too. Normal mode emits
the same C as before: johnny and civnet relift identical C and mem_image.bin
in normal mode (s18-* scratch trees); in identity mode they differ only by
the wrapper/body/RECOMP_TAIL transform (checked by undoing it textually).
KRNL386's scheduler loop now spins at a constant 16-bit sp instead of
overflowing.

**CurTDB (task 2).** KRNL386 hands the flat address of DGROUP:122C to the
32-bit side in the boot block (+1Ch, seg1 6B8D..), and no 16-bit code sets it
on a switch: on Win98 ring 0 does. Reschedule's switch (73BA: 13E4 = di =
the next TDB, then VWIN32 01) is answered by setting 122C := di first; the
thread that takes the CPU (MSGSRV32's, at 72D6 -> 5F57) finds its own TDB.
VWIN32 08 stays 0 (1 was not needed).
Then 5F7F loads the new task's ss:sp from its TDB and 5F8A retfs into the
frame CreateTask built there (-> MSGSRV32 89:22EE). In identity mode a retf
is a C return, which went back up into 687A. dispatch_far now remembers the
far return its caller pushed; if the callee's last plain retf (w16_retf
records its target) went somewhere else and that is a lifted function, it
runs it ("retf to X, not the caller's Y" in W16_TRACE).

**MSGSRV32's own code runs.** In order:
- USER seg72 20E7: int 31h 0300 (real-mode int 2Fh 160Bh, identify TSRs):
  nothing real-mode, block back as sent.
- MSGSRV32 89:0E14 needs the SHELL VxD (17h) or exits 4C01: present; its PM
  API dx 19h (register the event callback 89:0541; es = a flat data
  selector, kept at [7Eh]), 1Bh/1Ch (event handled) no-ops.
- kernel32 Int21Dispatch 60h (truename, 8.3) after LFN open misses.
- MSGSRV32 LoadLibrary("SHELL.DLL"): Win98's 16-bit SHELL.DLL is now lifted
  too (6th module, segs 91-103). It has 47 moveable internal references
  (segment FFh = entry ordinal): ne_parse resolves them through the entry
  table (they came out as SEG_255 and did not compile). johnny/civnet have
  none (unchanged).
- SHELL's LibEntry (91:0010) -> LibMain (92:0000) runs; it reads WIN.INI and
  writes C:\WINDOWS\REG.DAT (inside work/root: SHELL's Win3.1 registry file,
  because KRNL386's 16-bit registry (VMM 100h+) answers failure; see gaps).

**Where it stops.** SHELL LibMain's epilogue (92:0135) pops a clobbered bp,
so KRNL386's caller (seg2 3DAD/3DAF `leave; ret 0Ah`) returns on sp 0010
and the module loader (seg2 163C..0E44) runs on a wrapped stack, ending in
"near ret under a far call into 2:0437". Probe values: LibMain enters at
11FF:7A8E bp 7A9A, frame bp 7A8C; at 0135 sp 7A7A bp 7A8C, but the word at
ss:7A8C (the saved bp) is not 7A9B by then. Between them LibMain calls
KERNEL _lclose (int 21h 3E through KRNL386's hook: 852A ret-jump, 866E,
89DF `pop [bp+6]; pop [bp+0Ch]; pop ax; pop bp; dec bp; iret`). Suspect the
ret-jump/iret path writing through the wrong bp (89DF's [bp+6]/[bp+0Ch]
land in LibMain's frame). Next: log bp/sp at 852A entry, 89DF and 9DF0 for
that 3E1A call, compare with a working _lclose (USER's at boot).

Not reached: the shell (calc.exe is a Win32 PE: KRNL386 WinExec ->
kernel32 CreateProcess, which needs a lifted calc in-process), calc's
window and paint (task 3), input (task 4).

ponytail (this session):
- VWIN32 01: CurTDB := di (the TDB Reschedule picked), not the r0 thread's
  own TDB.
- dispatch_far's switch-in rule: only a plain retf records its target (retf
  N does not), so the check also needs ret_ip to match; a target that is not
  a lifted function start is taken as a C return after all.
- int 31h 0300: only 2F/160B; any other real-mode int stops.
- SHELL VxD 19h: the event callback is kept nowhere (no SHELL events:
  appy time, VxD ShellExecute); 1Bh/1Ch do nothing.
- int 21h 60h: path uppercased, no 8.3 short-name mapping.
- REG.DAT: SHELL.DLL keeps a Win3.1 registry file in work/root/WINDOWS
  because the 16-bit registry answers 3F3h; route VMM 100h+ to regbox32.

**Regression.** check_m1 and check_w16 pass after each runtime change.
johnny and civnet relift in s18-* scratch trees (2f6d052 vs 513bced, then vs
e19b797): normal mode identical C and mem_image.bin; identity mode differs
only by the trampoline transform, and e19b797 changes nothing for either.

### Session 19: SHELL.DLL's LibMain returns (__WINFLAGS), MSGSRV32 into WNetInitialize -> lifted MPR (handoff)

pcrecomp w98 f1da364; win98 this commit. Relink work/w16 (ne_lift changed)
and relift w16test and xcopy (lift32 changed), README commands.

**Root cause (task 1): not the int 21h hook.** The saved bp in LibMain's
frame (ss:7A8C) is never overwritten (W16_WW=11FF:7A8C watch: 7A9B from
LibMain's prologue to its epilogue; the _lclose path's 852A/89DF/9DF0 bp/sp
are balanced, same as the 716C open before it). The bad bp comes from
SHELL's LibEntry: `mov ax, 0FFFFh; or ax, ax; jns` at 91:0015, where FFFF is
the end of a relocation CHAIN for KERNEL.178 (__WINFLAGS, head at 91:0191).
ne_lift resolved __AHINCR/__AHSHIFT and the real-mode selectors in a mov
immediate but not __WINFLAGS, so ax stayed FFFF, LibEntry took the InitTask
path (KERNEL.91), and InitTask's frame trick left bp = 0 and sp one word
low; LibEntry's epilogue then returned through that. Fix (generic): ne_lift
emits win16.WINFLAGS (0403h) for KERNEL.178. USER has two such sites too
(seg 72). johnny and civnet relift in s19-* scratch trees, normal and
identity mode, against e19b797: identical C and mem_image.bin (no
__WINFLAGS mov site in either).

**What runs after it.** SHELL's LibMain returns, LoadLibrary("SHELL.DLL")
completes, and MSGSRV32 (89:1601) calls USER's WNetInitialize (seg 71
0634 -> flat thunk 08A6/08C0/08E0 -> KRNL386 1:7618):
- 7618 writes a second stub form over the thunk and retfs into it:
  `B8 sel / 8E C0 / 66 BA imm32 / 66 EA off32 sel16` (es = sel, edx = imm).
  w16_retf now runs it as it does 76BA's form.
- kernel32's flat-thunk dispatcher (BFF718C7) finds the thunk unconnected
  for this process and goes back to 16-bit KRNL386 (718AE retf) to connect
  it, which jmp far32s into 718C7 again: two 16->32 frames with the same
  16-bit return. w16_enter longjmps to the outermost of that run.
- kernel32 walks the 16-bit module list with `lea esi, ds:[esi+9]` on a
  16-bit ds: lift32 added DS_BASE to a lea. lea is now the offset alone.
- int 21h 50h (set PSP) from kernel32; 51h/62h answer the current PSP.
- MPR.DLL (lifted) loads; its DllMain does CreateMutex("MPRMutex") then
  WaitForSingleObject. Win98's WaitForSingleObject goes straight to ring 0
  (VWIN32 5C, 0xBFF99B28) and ring 0 owns mutexes (CreateMutex(TRUE) is a
  5C with 0 ms; ReleaseMutex is 5F, 0xBFF92DC1); vxd32 blocked on every
  wait. Mutexes (kernel32 type 3) now have owner/recursion kept in vxd32,
  taken at once when free or ours, handed to one waiter on release.

**Where it stops.** In WNetInitialize's 32-bit half, MPR (7FBF6477, a
method, ecx = this) calls EnterCriticalSection on an object whose type byte
is not 4 (an uninitialized CS); kernel32 raises C0000005, and RaiseException
asks VWIN32 14 (0, CONTEXT*) for the thread context: unknown service, exit 5.
Next: find who builds that MPR object (its constructor should
InitializeCriticalSection at +0; probably a DllMain branch or a lifted
conditional), rather than answering 14.

**Tasks 2-4 not reached.** The 16-bit registry is still unrouted; SHELL's
LibMain writes work/root/WINDOWS/REG.DAT on every run (deleted again after
this session's runs; it comes back until VMM 100h+ goes to regbox32). No
shell start, no calc, no input yet.

Tools: W16_WW=ss:off (with W16_FN) prints the first function that sees that
word change. k32host's --watchdog N now dumps every thread's stack (a
deadlock shows the waiter and the holder).

ponytail (this session):
- __WINFLAGS = 0403h (win16.WINFLAGS), not read from KRNL386's own value.
- Mutexes: 64 slots, no abandonment when an owner dies, wait-all still
  wait-any; a mutex is "type byte 3" with no other check.
- int 21h 50h: the PSP is just remembered (no per-task switch of the JFT).
- Thunk return: the outermost frame of a run of equal returns is taken
  (assumes the inner ones are re-entries of the same call).

**Regression.** check_m1 (xcopy relifted with the lift32 lea change) and
check_w16 pass after the runtime changes. johnny/civnet relift identical in
both modes (above).

### Session 20: the MPR object is MPREXE's (MPREXE.EXE lifted and started in-process); stops in BroadcastSystemMessage (handoff)

pcrecomp w98 62d538b; win98 this commit. Recatalog and relift
w16test (disasm32 changed; README command + mprexe.exe mprserv.dll mspwl32.dll)
and xcopy.

**Root cause (task 1).** Not a DllMain branch. WNetInitialize's 32-bit half
(MPR 7FBF1C26) sees MPR's shared global [7FBF8040] empty, creates an event,
CreateProcessAs <sysdir>\MPREXE.EXE and waits; MPREXE calls MPR ordinal 7
(7FBF1BE3), which stores its object in [8040] and sets the event. MPREXE was
not in the guest's C:\, so CreateProcess failed, the wait was skipped, and
7FBF6477 ran with this = 0 (EnterCriticalSection(NULL)). Fixes:
- k32host copies MPREXE.EXE, MPRSERV.DLL, MSPWL32.DLL into the root; all
  three are lifted into w16test. kernel32's own CreateProcess runs MPREXE as
  a second process in the one address space.
- MPREXE's base (0x500000) is held from the host's start: native32_hold_range
  now takes up to 8 spans. Otherwise a host heap took it and kernel32 relocated
  the image where no lifted code is.
- vxd32: VMM 8 _PageAttach (no-op, one address space); VxD 4B Get_Version
  (absent); VxD 2B 15 and VWIN32 1F, CreateFile on \.\IFSMGR (no devices,
  file not found); VMM 1D RegQueryInfoKey and 15 RegSetValue (regbox32);
  VWIN32 5E ReleaseSemaphore (type 1, +8 count, +10 max), and waits now see an
  event/semaphore signalled *before* the wait (MPREXE set MPR's event before
  MSGSRV32 waited on it).
- **Original code ran natively**: MPREXE's thread start 0x005010C3 (only
  referenced by `push offset` ahead of its CreateThread wrapper) was not
  catalogued, because its bytes `53 56 57 6A 00` read as the string "SVWj"
  and disasm32's string guard rejected it. kernel32's PageCommit memory was
  PAGE_EXECUTE_READWRITE, so the icall ran the original bytes. Both fixed:
  the string guard skips data opening with two register pushes, and vxd32
  reserves/commits guest memory PAGE_READWRITE (an unlifted guest address now
  faults instead of running).

VWIN32 14 (thread context) was not needed after the fix and is still not
implemented (task 1b open).

**Where it stops.** WNetInitialize returns; MSGSRV32 (89:1601) calls USER's
BroadcastSystemMessage (USER seg 16:0054) and waits for good on a reply event
(16-bit WaitForSingleObject, kernel32 9420D). The receiver is MPREXE's worker
thread (5010C3), which has a USER queue but never pumps; it ends after its
30 s semaphore wait. On its ExitThread kernel32 QT_Thunks to KRNL386 0437:4BA0
(thunk 34) twice; the second entry has ss:esp 0000:FB6A and flat return 0
(W16_TRACE), so the 16-bit thread-exit cleanup (which would free the queue and
wake senders) never runs. Next: why the second thunk-34 entry gets ss 0
(probably the thread's 16-bit stack already freed before the call, or a
frame mismatch in w16_enter for a thread that is dying).

**Tasks 2-5 not reached.** REG.DAT routing, calc, input: as in session 19.

Tools: the watchdog prints the Win16Mutex, each ring-0 thread (kernel32
thread, process, host thread id, wait list) and held mutexes
(native32_w9x_dump_threads), dumps 48 frames, and flushes --calltrace. The
"no lifted function" stop prints the 32 bytes there.

ponytail (this session):
- VMM 8 PageAttach is a no-op; VxD 4B, VxD 2B 15 and VWIN32 1F answer "absent".
- RegQueryInfoKeyA is not in regbox32's list: like RegEnumKeyA it falls to
  advapi32 on the boxed handle (read-only).
- A wait entry whose +4 is above FFFFh is an object with a wait block there,
  else a wait block with its count at +4.
- The string guard's two-push exception ('P'..'W' twice) could take a real
  string such as "SV\0".
- Held bases are listed by hand in k32host (`later[]`: mprexe.exe).

**Regression.** check_m1 and check_w16 pass. johnny and civnet relift (ne_link,
s20-* scratch trees, f1da364 vs 62d538b, normal and identity): identical C
and mem_image.bin in all four (ne_link does not import disasm32).

### Session 21: thread exit runs to the end; the 16-bit registry is boxed; still in BroadcastSystemMessage (handoff)

pcrecomp w98 785d6ed (49da1ca, 785d6ed); win98 this commit. Runtime only, so
just rebuild w16test (no relift, no catalog change).

**Root cause (task 1).** Not a w16_enter frame mismatch, and the 16-bit stack
is not freed first. Both thunk-34 calls are FreeSelector calls from the
thread object's teardown, 0xBFF89302 (VWIN32 0B's blk routine). The first one
(0xBFF893E9) frees [thread+4C], which is the **thread's own fs** (its TIB
selector: the first frame 0xBFF867B0 loads fs from it). QT_Thunk's tail then
reloads fs from esi (0xBFF72881). That gives base 0 because KRNL386 seg1 4C18
turns a freed descriptor into a free-chain link with access 0. The second call
(0xBFF893FB, [thread+8C], the 16-bit stack) reads fs:[18h] from the IVT at
linear 0 (F000FF53), so [TIB+7C] = ss 0 and esp is garbage.
Fix (vxd32 recomp_set_seg): when a register is reloaded with the selector it
already holds and that descriptor is no longer present, the cached base
stays. ponytail: real x86 would #GP on that load, and I haven't read what
Win98 does with the fault.

**Second gap (task 1).** That teardown had never sent USER the thread-exit
signal. kernel32's _INIT gives VWIN32 0C a callback block whose **+24 is
0xBFF89912**: the ring-3 thread-termination routine. It decrements the process
thread count, runs DLL_THREAD_DETACH (0xBFF89ECA), sends UserSignalProc 300h
for a last thread and **20h (thread exit)** and 400h, and ends with
**VWIN32 2C** (hands back the ebx it was entered with). VWIN32 never called
it. run_termination (vxd32) now calls +24 and then blk+8, for both the
calling thread and a queued kill (w9x_wait). VWIN32 2C is a no-op.
ponytail: the order and "every termination" come from what each routine
needs (89912 needs the live TIB); I haven't read VWIN32 for it.

**Why the worker never pumps (task 1).** It isn't meant to. MPREXE's worker
(0x5010C3) first calls USER32!RegisterSystemThread(2, 0). USER seg67 3098
creates its queue and sets queue+14h bit 40h (a system thread: seg76 0AC4
skips such queues). Then it loops on a 30 s semaphore wait and exits when
idle. Now its exit sends signal 20h into USER (0A07:1E06 -> seg67 2134:
queue walk, seg76 0156, unlock of the queue selector via int 31h 0005).

**Where it stops.** MSGSRV32 (89:1601) calls BroadcastSystemMessage(0, NULL,
WM_DEVICECHANGE 219h, 49h, 0), so no BSF_NOHANG. Inside SendMessage
(seg54 9E2B -> 55 1DD4 -> 54 14A4 -> 54 160E) it does ResetEvent and then
WaitForSingleObject(-1) on **0x064B3E28, the event the worker's queue was
created with** (VMM HeapAllocate during the worker's RegisterSystemThread,
right after MSGSRV32 freed an earlier object at the same address). The
worker's exit signal does not set it, so the wait never ends. Next:
- find which window or queue BSM picked (it should skip a queue with +14h
  bit 40h, or never list a window owned by one). A top-level window whose
  owner queue is the worker's would mean a "current queue" mix-up between
  MPREXE's two threads (one Win16 TDB per process).
- or check that USER's signal-20h path (seg67 2134..) is meant to wake
  senders blocked on the dying queue, and see why it didn't here.

**Task 2.** KRNL386's VMM 100h..10Eh registry now goes through regbox32
(identity.c -> vxd32 native32_w9x_reg16). Each service's arguments were
identified from its entry's retf: 100 Open, 101 Create, 102 Close,
103 DeleteKey, 104 SetValue, 105 QueryValue, 106 EnumKey, 107 DeleteValue,
108 EnumValue, 109 QueryValueEx, 10A SetValueEx, 10B Flush (0),
10C/10D/10E Load/UnLoad/SaveKey (ERROR_ACCESS_DENIED). Win16's HKCR (1)
maps to HKEY_CLASSES_ROOT. One boot makes about 200 calls (61 opens OK,
3 SetValueEx, all boxed under HKCU\Software\pcrecomp\w98\xcopy).
**REG.DAT is still written, and that is Win98's own behaviour:** SHELL.DLL's
LibMain (seg92 0010..0124) does GetWindowsDirectory + LoadString D0h
("REG.DAT"), and when _lopen fails it does _lcreat and writes its resource 64h
(the 352-byte "SHCC3.10" empty database). It writes only C:\WINDOWS\REG.DAT
in work/root. It doesn't depend on the registry: before this session the
registry returned 3F3h and SHELL behaved the same.

**Tasks 3-5 not reached** (calc, input, VWIN32 14: nothing has asked for it).

Tools: W16_TRACE prints a 32->16 entry's first two argument words and the
name for int 21h 3Ch/3Dh/5Bh/716Ch opens, plus `VMM nnnh registry -> rc`.

ponytail (this session):
- A same-selector reload of a non-present descriptor keeps the cached base
  (vxd32 recomp_set_seg).
- VWIN32 runs 0C's +24 and then blk+8 on every termination; 2C is a no-op.
- The 16-bit registry's hive-file services are denied, and RegFlushKey is 0.

**Regression.** check_m1 (/?, /S /I copy, /P) and check_w16 pass after each
change. There were no lifter or disasm changes, so I didn't relift
johnny/civnet.

### Session 22: the broadcast's wait was a stale CurTDB; MSGSRV32 WinExecs calc, which loads and exits in its process init (handoff)

pcrecomp w98 5591521, 5ca1290; win98 this commit. Relift w16test (README
line: + calc.exe msvcrt.dll shlwapi.dll comctl32.dll shell32.dll) and
ne_link (+ commctrl.dll). No lifter/disasm change, so johnny/civnet were
not relifted.

**Root cause (task 1).** The window and queue were right: the broadcast's
SendMessage went to MSGSRV32's own window (hq 117F). But USER's "current
queue" (KRNL386 seg1 5B32, `[thread+30]`) gave the worker's queue (13CF).
That happened because MPREXE's worker registered its queue into MSGSRV32's
thread object. RegisterSystemThread (USER seg67 3098) calls SetThreadQueue(0, hq)
(KRNL386 seg1 5C64), which finds "the current thread" through 5C21. 5C21
takes kernel32's current thread only if its TDB (thread+14) is CurTDB
(DGROUP:122C), and otherwise takes CurTDB's thread. CurTDB was only ever set by
KRNL386's own task switches (VWIN32 01), so while the worker ran, CurTDB was
still MSGSRV32's. SendMessage then saw curq != the window's queue, took the
inter-thread path, and waited on the worker queue's event.
Fix (vxd32 cur_task): whichever thread takes the machine (w9x_on_enter,
the end of w9x_wait, guest_thread start) writes its thread+14 into CurTDB
(identity records the address at the first VWIN32 01). With that, the
broadcast completes at once, and USER's thread-exit path needs no change.
ponytail: Win98's ring 0 does this when the Win16Mutex changes hands (from
memory, not read); doing it at every hand-over of the machine is the same
for 16-bit code.

**Then, on the way to calc (task 2).**
- VWIN32 16-bit 0F (KRNL386 seg1 750A: r0 thread, delta). USER's
  SendMessage loop calls it -2/+2 on the receiver: a priority boost, so a no-op.
- SHELL PM API 13h (USER seg55 5459, after reading the shift/ctrl/alt
  states). Nothing reads a result, so a no-op.
- int 21h 4B00 (Int21Dispatch): no DOS VM; answers 2 (missing) or 5.
- **WinExec bypassed KRNL386's int 21h hook.** KRNL386 WinExec (seg2 2A68) does
  a raw `int 21h` 4B00 and relies on its own handler (set with 2521h) to call
  LoadModule. identity's dos_int21 now sends AH=4Bh to the guest's vector when
  one is set, and runs the handler's ret-jump table (seg1 8624) in the
  handler's segment. ponytail: only 4Bh takes the hook.
- k32host copies CALC.EXE (into WINDOWS), MSVCRT, SHELL32, SHLWAPI,
  COMCTL32 and COMMCTRL (16-bit; COMCTL32's init LoadLibrary16s it) into
  the root, and holds calc's base (later[]).

**Where it stops.** MSGSRV32's WinExec(calc) -> LoadModule -> kernel32
CreateProcess: calc's process maps CALC, MSVCRT, SHLWAPI, COMCTL32 and SHELL32,
and COMMCTRL loads and its LibEntry runs. Then calc's process exits during
its init: kernel32's termination thread (BFF988D1, QueueToThread) and
thread signal 7 run. That is a deliberate exit after COMMCTRL's LibEntry
(14DF:009C), so the next thing to check is COMCTL32's DllMain/ThunkConnect32
against COMMCTRL's ThunkConnect16 (compare USER<->MPR, session 14). **The
dying thread still owns the Win16Mutex** (owner = its TDBX). Its teardown
then runs 16-bit code with its freed fs (thunk 43 enters with fs base 0, low-memory
reads), and MSGSRV32, the new thread and MPREXE all wait on crst 0001E644 for
good. Two follow-ups: (a) why the process init fails; (b) a terminating
thread should leave the Win16Mutex (or not reach 16-bit code on a freed fs).

**Not reached:** calc's window and screenshot (task 2), scripted input /
12+3= (task 3). VWIN32 14: nothing asked for it (task 4).
**REG.DAT (task 5):** unchanged, Win98's own SHELL.DLL behaviour (it stays
inside work/root).

ponytail (this session):
- CurTDB := thread+14 on every hand-over of the machine (vxd32 cur_task).
- VWIN32 16-bit 0F (priority boost) and SHELL PM 13h are no-ops.
- int 21h 4B00 from the host: 2 if the file is missing, else 5 (no DOS VM).
- Only int 21h AH=4Bh goes to the guest's int 21h hook.

**Regression.** check_m1 (/?, /S /I copy, /P) and check_w16 pass after
each runtime change.

### Session 23: calc's process init failed on the wrong COMMCTRL.DLL (and SHELL.DLL); calc's window procedure runs (handoff)

pcrecomp w98: this session's commit; win98 this commit. Relink work/w16 (README:
shell.new and kommctrl.dll replace shell.dll and commctrl.dll; w16_seeds.py
changed, regenerate work/seeds/w16.json) and relift w16test and xcopy
(drive32 changed).

**Root cause (task 1).** COMCTL32's DllMain is `ThunkConnect32(&data,
"commctrl.dll", "comctl32.dll", hinst, reason)` and returns FALSE when it
fails; that FALSE is what ended calc's process (kernel32's DLL-init failure
path: termination thread, QueueToThread). The 16-bit side must export
`Cctl1632_ThunkData16`. The CABs' `commctrl.dll` (48 KB, PRECOPY1/2) is
setup's own Win 3.x-style control DLL with no thunk data: LoadLibrary16
loaded it, ThunkConnect32 found no thunk data, freed it (the "far 14DF:009C"
in session 22 was its WEP, not an entry point) and failed. Win98 installs
**KOMMCTRL.DLL as COMMCTRL.DLL** (subase.inf: `commctrl.dll, kommctrl.dll`).
The same thing applies to SHELL32's `ThunkConnect32("shell.dll",
"shell32.dll")` (`Shl3216_ThunkData16`): Win98 installs **SHELL.NEW as
SHELL.DLL** (shell.inf, flag 8, a forced rename). Session 21's REG.DAT came
from that setup-time SHELL.DLL. Fix: k32host copies kommctrl.dll and
shell.new into work/root under their installed names (overwriting), and
ne_link and w16_seeds use them. Other renames in the INFs that will matter
later: `commdlg.dll, commdlg.new` and `ver.dll, ver.new`.

**Task 2 (a dying thread and the Win16Mutex).** vwin32_thread_done (VWIN32 2B)
now gives up the Win16Mutex if the dying thread's TDBX owns it: recursion
and owner cleared, the count lowered by the recursion, and handed to the
first waiter as LeaveCriticalSection's 1E does. After the root-cause fix calc
no longer dies, so this path did not run in the calc run (it is the generic
guard). The 16-bit teardown on a freed fs (thunk 43 entering with fs base 0)
was not reproduced after the fix and was not changed.

**Then, on the way to calc's window (all generic unless marked).**
- ne_codemap: a near call to a callee that opens `pop r16` and jumps through
  the same register on its straight line (USER seg 54 9BE0: `pop bx; ...;
  jmp bx`) makes the return address a function: the callee comes back
  through dispatch_near, and 9BF0 was a near-call miss.
- vxd32: the FT_Prolog form of the flat 32->16 stub (user32 0xBFF5D4AA,
  `call $+5; pop edx; sub edx,25h; push edx; push imm; push FT_PrologPrime;
  ret`), recognized by its bytes like the QT form: edx = [[stub+imm] +
  cl*4], then kernel32 0xBFF7268D. It had run as native bytes and faulted.
- identity: a 32-bit window procedure's 16-bit face is a thunk KERNEL writes
  at run time (`66 68 proc32; 66 68 x; EA off sel`, calc's 0100592A via
  KRNL386 2:7616): dispatch_far recognizes the bytes and does the two
  pushes and the far jump. A far-call miss now also prints the 24 bytes at
  the target.
- w16_seeds: USER 30:3D12, where KRNL386 2:7616 jumps through its table at
  4:2214 (ponytail: a seed; the table is filled at run time).
- ne_codemap: a call target that a jump table's guessed extent had claimed
  as data is code (GDI seg 1: a table guessed over 75A0..7607 hid 7600,
  which 7860 calls; lifted as a short linear stub with unreachable
  branches).
- disasm32/drive32/generate: `jmp cs:[reg + table]` (a byte-offset index,
  user32 0xBFF55BDA) is a jump table, like `jmp [reg*4 + table]`. Only with
  the cs: prefix. Note: work/cat catalogs are cached, so disasm32's part only
  applies to a fresh catalog; drive32's arms already put 0xBFF55BF8 in
  user32's body.
- identity: VWIN32 16-bit 0E (KRNL386 1:7584), USER seg 54 96B6's
  foreground boost (0 for the old queue, 1 for the new one): a no-op.

**Where it stops.** Calc's process init completes (COMCTL32 registers its
classes through the FT thunks into COMMCTRL, SHELL32 connects), USER sends
calc's first messages, and calc's window procedure (0100592A) runs from
16-bit USER and returns (ax 000A). Then USER seg 52 0F49 (from seg 54 8A9B
<- 85C4 <- 9582 <- 9262) faults reading `es:[eax+46h]` with a garbage 32-bit
eax that it popped off the stack (a window pointer argument). The next step
is the caller's frame: who pushed that dword, and whether the 16->32->16
round trip through 3D12 (its `add sp, 8; retf 0Ch`, 12 bytes of arguments
when [esi+2Fh] bit 0 adds the extra word in 9E2D) leaves sp where USER
expects.

**Not reached:** calc's window and first paint (task 3), so no screenshot
against shots/calc_12p3_5.png; scripted input / 12+3= (task 4).

ponytail (this session):
- The Win16Mutex pointer 0xBFFC9470 (Win98 SE kernel32) in vwin32_thread_done.
- The FT_Prolog continuation 0xBFF7268D (Win98 SE kernel32) hardcoded in ft_stub.
- USER 30:3D12 is a seed, not found from the run-time table at KRNL386 4:2214.
- VWIN32 16-bit 0E (foreground boost) is a no-op.
- The run-time wndproc thunk is matched by its byte pattern only.
- `jmp cs:[reg+disp]` is a jump table only with the cs: prefix.

**Regression.** check_m1 (xcopy relifted) and check_w16 pass. johnny and
civnet relifted into s23-* scratch trees, normal and identity mode, with
5ca1290 and with this session's pcrecomp: identical C and mem_image.bin in
all four (civnet without work/win16_imports.json, the same for both sides).

### Session 24: the 0F49 fault was two 16->32 stubs merged by their return address; calc's window paints (handoff)

pcrecomp w98 4797218, 0a6a480; win98 this commit. Relink work/w16 (README
line; w16_seeds.py changed, regenerate work/seeds/w16.json; drv16.c now
defines MOUSE_Ord1..3, so the stub generator drops those). No lifter or
disasm change, so johnny/civnet were not relifted.

**Root cause (task 1).** Not sp: 3D12's frame balances (`add sp,8` drops
the thunk's two pushed dwords, `retf 0Ch` the 12 argument bytes when
[esi+2Fh] bit 0 is set; traced 9E2D in = 9E9D out on every call). Calc's
window procedure sends a message to its own window (WM_NCACTIVATE, 86h),
so USER 30:3D12 runs nested: the outer one on ss 1427, the inner on 1477,
and both call KRNL386 3:2726 from 3D12, so both 16->32 stubs have the
same far return (0457:2737). w16_enter's "outermost of a run with the
same return" rule (meant for a flat thunk re-entering itself through
KRNL386 to connect) merged them and longjmp'd the inner return to the
outer stub's C frame. The inner 3DA6 then returned up the outer C chain,
skipped 9E9D (ds, esi, edi and 14 bytes left on the stack), and 8A9B read
its window pointer from [bp+0Ah] of the inner frame on the outer ss.
Fix (identity.c): each stub records the 16-bit ss:sp of its return;
stubs merge only when that frame is the same. W16_TRACE says when stubs
share a return but not a frame.

**Task 2.** The 3D12 seed is gone. KRNL386 2:7616 indexes a table that
USER registers with KERNEL.619 (RegisterCBClient(id 3, relay16 30:3CAA,
relay32)): 16:16 entries up to FFFFh, each selector word on seg 30's
selector fixup chain. w16_seeds.py finds every KERNEL.619 call, the
`push sel; push off` before it, and seeds the table's offsets (USER
30:3D12 and 30:3E13; GDI 47:01DA's table is empty).

**Task 3: calc's window and first paint.** `W16_WINDOW=1`, PrintWindow on
hiddenrun's desktop (`HIDDENRUN_DESKTOP=pcrecomp_w98`, new in
hiddenrun.py), shot at 30 s and 95 s (the same): out/s24calc_95.png. Win98's
calc, drawn by Win98's own USER/GDI/DISPLAY into the framebuffer at
60,83 (260x257): caption, Edit/View/Help menu, display "0.", all 28
buttons with their labels, red/blue as in shots/calc_12p3_5.png (the
host-DLL calc). By region: same layout and labels; differences: (a) the
caption buttons show "0 1 r" (USER draws them as Marlett glyphs; the
DISPLAY stand-in's ExtTextOut has no Marlett), (b) "6" and "3" are black,
not blue, (c) the 9x frame/3D style vs Win11 (expected). The display
reads 0. (no input landed, task 4). After the paint every thread waits
(calc in GetMessage): the watchdog's dump.

**Task 4: scripted input (plumbed, no effect yet).** USER imports MOUSE
statically, so its calls took the generated "unimplemented" stubs
(MOUSE.Inquire returned 0). drv16 now defines MOUSE_Ord1..3 (the stub
generator skips hand-written names), ms_enable keeps USER's event proc
(1:07FE), and `W16_INPUT="click X,Y@T;..."` starts a host thread that takes
the machine (mach_enter) per event and calls the proc as MOUSE.DRV's
interrupt handler would (ax 8003h/8005h absolute+move+down/up, bx/cx
scaled to FFFFh, dx 2) on a private 8 KB 16-bit stack. All ten events of
12+3= reach 1:07FE and return, but calc does not change and nothing is
traced in between (no far calls): USER's proc takes its early-out.
Next: 1:07FE tests [0FD0] (then ss:[sp+0Ah] against cs:[568h], an
interrupt-stack check?), [66h]/[68h] (mouse/cursor enabled), and the 9ECh
result; one of them drops to 9DDh. Read those words at the first event
(W16_TRACE + a peek at DGROUP 7C8h) and see who sets them (likely
USER's EnableHardwareInput / the cursor init that MOUSE.Inquire now
answers differently, or a SYSTEM timer that drains the queue: SYSTEM.2
CreateSystemTimer is still an unimplemented stub returning 0).

**Task 5.** COMMDLG and VER did not come up (calc does not load them);
the INF renames are noted in session 23.

ponytail (this session):
- 16->32 stubs merge only on the same 16-bit return frame (ss:sp equal).
- W16_INPUT: clicks only, a host thread holding the machine stands in for
  the mouse interrupt, its own 8 KB 16-bit stack.
- MOUSE only: USER's static KEYBOARD and SYSTEM imports still go to the
  generated stubs (KEYBOARD.1 purge 8 there vs Inquire's 4 in drv16:
  check before switching them).

**Regression.** check_w16 and check_m1 pass after each runtime change (one
check_m1 run right after a rebuild missed the /S /I copy; three reruns
passed: a build race, not reproduced).

### Session 24, part 2: the mouse proc runs; moves reach calc, button messages do not (handoff)

pcrecomp w98 1554a36. No relink needed beyond part 1's.

**Why part 1's events did nothing.** w16_call took `cs >> 3` as the
segment index; USER's event proc lives at a run-time selector (099F),
whose index is past RECOMP_NUM_SEG, so the dispatch lookup failed and
w16_call returned 0 without running anything. w16_call now uses seg_of
(identity selectors map to themselves, as before), and the input thread
reports a proc that is not lifted.

**Now.** USER 1:07FE runs for every event: [0FD0]=0, [66h]=[68h]=1, the
9ECh decode gives 0201h/0202h (left down/up) plus 0200h (move); the
position is right ([76h],[78h] = 127,250 in a 0,0,800,600 rect); each is
stored by 0655 into the system queue and KERNEL.451 (VWIN32 07) sets host
event 3E0h/3E4h. USER's input thread (VWIN32 06 wait on that event) wakes
each time and calc's thread wakes too: calc's button gets WM_NCHITTEST
(84h, lp 00FA007F) and WM_SETCURSOR (20h, HTCLIENT, 200h) and so does its
parent. **No WM_LBUTTONDOWN/UP is delivered**, so nothing is pressed and
the display stays "0." (out/s24in_70.png).

Lead: every hardware event's time is 0. 0655/0976 take it from `les si,
ds:[0]` in USER's DGROUP (seg 84), which is 0000:0000 here, so es:[si]
reads 0. Something (KRNL386 or USER init, probably from SYSTEM's
GetSystemMSecCount pointer or a timer USER's SYSTEM.2 CreateSystemTimer
callback updates) should store a far pointer to a running tick count
there. With all times 0 the button messages are likely dropped or merged
by the reader's time/double-click checks. Next: find the writer of USER
DGROUP:0000 (grep the lifted USER segs for stores to ds:0 / the KERNEL
export that returns a tick pointer), then put SYSTEM (and KEYBOARD, with
the pop counts checked against Win98's KEYBOARD.DRV exports: the
generated KEYBOARD.1 pops 8, drv16's Inquire 4) on the stand-ins.

Not reached this part: 12+3=, Marlett caption glyphs, the "6"/"3" colour.

Regression: check_w16 passes; check_m1 passes, but its first run after
a check_w16 rebuild failed twice this session ("/? DIFFERS" or the /S /I
copy missing) and passes on rerun: a reproducible ordering flake in the
checks (likely the build or the root's state), not yet looked at.

### Session 25: the tick source was VTDAPI 9; clicks and keys reach calc, 12+3= shows 15. (handoff)

pcrecomp w98 faa87d3, 4db6b92; win98 5ecde94 and this commit. Relink work/w16
(README ne_link line: drv16.c now defines SYSTEM_Ord1..6 and KEYBOARD_Ord1..7,
131, 132, 134, 135, so the stub generator drops those) and rebuild. No lifter
or disasm change, so johnny/civnet were not relifted.

**Tick source (task 1).** USER seg72 1EC0..1EF8 (its init) calls int 2Fh
1684h bx=442h (VTDAPI), then the entry with ax=9, and stores `0:ax` at
DGROUP:0; KRNL386 seg1 6E95 does the same into DGROUP:1A88 (seg2 4D58 reads
it). VTDAPI 9 returns the selector of VTD's system-time page (ms dword at
offset 0). identity.c answered ax=0 (a ponytail since session 9), so
USER's GetTickCount (1:110C, export 13) and every hardware event's time
(0655, 0976) read 0000:0000. Now `drv16_tick_sel()` gives a selector over a
host page that a host tick thread keeps at GetTickCount. The same thread runs
SYSTEM.DRV timers: CreateSystemTimer(rate, proc) stores up to 8, and each due
proc is called via w16_call holding the machine (mach_enter), on its own 8 KB
16-bit stack, as the input thread does. USER creates two: 250 ms -> 1:12A8 and
10000 ms -> 1:1164. USER's static SYSTEM and KEYBOARD imports now reach the
stand-ins. The pop counts come from USER's call sites, not the generated
stubs: KEYBOARD.1 Inquire pushes 4 (the stub had 8), .4 ToAscii 14 (18),
.131 MapVirtualKey 4 (8). 128/129/133 stay generated, and nothing calls
them in this run. KEYBOARD.Enable keeps the event proc (52:056C).

**Clicks (task 2).** With real times, clicks still did nothing. The reason:
USER's mouse proc queues the button messages (0841..086F) before it applies
the move in the same call (0887..), so a press with the move bit set was
queued at the old cursor (400,300, MSGSRV32's window). W16_ARGS on 3:3164
showed 0201 at 0190,012C and 0200 at 007F,00FA. A real mouse moves before it
clicks, so W16_INPUT now sends move (8001h), press (8002h), release (8004h).
`W16_INPUT="click 127,250@40;click 166,250@42;click 244,284@44;click
205,250@46;click 283,284@48"` (1 2 + 3 =) shows **15.**: out/s25click_15.png.

**Keys.** `W16_INPUT="key 1@40;key 2@41;key +@42;key 3@43;key =@44"` calls
KEYBOARD's event proc (al vk, ah 80h on release, bl scan). '+' is VK_ADD and
the others go through VkKeyScan. USER's TranslateMessage calls KEYBOARD.4
ToAscii, which uses the host's ToAscii with the guest's key state. Calc shows
**15.**: out/s25key_15.png.

**check_m1 flake (task 4).** Every host boot rewrites
work/root/WINDOWS/SYSTEM/USER32.DLL (the host-user32 proxy). w16test and xcopy
write different bytes (their stand-in addresses differ), so while one process
still has it mapped, the other's fopen("wb") fails. That produced "no USER32
proxy", rc 1, "/? DIFFERS", or a missing copy. With a w16test running in the
background, 20 of 20 xcopy runs failed. Fixes: write_proxy_dll leaves the file
alone when its bytes are already there, hiddenrun waits for the killed process
to end, and check_m1 keeps each run's log (work/m1_help/copy/p.txt). After the
fix: 15 of 15 runs next to a running w16test, and 5 of 5 loops of hiddenrun,
check_w16, check_m1. Residual: one check_m1 right after a w16test relink
lost the /P run's "copied" line once. Not reproduced in 3 tries; the next one
leaves its log in work/m1_p.txt. The proxy can still clash if a w16test and
an xcopy run at the same moment with different bytes; use separate roots per
host if that matters.

**Tracing added.** `W16_ARGS=fn,fn` prints 8 stack words at entry to the named
lifted 16-bit functions. `W16_BP=fn@-70` prints 16 words at ss:bp-70h there.
Both are on from the first W16_INPUT event, and W16_FN_INPUT gives the full
FN trace from then. W16_TRACE now prints the Pascal arguments of 32->16 enters
and 16->32 stubs. Useful USER entries: 3:12EF DISPATCHINPUT (drains the raw
queue into 3:3164, per message), 3:0284 DISPATCHMESSAGE32 (its MSG at
ss:sp+6), 3:018F GETMESSAGE32, 4:1562 (queue events, type at +1Ch).

**Not done (task 3).** Marlett: GDI realizes only raster fonts here
(dpNumFonts 0, the stand-in ExtTextOut draws .FNT glyphs). The caption
buttons ask for Marlett, the mapper falls back to a raster face, and the
glyph codes show as "0 1 r". A fix needs GDI's TrueType path (Win98's
marlett.ttf rasterized somewhere) or a stand-in that draws the four caption
glyphs when the face is Marlett. Not started. "6"/"3" colour: not looked at.
The ExtTextOut trace does not print the text colour yet.

ponytail (this session):
- Tick thread: one host thread, Sleep(5)-paced (host GetTickCount
  granularity ~15 ms), at most 8 system timers; the time is the host's
  GetTickCount, not ms since the guest's boot.
- KEYBOARD.ToAscii/MapVirtualKey use the host's layout (US here) in place of
  KEYBOARD.DRV's tables.
- W16_INPUT keys: unshifted only ('+' '-' '*' '/' as numpad keys).
- write_proxy_dll skips identical bytes; two hosts with different proxies
  running at once still clash.

**Regression.** check_w16 and check_m1 pass after each runtime change.

**Next.** (1) Marlett for the caption buttons. (2) Why "6" and "3" are black:
log the DRAWMODE text colour in ExtTextOut and compare with the
SetTextColor calls calc makes for those buttons. (3) Shifted keys in
W16_INPUT. (4) Watch work/m1_p.txt for the residual check_m1 flake.

### Session 26: Marlett caption buttons, shifted keys, check_calc gate, Solitaire as the shell (handoff)

Relink work/w16 with the README ne_link line plus `work/sys/winmine.exe
work/sys/sol.exe work/sys/cards.dll` (lifted segs 106-120), after
`py -3 tools/w16_seeds.py work/sys > work/seeds/w16.json` (new SOL 4:1644 seed).
No lifter or disasm change is committed (a tried ne_codemap change was
reverted), so johnny/civnet were not relifted.

**Gate.** `sh tools/check_calc.sh` boots to calc, keys 1 2 + 3 =, and compares
calc's display (shot box 72,130-310,155) with work/calc_ref_15.png (cut from
out/s25key_15.png on first use; a Win98 render, never committed).

**Shifted keys.** W16_INPUT `key C@T` sends Shift down/up around a key whose
VkKeyScan has the shift bit; `text STR@T` types a string. `text 12*3=@40`
shows 36. (out/s26shift_48.png).

**Marlett.** GDI asks the display driver's RealizeObject (style 3) with the
LOGFONT before it falls back to raster fonts. For face MARLETT, drv16 now
answers with a device font: Win98's marlett.ttf (k32host copies it as
SYSTEM\MARLETT.TT_), rasterized by a process-private host font
(AddFontMemResourceEx) at the LOGFONT height into a FNT 3.0 FONTINFO, which the
existing ExtTextOut/GetCharWidth read. Caption buttons draw (minimize, grey
maximize, close): out/s26marlett.png. Put as WINDOWS\FONTS\*.TTF, GDI's own
TrueType side takes another boot path and dies in VMM 0001:0009 - so not there.

**"6"/"3" black.** ExtTextOut's trace now prints fg/bg. Calc's digits always
reach ExtTextOut with fg 001F (blue); "*" "-" with F800. Yet some runs show
them black/grey and others blue (s26t: blue at 30/39/50 s; s26colk: black).
No later DISPLAY call overlaps those rects. Not found; suspect a write outside
DISPLAY (or a present race). Still open.

**W16_SHELL.** w16test writes `shell=%W16_SHELL%` (default calc.exe); k32host
copies calc/notepad/winmine/sol to WINDOWS and cards.dll to SYSTEM (names as
installed: unchanged in the INFs).

**Solitaire** (`W16_SHELL=sol.exe`): boots and deals, out/s26sol_44.png. Fixes:
- USER's GetCodeInfo probes SOL seg 10 through a not-present selector (KRNL386's
  load-on-call/discarded mark, access byte 7B). KRNL386's exception 0Bh handler
  (seg1 0B06) only does __GP recovery (9617 matches the faulting module's
  __GP table), so raising #NP does not load anything. Now lifted memory accesses
  go through vxd32 `recomp_sel_access_base`; for a not-present selector bound to
  a lifted segment, identity.c `w16_np` gives that segment's bytes in the linked
  image (placeholder SEL(n)). Bookkeeping lookups (`recomp_sel_base`) are
  unchanged.
- w16_drv_far and w16_jmpf read a selector's base to look for "PCRD": skipped
  for not-present selectors.
- SOL 4:1644 seed (callback stored as `mov ax,1644h; mov bx,[bp-6]; mov dx,seg`).
A click on the stock (`click 80,120@40`) reaches USER's mouse proc but the
shot shows no change (out/s26solclick_46.png); not looked at further.

**Winmine** (`W16_SHELL=winmine.exe`): faults 1.5 s in, KRNL386 seg1 9002
writing F000FF8B (an unread-yet selector?). Not looked at.

**Notepad**: not run. Its image base is 0x400000, the same as xcopy32.mod
(w16test's own EXE), and it has relocations, so kernel32's loader would move
it off the lifted addresses. Needs a different host EXE base or relocated-image
support in the lift.

**Explorer** (task 4): not started.

ponytail (this session):
- Marlett only, chars 20h..7Fh, mono, host rasterizer on Win98's font file.
- Not-present selector reads use the image's bytes; KRNL386 still thinks the
  segment is unloaded. Writes there would go to the image.
- W16_INPUT: Shift only, no Ctrl/Alt.

**Regression.** check_w16, check_m1, check_calc pass (see commit).

**Next.** (1) Sol's click: trace USER's queue (3:12EF) for the 0201 at 80,120
and sol's wndproc. (2) Winmine's seg1 9002 fault: W16_TRACE around it.
(3) Notepad: host EXE base. (4) Explorer. (5) The black digits.

### Session 27: Win16 tasks wake on input (sol deals and drags), winmine runs, notepad lifted (handoff)

pcrecomp w98 f0490d0, f95c689. Relink work/w16 with the README ne_link line
(now with winmine/sol/cards) after `py -3 tools/w16_seeds.py work/sys >
work/seeds/w16.json` (new WINMINE 1:03EE seed). Relift w16test with the
README line (now + `work/rebased/notepad_1100.exe work/sys/comdlg32.dll`).
No lifter or disasm change, so johnny/civnet were not relifted.

**Sol's click.** USER queued the button messages and PostEvent'd sol's
task, but sol never ran. Calc is a Win32 queue (flag 4 at queue+14h): its
GetMessage waits through KERNEL.460, a kernel32 wait. Sol is Win16: it waits
in KERNEL.30 WaitEvent -> Reschedule, which with nothing ready switches
(VWIN32 01) to the boot task (DGROUP:13E2, TDB 0027). The boot thread was
never parked through start_thread (it runs on the host's own thread), so
`native32_w9x_switch_to` took it for an unstarted thread and started a new
guest thread for its r0 instead of setting its wake event: the boot thread
slept forever. Fixed (vxd32: g_boot_r0 is only ever woken). Then KRNL386's
idle path (seg1 6060..60BA) ran for the first time: int 2Fh 1689h (now a
no-op) and VWIN32 11h (block until a task is ready) / 10h (PostEvent's wake,
seg1 75D5, while DGROUP:13E6 bit 0 says the scheduler idles): one host
event, waited on with the machine given up. A click on the deck deals
(out/s27sol_deal.png), and `drag 570,209,324,119@40` (new W16_INPUT verb:
press, 8 moves, release) puts the ace of hearts on a foundation, score 10
(out/s27sol_drag.png). Gate: `sh tools/check_sol.sh` (reference
work/sol_ref_ace.png cut from out/s27sol_drag.png; the deal is the same
every run).

**Winmine.** The F000FF8B write was KRNL386 killing a task whose LoadModule
failed (seg2 09F7 with error 2 -> seg1 9002 reads TDB+54, the thread, still
0, so [0+38h] is the real-mode vector F000:FF53). Error 2 = an import not
found: winmine imports SOUND (1, 2, 4, 5, 9, 10). SYSTEM.INI now says
`sound.drv=sound.drv` and drv16 has a SOUND stand-in (no device: OpenSound
-1 = S_SERDVNA, the rest no-ops; the MMSOUND stand-in is gone). Then its
window procedure 1:03EE (no export, reached only through RegisterClass) is a
seed, and GDI calls DISPLAY.21 DIBtoDevice for its tiles and LED digits
(drv16 `disp_dibtodev`, clipped; X,Y is the DIB's top-left, the LED strips
come with negative Y and the clip picks the digit). A click uncovers a cell
and the timer runs: out/s27winmine_click.png. Gate: `sh
tools/check_winmine.sh` (mines are random: the clicked cell must differ from
its covered neighbour and the timer digits must change).

Not fixed: the cleanup path itself (seg1 9002 on a task with no thread)
would still fault on any other failed Win16 load.

**Notepad.** Rebased with pcrecomp tools/pe/rebase.py to 0x01100000 and
renamed work/rebased/notepad_1100.exe (lift.py keys catalogs by file name;
the 0x400000 one in work/cat would give 0 functions). k32host holds its
base from the start (as calc's and mprexe's; else a host heap lands there
and kernel32 relocates it) and copies it as WINDOWS\NOTEPAD.EXE; COMDLG32
goes into SYSTEM. With W16_SHELL=notepad.exe its lifted code runs from
011010CC to its init's SetWindowTextA (notepad 1:2155 -> user32 BFF51BD4)
and faults in kernel32 BFF7286D (the 16->32 return: `mov ebp,[bp-20h]` on a
16-bit ss whose base reads as 14DF0000, i.e. the selector value, not a
base): a nested 32->16->32 thunk (KRNL386 2:7616 relay, kernel32 BFF943F0)
unwinds onto another task's stack (ss 14DF -> 148F -> 14D7 in the trace,
"unwound to the far call at ss:sp 148F:8914"). Not looked at further.

**"6"/"3" black.** Without keys (W16_TRACE run, shot at 38 s) 6, 3, 5, 4
are all blue (0,0,255) in the shot; no later draw touches the "6" at
202,212. The black ones (6, *, 3, -, . in one rectangle) show only after the
keys of check_calc: a repaint after a key, not the first paint. Next: trace
from the "+" key on for ExtTextOut/BitBlt into 195..275 x 205..290.

**Explorer**: not started.

ponytail (this session):
- VWIN32 10h/11h: one auto-reset host event for the Win16 idle.
- int 2Fh 1689h: no-op.
- SOUND stand-in: no device, only winmine's six ordinals.
- DIBtoDevice: X,Y read as the whole DIB's top-left; no lpTranslate.
- W16_INPUT drag: 8 even steps, left button only.

**Regression.** check_m1, check_w16, check_calc, check_sol, check_winmine
pass. The gates now delete their old screenshots first (a run that made no
shot could pass on the last one).

**Next.** (1) Notepad's nested-thunk fault (SetWindowTextA from a Win32
task: which stack does the 16->32 return land on). (2) The black digits
after keys. (3) Sol's "Time: 0" (its timer never ticks; winmine's does).
(4) Explorer.

### Session 28: notepad as the shell (types text), timers fire (sol's clock), host selector pool (handoff)

pcrecomp w98 414589d, 6f10482. Relink work/w16 with the README ne_link line
(ne_lift changed: identity mode only), then build. The w16test lift is
unchanged. johnny and civnet were relifted into s28-* scratch trees, in
normal and identity mode, with f95c689 and 414589d. Normal mode: identical C
and mem_image.bin. Identity mode: the same mem_image.bin, and the C differs
only in the new `(far-call shape ...)` check lines.

**Notepad (task 1).** The 16->32 return did not land on another task's
stack. The 16-bit side came back with bp off. USER 1:101E (SetWindowText's
16-bit side) calls 56E5 as `push cs; call near` (a far call into its own
segment). 56E8 pushes 5702, jumps to 570F/5720, and 5720's `ret` goes to
5702, whose `retf` returns to 101E. The lifted call took the C return as
"back at 103E", skipped 5702's code, and popped cx/bp from the wrong slots:
bp 89A0 went to kernel32 0xBFF7286D (`[bp-20h]`), which then got esp = sp
on a stale ss. Fix (ne_lift, identity mode only): after `push cs; call
near`, if ret_ip is not the call's return, run what the callee returned
into (w16_retjump), as the existing ret-jump case does. A 16->32 stub's
return now sets ret_ip to where it came back, because that check reads it
(KRNL386 3:052F). W16_TRACE logs w16_enter's return state (ss/ds/di/bp in
and out), which is how this was found. Notepad as the shell:
out/s28np_hi.png ("Hi!" typed). Gate: `sh tools/check_notepad.sh`.

**Calc's "6"/"3" (task 2).** They are blue (0,0,255) in the PNG pixels,
the same as "5". The Read tool's downscaled preview makes thin blue glyphs
look black. Nothing to fix. Check colours by pixel, not by eye.

**Sol's clock (task 3).** USER's timer proc (USER seg1 1164) ran every tick
but never found a timer due: its tick pointer (DGROUP:0, from VTDAPI 9) was
0007:0000. KRNL386 chains every free LDT descriptor into its own free list
(access 0, the next link in the limit), so the host's bump allocator found
nothing free, wrapped and returned 0007 (entry 0) for *every* later host
selector: drv16's tick dword and timer stack, TDBs from kernel32 thunk 32,
rm_sel, the host selector, xss. Fix: vxd32 reserves LDT F800..FFF8 for the
host (present, base 0) when the LDT alias is set up, before KRNL386 boots,
and hands those out once the bump range is used up. Then sol's TimerProc
came through a MakeProcInstance thunk (`mov ax, ds; jmp far`) in a KRNL386
block: dispatch_far recognises it, without running it. Sol's clock runs
(out/s28sol_clock.png, "Time: 15"), its deal is now seeded from the clock
(it differs every run), and carets blink. check_sol now deals a card from
the deck and checks that the clock changes between two shots; check_notepad
compares the text only up to the caret.

**Not fixed: the failed-load cleanup (task 3b).** KRNL386 seg1 9002
reads TDB+54 (the task's thread) for a task whose LoadModule failed. On
Win98 the thread exists from CreateTask on; here it is made at the task's
first switch-in, so it is 0 there, and [0+38h] reads the real-mode vector.
Next: set TDB+54 when the host makes the TDB (vxd32 krnl386 thunk 32, "new
task"). Which thread value goes there is not looked at yet (no guess made).

**Explorer (task 4): not started.** Plan: lift EXPLORER.EXE plus SHELL32,
SHDOCVW, BROWSEUI, COMCTL32, OLE32, OLEAUT32, SHLWAPI and their imports
from work/sys (Win98 names) into w16test. Rebase any that collide (as with
notepad_1100). Use W16_SHELL=Explorer.exe. Before the first run, add guards:
no host shell, tray, hotkey or AppBar calls (they resolve to lifted
user32/shell32 only), and file views stay inside work/root (vxd32 already
boxes paths).

**hiddenrun.** taskkill /T /F timed out on a guest host that had stopped
in a wait (WMI also hung on it). Every gated run left two w16test.exe
processes, which kept work/root files open: check_m1 then failed with
"no USER32 proxy", and check_w16 with rc=1. hiddenrun now calls
TerminateProcess and then stops every process with the same exe path.

ponytail (this session):
- ne_lift: only `push cs; call near` gets the ret-jump check (the path
  walk in _ret_jumps stops at calls, so it does not find these).
- vxd32 host pool: 256 descriptors, never given back.
- MakeProcInstance thunk: matched only as `B8 imm16 EA off sel`.

**Regression.** check_m1, check_w16, check_calc, check_sol (new form),
check_winmine and check_notepad pass.

**Next.** (1) Explorer, from the plan above. (2) TDB+54 at task creation
(the failed-load cleanup). (3) Notepad's scroll bars show navy
(COLOR_SCROLLBAR? check by pixel). Keys 1 and 2 never repaint calc's
buttons, while + 3 = do. Worth a look.

### Session 29: Explorer runs its own code up to the Start button; Win98's own appearance; TDB+54 (handoff)

pcrecomp w98 ef4ec03 (TDB+54), 428630a (Explorer runtime). Relift w16test with
the README line (now + explorer_1400.exe, shdocvw.dll, rebased ole32.dll), after
`tools/vtseeds.py` for ole32, shdocvw, shell32 and explorer_1400, and run
`py -3 tools/reginst.py w98\xcopy` once (it also empties the box's Run and
RunOnce keys each time). No lifter or disasm change, so johnny and civnet were
not relifted. The shell32 lift changed (seeds), and every gate still passes.

**TDB+54 (task 1).** KRNL386 seg1 9002, the task kill, reads TDB+54 as the
task's kernel32 thread and clears [thread+38h]. Vxd32's thunk 32 (a Win32
task's TDB, made by the host) now sets it from creation. Win16 tasks are made
by KRNL386 itself: their thread comes from kernel32 inside CreateTask
(16->32 stub from 04BF:733x, then VWIN32 8). Winmine's old failure (a missing
SOUND import) killed the task before that point. So that path can still read
0 on another failed Win16 load. A real-Win98 ordering check is still needed:
is the thread made before import resolution there?

**Explorer (task 2), how far it gets.** SYSTEM.INI shell=Explorer.exe. Its
lifted code runs: shell32's Shl3216 thunks, the registry, ole32 and shdocvw
class factories, the tray thread. It stops in the tray's Start-button
setup. USER thunk 28 -> 0B07:01C8 SendMessages into Explorer's 32-bit window
procedure through a KRNL386 window-proc relay (0DB7:0232 -> 2:7616 -> far32
user32 0xBFF53955). The far32's arguments read as 16-bit words
(154F154F 1E280001), and the return (8B4E154F) faults in w16_retf32. This is
the same relay family as session 28's notepad nested thunk; look at the
ss:esp that 2:7616 hands over. No Explorer paint yet: the teal desktop in
the shots is USER's own desktop window, and the earlier "rundll32" message
box (out/s29ex_58.png, from before the RunOnce fix) was drawn by Explorer's
ShellExecute.

Fixed on the way (in order):
- shell32's flat thunks arrive already patched (INSTDATA 0x7FD38508/28 is
  int3 filler in the file). vxd32 recognises the patched QT
  (`33 C9 8A 4D FC 8B 14 8D tbl B8 tgt FF E0`) and FT
  (`0F B6 D1 8B 14 95 tbl 68 tgt C3`) forms. They are never run.
- Explorer ran the host's RunOnce entries. A missing box key falls through
  to the real one (read-only), so Explorer would ShellExecute Windows 11's
  startup lines. reginst creates empty Run/RunOnce/RunOnceEx/RunServices(Once)
  keys under HKLM and HKCU, and skips Active Setup lines.
- USER writes RunOnce *DisplayFallback ("rundll32 ... desk.cpl ,-1") when
  HKCC\Display\Settings is missing (USER seg 21 3A67). reginst writes it:
  800,600, 16 bpp, 96 dpi.
- timezone.inf leaves "!!!First Boot!!!", which makes Explorer WinExec
  timedate.cpl /m (0x0140A1F4). reginst writes TimeZoneInformation from
  Win98's first zone whose bias matches the host's.
- int 21h 5704-5707 (DOS 7 access and creation times); VWIN32 11
  (CreateFileMapping), 1A/1B (Suspend/ResumeThread: a suspended new thread
  starts on its resume), 59 (InterlockedIncrement/Decrement without xadd);
  VMM 9 (UnmapViewOfFile of a shared view).
- ole32 is rebased to 0x7F400000 (its base 0x7FF20000 + C1000 runs into the
  host's 0x7FFE0000 page, so kernel32 relocated it and its DllMain ran
  unlifted, which faulted). browseui is not lifted yet: with it plus the
  first ole32 catalog, the exe (452 MB image) failed CreateProcess with 308.
- tools/vtseeds.py: relocated code pointers in runs of 2+ (or outside .text),
  minus switch tables, become catalog seeds. COM methods are reached only
  through vtables.
- identity: a far call into a not-present selector (gdi32 thunk A5 -> GDI seg
  41, selector 071F, load-on-call) runs the lifted segment named by the
  module's in-memory segment table. The base scan skips not-present
  selectors (their "base" read 01000000).

**Appearance.** Once Display\Settings existed, USER read
HKCU\Control Panel\Colors (and metrics). Those values fell through to
Windows 11's: white faces, a black desktop, a light gradient caption. New in
regbox32: a box key holding a value "pcrecomp:whole" never falls through for
a missing value. reginst marks Colors, Desktop and Desktop\WindowMetrics.
Now windows are Win98 grey (198,195,198), the caption is navy, and the
desktop is teal (0,130,132). Windows sit 8,13 px up and left of the old
references (the old ones used Windows 11's metrics). check_calc and
check_notepad now search ±20 px. check_notepad compares "Hi!" only, because
the caption itself changed. check_winmine compares the clicked cell with its
own earlier image (flood fill could uncover its neighbour too: a flaky gate).

**Guards.** Explorer's windows, tray, hotkeys and shell hooks are Win98
USER's, inside the guest: nothing reaches the host's shell. Its file opens
all go through vxd32's work/root box. The startup lists and appearance keys
are now the box's own.

**Task 3.** Notepad's scroll bars are still navy with Win98's own colours
(out/notepad gate shot), so it is not a host-colour leak. Next: the scroll
bar brush (COLOR_SCROLLBAR is grey 192 by default; probably the stand-in's
pattern or mono brush). Calc keys 1/2: not looked at.

**Task 4.** No check_explorer.sh: no Explorer milestone with a paint yet.

ponytail (this session):
- reginst: AddReg only; flags 1/2/0x10001/0x10000/0x20000 read; time zone
  picked by bias only; Display\Settings values written by hand (setup's
  class installer writes them on Win98).
- VWIN32 11: the handle stays as it is (one DOS handle table).
- VWIN32 1A/1B: a running thread is only counted, not stopped.
- VMM 9: no-op (read as a flush).
- not-present far call: KRNL386 still has the segment unloaded.
- regbox "pcrecomp:whole": the marker shows in an enumeration.
- vtseeds: a lone code pointer inside .text is skipped.

**Regression.** check_m1, check_w16, check_calc, check_sol, check_winmine,
check_notepad pass (calc/notepad/winmine with the new forms above). One
check_m1 run failed with "no USER32 proxy" while a w16test from a trace run
was still alive. It passed alone.

**Next.** (1) The window-proc relay fault in the tray (above). Then the
desktop's first paint (shdocvw's desktop or shell32's) and the taskbar.
(2) Lift browseui (Start menu bands) and keep the exe under the image limit:
try fewer garbage functions (ole32's catalog still has about 14k
unimplemented ops from data decoded as code). (3) Then Start -> Run -> calc.
