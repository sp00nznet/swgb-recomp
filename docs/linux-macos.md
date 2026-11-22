# Linux and macOS, native

The plan for running the recompiled game as a native Linux program (then a
native Mac one) with no Wine: the same lifted C as the Windows build, on
pcrecomp's `runtime/win32hle` instead of `runtime/native32`. This is how
Tiberian Sun, Red Alert 2 and Civilization III run on Linux, and Red Alert 2
on macOS (their `src/linux`). This page has the design, every gap with its
fix, the build steps and the effort. **State (2026-11-21): designed and
scaffolded. The lifted C compiles under gcc; nothing has been linked or run
on Linux yet** (no 32-bit toolchain on the machine; see *What is done*).

## The design

| | Windows (today) | Linux and macOS |
|---|---|---|
| Lifted C | `src/recomp/gen`, MSVC `/O1` | the same files, gcc or clang `-O1` |
| Runtime | pcrecomp `native32`: imports go to the real DLLs | pcrecomp `win32hle`: imports are answered in C on libc, POSIX and SDL2 |
| Host | `src/runtime/host.c` | `src/linux/main.c`, `src/linux/shims.c` |
| DirectDraw | `softdd.c` (Win32 GDI underneath) | win32hle's `ddraw.c`, shown by its `screen.c` |
| Memory | 32-bit process, guest address = host address | Linux `-m32`: the same (flat). macOS and 64-bit Linux: the guest's 4 GB is one block of the host's (based) |
| Shared, portable | `inflate.c` (through `guest.h`), `keyscript.c` | the same files |

The Windows host stays as it is. The native host is a second, separate
host, as in the sibling projects. The two hosts share every piece that has
no Win32 in it: the deflate decoder (`inflate.c`) and the `--keys` grammar
(`keyscript.c`). `src/runtime/guest.h` picks the runtime for those, and
gives them `GUEST_PTR(va)`, which is a cast on Windows and `hle_ptr` (flat
or based) on win32hle.

`CMakeLists.txt` sends Linux and macOS to `cmake/native.cmake` before its
Windows-only checks, so the Windows configuration is unchanged.

### DirectDraw: win32hle's, not softdd

softdd stays the Windows DirectDraw, and the native build uses win32hle's.
softdd is written against Win32 itself: DIB sections, memory DCs, GDI text
into 8-bit surfaces, `StretchDIBits`, and `WM_PRINT` into the frame for the
text fields. Host C on win32hle cannot call those, because every Win32 call
there is a guest-ABI shim. win32hle's `ddraw.c` already has what the game
needs: 8- and 16-bit surfaces, palettes, colour key, stretch, flip, a
virtual display mode, and `GetDC` through its own GDI. Its `screen.c`
already has what Track 2 is adding to softdd: sharp, smooth, CRT, nearest
and integer scaling, letterboxing, F11 fullscreen and mouse mapping. Making
softdd portable would mean writing a GDI for it, which win32hle already is.

What softdd does that win32hle's DirectDraw does not do yet is listed under
the gaps below: palette-true text in 8-bit surface DCs, and the text fields.
`--shot` and `--record` are the native host's own (`main.c`, through
`hle_dd_frame`), as in Red Alert 2's.

### The lifted C under gcc

Checked 2026-11-21 against the shared lift (`A:\Games\star\swgb-recomp\src\recomp\gen`,
54 files, 238 MB of C), with Ubuntu 24.04's gcc 13 in WSL:

- `-fsyntax-only`, 64-bit based (`RECOMP_MEM_BASE`, the macOS model): **all
  54 files clean, no errors**. The generated C has no `__declspec`, no
  inline asm, no MSVC intrinsics. `recomp_types.h` already has gcc's side of
  every MSVC-specific spot: statement expressions, `x86intrin.h`, and
  `cpuid`.
- `-O1 -c` of `recomp_0000.c` (2.9 MB): 11 s. If time scales with size, the
  whole lift is about 15 minutes on one core and 2 to 3 minutes on eight.
  MSVC takes more than 30 minutes.
- `-m32` (flat) is untested: the machine has no multilib. Red Alert 2 and
  Tiberian Sun build their lifts both ways from the same generator.
- `src/linux/*.c`, `keyscript.c` and `inflate.c` against `win32hle.h`:
  clean under `-Wall -Wextra`.

The flags are Red Alert 2's: `-fno-strict-aliasing -fwrapv` (x86 integer
and aliasing semantics), and on win32hle the indirect-call caches and the
x87 ring (`SWGB_FAST_ICALLS`). The lift writes `_ST(i)`, which the ring
needs.

## The import census

`Battlegrounds.exe` makes 258 imports from 14 DLLs. win32hle answers 237 of
them by name or ordinal (WSOCK32's by its `SHIM2` aliases). The other 21
are answered in `src/linux/shims.c`, registered ahead of win32hle:

| Import | Answer here | Final answer |
|---|---|---|
| `MCIWndCreateA` (MSVFW32) | NULL: no movie, as on a PC without the films | Track 6's player (below) |
| `ICInfo` (MSVFW32) | FALSE: no codecs | the same |
| `mciSendCommandA` (WINMM) | `MCIERR_INVALID_DEVICE_NAME`: no CD music | CD audio from the Ogg tracks (below) |
| `DPLAYX` #1, #2, #4 | `DPERR_UNAVAILABLE`, no providers | the same: DirectPlay is out of scope |
| `IsDBCSLeadByte`, `GetPriorityClass`, `WinExec` | code page 1252, normal priority, refused | the same |
| `GetLastActivePopup`, `GetWindowThreadProcessId`, `IsClipboardFormatAvailable`, `SystemParametersInfoA`, `SetClassLongA`, `GetCaretBlinkTime`, `DrawTextExA`, `SetSysColors`, `IsIconic` | the smallest true answer (`DrawTextExA` is `DrawTextA`) | the same, or moved into win32hle |
| `CreateICA` (GDI32) | a 1x1 32-bit DC | the same |
| `ImmGetOpenStatus`, `ImmGetDefaultIMEWnd` | no input method | the same |

`shims.c` also does, the win32hle way, what `host.c` does on Windows:
`GetDriveTypeA` and `GetLogicalDrives` show the play disc as a CD-ROM,
`CreateFileA` and `OpenFile` are wrapped for `--files` and `on:NAME`, and
`MessageBoxA` answers itself on a test run.

## The gaps, and their fixes

### 1. `host.c`'s Win32 specifics

Each one, and what it becomes on win32hle:

| `host.c` | Why on Windows | Native |
|---|---|---|
| `GetModuleHandleA`, `GetModuleFileNameA`, `GetCommandLineA`, `LoadIconA`, `LoadCursorA` shims | the process is the host, the module the guest | win32hle's module table: the image is added as `C:\Battlegrounds.exe` (`hle_module_add`), resources come from it, `hle_set_command_line` |
| Window subclassing (`host_wndproc`) | the fullscreen window, windowed: always active, mouse scaled from the client area, clicks given to the text fields | `screen.c` maps the mouse onto the virtual display. win32hle hit-tests the field itself (`hle_window_at`). **Still to check:** whether losing focus pauses the game (win32hle's `WM_ACTIVATEAPP` on SDL focus) |
| `GetGUIThreadInfo` in the `--keys` thread | keys go to the focused text field | `hle_input_key` already posts to the focus window |
| `native32_headless`'s private desktop | windows that are visible get `WM_PAINT`, but on no screen | win32hle generates `WM_PAINT` itself, and `hle_screen_open(headless)` opens no window |
| The `--keys` thread (`PostMessageA`, `g_vk_until`) | scripted input | `keyscript.c` (the same grammar) and `hle_input_mouse`/`hle_input_key` (`main.c`) |
| `CreateMutexA` with `--multi` | one copy per desktop | win32hle's named objects are per process: `--multi` is accepted and does nothing |
| `cd_pick` (drive letters, `RegSetKeyValueA`) | the play disc | `--cd DIR` or `$SWGB_CD` (disc 2 mounted, or copied) becomes `D:`, and `CDPath` is seeded in win32hle's registry |
| `mute_process` (WASAPI session) | test runs are silent | `hle_dsound_set_master(0)`, and SDL's dummy audio driver when headless |
| Vectored exception handler, `TerminateProcess` | the fault report | `sigaction` on an alternate stack, `_exit` |
| `SetDllDirectoryA` (`language.dll`) | the string DLL beside the game | win32hle's `LoadLibraryA` maps a resource DLL from the game folder |

**Separating host.c.** The rules for parallel work forbid moving code
around in `host.c` while Tracks 2, 3 and 6 edit it. So it is unchanged for
now, and its `--keys` parser is a second copy of `keyscript.c`'s grammar.
Once those tracks have merged, `host.c` should parse with `keyscript.c`
instead. That is a deletion with no change in behaviour.

### 2. Text fields (EDIT controls)

The brief said win32hle has no EDIT control. It has one now: `user32.c`'s
`edit_proc` keeps the text, the selection, the focus, `EM_*` and `EN_*`. But
its `WM_PAINT` draws nothing. On Windows, softdd composes each visible child
into a copy of the frame with `WM_PRINT` and draws the caret
(`softdd.c`, `compose`). The native equivalent:

- **pcrecomp (`user32.c`):** the EDIT draws itself into a DC on `WM_PAINT`
  and `WM_PRINT`: its font (`WM_SETFONT`), the colours its parent chooses
  on `WM_CTLCOLOREDIT` (the game's procedure, guest code), the selection,
  and the caret, blinking at `GetCaretBlinkTime`. Plus a host-side way to
  walk a window's children (`hle_window_child`/`hle_window_next`, or
  `GetWindow` exposed).
- **here (`src/linux/fields.c`, new):** `hle_dd_frame_hook` returns a copy
  of the primary with each visible child printed into it at its place. This
  is `compose` from softdd, against `hle_gdi_surface_dc`.

Effort: 2 to 3 days. The pcrecomp half is useful to any game with text
fields over DirectDraw.

### 3. Text in 8-bit surfaces

The game draws its labels, buttons and HUD text with GDI into 8-bit
surfaces' DCs. softdd needed the current palette loaded into the DC to get
the right colours (fixed on main, `sync_dib_palette`). win32hle's
`gdidc.c` maps a colour to an 8-bit index as a grey ramp ("nothing blits
into an 8bpp DIB yet").

**Fix (pcrecomp):** `ddraw.c`'s `GetDC` hands the surface's palette, or else
the primary's, to `hle_gdi_surface_dc` as the colour table, and `gdidc.c`
matches colours against it (nearest entry, cached per palette). Effort: 1
day.

### 4. Movies (MCIWnd): shared with Track 6

The game plays its films through `MCIWndCreateA`: a child window over its
own. On Windows, Track 6 is building a player that decodes with FFmpeg's
libraries into softdd frames and DirectSound. For this port to reuse it,
the player needs a portable core:

- the decoder (FFmpeg: Indeo 5 video, PCM or ADPCM audio) has no Win32 in
  it, and gives frames and audio to two callbacks;
- the MCIWnd shim and the window are per host. On Windows the frames go to
  softdd and the sound to DirectSound or waveOut. Natively, the frames go
  into the primary (through `hle_dd_frame_hook`, like the text fields) and
  the sound to `hle_audio_stream_*`, as win32hle's `bink.c` does with
  ffmpeg;
- skip on Esc or a click, headless and `--record` the same.

Until then `shims.c` returns NULL, and the game goes on without the film.
Effort after Track 6: 1 to 2 days.

### 5. Music: `WINMM`'s CD audio, and Steam's `WIN32.dll`

The lifted `Battlegrounds.exe` (v1.1, `0x3C03FD69`) imports the real
`WINMM.dll` and plays its music from disc 2's audio tracks through
`mciSendCommandA` (`cdaudio`). Steam's `WIN32.dll` is imported only by the
Clone Campaigns executables (`battlegrounds_cc.exe`, `battlegrounds_x1.exe`,
Track 4). It is a winmm wrapper: 151 exports, the waveOut and timer ones
forwarded, and the MCI CD calls answered by playing `Music\TrackNN.ogg`
through `libvorbisfile-3.dll`.

So it does not need to be lifted. win32hle binds imports by name whatever
the DLL, so `WIN32.dll`'s waveOut and `time*` imports already land on
win32hle's winmm. What remains for both executables is one module: **MCI
`cdaudio` over the Steam release's Ogg tracks.** It handles `MCI_OPEN`,
`MCI_SET` (TMSF), `MCI_STATUS` (number of tracks, length, position, mode),
`MCI_PLAY` from/to, `MCI_STOP`, `MCI_PAUSE`, `MCI_CLOSE` and the notify
message. It decodes with stb_vorbis (one public-domain file in
`third_party`, so no 32-bit libvorbis package is needed) and plays on
`hle_audio_stream_*` natively, or on waveOut on Windows.

The Windows build gains from it too: a Steam install has no audio CD, so
today the recompiled game has no music there. Effort: 2 days, testable on
Windows first.

### 6. Exceptions

`host.c` sets `native32_guest_seh = 1`. The game has 702 C++ frames and 14
`__try` frames in its CRT, and the lift gives them landing pads (setjmp,
`RECOMP_EH_FRAME`: portable). win32hle's `RaiseException` ends the run
("guest exceptions are not handled"). The game never catches a C++
exception itself (`docs/exceptions.md`), so a normal run should never
raise. If one does, the fix is to port `native32/seh32.c` and `eh32.c`'s
dispatch, which is driven by guest data, onto win32hle's `RaiseException`
and `RtlUnwind`. That is a pcrecomp change, about 2 days, done only if a
run needs it.

### 7. Things found only by running it

The sibling ports each found a dozen of these (`civ3/docs/linux.md`). This
game's likely ones:

- **The registry.** On Windows the game reads the installer's keys (paths,
  `CDPath`). win32hle's registry starts empty, seeded by the host:
  `main.c` sets `CDPath`. Others will show up as reads that fail.
- **Case and paths.** `hle_path.c` matches components without case, but the
  game also builds paths from `GetCurrentDirectoryA` and from registry
  values.
- **Activation.** See subclassing above.
- **Timing.** `timeGetTime`, `Sleep(0)` and `GetVerticalBlankStatus` pacing
  (softdd fakes a 60 Hz blank; check `ddraw.c`'s).

Each is the usual permissive-bind loop: run, read the stub's name or the
fault report, answer it.

### 8. DirectPlay

Out of scope, as on Windows (`ROADMAP.md`, *Later*). `shims.c` refuses
`DirectPlayCreate` and `DirectPlayLobbyCreateA`, and enumerates no service
providers.

### 9. macOS

A Mac runs 64-bit programs only, so the guest's memory is one 4 GB block at
`0x100000000000` (`SWGB_MEM_BASE`, `cmake/native.cmake`). The lifted C
already compiles that way (above), and win32hle is written for it: every
guest pointer goes through `hle_ptr` and `hle_va`. In this repo,
`inflate.c` was the one host file that cast guest addresses to pointers,
and it now uses `GUEST_PTR`. `main.c` and `shims.c` use only win32hle's
accessors. Red Alert 2 needed nothing more for the Mac than its build
script and the fixed base. It needs a Mac to try: Xcode's command line
tools and Homebrew's `cmake ninja pkgconf sdl2 sdl2_ttf`.

## Building

You need a lift (`run_lift.py`, on any OS with Python and capstone), and the
game folder as `game/`.

**Linux** (Debian or Ubuntu, including WSL2):

```
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install gcc-multilib cmake ninja-build pkg-config fonts-liberation \
                 libsdl2-dev:i386 libsdl2-ttf-dev:i386
./build-linux.sh
build-linux/swgbrecomp --run --game game --cd /media/$USER/SWGB2
```

`CMAKE_ARGS=-DWITH_FFMPEG=ON` adds movies through ffmpeg (also
`libavformat-dev:i386 libavcodec-dev:i386 libswresample-dev:i386
libavutil-dev:i386`). `-DSWGB_GEN_LIB=path/libswgb_gen.a` links a lift built
in another worktree, as `swgb_gen.lib` does on Windows.

**macOS:**

```
brew install cmake ninja pkgconf sdl2 sdl2_ttf
./build-macos.sh
build-macos/swgbrecomp --run --game game
```

The command line is the Windows host's: `--headless`, `--record`,
`--frames`, `--shot`, `--keys`, `--watchdog`, `--files`, `--exe`, `--game`,
`--args`, plus `--cd DIR` (a folder, not a letter), `--fullscreen` and
`--scale sharp|smooth|crt|nearest|integer` (F11 and F12 in the window).

## What is done (2026-11-21)

- `src/runtime/guest.h`; `inflate.c` on it (no change on Windows: rebuilt,
  and the headless run reaches the main menu).
- `src/runtime/keyscript.c`: the `--keys` grammar with no Win32.
- `src/linux/main.c` and `shims.c`: the native host and the 21 missing
  imports, checked with gcc against `win32hle.h` (no SDL needed for that).
- `cmake/native.cmake`, `cmake/linux-i386.cmake`, `build-linux.sh`,
  `build-macos.sh`, and the branch in `CMakeLists.txt`. Configuring in WSL
  goes as far as `pkg_check_modules(sdl2)` and stops there, because SDL2 is
  not installed.
- The whole lift through gcc's front end, clean.

Not done: a link or a run on Linux. That needs `gcc-multilib` and i386 SDL2
and SDL2_ttf (or the 64-bit SDL2 and a based build), installed with the
user's approval.

## Effort

From the sibling ports' history and the gaps above, for one person who knows
the code. The first item needs the toolchain installed.

| Step | Days |
|---|---|
| Toolchain, first full compile and link (flat, `-m32`) | 0.5 |
| Boot to the main menu: the permissive-bind loop (registry, paths, activation, timing) and 8-bit text (gap 3) | 3 to 4 |
| Text fields (gap 2, pcrecomp and here) | 2 to 3 |
| A skirmish played with the mouse and keys; sound | 3 to 5 |
| Music: MCI `cdaudio` over Ogg, shared with Windows (gap 5) | 2 |
| Movies, after Track 6 (gap 4) | 1 to 2 |
| `setup.sh` native path, `conformance.py` and `docs/testing.md` on Linux | 2 |
| **Linux, playable** | **about 3 to 4 weeks** |
| macOS (64-bit based), with a Mac | 1 more week |

The risk is in the bring-up loop: it may be shorter (Red Alert 2 needed
little beyond Tiberian Sun's fixes), or longer if the game raises
exceptions (gap 6) or depends on Windows' window activation in ways
win32hle does not model.
