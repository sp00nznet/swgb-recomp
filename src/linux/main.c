/*
 * Star Wars: Galactic Battlegrounds - the native host, for Linux and macOS.
 *
 * The same lifted C as the Windows build (src/recomp/gen), on pcrecomp's
 * runtime/win32hle instead of runtime/native32: every Win32 import the game
 * makes is answered by win32hle's own implementation (files, windows and
 * their controls, DirectDraw and DirectSound on SDL2, the registry, Winsock),
 * so no Windows and no Wine. softdd is not here: win32hle's DirectDraw is the
 * virtual display, and screen.c shows it (docs/linux-macos.md, "DirectDraw").
 *
 * What is here is what is specific to this game, as on Windows (host.c):
 * where the image goes, the command line, the play disc, scripted input,
 * --shot and --record, muting, and the fault report. Modelled on Red Alert 2's
 * and Civilization III's src/linux.
 *
 * 32-bit (Linux, -m32) it is linked non-PIE at 0x08048000, so the guest's
 * 0x00400000..0x00A5A000 is free; 64-bit (macOS, or Linux x86-64/arm64) the
 * guest's memory is one block of the host's and the image goes at its own
 * address in it.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>

#include "win32hle.h"
#include "pe_loader.h"
#include "host.h"
#include "keyscript.h"
#include "swgb_linux.h"

extern const uint32_t swgb_entry_va;   /* recomp_dispatch.c */

#define IMAGE_BASE 0x00400000u
#define EXE_STAMP  0x3C03FD69u         /* Battlegrounds.exe v1.1 (run_lift.py refuses others) */
#define REG_GAME   "Software\\LucasArts Entertainment Company LLC\\Star Wars Galactic Battlegrounds\\1.0"

char swgb_cd_letter;
int  swgb_test_run;
static int g_log_files;
static unsigned g_watchdog_s;

/* ---- what the lift calls into ---------------------------------------------- */
void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "\n[not-lifted] sub_%08X  (called from 0x%08X)\n", va, g_cur_func);
    fflush(stderr);
    _exit(2);
}

/* ---- file opens: --files, and on:NAME in --keys ----------------------------- */
static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_seen_cond = PTHREAD_COND_INITIALIZER;
static char g_wait_file[64];
static int  g_file_seen;

void swgb_file_seen(const char* name, int ok) {
    if (g_log_files) printf("[host] open %s -> %s\n", name, ok ? "ok" : "missing");
    pthread_mutex_lock(&g_seen_lock);
    if (g_wait_file[0] && strcasestr(name, g_wait_file)) {
        g_wait_file[0] = 0;
        g_file_seen = 1;
        pthread_cond_signal(&g_seen_cond);
    }
    pthread_mutex_unlock(&g_seen_lock);
}

/* ---- the frame: --shot and --record ------------------------------------------
 * The primary as win32hle's DirectDraw holds it (8-bit through its palette,
 * or RGB565), as 24-bit RGB rows, top-down, into the caller's buffer (grown
 * as needed: --shot and --record each have their own). Read holding the
 * machine: a mode change can free the primary under us. */
typedef struct { uint8_t* rgb; size_t n; } frame_buf;

static int grab(frame_buf* fb, int* w_out, int* h_out) {
    const uint8_t* px;
    const uint32_t* pal;
    int w, h, pitch, bpp, ok;
    mach_enter();
    ok = hle_dd_frame(&px, &w, &h, &pitch, &bpp, &pal, NULL) && (bpp == 8 || bpp == 16) && w > 0 && w <= 4096 && h > 0;
    if (ok && fb->n < (size_t)w * h * 3) {
        free(fb->rgb);
        fb->n = (size_t)w * h * 3;
        fb->rgb = (uint8_t*)malloc(fb->n);
        if (!fb->rgb) fb->n = 0, ok = 0;
    }
    for (int y = 0; ok && y < h; y++) {
        const uint8_t* s = px + (size_t)y * pitch;
        uint8_t* d = fb->rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++, d += 3) {
            if (bpp == 8) {
                uint32_t e = pal ? pal[s[x]] : s[x] * 0x010101u;   /* PALETTEENTRY: r, g, b */
                d[0] = (uint8_t)e, d[1] = (uint8_t)(e >> 8), d[2] = (uint8_t)(e >> 16);
            } else {
                uint16_t p = ((const uint16_t*)s)[x];
                d[0] = (uint8_t)((p >> 11) << 3), d[1] = (uint8_t)((p >> 5 & 63) << 2), d[2] = (uint8_t)((p & 31) << 3);
            }
        }
    }
    mach_leave();
    if (ok) *w_out = w, *h_out = h;
    return ok;
}

/* From the --shot thread and the --keys thread: one at a time. */
static void write_bmp(const char* path) {
    static frame_buf fb;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    int w, h;
    pthread_mutex_lock(&lock);
    FILE* f = NULL;
    if (!grab(&fb, &w, &h)) fprintf(stderr, "[shot] no frame yet for %s\n", path);
    else if (!(f = fopen(path, "wb"))) fprintf(stderr, "[shot] cannot write %s\n", path);
    if (!f) { pthread_mutex_unlock(&lock); return; }
    uint32_t row = (uint32_t)(w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54, hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1, hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t line[4096 * 3 + 4] = { 0 };
    for (int y = h - 1; y >= 0; y--) {                    /* BMP rows are bottom-up, pixels BGR */
        const uint8_t* s = fb.rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) line[3 * x] = s[3 * x + 2], line[3 * x + 1] = s[3 * x + 1], line[3 * x + 2] = s[3 * x];
        fwrite(line, 1, row, f);
    }
    fclose(f);
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "[shot] %dx%d -> %s\n", w, h, path);
}

static char g_shot_file[16][PATH_MAX];
static unsigned g_shot_s[16];
static int g_shots;
static void* shot_thread(void* unused) {
    (void)unused;
    uint32_t t0 = hle_ticks_ms();
    for (int i = 0; i < g_shots; i++) {
        int32_t wait = (int32_t)(t0 + g_shot_s[i] * 1000u - hle_ticks_ms());
        if (wait > 0) usleep((useconds_t)wait * 1000u);
        write_bmp(g_shot_file[i]);
    }
    return NULL;
}

/* --record out.mp4: 30 frames a second against the clock, piped to ffmpeg,
 * so the mp4 plays at the game's real speed (as softdd records on Windows).
 * --frames N closes it after N and ends the run. */
static const char* g_record;
static long g_record_frames;
static FILE* g_ffmpeg;
static pthread_mutex_t g_rec_lock = PTHREAD_MUTEX_INITIALIZER;

static void record_close(void) {
    pthread_mutex_lock(&g_rec_lock);
    if (g_ffmpeg) { pclose(g_ffmpeg); g_ffmpeg = NULL; fprintf(stderr, "[record] closed %s\n", g_record); }
    pthread_mutex_unlock(&g_rec_lock);
}
static void on_exit_close(int code) { (void)code; record_close(); }

static void* record_thread(void* unused) {
    (void)unused;
    int rec_w = 0, rec_h = 0;
    long n = 0;
    frame_buf fb = { NULL, 0 };
    uint32_t next = hle_ticks_ms();
    for (;;) {
        next += 33;
        int32_t wait = (int32_t)(next - hle_ticks_ms());
        if (wait > 0) usleep((useconds_t)wait * 1000u);
        int w, h;
        if (!grab(&fb, &w, &h)) continue;
        pthread_mutex_lock(&g_rec_lock);
        if (!g_ffmpeg && n) { pthread_mutex_unlock(&g_rec_lock); return NULL; }   /* closed */
        if (!g_ffmpeg) {
            char cmd[PATH_MAX + 256];
            rec_w = w, rec_h = h;                         /* a later mode is cropped or padded to the first */
            snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r 30 -i - "
                     "-c:v libx264 -preset veryfast -pix_fmt yuv420p '%s'", rec_w, rec_h, g_record);
            g_ffmpeg = popen(cmd, "w");
            if (!g_ffmpeg) { fprintf(stderr, "[record] cannot start ffmpeg\n"); pthread_mutex_unlock(&g_rec_lock); return NULL; }
            fprintf(stderr, "[record] %dx%d at 30 fps -> %s\n", rec_w, rec_h, g_record);
        }
        static uint8_t pad[4096 * 3];
        for (int y = 0; y < rec_h; y++) {
            if (y < h && w >= rec_w) fwrite(fb.rgb + (size_t)y * w * 3, 3, (size_t)rec_w, g_ffmpeg);
            else if (y < h) { fwrite(fb.rgb + (size_t)y * w * 3, 3, (size_t)w, g_ffmpeg); fwrite(pad, 3, (size_t)(rec_w - w), g_ffmpeg); }
            else fwrite(pad, 3, (size_t)rec_w, g_ffmpeg);
        }
        int bad = ferror(g_ffmpeg);
        pthread_mutex_unlock(&g_rec_lock);
        if (bad) { fprintf(stderr, "[record] ffmpeg stopped taking frames\n"); record_close(); return NULL; }
        if (++n == g_record_frames) {
            fprintf(stderr, "[record] %ld frames: stopping (--frames)\n", n);
            record_close();
            hle_exit(0);
        }
    }
}

/* ---- scripted input (--keys) -------------------------------------------------
 * The grammar is keyscript.h's, the same as the Windows host's. Input goes in
 * as a mouse and keyboard would give it (hle_input_*): win32hle hit-tests the
 * click and sends keys to the focus, so a text field gets its own. The
 * window is at the screen's origin, so game pixels are screen pixels. */
static keyscript_step g_steps[KEYSCRIPT_MAX];
static int g_nsteps;

static void mouse(uint32_t msg, int x, int y) { mach_enter(); hle_input_mouse(msg, x, y); mach_leave(); }
static void key(int down, int vk) { mach_enter(); hle_input_key(down, (uint32_t)vk, 0); mach_leave(); }

static void wait_for_file(const char* name) {
    pthread_mutex_lock(&g_seen_lock);
    if (!g_wait_file[0] && !g_file_seen) snprintf(g_wait_file, sizeof g_wait_file, "%s", name);
    while (!g_file_seen) pthread_cond_wait(&g_seen_cond, &g_seen_lock);
    g_file_seen = 0;
    pthread_mutex_unlock(&g_seen_lock);
}

static void* keys_thread(void* unused) {
    (void)unused;
    uint32_t t0 = hle_ticks_ms();
    for (int i = 0; i < g_nsteps; i++) {
        keyscript_step* st = &g_steps[i];
        if (st->on[0]) {
            wait_for_file(st->on);
            t0 = hle_ticks_ms();
            printf("[keys] %s opened: times count from here\n", st->on);
            continue;
        }
        for (int j = i + 1; j < g_nsteps; j++)            /* the next wait listens from now */
            if (g_steps[j].on[0]) {
                pthread_mutex_lock(&g_seen_lock);
                if (!g_wait_file[0] && !g_file_seen) snprintf(g_wait_file, sizeof g_wait_file, "%s", g_steps[j].on);
                pthread_mutex_unlock(&g_seen_lock);
                break;
            }
        int32_t wait = (int32_t)(t0 + st->at_ms - hle_ticks_ms());
        if (wait > 0) usleep((useconds_t)wait * 1000u);
        if (st->shot[0]) { write_bmp(st->shot); continue; }
        if (st->move) {
            mouse(0x0200u, st->x, st->y);                 /* WM_MOUSEMOVE */
            if (st->click) {
                usleep(100000);
                mouse(0x0201u, st->x, st->y);             /* WM_LBUTTONDOWN */
                usleep(150000);
                mouse(0x0202u, st->x, st->y);             /* WM_LBUTTONUP */
            }
            printf("[keys] %.1fs %s %d,%d\n", st->at_ms / 1000.0, st->click ? "click" : "move", st->x, st->y);
        } else {
            key(1, st->vk);                               /* TranslateMessage makes the WM_CHAR */
            usleep(150000);
            key(0, st->vk);
            printf("[keys] %.1fs key %02X\n", st->at_ms / 1000.0, st->vk);
        }
    }
    return NULL;
}

/* ---- faults and the watchdog ------------------------------------------------- */
static void report(const char* why) {
    fprintf(stderr, "\n=== %s ===\n  in lifted sub_%08X, last import %s\n", why, g_cur_func,
            g_cur_import ? g_cur_import : "(none)");
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    fprintf(stderr, "last indirect calls (newest first):\n");
    for (int i = 1; i <= 12 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* nm = hle_name(g_icall_trace[k]);
        fprintf(stderr, "  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
}

static void on_fault(int sig, siginfo_t* si, void* uc) {
    static volatile int once;
    (void)uc;
    if (__sync_lock_test_and_set(&once, 1)) _exit(3);
    char why[96];
    snprintf(why, sizeof why, "fault: signal %d, address %p", sig, si->si_addr);
    report(why);
    record_close();
    fflush(stderr);
    _exit(3);
}

static void* watchdog(void* unused) {
    (void)unused;
    sleep(g_watchdog_s);
    fprintf(stderr, "\n[watchdog] %u s: in sub_%08X, last import %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, g_cur_import ? g_cur_import : "(none)", g_icall_count);
    fflush(stdout);
    hle_exit(4);
    return NULL;
}

static void spawn(void* (*fn)(void*)) {
    pthread_t t;
    pthread_create(&t, NULL, fn, NULL);
    pthread_detach(t);
}

/* ---- startup ------------------------------------------------------------------ */
static uint32_t exe_stamp(const char* path) {
    uint8_t head[4096] = { 0 };
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(head, 1, sizeof head, f);
    fclose(f);
    uint32_t nt = n >= 0x40 ? *(const uint32_t*)(head + 0x3C) : 0;
    return nt && nt < sizeof head - 12 ? *(const uint32_t*)(head + nt + 8) : 0;
}

/* An absolute path for a file named on the command line: the run changes
 * into the game folder before it writes anything. */
static void absolute(const char* p, char* out, size_t n) {
    char cwd[PATH_MAX];
    if (p[0] == '/') snprintf(out, n, "%s", p);
    else snprintf(out, n, "%s/%s", getcwd(cwd, sizeof cwd) ? cwd : ".", p);
}

/* The play disc (disc 2), mounted or copied: a folder with Game/Avi in it,
 * from --cd DIR or $SWGB_CD. It becomes drive D:, which the game finds as a
 * CD-ROM (shims.c) through CDPath, the letter it checks after its first run. */
static void cd_pick(const char* dir) {
    char full[PATH_MAX], probe[PATH_MAX + 16];
    struct stat st;
    if (!dir || !*dir) dir = getenv("SWGB_CD");
    if (!dir || !*dir || !realpath(dir, full)) {
        printf("  no play disc: mount disc 2 and pass --cd DIR (or set SWGB_CD)\n");
        return;
    }
    snprintf(probe, sizeof probe, "%s/Game", full);
    if (stat(probe, &st) != 0) printf("  %s has no Game folder: is it disc 2?\n", full);
    swgb_cd_letter = 'D';
    hle_set_drive('D', full);
    hle_reg_set_string(0x80000002u, REG_GAME, "CDPath", "D:\\");   /* HKEY_LOCAL_MACHINE */
    printf("  play disc: D: = %s\n", full);
}

static void usage(void) {
    printf("usage: swgbrecomp [--run] [--headless] [--record out.mp4] [--frames N] [--sound]\n"
           "                  [--shot S file.bmp] [--keys SCRIPT] [--watchdog S] [--files] [--multi]\n"
           "                  [--exe work/Battlegrounds.exe] [--game game] [--cd DIR]\n"
           "                  [--args \"<game switches>\"] [--fullscreen] [--scale sharp|smooth|crt|nearest|integer]\n"
           "                  [--trace]\n");
}

int main(int argc, char** argv) {
    const char* exe = "work/Battlegrounds.exe";
    const char* game = "game";
    const char* cd = NULL;
    const char* scale = NULL;
    char guest_args[512] = "";
    int run = 0, sound = 0, headless = 0, fullscreen = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--sound")) sound = 1;
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            static char rec[PATH_MAX];
            absolute(argv[++i], rec, sizeof rec);
            g_record = rec;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_record_frames = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc && g_shots < 16) {
            g_shot_s[g_shots] = (unsigned)strtoul(argv[++i], NULL, 0);
            absolute(argv[++i], g_shot_file[g_shots++], PATH_MAX);
        }
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
            const char* bad = "";
            g_nsteps = keyscript_parse(argv[++i], g_steps, KEYSCRIPT_MAX, &bad);
            if (g_nsteps < 0) { fprintf(stderr, "--keys: unknown key \"%s\"\n", bad); return 1; }
            for (int k = 0; k < g_nsteps; k++)
                if (g_steps[k].shot[0]) { char p[PATH_MAX]; absolute(g_steps[k].shot, p, sizeof p);
                                          snprintf(g_steps[k].shot, sizeof g_steps[k].shot, "%s", p); }
        }
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--files")) g_log_files = 1;
        else if (!strcmp(argv[i], "--multi")) {}           /* named objects are this process's own here */
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--cd") && i + 1 < argc) cd = argv[++i];
        else if (!strcmp(argv[i], "--args") && i + 1 < argc) snprintf(guest_args, sizeof guest_args, " %s", argv[++i]);
        else if (!strcmp(argv[i], "--trace")) win32hle_trace = 1;
        else { usage(); return !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") ? 0 : 1; }
    }
    swgb_test_run = headless || g_nsteps || g_watchdog_s;

    char game_full[PATH_MAX], exe_full[PATH_MAX];
    if (!realpath(game, game_full)) { fprintf(stderr, "no game folder at %s\n", game); return 1; }
    if (!realpath(exe, exe_full)) {                       /* no work/ copy: the game folder's own */
        snprintf(exe_full, sizeof exe_full, "%s/Battlegrounds.exe", game_full);
        exe = exe_full;
    }
    uint32_t stamp = exe_stamp(exe_full);
    if (stamp != EXE_STAMP) {
        fprintf(stderr, "%s is not the build this host was lifted from (PE timestamp 0x%08X, wanted 0x%08X)\n",
                exe_full, stamp, EXE_STAMP);
        return 1;
    }
    hle_set_drive('C', game_full);                        /* the game's folder is C:\ */

    signal(SIGPIPE, SIG_IGN);                             /* a recorder whose ffmpeg went away gets an error */
    {   /* faults report on their own stack: a guest stack overflow is one */
        static uint8_t alt[1 << 16];
        stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt };
        sigaltstack(&ss, NULL);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
    }

    swgb_register_shims();                                /* first: the game's own answers win */
    recomp_host_init();
#ifdef __APPLE__
    printf("Galactic Battlegrounds recomp host (native macOS)\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);
#else
    printf("Galactic Battlegrounds recomp host (native Linux)\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);
#endif
    cd_pick(cd);

    pe_image img;
    if (recomp_pe_map(exe_full, &img) != 0 || img.base != IMAGE_BASE) {
        fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, IMAGE_BASE);
        return 1;
    }
    hle_module_add("C:\\Battlegrounds.exe", img.base, img.resource_rva, img.resource_size, 1);
    int unresolved = recomp_pe_bind(&img, hle_resolve_or_stub);
    printf("  mapped %s: 0x%08X-0x%08X, %d imports not implemented yet\n", exe, img.base, img.base + img.span, unresolved);
    char cmdline[600];
    snprintf(cmdline, sizeof cmdline, "\"C:\\Battlegrounds.exe\"%s", guest_args);
    hle_set_command_line(cmdline);
    printf("  guest command line %s\n", cmdline);
    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", swgb_entry_va);
        return 0;
    }
    if (chdir(game_full) != 0) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }

    /* Test runs are silent unless --sound, as on Windows; headless, SDL's
     * dummy drivers keep the run off the screen and the speakers. */
    if (!swgb_test_run) sound = 1;
    if (!sound) {
        hle_dsound_set_master(0.0f);
        if (headless) setenv("SDL_AUDIODRIVER", "dummy", 0);
    }
    printf("  audio: %s\n", sound ? "on" : "muted (a test run; --sound to hear it)");
    if (scale && !hle_screen_scale(scale)) fprintf(stderr, "[host] no scaling \"%s\": sharp, smooth, crt, nearest or integer\n", scale);
    if (hle_screen_open("Star Wars Galactic Battlegrounds", fullscreen, headless) != 0) return 1;
    hle_on_exit = on_exit_close;
    if (g_record) spawn(record_thread);
    if (g_shots) spawn(shot_thread);
    if (g_nsteps) spawn(keys_thread);
    if (g_watchdog_s) spawn(watchdog);
    printf("  entering 0x%08X\n\n", swgb_entry_va);
    uint32_t rc = hle_call_guest(swgb_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", rc);
    hle_exit((int)rc);
    return 0;
}
