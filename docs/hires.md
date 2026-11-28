# High resolution and widescreen

Galactic Battlegrounds has three sizes of its own: 800x600, 1024x768 and
1280x1024 (Options, Screen Size, or the `800`/`1024`/`1280` switches). The
menus always run at 800x600; a game, a scenario or the editor switches to the
chosen size. The remaster runs all three, and any size from 1024x720 up to
4096x4096, widescreen included: more of the battlefield on screen, not a
stretched picture.

```
swgbrecomp.exe --res 1920x1080      the size for this run
swgbrecomp.exe --res screen         the screen's size
swgbrecomp.exe --res game           the game's own choice (Options, Screen Size)
swgbrecomp.ini  [swgb] resolution=  the same, kept beside the program
swgbrecomp.exe --classic            the original game: 800x600
```

The size is the game's drawing size. The window shows it at whatever size the
window is; pick a size that divides your screen for the sharpest picture
(1920x1080 on a 4K screen is a whole 2x). When the game changes size (the
4:3 menus, then a 16:9 battle) the window takes the new shape and keeps its
height.

## The game's own sizes

Starting a game at 1024x768 or 1280x1024 crashed: a null read in the mouse
pointer's "center" (sub_0049F5C0), right after the mode change. Across a mode
change the game suspends its software pointer (sub_0049E540 clears its draw
area) and the next main-loop tick puts it back (sub_00428F80); the game
start centres the pointer in between. On real DirectDraw, `SetDisplayMode`
made Windows send the window its size and activation messages, and their
handlers had resumed the pointer before the game got there; the software
DirectDraw changes nothing on the screen and sends nothing. The fix is one
line after the pointer's first read (`run_lift.py` PATCHES): a suspended
pointer is not centred.

## Any size: the class layout

The in-game screen is laid out per size in `screen_coords.txt` (INTERFAC.DRS
`bina` 50006: a column of rectangles for each of the three sizes) and in a
few switches in the code, over a bottom panel drawn from one picture per size
and civilization (SLP 51101-51146: 800x170, 1024x200 and 1280x250, each with
its hotspot at the panel's place on that screen).

Any other size borrows one of the game's layouts, its *class*: 1280x1024 when
the size is at least that big, otherwise 1024x768. Then
(`src/runtime/hires.c`, called from `run_lift.py`'s patches):

- **The size switches see the class.** Each place the game compares its
  screen's width or height with 800, 1024 or 1280 (or 1600x1200, for which it
  has no pictures) to pick a layout gets the class's value for the real one
  (`swgb_cls`, at the compares in `run_lift.py` CLASS_SITES). Nothing else
  does: the draw areas, the map view and the mouse are the real size.
- **The panels move.** Every panel on the in-game screen is moved from the
  class layout to the real size as its place is set (TPanel's two
  `set_rect`s, sub_004B5780 and sub_004B5660): the bottom panel's contents
  down by the extra height; everything right of the split (the age plate,
  the resources, the minimap and its buttons, the menu buttons) right by the
  extra width. The two panels across the split, the unit display and the map
  view, get the extra width, and the map view the extra height. The screen's
  own draw puts one picture by hand, and it moves the same way (`swgb_xy`).
  A rect already past the class's edges was placed for the real size by the
  game (anchored to an edge) and is left alone, and so is a dialog (the menu,
  the objectives, diplomacy): the game centres it on the real size, and a
  panel first placed so keeps its place.
- **The panel picture widens.** When the game reads the class's panel picture
  from INTERFAC.DRS (sub_004D4B30, every DRS read), the host answers with a
  wider one: the same picture with one column inside the unit display repeated
  across the extra width (1280x1024: column 454, inserted at 455; 1024x768:
  361 at 362), and its hotspot lowered by the extra height. It is made from
  the game's own picture the first time it is asked for, and kept.

The split is inside the unit display's screen, between its four status lamps
and the age plate, in all six civilizations' pictures: a column there is the
screen's frame, its scan lines and the bar above them, so repeated it reads
as a longer screen.

`SWGB_HIRES_LOG=1` prints each compare that read a class value, each panel
moved and each picture widened.

## What works

Each started a Standard Game from the menu on netlab's Windows test VMs
(`--res`, a scripted run) and was looked at in the battle: drawn at the
size, the interface laid out for it.

| Size | Class | Checked |
|---|---|---|
| 800x600, 1024x768, 1280x1024 | the game's own | a skirmish, the command centre selected |
| 1366x768 | 1024x768 | a skirmish, the command centre selected |
| 1920x1080 | 1280x1024 | a skirmish for six and a half minutes, recorded (`--record`): workers trained, the map scrolled, the clock at the game's own speed; the menu, objectives, chat and diplomacy dialogs; in a window; a LAN game on two machines |
| 2560x1080, 2560x1440, 3840x2160 | 1280x1024 | a skirmish |

The scenario editor needs none of this: it lays itself out for the screen it
is given (checked at 1920x1080).

## Not yet

- The end-of-game screens (the achievements, the timeline) at the
  remaster's sizes: not looked at yet.
- Changing the size while a game is running (the game's own Options can
  switch between its three).
