#!/bin/sh
# The same setup as Setup.cmd, from Git Bash or MSYS2 on Windows. Windows 98
# here is a 32-bit Windows program built with MSVC (x86), so there is no Linux
# or macOS build: this only hands its options to tools/setup.ps1.
#
#   ./setup.sh                 # every step (a rerun skips the finished ones)
#   ./setup.sh --iso FILE      # your Windows 98 SE .iso (else one in this folder)
#   ./setup.sh --force         # redo every step
#   ./setup.sh --yes           # install what is missing without asking
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) ;;
  *) echo "Windows 98 recomp builds and runs on Windows only (MSVC x86): run Setup.cmd there." >&2; exit 1 ;;
esac
cd "$(dirname "$0")" || exit 1
set --  "$@" end
for a; do
  shift
  case "$a" in
    end) ;;
    --force) set -- "$@" -Force ;;
    --yes) set -- "$@" -Yes ;;
    --iso) iso=1 ;;
    *) if [ "$iso" = 1 ]; then set -- "$@" -Iso "$(cygpath -w "$a")"; iso=; else
         echo "usage: ./setup.sh [--iso FILE] [--force] [--yes]" >&2; exit 2; fi ;;
  esac
done
exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$PWD/tools/setup.ps1")" "$@"
