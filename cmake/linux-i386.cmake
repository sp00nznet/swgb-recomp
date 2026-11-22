# The native Linux host (src/linux, on pcrecomp's runtime/win32hle): a 32-bit
# program, so the guest's addresses are host addresses. gcc or clang with
# -m32, and SDL2 and SDL2_ttf for i386 (docs/linux-macos.md lists the packages).
#
#   cmake -S . -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_FLAGS_INIT "-m32")
# Not position-independent: i386 PIC spends a register on the GOT, and the
# host sits at 0x08048000, clear of the guest's 0x00400000..0x00A5A000.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32 -no-pie")
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
# The 32-bit .pc files first: Debian and Ubuntu, Arch (lib32), Fedora (/usr/lib).
set(ENV{PKG_CONFIG_PATH} "/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib32/pkgconfig:/usr/lib/pkgconfig")
