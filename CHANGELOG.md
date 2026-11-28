# Changelog

All notable changes. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- High resolution and widescreen (`docs/hires.md`): any size from 1024x720
  to 4096x4096 (`--res WxH`, `--res screen`, or `resolution=` in
  `swgbrecomp.ini`), more of the map on screen. The game lays out and draws
  for the nearest of its own sizes (its size compares see that size:
  `run_lift.py` CLASS_SITES); the host moves its in-game panels to the real
  size as they are placed, leaves its dialogs centred, and widens the bottom
  panel's picture (`src/runtime/hires.c`).
- LAN multiplayer (`docs/multiplayer.md`): the game's DirectPlay is
  pcrecomp's `dplay32` (sp00nznet/pcrecomp#208) in place of Windows', which
  no longer finds games between machines; games are found by a LAN broadcast,
  or at `--lan-host` (`lan_host=`). The game's version check of `dplayx.dll`
  is answered by the host (sub_00477DA0).
- `--classic`: the original game, 800x600.
- `swgbrecomp.ini` beside the program (`[swgb] resolution=`, `lan_host=`).
- `--keys`: `rclick:X:Y`, `hold:KEY:SECONDS`, `f11`/`f12`; `--profile`, a
  sampling profile of the lifted code; the watchdog prints the game's state.

### Fixed
- The game ran a fifth fast. Drawing as fast as it could, about a thousand
  frames a second, its clock gained: 1.81 game seconds a second at Normal,
  where the game means 1.5. The software DirectDraw now holds it to 60 frames
  a second, as a display's refresh did (`SWGB_FPS`), and the clock keeps
  1.5.
- The game's own 1024x768 and 1280x1024: starting a game at either crashed
  (the mouse pointer centred while suspended across the mode change).
- The arrow keys scroll the map in a scripted run (`GetKeyboardState`).
- A scripted key typed its letter twice ("ab" came out "aabb").
- `--record` with `--watchdog`: the end of the run closed the video under
  the game's write of a frame, and the process died (0xC0000409) before its
  log was flushed.
- `--headless`: a game that died that way was started again by the process
  that launched it, on the visible desktop; its exit code is reported now.

### Changed
- The window takes the shape of the game's size when it changes (the 4:3
  menus, then a widescreen battle), keeping its height.
- `--record` records at the remaster's size, the 800x600 menus scaled into
  it; it took the first frame's size, and a game at another size came out
  garbled.
- `docs/testing.md`: test runs go in a logged-on session (a scheduled task);
  started straight from SSH the battle draws one frame and stops.

## [0.1.0] - 2026-11-20

- Builds against pcrecomp main; the `pcrecomp-ingame` branch is merged.

### Changed
- Brought in line with the other recomps: the README in their order (Getting
  Started, Status, Screenshots, Documentation, Contributors, License), the
  build steps and command line moved to `docs/building.md`, `NOTICE` renamed
  `NOTICE.md` and saying what the MIT license covers and what it doesn't.
- Setup and the launcher fetch pcrecomp on `pcrecomp-ingame` (was `pcrecomp-msvceh`, which lacks the headless desktop).

### Added
- `CONTRIBUTING.md`, `CONTRIBUTORS.md`, `docs/building.md`, `docs/testing.md`.
- `setup.sh`, which says Linux and macOS are not supported yet and stops.
- `.github/workflows/installer.yml`, the recomp-launcher installer for
  Windows, run by hand only (`workflow_dispatch`).

### Added
- Lift driver for `Battlegrounds.exe` v1.1 (`run_lift.py`): whole program,
  14,273 functions, 0 lift errors, 4.0M lines of C, with MSVC exception
  frames (needs pcrecomp branch `pcrecomp-msvceh`, now inside `pcrecomp-ingame`).
- Host on pcrecomp native32 (`src/runtime/host.c`): scripted mouse and
  keys (`--keys`), headless runs
  with `--record`, `--shot`, `--watchdog`, `--files`, muted test runs.
- `softdd`: X-Wing's software DirectDraw without Direct3D; 8-bit modes from
  800x600 to 1280x1024 enumerated, 8-bit recording through the palette.
- The game's deflate decoder done by the host with miniz
  (`src/runtime/inflate.c`; the original is a stack-copying coroutine).
- `tools/bgupdate.py`: unpacks LucasArts' `BGUpdate1_1.exe` without running it.
- `Setup.cmd`, launcher manifest, `tools/conformance.py` (5 milestones, 5/5),
  netlab recipe (`projects/swgb-recomp.env` in recomp-netlab).
- Reaches the main menu; hover and the Exit button work.
- In game: a random-map skirmish starts and plays headless (dialogs painted on
  the private desktop, scripted keys into the focused edit control, the
  `THIS_COD` blitter's jump tables
  seeded).

### Changed
- The CD check runs against a real CD-ROM drive: disc 2 in the drive or its
  image mounted (`CD_DRIVE` or `--cd E` names the drive; the host stores that
  letter in `CDPath`). Setup no longer copies disc 2 into `cd\`; it mounts an
  `.iso` or `.cue`/`.bin` of it and leaves it mounted.

### Fixed
- The lift read only `.text`, so functions in `THIS_COD` were empty bodies
  that never popped their return address (docs/lifting.md).
