#!/bin/sh
# Configure and build the native macOS host: the lifted C on pcrecomp's
# runtime/win32hle, a 64-bit program (arm64 or x86-64) with no Wine. The
# guest's 4 GB is one block of the host's memory (cmake/native.cmake,
# docs/linux-macos.md).
#
#   ./build-macos.sh                       # build-macos/swgbrecomp
#   build-macos/swgbrecomp --run --game game
#
# Needs a lift (run_lift.py), Xcode's command line tools, and Homebrew's
# cmake, ninja, pkgconf, sdl2 and sdl2_ttf. BUILD_DIR, PCRECOMP and CMAKE_ARGS
# override. HLE_BASED=1 builds the same way on 64-bit Linux.
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build-macos}
PCRECOMP=${PCRECOMP:-$(cd .. && pwd)/pcrecomp}
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  # shellcheck disable=SC2086  # CMAKE_ARGS is a list of flags
  cmake -S . -B "$BUILD_DIR" -G Ninja -DPCRECOMP="$PCRECOMP" $CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"
