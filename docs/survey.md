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
