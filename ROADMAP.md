# Roadmap

## Next
- Play a skirmish for real: train workers, build, a few minutes against the AI.
- Campaign 1 mission 1.
- The mouse cursor (the game draws its own from `mcursors`) in recordings.
- netlab QA on a test VM (it reports SKIP without game data).

## Feature tracks, most feasible first

Each track says what is known, the work, and the risk. The order is by
feasibility: what is already half there, then what is mechanical, then
what needs research or a new subsystem.

### 1. The game's own resolutions (800x600, 1024x768, 1280x1024)
- **Known:** softdd takes any mode the game asks for. Options -> Screen
  Size -> 1024 x 768, then a Standard Game: `SetDisplayMode(1024, 768, 8)`,
  a 1024x768 primary, and the map and HUD laid out for it (checked
  2026-11-21). The menus stay 800x600, as in the original.
- **Work:** check 1280x1024 the same way; a scripted run per size in
  `docs/testing.md`; screenshots against the original.
- **Risk:** low.

### 2. Presentation: scaling, fullscreen, pacing
- **Known:** windowed, the frame is `StretchDIBits`-ed to the whole client
  area (85% of the screen height at 4:3, or `--scale N`), so a window
  resized to another shape distorts it, and there is no smoothing choice
  or fullscreen. A primary without a back buffer presents on every
  `Unlock`/`ReleaseDC`: hundreds of presents a second in the menus.
- **Work:** aspect-correct letterboxing; nearest, integer and smooth
  scaling (pcrecomp's `win32hle/screen.c` has these modes for SDL2 to
  borrow from); a borderless fullscreen toggle (Alt+Enter) that never
  changes the desktop's mode; coalesce presents to the display's refresh.
  Mouse mapping (`to_game`) and the text-field composition follow the
  letterbox.
- **Risk:** low. All of it is in `softdd.c` and `host.c`.

### 3. Widescreen and any resolution
- **Known:** the game offers three 4:3 sizes and picks the mode from them;
  softdd draws any size. The community does this for Clone Campaigns by
  patching the exe (Expanding Fronts, the All-In-One patch, the WSGF
  patch); their changes are a map of which constants matter.
- **Work:** find where the Screen Size choice becomes `SetDisplayMode`'s
  arguments and the checks on it; a host option (`--res WxH`, or the
  desktop's size) that replaces the 1280x1024 entry; then the layout: the
  HUD's interface art is drawn for 800, 1024 and 1280 wide, so the panels
  need placing (or a filled gap) at other widths, and the map view's
  clipping, scrolling limits and the minimap need checking.
- **Risk:** medium. The mode is easy; a HUD that looks right is the work.

### 4. Clone Campaigns
- **Known:** the Steam install has `battlegrounds_cc.exe` without the
  SteamStub wrapper (no `.bind` section, `.text` entropy 6.5 against 8.0
  for the wrapped `battlegrounds_x1.exe`), PE stamp `0x3CF57142`
  (2002-05-29), the same 14 DLLs and 258 imports as the base game, image
  0x3B1000 bytes (fits below 0x00A5A000). `host.c` has no exe-specific
  address but the image base.
- **Work:** confirm `battlegrounds_cc.exe` is the same program as the
  unwrapped `battlegrounds_x1.exe` (`tools/steamstub_swgb.py`, then
  compare); teach `run_lift.py` a second build: its own catalog, its own
  generated sources and target, the ~50 per-exe seeds (the `THIS_COD`
  blitter's jump tables and the rest) found again, its EH frames; the
  `_x1` data (`genie_x1.dat`, `*_x1.drs`, `language_x1.dll`). Every softdd
  fix is shared.
- **Risk:** medium, mostly time: a second 4M-line lift and build. It
  unlocks most community work (Expanding Fronts, the widescreen patches)
  as references.

### 5. Higher frame rates
- **Known:** nothing measured yet. present counts are not frames (see 2).
  Expanding Fronts advertises 60 fps, so the original runs below that in
  some states.
- **Work:** count real frames in a skirmish (Flips per second against the
  wall clock); find the engine's limiter (the `timeGetTime` loop around
  `0x00428A80` is a first lead); see what is tied to it (simulation speed,
  animation, scrolling). Raise the cap only where drawing is independent
  of the simulation tick.
- **Risk:** medium-high. If the simulation steps per drawn frame, a higher
  rate changes game speed and multiplayer sync; a 2D sprite engine has
  nothing to interpolate.

### 6. Cutscenes
- **Known:** the game plays its films through `MCIWndCreateA` (MSVFW32).
  The AVIs are Indeo 5 (`IV50`), which Windows 10 and 11 no longer ship
  a decoder for, and an MCIWnd is a child window, which only reaches the
  picture through softdd's composition. The Steam install has only Clone
  Campaigns' `xintro.avi` and `xlogo1.avi`; the base game's campaign films
  are on disc 2 (`Game\Avi`), which Steam does not have.
- **Work:** shim the MCIWnd calls the game makes; decode the video and
  audio with FFmpeg's libraries (Indeo 5 is supported) into softdd frames
  and DirectSound; skip on Esc or a click; the same path headless and in
  `--record`. Test with the Clone Campaigns intro, then disc 2's films.
- **Risk:** medium. A new subsystem, and base-game films need disc 2 to
  test.

### 7. Linux and macOS
- **Known:** pcrecomp's `win32hle` implements Win32 on libc, POSIX and
  SDL2: DirectDraw, DirectSound, GDI with fonts, windows and their controls,
  a scaling SDL2 screen. Tiberian Sun, Red Alert 2 and Civilization III run
  on it natively on Linux (Red Alert 2 on macOS too), with no Wine. The
  whole lift passes gcc's front end as it is (54 files, 64-bit based, no
  errors), and gcc `-O1` builds it in minutes. win32hle answers 237 of the
  game's 258 imports; `src/linux/shims.c` answers the other 21.
- **Design** ([docs/linux-macos.md](docs/linux-macos.md)): a second host,
  `src/linux`, on win32hle. Its DirectDraw and `screen.c` replace softdd
  there, and softdd stays the Windows one. Portable pieces are shared
  (`inflate.c` through `guest.h`, the `--keys` grammar in `keyscript.c`).
  `cmake/native.cmake` is the build, reached from `CMakeLists.txt` on Linux
  and macOS.
- **Done:** that scaffolding; the native host and shims, checked with gcc
  against win32hle; no change on Windows.
- **Work:** toolchain (`gcc-multilib`, i386 SDL2 and SDL2_ttf), then the
  link and the bring-up loop to the main menu. 8-bit surface text in the
  palette, and EDIT painting plus child-window walking (both pcrecomp
  changes, in a pcrecomp worktree). Text-field composition through
  `hle_dd_frame_hook`. MCI `cdaudio` over the Steam Ogg tracks, which also
  gives Windows music and covers Clone Campaigns' `WIN32.dll` (which needs
  no lift). Movies on Track 6's portable decoder core. Exceptions only if a
  run raises. `setup.sh` and the harness on Linux. Then macOS (64-bit
  based, needs a Mac).
- **Estimate:** about 3 to 4 weeks to a playable Linux build; macOS one
  more week.
- **Risk:** medium. The hard part exists, and the rest is the bring-up loop
  the sibling ports went through.

## Later
- DirectPlay multiplayer over LAN.

## Done
- Text in 8-bit surfaces' DCs (labels, buttons, the HUD) in the palette in
  use.
- Text fields: the game's EDIT controls composed into softdd's frames, with
  the caret; no ghost box; focus kept across a trip to another window.

## Out of scope
- Shipping any game file or generated source.
- The Zone.com service, which no longer exists.
