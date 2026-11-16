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
