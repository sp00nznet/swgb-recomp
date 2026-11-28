# One-click setup for the Star Wars: Galactic Battlegrounds static recompilation. Run it by
# double-clicking Setup.cmd in the repo folder; docs/building.md's "Step by step" is
# the same thing by hand, command for command.
#
# It checks the tools, copies your installed game into game\ (updated to v1.1
# from BGUpdate1_1.exe if it is not), mounts your disc 2 image if you give one, finds every function, lifts Battlegrounds.exe to C, builds
# build\swgbrecomp.exe and leaves a shortcut to it in this folder. Each
# step is skipped when its output already exists (-Force redoes them), so a
# rerun after a failure resumes. Everything it does goes to setup.log.
#
#   -GameDir <installed Game folder>                           skip the question (-Game works too)
#   -PlayDisc <disc 2: drive, .iso or .cue>                    skip the question
#   -PatchExe <BGUpdate1_1.exe>                                skip the question
#   -Yes                                                       answer yes to every install question
#
# The toolkit is ..\pcrecomp (PCRECOMP overrides it). Under recomp-launcher
# (RECOMP_LAUNCHER=1) it never waits for Enter, and exit code 0 means the
# shortcut is there.
param([switch]$Force, [switch]$Yes, [Alias('Game')][string]$GameDir = "", [string]$PlayDisc = "", [string]$PatchExe = "")
$Game = $GameDir

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Toolkit = $env:PCRECOMP
if (-not $Toolkit) { $Toolkit = Join-Path (Split-Path -Parent $Root) 'pcrecomp' }
$env:PCRECOMP = $Toolkit   # run_lift.py reads it too
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
function Log($t) { Add-Content -Path $Log -Value $t -Encoding UTF8 }
Log "==== setup $(Get-Date -Format s)"

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Step($n, $t) { Write-Host ""; Say "[$n/6] $t" 'Cyan' }
function Pause-End { if (-not $Yes -and $env:RECOMP_LAUNCHER -ne '1') { Read-Host "Press Enter to close" | Out-Null } }
$Mounted = $null
function Fail($t) {
  if ($Mounted) { Dismount-DiskImage -ImagePath $Mounted -ErrorAction SilentlyContinue | Out-Null }
  Say ""; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Pause-End; exit 1
}
trap { Fail "$_" }
function Ask($q) { if ($Yes) { Say "$q yes"; return $true }; $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# Run a program with its output in the log. Windows PowerShell turns a native
# program's stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)   # @(): a one-element slice would splat as characters
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) {
  Say "  $what..."
  $code = Exec $cmd
  if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}

if ($env:RECOMP_LAUNCHER -ne '1') { Clear-Host }
Say "Star Wars: Galactic Battlegrounds: static recompilation setup" 'White'
Say "You need your own copy: the game installed from its US disc 1, disc 2 (or an .iso/.cue of it), LucasArts' BGUpdate1_1.exe if the install is v1.0, and about 8 GB free."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools"
# A Python that answers "Python 3.x". The Store's placeholder "python" (the
# one that opens the Store) answers nothing, so asking is the reliable test.
function Find-Python {
  $user = @(Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | ForEach-Object { ,@($_.FullName) })
  foreach ($c in (@(@('py', '-3'), @('python'), @('python3')) + $user)) {
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
  if (-not $pyargs) { Fail "Python installed, but it can't be found. Install Python 3.12 from python.org yourself." }
}
Say "  Python: $($pyargs -join ' ')"

if ((Exec ($pyargs + @('-c', 'import capstone'))) -ne 0) {
  Say "  The Python package capstone is missing (about 15 MB)."
  if (-not (Ask "  Install it now (pip, for your user only)?")) { Fail "capstone is required." }
  Run "Installing capstone" ($pyargs + @('-m', 'pip', 'install', '--user', 'capstone'))
}

if (-not (Test-Path (Join-Path $Toolkit 'tools\lift\lift32.py'))) {
  Say "  The pcrecomp toolkit is not beside this folder ($Toolkit)."
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    if (-not (Ask "  git is needed to fetch it. Install Git for Windows now (winget, about 60 MB)?")) { Fail "git is needed to fetch pcrecomp." }
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing. Install Git for Windows yourself." }
    Exec @('winget', 'install', '-e', '--id', 'Git.Git', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
    Refresh-Path
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "Git did not install. Install Git for Windows yourself." }
  }
  if (-not (Ask "  Download it now (git, about 20 MB)?")) { Fail "pcrecomp is required at $Toolkit." }
  Run "Cloning pcrecomp" @('git', 'clone', '--depth', '1', 'https://github.com/sp00nznet/pcrecomp', $Toolkit)
}
# What this game needs from the toolkit, each by a line it adds: MSVC
# exception frames, the extent walk's seeds (run_lift.py uses both), the
# headless desktop and the DirectPlay for LAN games, all in pcrecomp main.
$need = @(
  @('MSVC exception frames (eh32.frame)', 'tools\lift\eh32.py', 'def frame('),
  @('the extent walk with seeds (generate.py)', 'tools\lift\generate.py', 'seeds=()'),
  @('the headless desktop', 'runtime\native32\native32.c', 'native32_headless'),
  @('the DirectPlay (dplay32)', 'runtime\native32\dplay32.c', 'dplay32_create_iid'))
function Missing { @($need | Where-Object {
  $f = Join-Path $Toolkit $_[1]
  -not ((Test-Path $f) -and (Select-String -Path $f -Pattern $_[2] -SimpleMatch -Quiet)) } | ForEach-Object { $_[0] }) }
$missing = Missing
# A checkout on main may move forward; any other branch is someone's work and is left alone.
$head = Join-Path $Toolkit '.git\HEAD'
if ($missing -and (Test-Path $head) -and (Get-Content $head) -eq 'ref: refs/heads/main') {
  if (Ask "  Your pcrecomp is missing $($missing -join ', '). Update it (git pull)?") {
    Run "Updating pcrecomp" @('git', '-C', $Toolkit, 'pull', '--ff-only')
    $missing = Missing
  }
}
if ($missing) {
  Fail "your pcrecomp at $Toolkit is missing $($missing -join ', '). Update it to main, or point PCRECOMP at such a checkout."
}
Say "  pcrecomp: $Toolkit"

# Visual Studio 2022 or its Build Tools, with the C++ x86 compiler; CMake and
# Ninja ship inside that workload, so build.cmd needs nothing else.
function Find-VS {
  foreach ($p in @("${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
                   "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe")) {
    if (Test-Path $p) {
      $vs = & $p -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
      if ($vs) { return $vs }
    }
  }
  return $null
}
if (-not (Find-VS)) {
  Say "  Visual Studio 2022 with the C++ tools is not installed. The recompiled game is C, and this is the compiler."
  if (-not (Ask "  Install the Visual Studio 2022 Build Tools with the C++ workload now (winget, about 3 GB)?")) { Fail "a C compiler (Visual Studio 2022 C++) is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing. Install Visual Studio 2022 Build Tools with 'Desktop development with C++' yourself." }
  Exec @('winget', 'install', '-e', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '--accept-package-agreements', '--accept-source-agreements',
         '--override', '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') | Out-Null
  if (-not (Find-VS)) { Fail "the Build Tools did not install. Install Visual Studio 2022 with 'Desktop development with C++' yourself." }
}
Say "  C compiler: $(Find-VS)"

# ---------------------------------------------------------------- the game
Step 2 "Copying your copy of Galactic Battlegrounds into game\"
# game\ is the installed game (Battlegrounds.exe beside Data\GENIE.DAT). Disc 2,
# the play disc (Game\Avi), stays in a CD-ROM drive: the real disc, or its
# image mounted. The game checks for it each time it starts.
function Is-Game($d) { $d -and (Test-Path (Join-Path $d 'Battlegrounds.exe')) -and (Test-Path (Join-Path $d 'Data\GENIE.DAT')) }
function Is-Disc2($d) { $d -and (Test-Path (Join-Path $d 'Game\Avi')) -and -not (Test-Path (Join-Path $d 'GameData')) }
if ((Is-Game 'game') -and (Test-Path 'work\Battlegrounds.exe') -and -not $Force) {
  Say "  game\ is already there (skipping)."
} else {
  $Game = $Game.Trim('"', ' ')
  if (-not $Game) {
    foreach ($d in @("${env:ProgramFiles(x86)}\LucasArts\Star Wars Galactic Battlegrounds\Game",
                     "$env:ProgramFiles\LucasArts\Star Wars Galactic Battlegrounds\Game")) {
      if (Is-Game $d) { $Game = $d; Say "  Found the installed game in $Game"; break }
    }
  }
  if (-not $Game -and $Yes) { Fail "the installed game wasn't found. Give its Game folder with -GameDir." }
  while (-not $Game) {
    $Game = (Read-Host "  Where is the installed game? Its Game folder (with Battlegrounds.exe)").Trim('"', ' ')
  }
  if (-not (Is-Game $Game)) { Fail "$Game has no Battlegrounds.exe with Data\GENIE.DAT beside it. Install the game from disc 1 and point Setup at its Game folder." }
  New-Item -ItemType Directory -Force game, work | Out-Null
  Say "  Copying the installed game (about 500 MB)..."
  Copy-Item -Recurse -Force (Join-Path $Game '*') game\
}

# v1.1, the last official update: its two changed files come out of LucasArts'
# BGUpdate1_1.exe without running it (tools\bgupdate.py).
function Stamp { $b = [IO.File]::ReadAllBytes((Resolve-Path 'game\Battlegrounds.exe')); $pe = [BitConverter]::ToInt32($b, 0x3C); [BitConverter]::ToUInt32($b, $pe + 8) }
if ((Stamp) -ne 0x3C03FD69) {
  $Patch = $PatchExe.Trim('"', ' ')
  if (-not $Patch) {
    foreach ($d in @($Root, (Split-Path -Parent $Root), (Join-Path $env:USERPROFILE 'Downloads'))) {
      $f = Get-ChildItem $d -File -Filter 'BGUpdate1_1.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($f) { $Patch = $f.FullName; Say "  Found $Patch"; break }
    }
  }
  if (-not $Patch -and $Yes) { Fail "game\Battlegrounds.exe is not v1.1. Give LucasArts' BGUpdate1_1.exe with -PatchExe." }
  while (-not $Patch) { $Patch = (Read-Host "  The game is v1.0. Where is BGUpdate1_1.exe (the v1.1 update)?").Trim('"', ' ') }
  Run "Unpacking the v1.1 update" ($pyargs + @('tools\bgupdate.py', $Patch, 'work\patch'))
  Copy-Item -Force 'work\patch\Eng\Game\Battlegrounds.exe' game\
  Copy-Item -Force 'work\patch\Common\Game\Data\GENIE.DAT' game\Data\
  if ((Stamp) -ne 0x3C03FD69) { Fail "the update did not give the v1.1 Battlegrounds.exe. Is it the US English BGUpdate1_1.exe?" }
}
Copy-Item -Force game\Battlegrounds.exe work\

$Cd = $null
foreach ($d in (Get-PSDrive -PSProvider FileSystem)) { if (Is-Disc2 $d.Root) { $Cd = $d.Root; break } }
if ($Cd -and -not $PlayDisc) {
  Say "  Disc 2 is in $Cd."
} else {
  $Cd = $PlayDisc.Trim('"', ' ')
  if (-not $Cd -and $Yes) { Fail "disc 2 (the play disc) wasn't found. Put it in the drive, or give its .iso or .cue with -PlayDisc." }
  while (-not $Cd) { $Cd = (Read-Host "  Where is disc 2, the play disc? Its drive (E:\), an .iso or a .cue").Trim('"', ' ') }
  # A .cue/.bin image becomes an .iso (its data track), which Windows mounts.
  if ($Cd -match '\.cue$') {
    New-Item -ItemType Directory -Force work | Out-Null
    if (-not (Test-Path 'work\disc2.iso') -or $Force) { Run "Converting $(Split-Path -Leaf $Cd) to work\disc2.iso" ($pyargs + @('tools\cue2iso.py', $Cd, 'work\disc2.iso')) }
    $Cd = Join-Path $Root 'work\disc2.iso'
  }
  if ($Cd -match '\.iso$') {
    # Windows mounts an ISO itself; no tool needed. Left mounted for playing.
    $img = Get-DiskImage -ImagePath (Resolve-Path $Cd).Path
    if (-not $img.Attached) { $img = Mount-DiskImage -ImagePath $img.ImagePath -PassThru }
    $Cd = (($img | Get-Volume).DriveLetter + ':\')
    Say "  Mounted the image as $Cd (it stays mounted for playing)"
  }
  if (-not (Is-Disc2 $Cd)) { Fail "$Cd is not disc 2 (it has no Game\Avi)." }
}

# ---------------------------------------------------------------- catalog
Step 3 "Finding every function (about ten minutes)"
if ((Test-Path 'work\functions.json') -and -not $Force) { Say "  Already done (skipping)." }
else { Run "Disassembling Battlegrounds.exe" ($pyargs + @((Join-Path $Toolkit 'tools\disasm\disasm32.py'), 'work\Battlegrounds.exe', '-o', 'work\functions.json')) }

# ---------------------------------------------------------------- lift
Step 4 "Recompiling Battlegrounds.exe to C (two to five minutes)"
if ((Test-Path 'src\recomp\gen\recomp_dispatch.c') -and -not $Force) { Say "  Already done (skipping)." }
else { Run "Lifting" ($pyargs + @('run_lift.py', '--all')) }

# ---------------------------------------------------------------- build
Step 5 "Building build\swgbrecomp.exe (30 minutes or more: 4M lines of C)"
if ((Test-Path 'build\swgbrecomp.exe') -and -not $Force) { Say "  Already built (skipping)." }
else {
  $env:CMAKE_ARGS = "-DPCRECOMP=$($Toolkit -replace '\\', '/')"
  Run "Compiling" @('cmd', '/d', '/c', "`"$Root\build.cmd`"")
}

# ---------------------------------------------------------------- shortcut
Step 6 "Making the shortcut"
$lnk = Join-Path $Root 'Galactic Battlegrounds.lnk'
$sh = New-Object -ComObject WScript.Shell
$s = $sh.CreateShortcut($lnk)
$s.TargetPath = Join-Path $Root 'build\swgbrecomp.exe'
$s.Arguments = '--run'
$s.WorkingDirectory = $Root
$s.IconLocation = (Join-Path $Root 'game\Battlegrounds.exe') + ',0'
$s.Save()
Say "  $lnk"
Say "  The CD check passes with disc 2 in the drive or its image mounted (double-click the .iso after a restart)." 'Yellow'

if (-not (Test-Path $lnk)) { Fail "the shortcut was not made." }
Write-Host ""
Say "Done. Double-click 'Galactic Battlegrounds' in this folder to start it." 'Green'
Pause-End
exit 0
