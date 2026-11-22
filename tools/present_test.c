/* The presenter's layout and mouse mapping, checked without the game:
 *   cl /nologo tools\present_test.c src\runtime\present.c /Isrc\runtime user32.lib gdi32.lib
 *   present_test.exe        (exit 0 and "OK" when every check holds)
 * A window of each shape is made (never shown) and, for every filter, a
 * client pixel is mapped to a game pixel through the same rectangle the
 * picture is drawn into. */
#include <stdio.h>
#include <stdlib.h>
#include "present.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static const int sizes[][2] = { { 800, 600 }, { 1200, 700 }, { 640, 480 }, { 1920, 1080 }, { 700, 1000 }, { 801, 601 }, { 2560, 1440 } };
    static const int games[][2] = { { 800, 600 }, { 1024, 768 }, { 1280, 1024 } };
    HWND w = CreateWindowExA(0, "STATIC", "t", WS_POPUP, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    for (int g = 0; g < 3; g++)
        for (int s = 0; s < 7; s++)
            for (int m = 0; m < PRES_MODES; m++) {
                int gw = games[g][0], gh = games[g][1], cw = sizes[s][0], ch = sizes[s][1];
                RECT r;
                present_layout(m, cw, ch, gw, gh, &r);
                int rw = r.right - r.left, rh = r.bottom - r.top;
                CHECK(r.left >= 0 && r.top >= 0 && r.right <= cw && r.bottom <= ch, "%d: %dx%d in %dx%d: outside", m, gw, gh, cw, ch);
                CHECK(abs(r.left - (cw - r.right)) <= 1 && abs(r.top - (ch - r.bottom)) <= 1, "%d: %dx%d in %dx%d: not centred", m, gw, gh, cw, ch);
                CHECK(rw == cw || rh == ch || (m == PRES_INTEGER), "%d: %dx%d in %dx%d: touches no side", m, gw, gh, cw, ch);
                CHECK(abs(rw * gh - rh * gw) <= gw + gh, "%d: %dx%d in %dx%d: aspect %dx%d", m, gw, gh, cw, ch, rw, rh);
                if (m == PRES_INTEGER && cw >= gw && ch >= gh)
                    CHECK(rw % gw == 0 && rh % gh == 0, "integer: %dx%d in %dx%d: %dx%d is not a multiple", gw, gh, cw, ch, rw, rh);

                /* the mouse, through a real window of that client size */
                RECT wr = { 0, 0, cw, ch };
                AdjustWindowRect(&wr, WS_POPUP, FALSE);
                SetWindowPos(w, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);
                present_filter = m;
                /* a picture smaller than the frame covers several game pixels per client pixel: the edges are within that many */
                int tx = gw / rw, ty = gh / rh;
                POINT p = { r.left, r.top };
                present_to_game(w, gw, gh, &p);
                CHECK(p.x <= tx && p.y <= ty, "mode %d top-left of the picture -> %ld,%ld", m, p.x, p.y);
                p.x = r.right - 1; p.y = r.bottom - 1;
                present_to_game(w, gw, gh, &p);
                CHECK(p.x >= gw - 1 - tx && p.y >= gh - 1 - ty && p.x < gw && p.y < gh, "mode %d bottom-right (%d,%d) -> %ld,%ld (want %d,%d)", m, (int)r.right - 1, (int)r.bottom - 1, p.x, p.y, gw - 1, gh - 1);
                p.x = -50; p.y = ch + 50;   /* in a bar, off the window: the nearest edge */
                present_to_game(w, gw, gh, &p);
                CHECK(p.x <= tx && p.y >= gh - 1 - ty, "mode %d outside -> %ld,%ld", m, p.x, p.y);
                /* monotonic across the picture: every game pixel is reached, in order */
                int last = -1, ok = 1;
                for (int x = r.left; x < r.right; x++) {
                    POINT q = { x, r.top };
                    present_to_game(w, gw, gh, &q);
                    if (q.x < last || q.x >= gw) ok = 0;
                    last = q.x;
                }
                CHECK(ok, "mode %d: x not monotonic", m);
                /* the picture's centre is the game's centre (within a pixel) */
                p.x = (r.left + r.right) / 2; p.y = (r.top + r.bottom) / 2;
                present_to_game(w, gw, gh, &p);
                CHECK(abs(p.x - gw / 2) <= 1 && abs(p.y - gh / 2) <= 1, "mode %d centre -> %ld,%ld", m, p.x, p.y);
            }
    printf(g_fail ? "%d FAILED\n" : "OK\n", g_fail);
    return g_fail != 0;
}
