# =============================================================================
# Star Wars: Galactic Battlegrounds - static recompilation host (32-bit, MSVC x86)
#
#   py -3 run_lift.py --all  # -> src/recomp/gen/  (needs work/functions.json)
#   build.cmd                # vcvarsall x86 + CMake + Ninja -> build/swgbrecomp.exe
#   build\swgbrecomp.exe --run --headless --record out.mp4 --watchdog 60
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(swgb C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "the host must be 32-bit: use build.cmd (vcvarsall x86)")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "runtime/native32 is MSVC x86 inline assembly")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# The C runtime linked in: a test VM has no 32-bit VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(PCRECOMP "${CMAKE_SOURCE_DIR}/../pcrecomp" CACHE PATH "pcrecomp checkout")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/recomp/gen/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "src/recomp/gen is empty -- run `py -3 run_lift.py --all` first")
endif()

add_executable(swgb
  src/runtime/host.c
  src/runtime/softdd.c
  src/runtime/hires.c
  src/runtime/inflate.c
  third_party/miniz/miniz.c
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/native32/dplay32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
  ${GEN_SOURCES}
)
target_include_directories(swgb PRIVATE
  src/runtime src/recomp/gen third_party/miniz ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(swgb PRIVATE RECOMP_RETADDR=0u _CRT_SECURE_NO_WARNINGS
                                            RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS)
# Linked high and not relocatable, so the guest's 0x00400000..0x00A5A000 is free.
target_link_options(swgb PRIVATE /BASE:0x60000000 /FIXED:NO /DYNAMICBASE:NO
                                       /NXCOMPAT /LARGEADDRESSAWARE /STACK:0x1000000
                                       /MANIFESTUAC:NO /MANIFEST:NO)
# /O1, not /O2: 3.9M lines of generated C. /O2 stalled the farm on Shogo's
# lift; /O1 keeps most of the speed at a fraction of the compile time.
set(GEN_OPT "/O1" CACHE STRING "optimization for the generated C")
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;${GEN_OPT}")
set_source_files_properties(src/runtime/host.c src/runtime/softdd.c src/runtime/hires.c src/runtime/inflate.c PROPERTIES COMPILE_OPTIONS "/W3")
set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "/w;/O2")
target_link_libraries(swgb PRIVATE user32 gdi32 kernel32 ole32 uuid dxguid winmm ws2_32 shlwapi advapi32)

option(SWGB_TRACE "record a ring buffer of lifted function entries" OFF)
if(SWGB_TRACE)
  target_compile_definitions(swgb PRIVATE RECOMP_TRACE=1)
endif()

# The output name keeps Windows' compatibility database off the host.
set_target_properties(swgb PROPERTIES OUTPUT_NAME swgbrecomp)
