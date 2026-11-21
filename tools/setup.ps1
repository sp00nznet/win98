# One-click setup for the Windows 98 SE static recompilation.
# Run it by double-clicking Setup.cmd in the repo folder; the README's
# "Building by hand" is the same thing, command for command.
#
# From your own Windows 98 SE CD image it extracts the CD and its CABs into
# work\, rebases three modules, writes the catalog seeds, links the 16-bit
# modules, lifts the 32-bit ones to C, builds build\w16test.exe, fills the
# guest's registry box from Win98's INFs, lays out its C:\ with the Start Menu,
# and leaves "Windows 98 (recomp).cmd", which starts Explorer. Each step is
# skipped when its output already exists (-Force redoes them). Everything it
# does goes to setup.log. -Yes installs what is missing without asking; -Iso is
# your .iso, else the one in this folder (or under work\iso) is used.
# RECOMP_LAUNCHER=1 skips the "Press Enter" at the end.
param([switch]$Force, [switch]$Yes, [string]$Iso = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
# The toolkit: PCRECOMP, else ../pcrecomp (cloned when missing).
$Toolkit = if ($env:PCRECOMP) { $env:PCRECOMP } else { Join-Path (Split-Path -Parent $Root) 'pcrecomp' }
$T = Join-Path $Toolkit 'tools'
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
$Steps = 11
function Log($t) { try { Add-Content -Path $Log -Value $t -Encoding UTF8 -ErrorAction Stop } catch {} }
Log "==== setup $(Get-Date -Format s)"

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Pause-End { if (-not $env:RECOMP_LAUNCHER) { Read-Host "Press Enter to close" | Out-Null } }
if ($Root -match '&') {
  Say "This folder's path has an '&' in it, which the build cannot handle:" 'Red'
  Say "  $Root" 'Red'
  Say "Move the folder somewhere else (for example C:\win98-recomp) and run Setup.cmd from there." 'Yellow'
  Pause-End; exit 1
}
function Step($n, $t) { Write-Host ""; Say "[$n/$Steps] $t" 'Cyan' }
function Fail($t) {
  Say "" ; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Pause-End; exit 1
}
function Ask($q) { if ($Yes) { Say "$q yes"; return $true }; $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }   # Enter = yes
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# Run a program with its output in the log. Windows PowerShell turns a native
# program's stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)
  # one write per command: a line at a time is minutes for a CAB's file list
  Log ((& $cmd[0] @rest 2>&1 | ForEach-Object { "$_" }) -join "`r`n")
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) {
  Say "  $what..."
  $code = Exec $cmd
  if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}
function Skip($path) { return ((Test-Path -LiteralPath $path) -and -not $Force) }

Clear-Host
Say "Windows 98 SE - static recompilation setup" 'White'
Say "You need your own Windows 98 SE CD image (.iso) and about 4 GB free."
Say "Lifting and building take about an hour, once."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools the pipeline needs"
function Find-Python {
  foreach ($c in @(@('py', '-3'), @('python'), @('python3'))) {
    if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $rest = @($c | Select-Object -Skip 1)
    $v = (& $c[0] @rest --version 2>&1 | Out-String).Trim()
    $ErrorActionPreference = $old
    if ($v -match '^Python 3\.(\d+)' -and [int]$Matches[1] -ge 10) { return ,$c }
  }
  return $null
}
$pyargs = Find-Python
if (-not $pyargs) {
  Say "  Python 3.10 or newer is not installed."
  if (-not (Ask "  Install Python 3.12 now (winget, for your user only, about 30 MB)?")) { Fail "Python 3 is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail "winget is missing. Install 'App Installer' from the Microsoft Store, or install Python yourself."
  }
  Exec @('winget', 'install', '-e', '--id', 'Python.Python.3.12', '--scope', 'user', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  $pyargs = Find-Python
  if (-not $pyargs) { Fail "Python installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}
Say "  Python: $($pyargs -join ' ')"

# pillow: tools\hiddenrun.py (the hidden first boot) and the check_*.sh gates
if ((Exec ($pyargs + @('-c', 'import pefile, capstone, PIL'))) -ne 0) {
  Say "  The Python packages pefile, capstone and pillow are missing (about 25 MB)."
  if (-not (Ask "  Install them now (pip, for your user only)?")) { Fail "pefile, capstone and pillow are required." }
  Run "Installing pefile, capstone and pillow" ($pyargs + @('-m', 'pip', 'install', '--user', 'pefile', 'capstone', 'pillow'))
}

function Find-7z {
  $c = Get-Command 7z -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  foreach ($d in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
    if ($d -and (Test-Path (Join-Path $d '7-Zip\7z.exe'))) { return (Join-Path $d '7-Zip\7z.exe') }
  }
  return $null
}
$7z = Find-7z
if (-not $7z) {
  Say "  7-Zip (it opens the .iso and the CD's CAB files) is not installed."
  if (-not (Ask "  Install 7-Zip now (winget, about 2 MB)?")) { Fail "7-Zip is required." }
  Exec @('winget', 'install', '-e', '--id', '7zip.7zip', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  $7z = Find-7z
  if (-not $7z) { Fail "7-Zip did not install. Install it from https://www.7-zip.org, then run Setup.cmd again." }
}
Say "  7-Zip: $7z"

if (-not (Test-Path (Join-Path $T 'ne\ne_link.py'))) {
  Say "  The pcrecomp toolkit is not beside this folder ($Toolkit)."
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "git is needed to fetch pcrecomp. Install Git for Windows, then run Setup.cmd again." }
  if (-not (Ask "  Download it now (git, about 20 MB)?")) { Fail "pcrecomp is required at $Toolkit." }
  Run "Cloning pcrecomp" @('git', 'clone', '--depth', '1', 'https://github.com/sp00nznet/pcrecomp', $Toolkit)
}
# Win98 needs the Win9x VxD runtime (vxd32.c) and Win16 identity mode.
$tkOk = { (Test-Path (Join-Path $Toolkit 'runtime\native32\vxd32.c')) -and (Test-Path (Join-Path $Toolkit 'runtime\win16\identity\identity.c')) }
if (-not (& $tkOk) -and (Test-Path (Join-Path $Toolkit '.git'))) {
  Say "  Your pcrecomp is older than this build needs."
  if (Ask "  Update it now (git pull)?") { Run "Updating pcrecomp" @('git', '-C', $Toolkit, 'pull', '--ff-only') }
}
if (-not (& $tkOk)) { Fail "your pcrecomp at $Toolkit has no Windows 98 runtime (vxd32.c, win16\identity). Update it: git -C `"$Toolkit`" pull" }
$env:PCRECOMP = $Toolkit
Say "  pcrecomp: $Toolkit"

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) } else { $null }
if (-not $vs) {
  Say "  Visual Studio 2022 with the C++ tools is not installed."
  if (-not (Ask "  Install the Visual Studio 2022 Build Tools with the C++ workload now (winget, about 3 GB, asks for admin)?")) {
    Fail "Visual Studio 2022 (or its Build Tools) with 'Desktop development with C++' is needed. Install it, then run Setup.cmd again."
  }
  Exec @('winget', 'install', '-e', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '--accept-package-agreements', '--accept-source-agreements',
         '--override', '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') | Out-Null
  $vs = if (Test-Path $vswhere) { (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) } else { $null }
  if (-not $vs) { Fail "the Visual Studio Build Tools did not install. Install them with 'Desktop development with C++', then run Setup.cmd again." }
}
Say "  Visual Studio: $vs"
foreach ($tool in @(@('cmake', 'Kitware.CMake', '30 MB'), @('ninja', 'Ninja-build.Ninja', '1 MB'))) {
  if (Get-Command $tool[0] -ErrorAction SilentlyContinue) { continue }
  Say "  $($tool[0]) is not installed."
  if (-not (Ask "  Install $($tool[0]) now (winget, about $($tool[2]))?")) { Fail "$($tool[0]) is required." }
  Exec @('winget', 'install', '-e', '--id', $tool[1], '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  if (-not (Get-Command $tool[0] -ErrorAction SilentlyContinue)) { Fail "$($tool[0]) installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}

# ---------------------------------------------------------------- the CD
New-Item -ItemType Directory -Force work | Out-Null
Step 2 "Extracting your Windows 98 SE CD into work\cd"
if (Skip 'work\cd\win98\BASE4.CAB') { Say "  Already done (skipping)." }
else {
  $Iso = $Iso.Trim('"', ' ')
  # a folder holding the .iso (what the launcher passes) is that .iso
  if ($Iso -and (Test-Path -LiteralPath $Iso -PathType Container)) { $i = @(Get-ChildItem -LiteralPath $Iso -Filter *.iso -File)[0]; if ($i) { $Iso = $i.FullName } }
  if (-not $Iso) {
    $found = @(Get-ChildItem -Path $Root -Filter *.iso -File) + @(if (Test-Path work\iso) { Get-ChildItem -Path work\iso -Filter *.iso -File -Recurse })
    if ($found.Count -gt 0) { $Iso = $found[0].FullName; Say "  Found $Iso" }
  }
  if (-not $Iso -and $Yes) { Fail "no .iso given or found in $Root. Give it with -Iso." }
  while (-not ($Iso -and (Test-Path -LiteralPath $Iso -PathType Leaf))) {
    $Iso = (Read-Host "  Paste the path of your Windows 98 SE .iso").Trim('"', ' ')
  }
  Run "Extracting $(Split-Path -Leaf $Iso)" @($7z, 'x', '-y', '-owork\cd', $Iso)
  if (-not (Test-Path 'work\cd\win98\BASE4.CAB')) { Fail "$Iso is not a Windows 98 SE CD (no WIN98\BASE4.CAB on it)." }
}

Step 3 "Extracting the CD's CAB files into work\sys (a few minutes)"
if (Skip 'work\sys\.extracted') { Say "  Already done (skipping)." }
else {
  $cabs = @(Get-ChildItem -Path work\cd\win98 -Filter *.CAB -File | Sort-Object Name)
  Say "  $($cabs.Count) CABs..."
  # A file that spans two CABs reports as a sub-item error in the first (7-Zip
  # exit code 2); the files this build needs are checked below.
  foreach ($c in $cabs) {
    $code = Exec @($7z, 'e', '-y', '-owork\sys', $c.FullName)
    if ($code -ne 0) { Log "  $($c.Name): 7-Zip exit code $code" }
  }
  foreach ($f in 'kernel32.dll', 'KRNL386.EXE', 'explorer.exe', 'xcopy32.mod') {
    if (-not (Test-Path "work\sys\$f")) { Fail "no $f in the CABs: is this the Windows 98 SE CD?" }
  }
  Set-Content -Path 'work\sys\.extracted' -Value (Get-Date -Format s)
}

# ---------------------------------------------------------------- prepare
Step 4 "Rebasing notepad, Explorer and ole32 (seconds)"
if ((Skip 'work\rebased\notepad_1100.exe') -and (Skip 'work\rebased\explorer_1400.exe') -and (Skip 'work\rebased\ole32.dll')) { Say "  Already done (skipping)." }
else {
  # each lifted module needs its own address: notepad and Explorer off 0x400000
  # (the host's own EXE), ole32 off 0x7FFE0000 (Windows 11's shared user page)
  Run "Rebasing" ($pyargs + @("$T\pe\rebase.py", 'work\sys', 'work\rebased', 'notepad.exe=0x01100000', 'explorer.exe=0x01400000', 'ole32.dll=0x7F400000'))
  Move-Item -Force 'work\rebased\notepad.exe' 'work\rebased\notepad_1100.exe'
  Move-Item -Force 'work\rebased\explorer.exe' 'work\rebased\explorer_1400.exe'
}

Step 5 "Writing the catalog seeds into work\seeds (seconds)"
if ((Skip 'work\seeds\w16.json') -and (Skip 'work\seeds\kernel32.dll.json') -and (Skip 'work\seeds\shdoc401.dll.json')) { Say "  Already done (skipping)." }
else { Run "Seeds" ($pyargs + @('tools\seeds.py')) }

Step 6 "Lifting the 16-bit modules: KRNL386, USER, GDI and friends (a few minutes)"
if (Skip 'work\w16\mem_image.bin') { Say "  Already done (skipping)." }
else {
  $env:NE_IDENTITY_SEL = '1'
  $m = @('KRNL386.EXE', 'GDI.EXE', 'USER.EXE', 'DDEML.DLL', 'MSGSRV32.EXE', 'shell.new', 'kommctrl.dll', 'winmine.exe', 'sol.exe', 'cards.dll') | ForEach-Object { "work\sys\$_" }
  Run "ne_link" ($pyargs + @("$T\ne\ne_link.py", '--work', 'work\w16', '--seeds', 'work\seeds\w16.json', '--shims', "$Toolkit\runtime\win16\identity") + $m)
  Remove-Item Env:NE_IDENTITY_SEL
}

Step 7 "Lifting the 32-bit modules to C (30 to 60 minutes, once)"
$sys = { param($l) $l | ForEach-Object { if ($_ -like '*\*') { "work\$_" } else { "work\sys\$_" } } }
$lifts = [ordered]@{
  'calc'    = @('calc.exe')
  'notepad' = @('notepad.exe')
  'xcopy'   = @('xcopy32.mod', 'kernel32.dll')
  'w16test' = @('xcopy32.mod', 'kernel32.dll', 'gdi32.dll', 'advapi32.dll', 'user32.dll', 'mpr.dll', 'mprexe.exe', 'mprserv.dll',
                'mspwl32.dll', 'calc.exe', 'msvcrt.dll', 'shlwapi.dll', 'comctl32.dll', 'shell32.dll', 'rebased\notepad_1100.exe',
                'comdlg32.dll', 'rebased\explorer_1400.exe', 'shdocvw.dll', 'rebased\ole32.dll', 'browseui.dll', 'shdoc401.dll')
}
foreach ($k in $lifts.Keys) {
  if (Skip "src\gen\$k\recomp_dispatch.c") { Say "  ${k}: already done (skipping)."; continue }
  $n = $lifts[$k].Count
  Run "Lifting $k ($n module$(if ($n -gt 1) { 's' }))" ($pyargs + @('tools\lift.py', $k) + (& $sys $lifts[$k]))
}

Step 8 "Building build\w16test.exe (10 to 30 minutes)"
# Always: the build only compiles what changed (seconds when nothing did).
if ($Force -and (Test-Path build)) { Remove-Item -Recurse -Force build }
$buildCmd = Join-Path $Root 'build.cmd'
Run "Compiling" @('cmd', '/c', ('call "' + $buildCmd + '"'))
if (-not (Test-Path 'build\w16test.exe')) { Fail "the build made no build\w16test.exe." }

Step 9 "Filling the guest's registry box from Win98's INFs (seconds)"
# HKCU\Software\pcrecomp\w98\xcopy, nothing else (pcrecomp regbox32)
$box = 'HKCU:\Software\pcrecomp\w98\xcopy\HKLM'
if ((Test-Path $box) -and -not $Force) { Say "  Already done (skipping)." }
else { Run "reginst" ($pyargs + @('tools\reginst.py', 'w98\xcopy')) }

Step 10 "Laying out the guest's C:\ (work\root) and its Start Menu (seconds)"
if (Skip 'work\root\WINDOWS\Start Menu\Programs\Accessories\Notepad.lnk') { Say "  Already done (skipping)." }
else {
  # The host builds work\root on start; it is booted once on a hidden desktop,
  # muted, for that, then setup's shortcuts are written as grpconv would.
  $env:PCRECOMP_MUTE = '1'; $env:HIDDENRUN_DESKTOP = 'pcrecomp_w98'
  Run "First boot (hidden)" ($pyargs + @('tools\hiddenrun.py', '--secs', '8', '--out', 'work\firstboot', '--', 'build\w16test.exe', '--mute'))
  if (-not (Test-Path 'work\root\WINDOWS\EXPLORER.EXE')) { Fail "the first boot did not lay out work\root (see work\firstboot.log)." }
  Run "Start Menu" ($pyargs + @('tools\startmenu.py'))
}

Step 11 "Making the launcher"
$lnk = Join-Path $Root 'Windows 98 (recomp).cmd'
Set-Content -Path $lnk -Encoding ASCII -Value @(
  '@echo off',
  'rem Starts Windows 98 SE''s own Explorer, recompiled, in a window (muted; close the window to quit).',
  'cd /d "%~dp0"',
  'set W16_WINDOW=1',
  'set W16_LIBMAIN=KERNEL',
  'set W16_SHELL=Explorer.exe',
  'start "" build\w16test.exe --mute')
Say "  $lnk"

Write-Host ""
Say "Done. Double-click 'Windows 98 (recomp).cmd' to start Windows 98's Explorer." 'Green'
Pause-End
