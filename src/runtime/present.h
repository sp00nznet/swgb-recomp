/* present: how softdd's frame reaches the window (present.c has the why):
 * aspect-correct letterboxing, the scaling filters, borderless fullscreen,
 * and pacing to the display's refresh. */
#ifndef PRESENT_H
#define PRESENT_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#define SOFTDD_PACE 0x5D02          /* the one-shot timer that shows a coalesced frame */

enum { PRES_SHARP, PRES_SMOOTH, PRES_NEAREST, PRES_INTEGER, PRES_MODES };

extern int present_filter;           /* --filter, Alt+F12 */
extern int present_fullscreen_start; /* --fullscreen */
extern int present_refresh;          /* --refresh HZ; 0: ask the monitor */
extern int present_log;              /* --present-log: presents and shown frames per second */

int         present_filter_by_name(const char* name);   /* -1 if unknown */
const char* present_filter_name(int mode);

/* The picture's rectangle in a client area of cw x ch: the source's aspect
 * kept, centred (integer: whole multiples while they fit). */
void present_layout(int mode, int cw, int ch, int sw, int sh, RECT* out);

/* A client pixel -> a game pixel through that rectangle, clamped to the
 * picture (a click in a bar lands on the nearest edge). */
void present_to_game(HWND wnd, int gw, int gh, POINT* p);

/* Draw a frame (8-bit palettized, or 16-bit RGB565) into the window. */
void present_paint(HWND wnd, const uint8_t* bits, int w, int h, int pitch, int bpp, const PALETTEENTRY* pal);

/* Borderless fullscreen on the window's monitor, the desktop's mode never
 * touched. on: 1, 0, or -1 to toggle. */
void present_set_fullscreen(HWND wnd, int on);
int  present_is_fullscreen(void);
void present_cycle_filter(HWND wnd);
void present_set_client_size(HWND wnd, int w, int h);   /* a test hook: --keys size:W:H */

/* Pacing. A frame is shown when a refresh interval has passed since the last
 * one shown; a frame that is not shown is remembered (dirty) and a one-shot
 * timer shows the latest after the interval. */
int  present_due(HWND wnd);          /* 1: show this one */
void present_painted(void);          /* a frame was shown */
void present_defer(HWND wnd);        /* not shown: arm the timer */
int  present_dirty(void);
void present_disarm(HWND wnd);       /* the timer fired (or any tick): clear it */
void present_invalidate(HWND wnd);   /* resized, uncovered, display change: show the current frame again */
void present_note(int presented, int shown);   /* per-second counts for --present-log */

/* --keys wshot=FILE: the window as it is, bars and all, written as a .bmp
 * from the next frame (also headless: the window is just not shown). */
void present_wshot_request(const char* path);
int  present_wshot_pending(void);
void present_wshot_write(HWND wnd, const uint8_t* bits, int w, int h, int pitch, int bpp, const PALETTEENTRY* pal);

#endif
