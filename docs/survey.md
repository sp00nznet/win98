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
