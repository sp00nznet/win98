# Changelog

## Unreleased

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
