# LAN multiplayer

Galactic Battlegrounds plays over DirectPlay, and Windows' DirectPlay is an
optional feature now that no longer finds games between two machines. The
recomp gives the game pcrecomp's own DirectPlay instead
(`runtime/native32/dplay32.c`): the same calls the game always made, carried
over TCP on your LAN. Nothing to install, on either machine.

## Playing

1. Both players: **Multiplayer**. The connection type is *Internet TCP/IP
   Connection For DirectPlay*.
2. The host: **Create**, then set the game up as usual.
3. Everyone else: **Show Games**, pick the host's game, **Join**.
4. Everyone ticks **I'm Ready!**; the host presses **Start Game**.

Games are found by asking the LAN (a UDP broadcast). When the host is on
another subnet, or a broadcast doesn't get through, name it:

```
swgbrecomp.exe --lan-host 192.168.1.20
swgbrecomp.ini  [swgb] lan_host=192.168.1.20      (several: 192.168.1.20,10.0.0.5)
```

The host needs TCP and UDP port 47700 open (Windows asks the first time you
host; allow it on private networks). `PCRECOMP_DPLAY_PORT` moves it, on every
machine alike.

## How it works

The session is a star: the host listens on port 47700, each joiner connects
to it, and the host relays what each machine sends. The game's players and
their data, the session's settings and every message the game sends go
through it as the game sent them. `PCRECOMP_DPLOG=1` prints every DirectPlay
call.

The game creates DirectPlay through `CoCreateInstance` and asks for
`IDirectPlay4`, the wide-string interface, though it hands it char strings;
it also asks for the first `IDirectPlay` interface to close the session
(`EnableNewPlayers`) when the game starts. Its own version check of
`dplayx.dll` is answered by the host (sub_00477DA0): the DirectPlay is
dplay32, and the file may not be there at all.

## What works

Two machines on netlab (the Windows test VM and its clone), each running the
recomp: the host creates the game, the joiner finds it and joins, both are
ready, the host starts it, and both play the same random-map game, each
seeing its own civilization, at 800x600 and at 1920x1080.

Not here: games over the internet without a LAN between you (use a VPN that
makes one, and `lan_host`); the Zone.com lobby, which no longer exists; the
host leaving and another player taking over the session (the game ends).
