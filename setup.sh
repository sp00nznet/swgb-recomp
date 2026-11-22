#!/bin/sh
# Linux and macOS: not yet. A native host on pcrecomp's win32hle (no Wine) is
# being built (docs/linux-macos.md; build-linux.sh and build-macos.sh build it
# by hand from a lift), but it does not run the game yet. Rather than a
# script that fails halfway, this one says so and stops.
echo "Star Wars: Galactic Battlegrounds: the recompiled game builds and runs on Windows only for now."
echo "On Windows, double-click Setup.cmd. Linux and macOS: docs/linux-macos.md has the state of the native port."
exit 1
