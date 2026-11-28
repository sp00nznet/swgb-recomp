# Roadmap

## Next
- **pcrecomp's shared presenter** (the window, F10 settings, F11 fullscreen,
  F12 scaling, the pad) once it is in pcrecomp main; the resolution and the
  LAN host in its F10 menu.
- The end-of-game screens at the remaster's sizes (not looked at yet).
- A resolution that follows the window, changed while a game runs.
- The minimap shows the units and buildings but not the explored terrain
  (so since 0.1.0, at every size).
- Play a skirmish to a finish; campaign 1 mission 1.
- Movies: the intro and campaign films go through `MCIWndCreateA` from the
  play disc; check they play, and that headless runs keep them off screen.
- Child windows (the game's text fields are real Win32 edit controls) do not
  appear in softdd's frames; draw them into the picture.
- netlab QA on a test VM (it reports SKIP without game data).

## Later
- **Linux and macOS**: a Wine route (clang-cl or mingw cross-build, as in
  redalert2-recomp's `setup.sh --wine`), tried and tested before `setup.sh` offers it.
- The Clone Campaigns expansion (`battlegrounds_x1.exe`).

## Out of scope
- Shipping any game file or generated source.
- The Zone.com service, which no longer exists.
