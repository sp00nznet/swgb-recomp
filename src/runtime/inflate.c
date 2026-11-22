/*
 * The game's deflate decoder (sub_0062343B), done by the host with miniz.
 *
 * genie.dat, scenarios, campaigns and saved games are raw deflate streams.
 * Ensemble's decoder is hand-written assembly and a coroutine: when its input
 * runs dry or its output fills, it copies the guest stack between its entry
 * and its current depth into the state block, and on the next call pushes it
 * back and jumps to the saved return address (0x00622D7D / 0x00622DCD). A
 * lift cannot resume that: the "return addresses" in a lifted guest stack are
 * placeholders, and where the code really was lives on the host stack, which
 * is gone by then. The first decode wrote through a null pointer. So the
 * whole entry point is the host's, with the same contract:
 *
 *   int inflate(in, in_off, &in_len, out, out_off, &out_len, state, final)
 *   cdecl; in_len and out_len come back as the bytes consumed and produced.
 *   0: call again (input used up or output full); -1: end of the stream;
 *   -2: corrupt data.
 *
 * `state` is the guest's 0xAD90-byte block (sub_00623435 is its size). Its
 * resume index at +0xA958 is 0 in a fresh block, which is when the host
 * starts a new stream for it.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"   /* native32 on Windows, win32hle on Linux and macOS */
#include "miniz.h"

#define STATE_RESUME 0xA958u
#define MAX_STREAMS 16

static struct { uint32_t state; mz_stream z; } g_z[MAX_STREAMS];

static mz_stream* stream_for(uint32_t state) {
    int k = -1;
    for (int i = 0; i < MAX_STREAMS; i++)
        if (g_z[i].state == state) { k = i; break; }
    if (k >= 0 && MEM32(state + STATE_RESUME) != 0) return &g_z[k].z;
    if (k < 0)
        for (int i = 0; i < MAX_STREAMS; i++)
            if (!g_z[i].state) { k = i; break; }
    if (k < 0) k = (int)(state >> 4) % MAX_STREAMS;   /* ponytail: evict; 16 live streams is plenty */
    if (g_z[k].state) mz_inflateEnd(&g_z[k].z);
    memset(&g_z[k].z, 0, sizeof g_z[k].z);
    if (mz_inflateInit2(&g_z[k].z, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) return NULL;
    g_z[k].state = state;
    MEM32(state + STATE_RESUME) = 1;   /* not fresh any more */
    return &g_z[k].z;
}

/* Lifted call: return address at esp, arguments above it; `ret` pops it. */
void swgb_inflate(void) {
    uint32_t a[8];
    for (int i = 0; i < 8; i++) a[i] = MEM32(g_esp + 4 + 4 * i);
    uint32_t in = a[0] + a[1], in_len = MEM32(a[2]), out = a[3] + a[4], out_len = MEM32(a[5]);
    mz_stream* z = stream_for(a[6]);
    int r = -2;
    if (z) {
        z->next_in = (const unsigned char*)GUEST_PTR(in);
        z->avail_in = in_len;
        z->next_out = (unsigned char*)GUEST_PTR(out);
        z->avail_out = out_len;
        int s = mz_inflate(z, MZ_NO_FLUSH);
        MEM32(a[2]) = in_len - z->avail_in;
        MEM32(a[5]) = out_len - z->avail_out;
        r = s == MZ_STREAM_END ? -1 : (s == MZ_OK || s == MZ_BUF_ERROR) ? 0 : -2;
        if (r == -2) fprintf(stderr, "[inflate] state 0x%08X: corrupt stream (%d)\n", a[6], s);
    }
    g_eax = (uint32_t)r;
    g_esp += 4;
}
