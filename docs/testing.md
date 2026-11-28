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

## Where a test runs

Run it in a logged-on Windows session (a scheduled task with `/IT`, as
netlab's `run` does), not straight from an SSH shell: there the game reaches
the battle, draws its first frame and stops (no input, no world turns,
garbled text), while the menus seem fine. `--headless` still keeps every
window on a private desktop.

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

More `--keys` actions: `rclick:X:Y`, `hold:KEY:SECONDS` (a key held down, as
the arrow keys scroll the map), and the keys `f1` to `f12` (`f11` shows the
game clock). At Normal the clock runs 1.5 game seconds to the second: two
shots 100 s apart are 2:30 apart on it, the check that the game runs at
its own speed. `--profile` samples which lifted function runs, every
millisecond, and prints the 30 most seen when the run ends; the watchdog's
`[state]` line says whether the game was active, its mode (2 the menus, 4 a
game) and its comm handler's state.

## At another size

The same run with `--res 1920x1080` (or `--args 1280` for the game's own
1280x1024) plays the skirmish at that size; `SWGB_HIRES_LOG=1` prints each
panel the remaster moved. In the battle the command center is near the
middle of the map view (930,330 at 1920x1080) and the first command button,
which trains a worker, at 47,879. Run with a window (no `--headless`), the
New Player dialog takes `enter` for OK: a scripted click on its OK does not
land there.

## Over the LAN

Two machines, each with the game, `work\Battlegrounds.exe` and disc 2
mounted, and TCP and UDP port 47700 open on the host. Both start from the
main menu at the same moment (`PCRECOMP_DPLOG=1` logs the DirectPlay calls):

```
host:   --keys "20:move:275:410,21:click:275:410,30:h,31:o,33:move:323:352,34:click:323:352,45:move:600:453,46:click:600:453,76:move:88:517,77:click:88:517,84:move:118:568,85:click:118:568"
joiner: --keys "20:move:275:410,21:click:275:410,30:j,31:o,33:move:323:352,34:click:323:352,55:move:600:203,56:click:600:203,63:move:600:240,64:click:600:240,66:move:600:403,67:click:600:403,72:move:88:517,73:click:88:517"
```

Multiplayer, a player's name, Create (the host) or Show Games, the game and
Join (the joiner), I'm Ready! on both, Start Game. It passes when the host's
log has `node 2 joined`, the joiner's `joined "..." at <host>`, and both are
in the game (`[state] ... mode 4`), each drawing its own side of the same
map. On a [netlab](https://github.com/sp00nznet/netlab) farm,
`scenarios/swgb/lan.sh testbox3 testbox2 1920x1080` runs both and says PASS
or FAIL.
