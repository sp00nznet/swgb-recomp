/*
 * keyscript: the --keys script, parsed with no Win32 in sight, so every host
 * reads the same syntax (docs/testing.md):
 *
 *   "SECONDS:what,..."  what is a key name (esc, enter, space, tab, up, down,
 *                       left, right, bksp, f1..f10, a letter or digit),
 *                       "click:X:Y" or "move:X:Y" (game pixels), or
 *                       "shot=FILE" (the frame then, as a .bmp)
 *   "on:NAME"           wait until the game opens a file whose name has NAME
 *                       in it; the times after it count from then
 *
 * The Linux and macOS host (src/linux) parses with this; the Windows host's
 * own copy in host.c is the same grammar and moves here once the tracks that
 * edit host.c have merged (docs/linux-macos.md, "Separating host.c").
 */
#ifndef SWGB_KEYSCRIPT_H
#define SWGB_KEYSCRIPT_H
#include <stdint.h>

#define KEYSCRIPT_MAX 128

typedef struct {
    uint32_t at_ms;     /* from the start, or from the last on: */
    int vk;             /* a Windows virtual-key code, or 0 */
    int x, y;           /* game pixels, for move and click */
    int move, click;    /* move:X:Y, click:X:Y (a click moves first) */
    char on[64];        /* on:NAME, or "" */
    char shot[512];     /* shot=FILE as written, or "" */
} keyscript_step;

/* The virtual-key code of a key name, or 0. */
int keyscript_key(const char* name);

/* Parse spec into steps (at most max). Returns how many, or -1 with the bad
 * token in *bad (a pointer into a static copy). */
int keyscript_parse(const char* spec, keyscript_step* steps, int max, const char** bad);

#endif
