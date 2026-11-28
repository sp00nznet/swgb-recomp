/*
 * High resolution and widescreen (docs/hires.md).
 *
 * The game has three sizes of its own, 800x600, 1024x768 and 1280x1024 (its
 * Options screen, the registry's "Screen Size", the 800/1024/1280 switches).
 * Its in-game screen is laid out for each in screen_coords.txt (INTERFAC.DRS
 * 'bina' 50006, a column per size) and in a few size switches in the code,
 * over a bottom panel drawn from one picture per size and civilization (SLP
 * 51101-51146: 800x170, 1024x200, 1280x250).
 *
 * Any other size borrows one of the game's own layouts, its "class": 1280x1024
 * for sizes at least that big, else 1024x768. The game is told the class
 * wherever it switches on the size (swgb_cls, at the compares run_lift.py
 * lists), and draws at the real size. Then:
 *
 *  - every panel on the in-game screen is moved for the real size as its
 *    rect is set (TPanel's set_rect, swgb_rect): the bottom panel's contents
 *    down by the extra height, everything right of the split (the age plate,
 *    resources, minimap and buttons) right by the extra width; a panel across
 *    the split (the unit display, the map view) gets the extra width, and the
 *    map view the extra height. More of the map on screen, nothing stretched;
 *  - the bottom panel's picture is widened to match: a column inside the
 *    unit display repeated at the split (swgb_fetch, the game's DRS reads,
 *    sub_004D4B30).
 *
 * The lift stays the game's: run_lift.py's patches call in here.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "hires.h"

int swgb_res_w, swgb_res_h;   /* 0: the game's own choice */

#define GAME_SCREEN_VTBL 0x0065CE24u   /* TRIBE_Screen_Game (handle_size = sub_004F5210) */

/* A layout class: the game's size, where its bottom panel starts, the split
 * column of the panel picture (and the column repeated there), and the
 * panel pictures' first id (+1..+6 by civilization). */
typedef struct { int w, h, panel_top, split, src, slp0; } Class;
static const Class k_classes[] = {
    { 1280, 1024, 774, 455, 454, 51140 },
    { 1024,  768, 568, 362, 361, 51120 },
};

static int g_log = -1;
static int hlog(void) {
    if (g_log < 0) g_log = GetEnvironmentVariableA("SWGB_HIRES_LOG", NULL, 0) != 0;   /* what moved, once each */
    return g_log;
}

/* The class of the remaster's size, or NULL when it is one of the game's
 * own (or none is set). */
static const Class* cls(void) {
    if (!swgb_res_w) return NULL;
    for (int i = 0; i < 2; i++)
        if (swgb_res_w == k_classes[i].w && swgb_res_h == k_classes[i].h) return NULL;
    if (swgb_res_w == 800 && swgb_res_h == 600) return NULL;
    return swgb_res_w >= 1280 && swgb_res_h >= 1024 ? &k_classes[0] : &k_classes[1];
}

int swgb_res_valid(int w, int h) {
    return (w == 800 && h == 600) || (w >= 1024 && h >= 720 && w <= 4096 && h <= 4096);
}

/* sub_0042C0C0, as it reads the size to enter: w, h are its eax, esi. */
void swgb_game_res(uint32_t* w, uint32_t* h) {
    if (!swgb_res_w) return;
    *w = (uint32_t)swgb_res_w;
    *h = (uint32_t)swgb_res_h;
}

/* A size compare (run_lift.py CLASS_SITES): the remaster's width or height
 * reads as its class's. */
uint32_t swgb_cls(uint32_t site, uint32_t v, int is_h) {
    const Class* c = cls();
    if (!c) return v;
    uint32_t m = v;
    if (!is_h && v == (uint32_t)swgb_res_w) m = (uint32_t)c->w;
    if (is_h && v == (uint32_t)swgb_res_h) m = (uint32_t)c->h;
    if (m != v && hlog()) {
        static uint32_t seen[64];
        int i;
        for (i = 0; i < 64 && seen[i] && seen[i] != site; i++) ;
        if (i < 64 && !seen[i]) { seen[i] = site; printf("[hires] 0x%08X: %s %u as %u\n", site, is_h ? "height" : "width", v, m); }
    }
    return m;
}

/* A dialog over the in-game screen (the menu, the objectives...) is centred by
 * the game on the real size, and may place itself again a little off centre
 * (by its drop shadow): a panel first placed centred is left alone from then
 * on. None of the layout's panels is centred so (92 of them at 1920x1080). */
static int dialog(uint32_t self, int x1, int y1, int x2, int y2) {
    static uint32_t seen[16][2];
    static int next;
    uint32_t vt = MEM32(self);
    for (int i = 0; i < 16; i++) if (seen[i][0] == self && seen[i][1] == vt) return 1;
    if (abs(x1 + x2 + 1 - swgb_res_w) > 1 || abs(y1 + y2 + 1 - swgb_res_h) > 1) return 0;
    seen[next][0] = self, seen[next][1] = vt, next = (next + 1) % 16;
    if (hlog()) printf("[hires] dialog %08X (vt %08X) at %d,%d: centred for the real size, left alone\n", self, vt, x1, y1);
    return 1;
}

/* A panel's rect (x1, y1, x2, y2, inclusive), moved from the class's layout
 * to the real size when the panel is on the in-game screen (its parent) and
 * the rect lies in the class's layout; one already past the class's edges was
 * placed for the real size (anchored to the right or the bottom) and is left
 * alone, as is a dialog. Nonzero if it moved. */
static int move_rect(uint32_t self, int* x1, int* y1, int* x2, int* y2) {
    const Class* c = cls();
    uint32_t parent = c ? MEM32(self + 0x40) : 0;
    if (!parent || MEM32(parent) != GAME_SCREEN_VTBL || dialog(self, *x1, *y1, *x2, *y2)) return 0;
    /* one pixel over is still the class's: the unit display ends at 1025 */
    if (*x2 > c->w || *y2 > c->h || *x2 < *x1 || *y2 < *y1) return 0;
    int dw = swgb_res_w - c->w, dh = swgb_res_h - c->h, ox1 = *x1, oy1 = *y1, ox2 = *x2, oy2 = *y2;
    if (*x1 >= c->split) *x1 += dw, *x2 += dw;
    else if (*x2 >= c->split) *x2 += dw;
    if (*y1 >= c->panel_top) *y1 += dh, *y2 += dh;
    else if (*y2 >= c->panel_top - 1) *y2 += dh;
    if (*x1 == ox1 && *y1 == oy1 && *x2 == ox2 && *y2 == oy2) return 0;
    if (hlog())
        printf("[hires] panel %08X (vt %08X): %d,%d %dx%d -> %d,%d %dx%d\n", self, MEM32(self),
               ox1, oy1, ox2 - ox1 + 1, oy2 - oy1 + 1, *x1, *y1, *x2 - *x1 + 1, *y2 - *y1 + 1);
    return 1;
}

/* A panel's place is set two ways: TPanel::set_rect(x1, y1, x2, y2)
 * (sub_004B5780, from its own handle_size after set_positioning), which
 * calls the class's set_rect(x, y, w, h) (base sub_004B5660; the map view
 * has its own, sub_00606DA0, which must see the real size), and that one
 * straight (the game screen sizes its unit display so). Both are moved, on
 * entry; the second is let be when the first is what called it. */
static uint32_t g_placing;   /* the panel sub_004B5780 is placing */

void swgb_rect(uint32_t self, uint32_t esp) {   /* sub_004B5780 */
    int x1 = (int)MEM32(esp + 4), y1 = (int)MEM32(esp + 8), x2 = (int)MEM32(esp + 0xC), y2 = (int)MEM32(esp + 0x10);
    g_placing = self;
    if (!move_rect(self, &x1, &y1, &x2, &y2)) return;
    MEM32(esp + 4) = (uint32_t)x1, MEM32(esp + 8) = (uint32_t)y1;
    MEM32(esp + 0xC) = (uint32_t)x2, MEM32(esp + 0x10) = (uint32_t)y2;
}
void swgb_rect_done(void) { g_placing = 0; }

void swgb_rect_xywh(uint32_t self, uint32_t esp) {   /* sub_004B5660 */
    if (self == g_placing) return;
    int x1 = (int)MEM32(esp + 4), y1 = (int)MEM32(esp + 8);
    int x2 = x1 + (int)MEM32(esp + 0xC) - 1, y2 = y1 + (int)MEM32(esp + 0x10) - 1;
    if (!move_rect(self, &x1, &y1, &x2, &y2)) return;
    MEM32(esp + 4) = (uint32_t)x1, MEM32(esp + 8) = (uint32_t)y1;
    MEM32(esp + 0xC) = (uint32_t)(x2 - x1 + 1), MEM32(esp + 0x10) = (uint32_t)(y2 - y1 + 1);
}

/* A point the game draws at by hand, in its class's coordinates (the
 * screen's draw, sub_004FEDB0): moved like a panel's corner. */
void swgb_xy(uint32_t px, uint32_t py) {
    const Class* c = cls();
    if (!c) return;
    if ((int)MEM32(px) >= c->split) MEM32(px) += (uint32_t)(swgb_res_w - c->w);
    if ((int)MEM32(py) >= c->panel_top) MEM32(py) += (uint32_t)(swgb_res_h - c->h);
}

/* ---- the bottom panel's picture ----------------------------------------- */

#pragma pack(push, 1)
typedef struct { char ver[4]; int32_t frames; char comment[24]; } SlpHead;
typedef struct { uint32_t cmd, outline, pal, props; int32_t w, h, hx, hy; } SlpFrame;
#pragma pack(pop)

/* A resource out of the game's INTERFAC.DRS, read by the host. */
static uint8_t* drs_read(uint32_t type, uint32_t id, uint32_t* size) {
    FILE* f = fopen("Data\\INTERFAC.DRS", "rb");
    uint8_t head[0x80];
    uint8_t* out = NULL;
    if (!f) return NULL;
    if (fread(head, 1, sizeof head, f) == sizeof head) {
        int v = -1;
        for (int i = 0; i + 4 <= 0x60 && v < 0; i++) if (!memcmp(head + i, "1.00", 4)) v = i;
        int32_t ntab = v < 0 ? 0 : *(int32_t*)(head + v + 16);
        for (int t = 0; t < ntab && !out; t++) {
            uint32_t te[3];
            fseek(f, v + 24 + 12 * t, SEEK_SET);
            if (fread(te, 4, 3, f) != 3 || te[0] != type) continue;
            for (uint32_t i = 0; i < te[2] && !out; i++) {
                uint32_t e[3];
                fseek(f, (long)(te[1] + 12 * i), SEEK_SET);
                if (fread(e, 4, 3, f) != 3 || e[0] != id) continue;
                out = (uint8_t*)malloc(e[2]);
                fseek(f, (long)e[1], SEEK_SET);
                if (fread(out, 1, e[2], f) != e[2]) { free(out); out = NULL; }
                else *size = e[2];
            }
        }
    }
    fclose(f);
    return out;
}

/* Frame 0 of an SLP as palette indices and a mask (0 = transparent).
 * Interface pictures use colour lists, skips and fills; anything else, and
 * the picture is left as it is. */
static int slp_decode(const uint8_t* d, uint32_t n, uint8_t* px, uint8_t* m, int w, int h) {
    const SlpFrame* fr = (const SlpFrame*)(d + sizeof(SlpHead));
    memset(m, 0, (size_t)w * h);
    for (int y = 0; y < h; y++) {
        const uint16_t* ol = (const uint16_t*)(d + fr->outline + 4 * y);
        if (ol[0] & 0x8000 || ol[1] & 0x8000) continue;
        uint32_t p = *(const uint32_t*)(d + fr->cmd + 4 * y);
        int x = ol[0];
        for (;;) {
            if (p >= n) return 0;
            uint8_t c = d[p++];
            int k;
            if ((c & 3) == 0 || (c & 0xF) == 2) {
                k = (c & 3) == 0 ? c >> 2 : ((c & 0xF0) << 4) | d[p++];
                if (x + k > w || p + k > n) return 0;
                memcpy(px + y * w + x, d + p, k); memset(m + y * w + x, 1, k);
                p += k; x += k;
            } else if ((c & 3) == 1 || (c & 0xF) == 3) {
                x += (c & 3) == 1 ? c >> 2 : ((c & 0xF0) << 4) | d[p++];
            } else if ((c & 0xF) == 7) {
                k = c >> 4 ? c >> 4 : d[p++];
                if (x + k > w) return 0;
                memset(px + y * w + x, d[p++], k); memset(m + y * w + x, 1, k);
                x += k;
            } else if ((c & 0xF) == 0xF) {
                break;
            } else {
                return 0;
            }
        }
    }
    return 1;
}

/* An SLP of one frame, w x h, from indices and a mask: colour lists and
 * skips, as the game's own pictures are made. */
static uint8_t* slp_encode(const SlpHead* head0, const SlpFrame* fr0, const uint8_t* px, const uint8_t* m,
                           int w, int h, int hy, uint32_t* size) {
    size_t cap = sizeof(SlpHead) + sizeof(SlpFrame) + 8 * (size_t)h + (size_t)h * (2 * (size_t)w + 8);
    uint8_t* o = (uint8_t*)calloc(1, cap);
    SlpHead* hd = (SlpHead*)o;
    SlpFrame* fr = (SlpFrame*)(o + sizeof *hd);
    *hd = *head0;
    hd->frames = 1;
    *fr = *fr0;
    fr->w = w, fr->h = h, fr->hy = hy;
    fr->outline = sizeof *hd + sizeof *fr;
    fr->cmd = fr->outline + 4 * h;
    uint32_t p = fr->cmd + 4 * h;
    for (int y = 0; y < h; y++) {
        const uint8_t* rm = m + y * w;
        int l = 0, r = 0;
        while (l < w && !rm[l]) l++;
        while (r < w - l && !rm[w - 1 - r]) r++;
        uint16_t* ol = (uint16_t*)(o + fr->outline + 4 * y);
        *(uint32_t*)(o + fr->cmd + 4 * y) = p;
        if (l == w) { ol[0] = ol[1] = 0x8000; o[p++] = 0x0F; continue; }
        ol[0] = (uint16_t)l, ol[1] = (uint16_t)r;
        for (int x = l; x < w - r; ) {
            int on = rm[x], k = 1;
            while (x + k < w - r && rm[x + k] == on && k < 4095) k++;
            if (k < 64) o[p++] = (uint8_t)(k << 2 | (on ? 0 : 1));
            else { o[p++] = (uint8_t)((k >> 8) << 4 | (on ? 2 : 3)); o[p++] = (uint8_t)k; }
            if (on) { memcpy(o + p, px + y * w + x, k); p += k; }
            x += k;
        }
        o[p++] = 0x0F;
    }
    *size = p;
    return o;
}

/* The class's panel picture for one civilization, widened to the real size
 * and dropped to its bottom edge. Made once per size and kept: the game may
 * draw straight from what the fetch returns. */
static uint8_t* wide_panel(const Class* c, uint32_t id, uint32_t* size) {
    static struct { uint32_t id; int w, h; uint8_t* d; uint32_t n; } cache[16];
    for (int i = 0; i < 16; i++)
        if (cache[i].d && cache[i].id == id && cache[i].w == swgb_res_w && cache[i].h == swgb_res_h) {
            *size = cache[i].n;
            return cache[i].d;
        }
    uint32_t n = 0;
    uint8_t* d = drs_read(0x736C7020u /* 'slp ' */, id, &n);
    if (!d) return NULL;
    const SlpHead* hd = (const SlpHead*)d;
    const SlpFrame* fr = (const SlpFrame*)(d + sizeof *hd);
    uint8_t* out = NULL;
    if (hd->frames >= 1 && fr->w == c->w && fr->h > 0 && fr->h < 1024) {
        int w = fr->w, h = fr->h, W = swgb_res_w, dw = W - w;
        uint8_t* px = (uint8_t*)malloc((size_t)w * h);
        uint8_t* m = (uint8_t*)malloc((size_t)w * h);
        uint8_t* wpx = (uint8_t*)malloc((size_t)W * h);
        uint8_t* wm = (uint8_t*)malloc((size_t)W * h);
        if (slp_decode(d, n, px, m, w, h)) {
            for (int y = 0; y < h; y++) {
                const uint8_t* sp = px + y * w; const uint8_t* sm = m + y * w;
                uint8_t* dp = wpx + y * W; uint8_t* dm = wm + y * W;
                memcpy(dp, sp, c->split); memcpy(dm, sm, c->split);
                memset(dp + c->split, sp[c->src], dw); memset(dm + c->split, sm[c->src], dw);
                memcpy(dp + c->split + dw, sp + c->split, w - c->split);
                memcpy(dm + c->split + dw, sm + c->split, w - c->split);
            }
            out = slp_encode(hd, fr, wpx, wm, W, h, fr->hy - (swgb_res_h - c->h), size);
        }
        free(px); free(m); free(wpx); free(wm);
    }
    free(d);
    if (!out) { fprintf(stderr, "[hires] panel picture %u: not widened\n", id); return NULL; }
    for (int i = 0; i < 16; i++)
        if (!cache[i].d) { cache[i].id = id, cache[i].w = swgb_res_w, cache[i].h = swgb_res_h, cache[i].d = out, cache[i].n = *size; break; }
    if (hlog()) printf("[hires] panel picture %u widened to %d wide (%u bytes)\n", id, swgb_res_w, *size);
    return out;
}

/* The game's DRS read (sub_004D4B30, fastcall: type, id, &allocated,
 * &size), on entry. Returns nonzero with *data set when the host answers. */
int swgb_fetch(uint32_t type, uint32_t id, uint32_t allocated, uint32_t size, uint32_t* data) {
    const Class* c = cls();
    if (!c || type != 0x736C7020u || id <= (uint32_t)c->slp0 || id > (uint32_t)c->slp0 + 6) return 0;
    uint32_t n = 0;
    uint8_t* d = wide_panel(c, id, &n);
    if (!d) return 0;
    if (allocated) MEM32(allocated) = 0;   /* the game must not free it */
    if (size) MEM32(size) = n;
    *data = (uint32_t)(uintptr_t)d;
    return 1;
}
