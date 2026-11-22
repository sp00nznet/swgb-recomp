/*
 * The runtime a game-specific helper is built on, picked once for every host:
 *
 *   Windows (host.c, softdd.c): pcrecomp's runtime/native32, the real Win32
 *   underneath, a 32-bit process in which a guest address is a host address.
 *
 *   Linux and macOS (src/linux): pcrecomp's runtime/win32hle, which answers
 *   the imports itself. 32-bit (Linux -m32) a guest address is a host
 *   address; 64-bit (macOS, Linux x86-64) the guest's 4 GB is one block of
 *   the host's and every guest address is converted (win32hle.h, hle_ptr).
 *
 * A helper that only touches guest memory and the register file (inflate.c)
 * includes this instead of either header, and turns a guest address into a
 * host pointer with GUEST_PTR. docs/linux-macos.md has the design.
 */
#ifndef SWGB_GUEST_H
#define SWGB_GUEST_H
#include <stdint.h>

#ifdef SWGB_WIN32HLE
#include "win32hle.h"
#define GUEST_PTR(va) hle_ptr((uint32_t)(va))
#else
#include "native32.h"
#define GUEST_PTR(va) ((void*)(uintptr_t)(uint32_t)(va))
#endif

#endif
