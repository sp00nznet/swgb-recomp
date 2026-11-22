# Building and the command line

`Setup.cmd` (`tools/setup.ps1`) runs exactly the steps below, skipping the ones
already done, and logs everything to `setup.log`. It clones pcrecomp main if it is not beside this folder; main
carries the toolkit work this game needs (MSVC `__try/__except`,
frame-pointer-omitted C++ frames, throws from a statically linked CRT,
`VirtualQuery` on lifted images, the headless desktop).

Linux and macOS: not yet playable. A native host on pcrecomp's `win32hle`
(no Wine) is designed and scaffolded: `src/linux`, `cmake/native.cmake`,
`build-linux.sh` and `build-macos.sh`. [linux-macos.md](linux-macos.md) has
the design, the gaps, the build steps and the state. `setup.sh` still stops.

## Step by step

Prerequisites: Windows 10 or 11; Python 3.10+ (`py -3 --version` should say
`Python 3.1x`; if `python` opens the Microsoft Store, that is the Store alias:
use `py -3` or turn the alias off in Settings); `py -3 -m pip install
capstone`; Visual Studio 2022 or its Build Tools with "Desktop development
with C++" (CMake and Ninja come with it); ffmpeg on `PATH` for `--record`.

1. Fetch the toolkit beside this folder:
   ```
   git clone https://github.com/sp00nznet/pcrecomp ..\pcrecomp
   ```
2. Copy the installed `Game` folder to `game\` (it has `Battlegrounds.exe`
   and `Data\GENIE.DAT`). To play, put disc 2 in the drive or mount its image
   (double-click the `.iso`; a `.cue`/`.bin` becomes one with
   `py -3 tools\cue2iso.py disc2.cue disc2.iso`). `CD_DRIVE=E` or `--cd E`
   names the drive if there is more than one.
3. If `game\Battlegrounds.exe` is v1.0, apply the update without running it:
   ```
   py -3 tools\bgupdate.py path\to\BGUpdate1_1.exe work\patch
   copy work\patch\Eng\Game\Battlegrounds.exe game\
   copy work\patch\Common\Game\Data\GENIE.DAT game\Data\
   ```
   Expected: `unpacked the v1.1 update into work\patch`.
4. `copy game\Battlegrounds.exe work\`, then catalog it (about ten minutes):
   ```
   py -3 ..\pcrecomp\tools\disasm\disasm32.py work\Battlegrounds.exe -o work\functions.json
   ```
   Expected, at the end: `Functions: 14272` and `Wrote work/functions.json`.
5. Lift it:
   ```
   py -3 run_lift.py --all
   ```
   Expected: `lifted 14273   not-lifted stubs 0   errors 0` and about 4.0M
   lines of C in `src\recomp\gen\`.
6. Build it (30 minutes or more on one machine): `build.cmd`. Expected, at
   the end: `Linking C executable swgbrecomp.exe`.
7. Check it without starting the game: `build\swgbrecomp.exe` maps and binds
   the image and prints `(dry run: image mapped and bound; ...)`.
8. Run: `build\swgbrecomp.exe --run`.

Trip-ups: a different build of `Battlegrounds.exe` is refused by `run_lift.py`
by its PE timestamp; a pcrecomp checkout on `main` lacks the EH support
(`AttributeError: module 'eh32' has no attribute 'frame'`).

## The command line

```
build\swgbrecomp.exe --run                                    windowed, with sound
build\swgbrecomp.exe --run --headless --record out.mp4        no window, record the frames
build\swgbrecomp.exe --run --headless --shot 20 menu.bmp      save the frame after 20 s
build\swgbrecomp.exe --run --keys "10:move:330:240,11:click:330:240"   scripted mouse and keys
build\swgbrecomp.exe --run --headless --watchdog 60 --files   stop after 60 s, log every file opened
py -3 tools\conformance.py                                    the milestones, PASS/FAIL
```

`--args "..."` passes switches to the game itself; `--game`, `--cd` and
`--exe` point at other folders. `NATIVE32_TRACE_EH=1` logs every guest
exception and where it lands.

## Build options

`build.cmd` configures CMake with Ninja for 32-bit MSVC and builds
`build\swgbrecomp.exe`. `-DGEN_OPT=/Od` (in `CMAKE_ARGS`) compiles the
generated C unoptimized, much faster; the default is `/O1`. On a
[recomp-netlab](https://github.com/sp00nznet/recomp-netlab) farm:
`netlab build swgb-recomp` (recipe `projects/swgb-recomp.env`, `/Od`), which
lands the build in `build-farm\`.

Layout: `run_lift.py` (lift driver), `src/runtime/` (host, `softdd`
DirectDraw, the deflate decoder), `tools/` (setup, the update unpacker, the
conformance harness), `third_party/miniz` (MIT, see `NOTICE`), and the
gitignored `game/`, `cd/`, `work/` and `src/recomp/gen/`.
