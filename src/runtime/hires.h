/* High resolution and widescreen (hires.c). */
#pragma once
#include <stdint.h>

extern int swgb_res_w, swgb_res_h;   /* the remaster's game size; 0: the game's own */

/* A size the remaster can run the game at: 800x600, or 1024x720 and up. */
int swgb_res_valid(int w, int h);
