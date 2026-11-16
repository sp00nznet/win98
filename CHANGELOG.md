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
