# Building and the command line

`Setup.cmd` (`tools/setup.ps1`) runs exactly the steps below, skipping the ones
already done, and logs everything to `setup.log`. It clones pcrecomp main if it is not beside this folder; main
carries the toolkit work this game needs (MSVC `__try/__except`,
frame-pointer-omitted C++ frames, throws from a statically linked CRT,
`VirtualQuery` on lifted images, the headless desktop, the DirectPlay over
TCP).

Linux and macOS: not yet. The host runs the lifted game inside a 32-bit
Windows process against the real Windows DLLs (pcrecomp `native32`), and the
Wine route has not been tried; `setup.sh` says so and stops.

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
   Expected: `lifted 14299   not-lifted stubs 0   errors 0` and about 4.0M
   lines of C in `src\recomp\gen\`.
6. Build it (30 minutes or more on one machine): `build.cmd`. Expected, at
   the end: `Linking C executable swgbrecomp.exe`.
7. Check it without starting the game: `build\swgbrecomp.exe` maps and binds
   the image and prints `(dry run: image mapped and bound; ...)`.
8. Run: `build\swgbrecomp.exe --run`.

Trip-ups: a different build of `Battlegrounds.exe` is refused by `run_lift.py`
by its PE timestamp; an older pcrecomp checkout lacks the EH support
(`AttributeError: module 'eh32' has no attribute 'frame'`) or the DirectPlay
(`runtime/native32/dplay32.c` not found): pull it.

## The command line

```
build\swgbrecomp.exe --run --res 1920x1080                    the game at 1920x1080 (docs/hires.md)
build\swgbrecomp.exe --run --classic                          the original game: 800x600
build\swgbrecomp.exe --run --lan-host 192.168.1.20            look for LAN games there too (docs/multiplayer.md)
build\swgbrecomp.exe --run                                    windowed, with sound
build\swgbrecomp.exe --run --headless --record out.mp4        no window, record the frames
build\swgbrecomp.exe --run --headless --shot 20 menu.bmp      save the frame after 20 s
build\swgbrecomp.exe --run --keys "10:move:330:240,11:click:330:240"   scripted mouse and keys
build\swgbrecomp.exe --run --headless --watchdog 60 --files   stop after 60 s, log every file opened
py -3 tools\conformance.py                                    the milestones, PASS/FAIL
```

`--args "..."` passes switches to the game itself; `--game`, `--cd` and
`--exe` point at other folders. `NATIVE32_TRACE_EH=1` logs every guest
exception and where it lands. `SWGB_FPS=N` holds the game to N frames a
second (60; 0 for as fast as it can, where its clock runs fast).

`swgbrecomp.ini`, beside the program, keeps settings between runs; the
command line wins over it:

```ini
[swgb]
resolution=1920x1080      ; or screen (the screen's size), or game (Options, Screen Size)
lan_host=192.168.1.20     ; where else to look for LAN games
```

## Build options

`build.cmd` configures CMake with Ninja for 32-bit MSVC and builds
`build\swgbrecomp.exe`. `-DGEN_OPT=/Od` (in `CMAKE_ARGS`) compiles the
generated C unoptimized, much faster; the default is `/O1`. On a
[netlab](https://github.com/sp00nznet/netlab) farm:
`netlab build swgb-recomp` (recipe `projects/swgb-recomp.env`, `/Od`), which
lands the build in `build-farm\`.

Layout: `run_lift.py` (lift driver), `src/runtime/` (host, `softdd`
DirectDraw, `hires` sizes, the deflate decoder), `tools/` (setup, the update unpacker, the
conformance harness), `third_party/miniz` (MIT, see `NOTICE`), and the
gitignored `game/`, `cd/`, `work/` and `src/recomp/gen/`.
