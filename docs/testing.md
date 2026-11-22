# Testing

Two checks, both headless: the conformance harness, and a scripted run into a
skirmish. Neither opens a window on your screens: `--headless` runs on
pcrecomp's private desktop (`native32_headless`), which is never shown, and
mutes the sound.

## The conformance harness

```
py -3 tools\conformance.py
```

Runs `build\swgbrecomp.exe --run --headless` with a scripted click on the
main menu's Exit button, reads the log and reports each milestone:

```
PASS: boot
PASS: genie
PASS: directdraw
PASS: menu
PASS: exit
5/5
```

| Milestone | Passes when |
|---|---|
| `boot` | the image maps, binds and enters the game (`entering 0x00632881`) |
| `genie` | `data\genie.dat` is opened and inflated by the host's decoder |
| `directdraw` | the 800x600x8 mode is set and 100 frames are presented |
| `menu` | the main menu's child windows exist |
| `exit` | the click on Exit ends the game with code 0 |

Exit code 1 on any FAIL; the full log is `work\conformance.log`. Without the
game data (`game\`, `work\Battlegrounds.exe`) every milestone is SKIP
and the exit code is 0, so it is safe on a machine with no copy.

## Into a skirmish

```
build\swgbrecomp.exe --run --headless --watchdog 120 --keys "20:move:345:250,21:click:345:250,30:a,31:b,33:move:323:352,34:click:323:352,44:move:600:168,45:click:600:168,55:move:118:568,56:click:118:568,95:move:310:170,96:click:310:170,105:shot=skirmish.bmp"
```

Single Player, a name typed into the New Player dialog, OK, Standard Game,
Start Game; then the command center is clicked. It passes when `skirmish.bmp`
shows the map with the command center selected
([the screenshot](screenshots/skirmish.png) is this run). Each `--keys` entry
is `seconds:action`: `move:X:Y`, `click:X:Y`, a key name, or `shot=FILE.bmp`.

`--watchdog` ends the run and says where the game was; `--files` logs every
file opened; `NATIVE32_TRACE_EH=1` logs every guest exception and where it
lands.

## The picture (scaling, fullscreen, pacing)

Headless too. The presenter's layout and mouse mapping, without the game:

```
cl /nologo tools\present_test.c srcuntime\present.c /Isrcuntime user32.lib gdi32.lib
present_test.exe                                  OK
```

With the game: resize the window, look at it, and click through the letterbox:

```
build\swgbrecomp.exe --run --headless --multi --watchdog 40 --present-log ^
  --keys "9:size:1200:700,10:wshot=win.bmp,11:wclick:180:661"
```

`win.bmp` is the 800x600 frame centred in 1200x700 (picture 133,0 933x700),
and `wclick:180:661` is the Exit button's place in that window, so the run
ends with code 0 (a click elsewhere leaves the game up and the watchdog ends
it with 4). `--present-log` prints presents against frames shown each second:
a loading phase of 60,000 presents a second shows as the display's refresh or
less, and an idle menu as 4 or 5.
