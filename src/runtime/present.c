/*
 * present: how softdd's frame reaches the window.
 *
 * The game draws a fixed-size frame (800x600 and up, 4:3). The window can be
 * any shape, so the picture is fitted into the client area with its aspect
 * kept and black bars around it: present_layout is the one rectangle that
 * both drawing and the mouse go through, so a click lands where it is drawn.
 *
 * Filters (the modes pcrecomp's win32hle/screen.c offers for SDL2, done on
 * the CPU here, so GDI is all it needs):
 *   sharp    integer-ish prescale look: nearest inside a texel, a one-pixel
 *            bilinear blend at its edges (the default: no shimmer, no blur)
 *   smooth   plain bilinear
 *   nearest  nearest neighbour at any size (uneven pixels at odd scales)
 *   integer  nearest, in whole multiples of the frame only (bars around)
 *
 * Fullscreen is the same window made borderless and sized to its monitor:
 * the desktop's display mode is never changed.
 *
 * Pacing: the game presents on every Unlock/ReleaseDC of a primary without a
 * back buffer, hundreds of times a second in the menus. A frame is shown when
 * a refresh interval has passed; otherwise it is remembered and a one-shot
 * timer shows the latest one, so the last frame before the game goes idle is
 * never lost.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "present.h"

int present_filter = PRES_SHARP;
int present_fullscreen_start;
int present_refresh;
int present_log;

static const char* const k_names[PRES_MODES] = { "sharp", "smooth", "nearest", "integer" };

int present_filter_by_name(const char* n) {
    for (int i = 0; i < PRES_MODES; i++)
        if (!_stricmp(n, k_names[i])) return i;
    return -1;
}
const char* present_filter_name(int m) { return m >= 0 && m < PRES_MODES ? k_names[m] : "?"; }

/* ------------------------------------------------------------- layout */

void present_layout(int mode, int cw, int ch, int sw, int sh, RECT* r) {
    int dw, dh;
    if (cw <= 0 || ch <= 0 || sw <= 0 || sh <= 0) { RECT z = { 0, 0, cw > 0 ? cw : 0, ch > 0 ? ch : 0 }; *r = z; return; }
    int whole = 0;
    if (mode == PRES_INTEGER) {
        int k = cw / sw < ch / sh ? cw / sw : ch / sh;
        if (k >= 1) { dw = sw * k; dh = sh * k; whole = 1; }
    }
    if (!whole) {
        if ((int64_t)cw * sh <= (int64_t)ch * sw) { dw = cw; dh = (int)((int64_t)cw * sh / sw); }
        else { dh = ch; dw = (int)((int64_t)ch * sw / sh); }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }
    r->left = (cw - dw) / 2;
    r->top = (ch - dh) / 2;
    r->right = r->left + dw;
    r->bottom = r->top + dh;
}

void present_to_game(HWND wnd, int gw, int gh, POINT* p) {
    RECT rc, r;
    if (!wnd || gw <= 0 || gh <= 0 || !GetClientRect(wnd, &rc) || !rc.right || !rc.bottom) return;
    present_layout(present_filter, rc.right, rc.bottom, gw, gh, &r);
    int rw = r.right - r.left, rh = r.bottom - r.top;
    int x = p->x < r.left ? r.left : p->x >= r.right ? r.right - 1 : p->x;
    int y = p->y < r.top ? r.top : p->y >= r.bottom ? r.bottom - 1 : p->y;
    /* through the pixel's centre: the last pixel of a smaller picture still reaches the last game pixel */
    p->x = (int)((int64_t)(2 * (x - r.left) + 1) * gw / (2 * (int64_t)rw));
    p->y = (int)((int64_t)(2 * (y - r.top) + 1) * gh / (2 * (int64_t)rh));
}

/* ------------------------------------------------------------- scaling */

static uint32_t* g_src;   static size_t g_src_n;     /* the frame as 0x00RRGGBB */
static uint32_t* g_img;   static size_t g_img_n;     /* the scaled picture */

static uint32_t* grow(uint32_t** p, size_t* cap, size_t n) {
    if (*cap < n) { free(*p); *p = (uint32_t*)malloc(n * 4); *cap = *p ? n : 0; }
    return *p;
}

static int to32(const uint8_t* bits, int w, int h, int pitch, int bpp, const PALETTEENTRY* pal) {
    if (bpp != 8 && bpp != 16 && bpp != 32) return 0;
    uint32_t* d = grow(&g_src, &g_src_n, (size_t)w * h);
    if (!d) return 0;
    uint32_t lut[256];
    if (bpp == 8)
        for (int i = 0; i < 256; i++)
            lut[i] = pal ? ((uint32_t)pal[i].peRed << 16) | ((uint32_t)pal[i].peGreen << 8) | pal[i].peBlue
                         : (uint32_t)i * 0x010101u;
    for (int y = 0; y < h; y++) {
        const uint8_t* s = bits + (size_t)y * pitch;
        uint32_t* o = d + (size_t)y * w;
        if (bpp == 8) for (int x = 0; x < w; x++) o[x] = lut[s[x]];
        else if (bpp == 32) memcpy(o, s, (size_t)w * 4);
        else for (int x = 0; x < w; x++) {
            uint32_t v = ((const uint16_t*)s)[x], r = v >> 11, g = (v >> 5) & 63, b = v & 31;
            o[x] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
    }
    return 1;
}

/* One axis of the scaler: for each destination pixel, the two source pixels
 * and the weight (0..256) of the second. */
typedef struct { int n; int* i0; int* i1; int* w; int cap; } axis_t;

static void axis_build(axis_t* a, int dn, int sn, int mode) {
    if (a->cap < dn) {
        free(a->i0); free(a->i1); free(a->w);
        a->i0 = (int*)malloc(dn * sizeof(int)); a->i1 = (int*)malloc(dn * sizeof(int)); a->w = (int*)malloc(dn * sizeof(int));
        a->cap = dn;
    }
    a->n = dn;
    float sharp = (float)dn / sn;   /* texel edges stay one destination pixel wide */
    for (int d = 0; d < dn; d++) {
        if (mode == PRES_NEAREST || mode == PRES_INTEGER) {
            int s = (int)(((int64_t)(2 * d + 1) * sn) / (2 * (int64_t)dn));
            a->i0[d] = a->i1[d] = s < sn - 1 ? s : sn - 1;
            a->w[d] = 0;
            continue;
        }
        float sx = ((float)d + 0.5f) * sn / dn - 0.5f;
        int x0 = (int)floorf(sx);
        float fr = sx - (float)x0;
        if (x0 < 0) { x0 = 0; fr = 0; }
        int x1 = x0 + 1;
        if (x1 >= sn) { x0 = x1 = sn - 1; fr = 0; }
        if (mode == PRES_SHARP && sharp > 1.0f) {
            fr = (fr - 0.5f) * sharp + 0.5f;
            fr = fr < 0 ? 0 : fr > 1 ? 1 : fr;
        }
        a->i0[d] = x0; a->i1[d] = x1; a->w[d] = (int)(fr * 256.0f + 0.5f);
    }
}

static inline uint32_t lerp(uint32_t a, uint32_t b, uint32_t w) {   /* w 0..256 */
    uint32_t rb = (((a & 0xFF00FFu) * (256 - w) + (b & 0xFF00FFu) * w) >> 8) & 0xFF00FFu;
    uint32_t g = (((a & 0x00FF00u) * (256 - w) + (b & 0x00FF00u) * w) >> 8) & 0x00FF00u;
    return rb | g;
}

/* g_src (sw x sh) -> g_img (dw x dh) */
static void scale(int sw, int sh, int dw, int dh, int mode) {
    static axis_t ax, ay;
    if (!grow(&g_img, &g_img_n, (size_t)dw * dh)) return;
    axis_build(&ax, dw, sw, mode);
    axis_build(&ay, dh, sh, mode);
    for (int y = 0; y < dh; y++) {
        const uint32_t* r0 = g_src + (size_t)ay.i0[y] * sw;
        const uint32_t* r1 = g_src + (size_t)ay.i1[y] * sw;
        uint32_t wy = (uint32_t)ay.w[y];
        uint32_t* o = g_img + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            int x0 = ax.i0[x], x1 = ax.i1[x];
            uint32_t wx = (uint32_t)ax.w[x];
            uint32_t top = wx ? lerp(r0[x0], r0[x1], wx) : r0[x0];
            if (!wy) { o[x] = top; continue; }
            o[x] = lerp(top, wx ? lerp(r1[x0], r1[x1], wx) : r1[x0], wy);
        }
    }
}

static BITMAPINFO* bmi32(int w, int h) {
    static BITMAPINFO b;
    memset(&b, 0, sizeof b);
    b.bmiHeader.biSize = sizeof b.bmiHeader;
    b.bmiHeader.biWidth = w;
    b.bmiHeader.biHeight = -h;
    b.bmiHeader.biPlanes = 1;
    b.bmiHeader.biBitCount = 32;
    return &b;
}

/* ------------------------------------------------------------- painting */

static RECT g_bars_client, g_bars_rect;   /* the layout the bars were last drawn for */
static int  g_bars_dirty = 1;

static void paint_bars(HDC dc, const RECT* client, const RECT* pic) {
    if (!g_bars_dirty && !memcmp(&g_bars_client, client, sizeof *client) && !memcmp(&g_bars_rect, pic, sizeof *pic)) return;
    HBRUSH k = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT a = { client->left, client->top, client->right, pic->top };
    RECT b = { client->left, pic->bottom, client->right, client->bottom };
    RECT c = { client->left, pic->top, pic->left, pic->bottom };
    RECT d = { pic->right, pic->top, client->right, pic->bottom };
    if (a.bottom > a.top) FillRect(dc, &a, k);
    if (b.bottom > b.top) FillRect(dc, &b, k);
    if (c.right > c.left) FillRect(dc, &c, k);
    if (d.right > d.left) FillRect(dc, &d, k);
    g_bars_client = *client;
    g_bars_rect = *pic;
    g_bars_dirty = 0;
}

void present_paint(HWND wnd, const uint8_t* bits, int w, int h, int pitch, int bpp, const PALETTEENTRY* pal) {
    RECT rc, r;
    if (!wnd || !GetClientRect(wnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    if (!to32(bits, w, h, pitch, bpp, pal)) return;
    present_layout(present_filter, rc.right, rc.bottom, w, h, &r);
    int dw = r.right - r.left, dh = r.bottom - r.top;
    HDC dc = GetDC(wnd);
    paint_bars(dc, &rc, &r);
    if (present_filter == PRES_NEAREST || present_filter == PRES_INTEGER) {
        SetStretchBltMode(dc, COLORONCOLOR);   /* GDI scales these: cheap at any size */
        StretchDIBits(dc, r.left, r.top, dw, dh, 0, 0, w, h, g_src, bmi32(w, h), DIB_RGB_COLORS, SRCCOPY);
    } else {
        scale(w, h, dw, dh, present_filter);
        SetDIBitsToDevice(dc, r.left, r.top, dw, dh, 0, 0, 0, dh, g_img, bmi32(dw, dh), DIB_RGB_COLORS);
    }
    ReleaseDC(wnd, dc);
}

/* ------------------------------------------------------------- wshot */

static volatile LONG g_wshot_due;
static char g_wshot_path[MAX_PATH];

void present_wshot_request(const char* path) {
    lstrcpynA(g_wshot_path, path, MAX_PATH);
    InterlockedExchange(&g_wshot_due, 1);
}
int present_wshot_pending(void) { return g_wshot_due != 0; }

void present_wshot_write(HWND wnd, const uint8_t* bits, int w, int h, int pitch, int bpp, const PALETTEENTRY* pal) {
    RECT rc, r;
    InterlockedExchange(&g_wshot_due, 0);
    if (!wnd || !GetClientRect(wnd, &rc) || rc.right <= 0 || rc.bottom <= 0 || !to32(bits, w, h, pitch, bpp, pal)) return;
    present_layout(present_filter, rc.right, rc.bottom, w, h, &r);
    int dw = r.right - r.left, dh = r.bottom - r.top, cw = rc.right, ch = rc.bottom;
    scale(w, h, dw, dh, present_filter);   /* the same scaler for every filter: the shot is what the code computes */
    uint32_t* win = (uint32_t*)calloc((size_t)cw * ch, 4);
    FILE* f = fopen(g_wshot_path, "wb");
    if (!win || !f) { free(win); if (f) fclose(f); return; }
    for (int y = 0; y < dh; y++) memcpy(win + (size_t)(r.top + y) * cw + r.left, g_img + (size_t)y * dw, (size_t)dw * 4);
    BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof fh + sizeof(BITMAPINFOHEADER) + (size_t)cw * ch * 4), 0, 0,
                            sizeof fh + sizeof(BITMAPINFOHEADER) };
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&bmi32(cw, ch)->bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(win, 4, (size_t)cw * ch, f);
    fclose(f);
    free(win);
    fprintf(stderr, "[wshot] window %dx%d, picture %d,%d %dx%d (%s) -> %s\n", cw, ch, (int)r.left, (int)r.top, dw, dh,
            present_filter_name(present_filter), g_wshot_path);
}

/* ------------------------------------------------------------- fullscreen */

static int   g_fs;
static LONG  g_fs_style, g_fs_ex;
static RECT  g_fs_rect;
static char  g_title[128];

int present_is_fullscreen(void) { return g_fs; }

static void show_state(HWND wnd) {
    if (!g_title[0]) GetWindowTextA(wnd, g_title, sizeof g_title - 32);
    char t[192];
    _snprintf(t, sizeof t - 1, "%s  [%s]", g_title, present_filter_name(present_filter));
    t[sizeof t - 1] = 0;
    SetWindowTextA(wnd, t);
}

void present_set_fullscreen(HWND wnd, int on) {
    if (!wnd) return;
    if (on < 0) on = !g_fs;
    if (on == g_fs) return;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        if (IsZoomed(wnd)) ShowWindow(wnd, SW_RESTORE);
        g_fs_style = GetWindowLongA(wnd, GWL_STYLE);
        g_fs_ex = GetWindowLongA(wnd, GWL_EXSTYLE);
        GetWindowRect(wnd, &g_fs_rect);
        if (!GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        /* WS_VISIBLE as it is: a window kept off the screen stays off it */
        SetWindowLongA(wnd, GWL_STYLE, WS_POPUP | (g_fs_style & WS_VISIBLE));
        SetWindowLongA(wnd, GWL_EXSTYLE, 0);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowLongA(wnd, GWL_STYLE, g_fs_style);
        SetWindowLongA(wnd, GWL_EXSTYLE, g_fs_ex);
        SetWindowPos(wnd, HWND_NOTOPMOST, g_fs_rect.left, g_fs_rect.top, g_fs_rect.right - g_fs_rect.left,
                     g_fs_rect.bottom - g_fs_rect.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    }
    g_fs = on;
    RECT rc;
    GetClientRect(wnd, &rc);
    fprintf(stderr, "[present] %s: client %ldx%ld\n", on ? "fullscreen (borderless)" : "windowed", rc.right, rc.bottom);
    present_invalidate(wnd);
}

void present_cycle_filter(HWND wnd) {
    present_filter = (present_filter + 1) % PRES_MODES;
    fprintf(stderr, "[present] filter: %s\n", present_filter_name(present_filter));
    if (wnd && !g_fs) show_state(wnd);
    g_bars_dirty = 1;
    present_invalidate(wnd);
}

void present_set_client_size(HWND wnd, int w, int h) {
    RECT r = { 0, 0, w, h };
    if (!wnd || g_fs) return;
    AdjustWindowRectEx(&r, GetWindowLongA(wnd, GWL_STYLE), FALSE, GetWindowLongA(wnd, GWL_EXSTYLE));
    SetWindowPos(wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    fprintf(stderr, "[present] client size set to %dx%d\n", w, h);
    present_invalidate(wnd);
}

/* ------------------------------------------------------------- pacing */

static int64_t g_qpf, g_last_us, g_interval_us = 16667;
static int     g_hz_known, g_dirty, g_armed;

static int64_t now_us(void) {
    LARGE_INTEGER t;
    if (!g_qpf) { QueryPerformanceFrequency(&t); g_qpf = t.QuadPart; }
    QueryPerformanceCounter(&t);
    /* split: seconds * 1e6 would overflow after days of uptime at a 10 MHz counter */
    return (t.QuadPart / g_qpf) * 1000000 + (t.QuadPart % g_qpf) * 1000000 / g_qpf;
}

static int monitor_hz(HWND wnd) {
    int hz = 60;
    if (present_refresh > 0) hz = present_refresh;
    else {
        MONITORINFOEXA mi;
        DEVMODEA dm;
        memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
        memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
        HMONITOR m = wnd ? MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY) : NULL;
        if (m && GetMonitorInfoA(m, (MONITORINFO*)&mi) &&
            EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            hz = (int)dm.dmDisplayFrequency;
    }
    return hz < 20 ? 20 : hz > 360 ? 360 : hz;
}

static void learn_hz(HWND wnd) {
    static int shown_hz;
    int hz = monitor_hz(wnd);
    g_interval_us = 1000000 / hz;
    if (hz != shown_hz) fprintf(stderr, "[present] pacing to %d Hz\n", hz);
    shown_hz = hz;
    g_hz_known = 1;
}

int present_due(HWND wnd) {
    if (!g_hz_known) learn_hz(wnd);
    return now_us() - g_last_us >= g_interval_us * 9 / 10;   /* a little under: jitter must not skip a refresh */
}

void present_painted(void) { g_last_us = now_us(); g_dirty = 0; }
int  present_dirty(void) { return g_dirty; }

void present_defer(HWND wnd) {
    g_dirty = 1;
    if (g_armed || !wnd) return;
    int64_t left = g_interval_us - (now_us() - g_last_us);
    UINT ms = left > 0 ? (UINT)((left + 999) / 1000) : 1;
    if (SetTimer(wnd, SOFTDD_PACE, ms, NULL)) g_armed = 1;   /* the 100 ms timer is the fallback */
}

void present_disarm(HWND wnd) {
    if (g_armed && wnd) KillTimer(wnd, SOFTDD_PACE);
    g_armed = 0;
}

void present_invalidate(HWND wnd) {
    g_bars_dirty = 1;
    g_hz_known = 0;   /* a new monitor, maybe */
    present_disarm(wnd);
    present_defer(wnd);
}

void present_note(int presented, int shown) {
    static int64_t t0;
    static long np, ns;
    int64_t t = now_us();
    if (!t0) t0 = t;
    np += presented;
    ns += shown;
    if (present_log && t - t0 >= 1000000) {
        fprintf(stderr, "[present] %.1f s: %ld presents, %ld shown (%d Hz, %s)\n", (double)(t - t0) / 1e6, np, ns,
                (int)(1000000 / g_interval_us), present_filter_name(present_filter));
    }
    if (t - t0 >= 1000000) { t0 = t; np = ns = 0; }
}
