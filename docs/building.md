# Building and the command line

`Setup.cmd` (`tools/setup.ps1`) runs exactly the steps below, skipping the ones
already done, and logs everything to `setup.log`. It clones pcrecomp main if it is not beside this folder; main
carries the toolkit work this game needs (MSVC `__try/__except`,
frame-pointer-omitted C++ frames, throws from a statically linked CRT,
`VirtualQuery` on lifted images, the headless desktop).

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
build\swgbrecomp.exe --run --fullscreen --filter integer      borderless fullscreen, whole-number scaling
py -3 tools\conformance.py                                    the milestones, PASS/FAIL
```

### The picture: scaling, fullscreen, pacing

The game draws a fixed frame (800x600, 1024x768, 1280x1024: always 4:3); the
window can be any shape. The frame is fitted into the client area with its
aspect kept and black bars, and the mouse is mapped through that same
rectangle (a click in a bar lands on the nearest edge of the picture).

| Flag | Hotkey | What |
|---|---|---|
| `--filter sharp\|smooth\|nearest\|integer` | `Alt+F12` cycles | `sharp` (default): nearest inside a texel, a one-pixel blend at its edges, so no shimmer and no blur at fractional scales. `smooth`: bilinear. `nearest`: nearest neighbour at any size (uneven pixels at odd scales). `integer`: nearest, whole multiples only (bars around, falls back to a fit when the window is smaller than the frame). The window title shows the filter. |
| `--fullscreen` | `Alt+Enter` toggles | A borderless window the size of the monitor the window is on. The desktop's display mode is never changed, so Alt+Tab and other monitors behave as for any window. |
| `--scale N` | | The window starts at N times the game's mode (default: 85% of the screen height). Resize it freely after that. |
| `--refresh HZ` | | Pace frames to HZ instead of the monitor's refresh rate. |
| `--present-log` | | Once a second on stderr: presents, frames shown, the refresh rate and filter. |

The game presents far more often than a display refreshes (every
`Unlock`/`ReleaseDC` of a primary without a back buffer: tens of thousands a
second while loading, a handful in an idle menu). A frame is drawn when a
refresh interval has passed since the last one; otherwise it is held, and a
one-shot timer draws the latest after the interval, so the last frame before
the game goes quiet is never lost. `--shot`, `--record` (30 fps by the wall
clock) and `--frames` still see every frame they need, and the game's text
fields are composed into the frame that is drawn.

For scripted tests (`--keys`, see testing.md), all headless: `size:W:H`
resizes the window's client area, `wshot=FILE` writes the window as it would
be drawn (bars and all, any filter), `wmove:X:Y` and `wclick:X:Y` take window
pixels and map them like the mouse does, and `alt+enter` and `alt+f12` press
the hotkeys.

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
DirectDraw, `present` for scaling, fullscreen and pacing, the deflate decoder), `tools/` (setup, the update unpacker, the
conformance harness), `third_party/miniz` (MIT, see `NOTICE`), and the
gitignored `game/`, `cd/`, `work/` and `src/recomp/gen/`.
