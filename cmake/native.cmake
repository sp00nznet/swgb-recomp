# =============================================================================
# The native Linux and macOS host: the lifted C on pcrecomp's runtime/win32hle
# (src/linux), with no Wine. Included by CMakeLists.txt on those systems;
# docs/linux-macos.md has the design, the gaps and the build steps.
#
#   ./build-linux.sh     # 32-bit Linux (cmake/linux-i386.cmake): flat memory
#   ./build-macos.sh     # 64-bit macOS: the guest's 4 GB is one block of the host's
#
# Modelled on Red Alert 2's and Civilization III's native builds.
# =============================================================================
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(PCRECOMP "${CMAKE_SOURCE_DIR}/../pcrecomp" CACHE PATH "pcrecomp checkout")
set(GEN_DIR "${CMAKE_SOURCE_DIR}/src/recomp/gen" CACHE PATH "the lifted C (run_lift.py's output)")
# As on Windows, a worktree may link a lift built elsewhere (libswgb_gen.a of
# a build with the same memory model) instead of compiling GEN_DIR again.
set(SWGB_GEN_LIB "" CACHE FILEPATH "a prebuilt libswgb_gen.a to link instead of compiling GEN_DIR")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
# ffmpeg is for movies (win32hle's bink.c, and Track 6's player when it
# comes): the game runs without it.
option(WITH_FFMPEG "movies through ffmpeg (needs its libraries for the target)" OFF)
if(WITH_FFMPEG)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
endif()

set(HLE ${PCRECOMP}/runtime/win32hle)
if(NOT EXISTS "${HLE}/win32hle.h")
  message(FATAL_ERROR "${HLE} is missing: PCRECOMP must be a pcrecomp checkout with runtime/win32hle")
endif()
file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\\.c$|/[.]_[^/]*$")

set(SWGB_NATIVE_DEFS RECOMP_RETADDR=0u RECOMP_LOCAL_REGS SWGB_WIN32HLE)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  list(APPEND SWGB_NATIVE_DEFS RECOMP_FLAT_MEMORY)       # a guest address is a host address
else()
  # 64-bit: where the guest's 4 GB goes. A fixed address lets the lifted code
  # add a constant, not a global it reloads after every store; on a Mac 16 TB
  # up is free on arm64 and x86-64 alike. Empty: wherever mmap puts it.
  if(APPLE)
    set(SWGB_MEM_BASE "0x100000000000ull" CACHE STRING "host address of the guest's 4 GB (empty: anywhere)")
  else()
    set(SWGB_MEM_BASE "" CACHE STRING "host address of the guest's 4 GB (empty: anywhere)")
  endif()
  if(SWGB_MEM_BASE)
    list(APPEND SWGB_NATIVE_DEFS RECOMP_MEM_BASE=${SWGB_MEM_BASE})
  endif()
endif()
# win32hle runs one guest thread at a time (its machine lock), so indirect
# calls can cache their targets; the lift writes the x87 stack as _ST(i), so
# it can be a ring with a function's own pushes in locals. Red Alert 2 went
# from 35 to 40-45 fps with these. Off: the indirect-call ring for a crash report.
option(SWGB_FAST_ICALLS "cache indirect-call targets, x87 ring, no icall trace ring" ON)
if(SWGB_FAST_ICALLS)
  list(APPEND SWGB_NATIVE_DEFS RECOMP_RESOLVE_CACHE=16384 RECOMP_ICALL_SITE_CACHE RECOMP_NO_ICALL_TRACE
                               RECOMP_X87_RING RECOMP_X87_LOCAL)
endif()
set(SWGB_NATIVE_INCS src/linux src/runtime ${GEN_DIR} third_party/miniz ${HLE} ${PCRECOMP}/runtime/recomp32)
# gcc and clang take the lifted C as it is (docs/linux-macos.md, "The lifted
# C under gcc"); these keep its integer and aliasing semantics x86's.
set(GEN_OPT "-O1" CACHE STRING "optimization for the generated C")
set(SWGB_GEN_FLAGS -w ${GEN_OPT} -fno-strict-aliasing -fwrapv)

if(SWGB_GEN_LIB)
  if(NOT EXISTS "${SWGB_GEN_LIB}")
    message(FATAL_ERROR "SWGB_GEN_LIB ${SWGB_GEN_LIB} does not exist")
  endif()
  add_library(swgb_gen STATIC IMPORTED)
  set_target_properties(swgb_gen PROPERTIES IMPORTED_LOCATION "${SWGB_GEN_LIB}")
  set(SWGB_GEN_FILE "${SWGB_GEN_LIB}")
else()
  file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/*.c")
  list(FILTER GEN_SOURCES EXCLUDE REGEX "/[.]_[^/]*$")   # macOS ._* files on some external drives
  if(NOT GEN_SOURCES)
    message(FATAL_ERROR "${GEN_DIR} is empty -- run `python3 run_lift.py --all` first")
  endif()
  add_library(swgb_gen STATIC ${GEN_SOURCES})
  target_include_directories(swgb_gen PRIVATE ${SWGB_NATIVE_INCS})
  target_compile_definitions(swgb_gen PRIVATE ${SWGB_NATIVE_DEFS})
  target_compile_options(swgb_gen PRIVATE ${SWGB_GEN_FLAGS})
  set(SWGB_GEN_FILE "$<TARGET_FILE:swgb_gen>")
endif()

add_executable(swgb
  src/linux/main.c
  src/linux/shims.c
  src/runtime/keyscript.c
  src/runtime/inflate.c
  third_party/miniz/miniz.c
  ${HLE_SOURCES}
)
target_include_directories(swgb PRIVATE ${SWGB_NATIVE_INCS})
target_compile_definitions(swgb PRIVATE ${SWGB_NATIVE_DEFS})
target_compile_options(swgb PRIVATE -fno-strict-aliasing -fwrapv)
set_source_files_properties(src/linux/main.c src/linux/shims.c src/runtime/keyscript.c src/runtime/inflate.c
                            PROPERTIES COMPILE_OPTIONS "-Wall;-Wno-unused-parameter")
set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w;-O2")
# Every object of the lift, referenced or not: the dispatch table is the only
# way in, and nothing may be dropped from behind it.
if(APPLE)
  target_link_libraries(swgb PRIVATE "-Wl,-force_load,${SWGB_GEN_FILE}")
else()
  target_link_libraries(swgb PRIVATE "-Wl,--whole-archive" "${SWGB_GEN_FILE}" "-Wl,--no-whole-archive")
endif()
if(NOT SWGB_GEN_LIB)
  add_dependencies(swgb swgb_gen)
endif()
target_link_libraries(swgb PRIVATE PkgConfig::SDL2 PkgConfig::SDL2_TTF pthread m)
if(WITH_FFMPEG)
  target_compile_definitions(swgb PRIVATE HLE_WITH_FFMPEG)
  target_link_libraries(swgb PRIVATE PkgConfig::FFMPEG)
endif()

set_target_properties(swgb PROPERTIES OUTPUT_NAME swgbrecomp)
