/*
 * Star Wars: Galactic Battlegrounds - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks,
 * machine lock and guest exceptions; see its header). Every import goes to
 * the real DLL -- DirectSound, DirectPlay, winmm, VfW -- except DirectDraw,
 * which is softdd (softdd.c): the game's fullscreen 8-bit mode in a window,
 * or nowhere with --headless, and the desktop's mode never changes. What is
 * here is only what is specific to this game: where the image goes, the
 * command line, the play disc, the window, scripted input, muting and the
 * fault report. Taken from X-Wing's host (xwing-recomp), which is Crazy
 * Taxi's.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so the guest's
 * 0x00400000..0x00A5A000 is free when main() maps the image.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <shlwapi.h>
#include <windowsx.h>

#include "native32.h"
#include "recomp_trace.h"
#include "softdd.h"
#include "softdd_int.h"
#include "hires.h"
#include "dplay32.h"

extern const uint32_t swgb_entry_va;  /* recomp_dispatch.c */

#define IMAGE_BASE 0x00400000u
#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

static DWORD g_watchdog_s;
static int   g_headless;
static int   g_hide;   /* --headless without a private desktop: keep windows off the screen */
static HWND  g_game_wnd;

/* --shot SECONDS FILE: the next frame presented after SECONDS, as a .bmp. */
static DWORD g_shot_s[16];
static char  g_shot_file[16][MAX_PATH];
static int   g_shots;
static DWORD WINAPI shot_thread(LPVOID unused) {
    (void)unused;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < g_shots; i++) {
        DWORD at = t0 + g_shot_s[i] * 1000, now = GetTickCount();
        if ((int)(at - now) > 0) Sleep(at - now);
        softdd_shot(g_shot_file[i]);
    }
    return 0;
}

/* ---- the process is the host, the module is the guest ----------------------
 * GetModuleHandleA(NULL) and GetModuleFileNameA(NULL) would name the host;
 * the game finds its own folder through them. */
static char g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 512];

static void shim_GetModuleHandleA(void) {
    g_eax = ARG(0) ? (uint32_t)(uintptr_t)GetModuleHandleA((LPCSTR)(uintptr_t)ARG(0)) : IMAGE_BASE;
    g_esp += 4 + 1 * 4;
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == IMAGE_BASE) {
        uint32_t n = (uint32_t)strlen(g_guest_exe);
        if (size) {
            uint32_t k = n < size ? n : size - 1;
            memcpy(out, g_guest_exe, k);
            out[k] = 0;
            n = k;
        }
        g_eax = n;
    } else {
        g_eax = GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size);
    }
    g_esp += 4 + 3 * 4;
}

static void shim_GetCommandLineA(void) {
    g_eax = (uint32_t)(uintptr_t)g_guest_cmdline;
    g_esp += 4;
}

/* A shim runs holding the machine, and a window call can wait on another
 * thread answering messages: run it with the machine let go. */
#define UNLOCKED(call) do { uint32_t esp_ = g_esp; mach_leave(); \
    call; DWORD err_ = GetLastError(); mach_enter(); \
    g_esp = esp_; SetLastError(err_); } while (0)

static void shim_MessageBoxA(void) {
    const char* text = ARG(1) ? (const char*)(uintptr_t)ARG(1) : "";
    const char* cap = ARG(2) ? (const char*)(uintptr_t)ARG(2) : "";
    printf("[host] MessageBoxA \"%s\": %s\n", cap, text);
    if (g_headless || g_watchdog_s) {   /* test runs never put a box on the screen */
        uint32_t b = ARG(3) & MB_TYPEMASK;
        g_eax = (b == MB_YESNO || b == MB_YESNOCANCEL) ? IDNO
              : (b == MB_RETRYCANCEL || b == MB_OKCANCEL) ? IDCANCEL
              : b == MB_ABORTRETRYIGNORE ? IDABORT : IDOK;
    } else {
        HWND w = (HWND)(uintptr_t)ARG(0);
        UINT type = ARG(3);
        int r;
        UNLOCKED(r = MessageBoxA(w, text, cap, type));
        g_eax = r;
    }
    g_esp += 4 + 4 * 4;
}

static void shim_OutputDebugStringA(void) {
    if (ARG(0)) printf("[dbg] %s", (const char*)(uintptr_t)ARG(0));
    g_esp += 4 + 1 * 4;
}

static void shim_ExitProcess(void) {
    UINT code = ARG(0);
    printf("[host] ExitProcess(%u) from sub_%08X\n", code, g_cur_func);
    softdd_finish();
    recomp_trace_flush();
    fflush(NULL);   /* ExitProcess skips the CRT's own flush: a piped log lost its tail */
    ExitProcess(code);
}

/* ---- the play disc -------------------------------------------------------------
 * The game wants disc 2, the play disc, in a real CD-ROM drive (your disc, or
 * your disc image mounted) and reads its movies from it (%sgame\avi\%s.avi).
 * The host only finds that drive (CD_DRIVE=E or --cd E, or the first CD-ROM
 * drive with Game\Avi on it) and stores its letter in CDPath, the one letter
 * the game checks after its first run (HKLM, virtualised to HKCU's
 * VirtualStore for this process as for the game). */
static char g_cd = 0;

static void cd_pick(const char* want) {
    char env[8], root[] = "?:\\", probe[16];
    if (want && *want) g_cd = (char)(want[0] & ~0x20);
    else if (GetEnvironmentVariableA("CD_DRIVE", env, sizeof env)) g_cd = (char)(env[0] & ~0x20);
    for (char c = 'D'; c <= 'Z' && !g_cd; c++) {
        root[0] = c;
        _snprintf(probe, sizeof probe, "%sGame\\Avi", root);
        if (GetDriveTypeA(root) == DRIVE_CDROM && GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) g_cd = c;
    }
    if (!g_cd) { printf("  no play disc found: put disc 2 in the drive or mount its image (or set CD_DRIVE)\n"); return; }
    root[0] = g_cd;
    RegSetKeyValueA(HKEY_LOCAL_MACHINE, "Software\\LucasArts Entertainment Company LLC\\Star Wars Galactic Battlegrounds\\1.0",
                    "CDPath", REG_SZ, root, sizeof root);
    printf("  play disc: %c:\n", g_cd);
}

/* "on:NAME" in --keys waits until the game opens a file whose name has NAME
 * in it, and the times after it count from then. */
static char g_wait_file[64];
static HANDLE g_file_seen;
static int g_log_files;      /* --files: every open */

static void file_seen(const char* name, int ok) {
    if (g_log_files) printf("[host] open %s -> %s\n", name ? name : "(null)", ok ? "ok" : "missing");
    if (g_wait_file[0] && name && StrStrIA(name, g_wait_file)) { g_wait_file[0] = 0; SetEvent(g_file_seen); }
}

static void shim_CreateFileA(void) {
    const char* name = (const char*)(uintptr_t)ARG(0);
    g_eax = (uint32_t)(uintptr_t)CreateFileA(name, ARG(1), ARG(2), (LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(3),
                                             ARG(4), ARG(5), (HANDLE)(uintptr_t)ARG(6));
    file_seen(name, g_eax != (uint32_t)(uintptr_t)INVALID_HANDLE_VALUE);
    g_esp += 4 + 7 * 4;
}

static void shim_OpenFile(void) {
    const char* name = (const char*)(uintptr_t)ARG(0);
    g_eax = (uint32_t)OpenFile(name, (LPOFSTRUCT)(uintptr_t)ARG(1), ARG(2));
    file_seen(name, g_eax != (uint32_t)HFILE_ERROR);
    g_esp += 4 + 3 * 4;
}

/* MCIWndCreateA plays the movies (the intro, the campaign's): an MCI
 * window over the game's, with the file on the play disc. */
static void shim_MCIWndCreateA(void) {
    const char* file = (const char*)(uintptr_t)ARG(3);
    printf("[host] MCIWndCreateA %s\n", file ? file : "(null)");
    HWND h;
    HWND parent = (HWND)(uintptr_t)ARG(0);
    HINSTANCE inst = (HINSTANCE)(uintptr_t)ARG(1);
    DWORD style = ARG(2);
    if (g_hide) style &= ~WS_VISIBLE;   /* never on the screen: the movie still plays */
    typedef HWND (__cdecl *mciwnd_t)(HWND, HINSTANCE, DWORD, LPCSTR);
    static mciwnd_t real;
    if (!real) real = (mciwnd_t)GetProcAddress(LoadLibraryA("msvfw32.dll"), "MCIWndCreateA");
    UNLOCKED(h = real(parent, GetModuleHandleA(NULL), style, file));
    (void)inst;
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4;   /* cdecl: the caller pops the four */
}

/* ---- the window ---------------------------------------------------------------
 * The game made its window for a fullscreen mode; windowed, it is told it is
 * always active (it pauses otherwise), and mouse messages are scaled from the
 * client area to the mode softdd presents. */
static WNDPROC g_game_proc;
static int g_scripted;   /* --keys (below) */

static void to_game(POINT* p) {
    DD* d = softdd_dd();
    RECT rc;
    if (!d || !d->w || !g_game_wnd || !GetClientRect(g_game_wnd, &rc) || !rc.right || !rc.bottom) return;
    p->x = p->x * d->w / rc.right;
    p->y = p->y * d->h / rc.bottom;
}

static LRESULT CALLBACK host_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ACTIVATEAPP: case WM_NCACTIVATE: w = TRUE; break;
    case WM_ACTIVATE: w = MAKEWPARAM(WA_ACTIVE, HIWORD(w)); break;
    case WM_KILLFOCUS: return 0;
    case WM_WINDOWPOSCHANGING: case WM_WINDOWPOSCHANGED: case WM_GETMINMAXINFO:
    case WM_NCCALCSIZE: case WM_STYLECHANGED: case WM_STYLECHANGING: case WM_SIZE:
        return DefWindowProcA(h, m, w, l);
    case WM_ERASEBKGND: return 1;
    }
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && !g_scripted) {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        to_game(&p);
        l = MAKELPARAM(p.x, p.y);
    }
    return CallWindowProcA(g_game_proc, h, m, w, l);
}

static void shim_CreateWindowExA(void) {
    uint32_t a[12];
    for (int i = 0; i < 12; i++) a[i] = ARG(i);
    if (g_hide) a[3] &= ~WS_VISIBLE;
    if (!a[8] && (a[3] & WS_POPUP)) { a[4] = a[5] = 64; a[6] = 800; a[7] = 600; }
    printf("[host] CreateWindowExA \"%s\" style %08X at %d,%d %dx%d parent %08X\n",
           a[2] && a[2] > 0xFFFF ? (const char*)(uintptr_t)a[2] : "", a[3], a[4], a[5], a[6], a[7], a[8]);
    HWND h;
    UNLOCKED(h = CreateWindowExA(a[0], (LPCSTR)(uintptr_t)a[1], (LPCSTR)(uintptr_t)a[2], a[3],
                                 a[4], a[5], a[6], a[7], (HWND)(uintptr_t)a[8],
                                 (HMENU)(uintptr_t)a[9], (HINSTANCE)(uintptr_t)a[10], (LPVOID)(uintptr_t)a[11]));
    DWORD err = h ? 0 : GetLastError();
    if (h && !a[8] && !g_game_proc) {
        /* The game's procedure is guest code: Windows' call to it faults on
         * the non-executable page and native32 runs the lifted body. */
        g_game_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)host_wndproc);
        g_game_wnd = h;
        PostMessageA(h, WM_ACTIVATEAPP, TRUE, 0);
        /* and active: the game keeps its own flag from WM_ACTIVATE
         * (sub_00429330), and while it is clear its main loop waits in
         * GetMessage; a window on a private desktop is never activated */
        PostMessageA(h, WM_ACTIVATE, WA_ACTIVE, 0);
    }
    SetLastError(err);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 12 * 4;
}

static void shim_ShowWindow(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    int cmd = (int)ARG(1);
    BOOL r;
    if (g_hide && cmd != SW_HIDE) cmd = SW_HIDE;
    UNLOCKED(r = ShowWindow(w, cmd));
    g_eax = r;
    g_esp += 4 + 2 * 4;
}

static void shim_GetForegroundWindow(void) {
    g_eax = (uint32_t)(uintptr_t)(g_game_wnd ? g_game_wnd : GetForegroundWindow());
    g_esp += 4;
}

static void shim_GetActiveWindow(void) {
    g_eax = (uint32_t)(uintptr_t)(g_game_wnd ? g_game_wnd : GetActiveWindow());
    g_esp += 4;
}

static void shim_DirectDrawCreate(void) {
    g_eax = (uint32_t)softdd_create((void**)(uintptr_t)ARG(1));
    g_esp += 4 + 3 * 4;
}

/* ---- scripted input (--keys) ---------------------------------------------------
 * "SECONDS:what,..." where what is a key name (esc, enter, space, up, down,
 * left, right, tab, f1..f12, a letter or digit), "hold:KEY:SECONDS",
 * "click:X:Y", "rclick:X:Y" or "move:X:Y" (game pixels), "shot=FILE", or
 * "on:NAME" (wait for a file open). While a script runs, the real mouse is
 * not read: only the script reaches the game. */
typedef struct { DWORD at; int vk, x, y, click, move, hold; char on[64], shot[MAX_PATH]; } step_t;

static step_t g_steps[128];
static int g_nsteps;
static volatile DWORD g_vk_until[256];
static volatile LONG g_cur_x = 400, g_cur_y = 300;

static const struct { const char* n; int vk; } g_keynames[] = {
    { "esc", VK_ESCAPE }, { "enter", VK_RETURN }, { "space", VK_SPACE }, { "tab", VK_TAB },
    { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT },
    { "bksp", VK_BACK }, { "f1", VK_F1 }, { "f2", VK_F2 }, { "f3", VK_F3 }, { "f4", VK_F4 },
    { "f5", VK_F5 }, { "f6", VK_F6 }, { "f7", VK_F7 }, { "f8", VK_F8 }, { "f9", VK_F9 }, { "f10", VK_F10 }, { "f11", VK_F11 }, { "f12", VK_F12 },
};

static int key_by_name(const char* n) {
    for (int i = 0; i < (int)(sizeof g_keynames / sizeof g_keynames[0]); i++)
        if (!_stricmp(n, g_keynames[i].n)) return g_keynames[i].vk;
    if (n[0] && !n[1] && isalnum((unsigned char)n[0])) return toupper((unsigned char)n[0]);
    return 0;
}

static void parse_keys(const char* spec) {
    char buf[4096];
    lstrcpynA(buf, spec, sizeof buf);
    for (char* tok = strtok(buf, ","); tok && g_nsteps < 128; tok = strtok(NULL, ",")) {
        step_t st = { 0 };
        char* c = strchr(tok, ':');
        if (!c) continue;
        if (!strncmp(tok, "on:", 3)) {
            lstrcpynA(st.on, tok + 3, sizeof st.on);
            g_steps[g_nsteps++] = st;
            continue;
        }
        st.at = (DWORD)(atof(tok) * 1000);
        char* what = c + 1;
        if (!strncmp(what, "shot=", 5)) {
            GetFullPathNameA(what + 5, MAX_PATH, st.shot, NULL);
        } else if (!strncmp(what, "click:", 6) || !strncmp(what, "rclick:", 7) || !strncmp(what, "move:", 5)) {
            st.click = what[0] == 'c' ? 1 : what[0] == 'r' ? 2 : 0;
            st.move = 1;
            sscanf(strchr(what, ':') + 1, "%d:%d", &st.x, &st.y);
        } else if (!strncmp(what, "hold:", 5)) {   /* hold:KEY:SECONDS */
            char k[16] = "";
            double secs = 1;
            sscanf(what + 5, "%15[^:]:%lf", k, &secs);
            st.hold = (int)(secs * 1000);
            if (!(st.vk = key_by_name(k))) { fprintf(stderr, "--keys: unknown key \"%s\"\n", k); exit(1); }
        } else if (!(st.vk = key_by_name(what))) {
            fprintf(stderr, "--keys: unknown key \"%s\"\n", what);
            exit(1);
        }
        g_steps[g_nsteps++] = st;
    }
    g_scripted = 1;
}

static DWORD WINAPI keys_thread(LPVOID unused) {
    (void)unused;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < g_nsteps; i++) {
        step_t* st = &g_steps[i];
        if (st->on[0]) {
            if (!st->x) { lstrcpynA(g_wait_file, st->on, sizeof g_wait_file); st->x = 1; }
            WaitForSingleObject(g_file_seen, INFINITE);
            t0 = GetTickCount();
            printf("[keys] %s opened: times count from here\n", st->on);
            continue;
        }
        for (int j = i + 1; j < g_nsteps; j++)   /* the next wait listens from now */
            if (g_steps[j].on[0]) {
                if (!g_steps[j].x) { lstrcpynA(g_wait_file, g_steps[j].on, sizeof g_wait_file); g_steps[j].x = 1; }
                break;
            }
        DWORD at = t0 + st->at, now = GetTickCount();
        if ((int)(at - now) > 0) Sleep(at - now);
        if (st->shot[0]) { softdd_shot(st->shot); continue; }
        HWND w = g_game_wnd;
        if (st->move) {
            g_cur_x = st->x, g_cur_y = st->y;
            if (w) PostMessageA(w, WM_MOUSEMOVE, 0, MAKELPARAM(st->x, st->y));
            if (st->click) {
                int r = st->click == 2;
                Sleep(100);
                g_vk_until[r ? VK_RBUTTON : VK_LBUTTON] = GetTickCount() + 150;
                if (w) PostMessageA(w, r ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, r ? MK_RBUTTON : MK_LBUTTON, MAKELPARAM(st->x, st->y));
                Sleep(150);
                if (w) PostMessageA(w, r ? WM_RBUTTONUP : WM_LBUTTONUP, 0, MAKELPARAM(st->x, st->y));
            }
            printf("[keys] %.1fs %s %d,%d\n", st->at / 1000.0, st->click == 2 ? "rclick" : st->click ? "click" : "move", st->x, st->y);
        } else {
            UINT sc = MapVirtualKeyA(st->vk, 0);
            {   /* keys go where the game's focus is: a text field is a real edit control */
                GUITHREADINFO gi = { sizeof gi };
                if (w && GetGUIThreadInfo(GetWindowThreadProcessId(w, NULL), &gi) && gi.hwndFocus) w = gi.hwndFocus;
            }
            int len = st->hold ? st->hold : 150;
            g_vk_until[st->vk & 0xFF] = GetTickCount() + len;
            /* the game's message loop translates it: its character comes once */
            if (w) PostMessageA(w, WM_KEYDOWN, st->vk, 1 | (sc << 16));
            Sleep(len);
            if (w) PostMessageA(w, WM_KEYUP, st->vk, 1 | (sc << 16) | 0xC0000000u);
            printf("[keys] %.1fs key %02X\n", st->at / 1000.0, st->vk);
        }
    }
    return 0;
}

static int held(int vk) { return (int)(g_vk_until[vk & 0xFF] - GetTickCount()) > 0; }

static void shim_GetAsyncKeyState(void) {
    int vk = (int)ARG(0);
    SHORT r = g_scripted ? 0 : GetAsyncKeyState(vk);
    if (held(vk)) r |= (SHORT)0x8000;
    g_eax = (uint16_t)r;
    g_esp += 4 + 1 * 4;
}

static void shim_GetKeyState(void) {
    int vk = (int)ARG(0);
    SHORT r = g_scripted ? 0 : GetKeyState(vk);
    if (held(vk)) r |= (SHORT)0x8000;
    g_eax = (uint16_t)r;
    g_esp += 4 + 1 * 4;
}

/* The battle screen scrolls on the arrow keys from here (sub_005F9760). */
static void shim_GetKeyboardState(void) {
    BYTE* k = (BYTE*)(uintptr_t)ARG(0);
    BOOL r = TRUE;
    if (g_scripted) memset(k, 0, 256);
    else r = GetKeyboardState(k);
    for (int vk = 0; vk < 256; vk++) if (held(vk)) k[vk] |= 0x80;
    g_eax = (uint32_t)r;
    g_esp += 4 + 1 * 4;
}

/* The cursor, in game pixels. Scripted, it is wherever the script put it. */
static void shim_GetCursorPos(void) {
    POINT* pt = (POINT*)(uintptr_t)ARG(0);
    if (g_scripted || !g_game_wnd) { pt->x = g_cur_x; pt->y = g_cur_y; }
    else { GetCursorPos(pt); ScreenToClient(g_game_wnd, pt); to_game(pt); }
    g_eax = TRUE;
    g_esp += 4 + 1 * 4;
}

static void shim_SetCursorPos(void) {
    g_cur_x = (LONG)ARG(0), g_cur_y = (LONG)ARG(1);
    g_eax = TRUE;
    g_esp += 4 + 2 * 4;
}

/* The game's client <-> screen conversions work in its mode's pixels: the
 * window is wherever softdd put it, so its client origin is the game's 0,0. */
static void shim_ScreenToClient(void) { g_eax = TRUE; g_esp += 4 + 2 * 4; }
static void shim_ClientToScreen(void) { g_eax = TRUE; g_esp += 4 + 2 * 4; }

/* LoadIconA/LoadCursorA with the game's own instance come from the
 * executable on disk, loaded as data: the mapped image is not a loaded module. */
static HMODULE game_resources(void) {
    static HMODULE h;
    if (!h) h = LoadLibraryExA(g_guest_exe, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    return h;
}
static void shim_LoadIconA(void) {
    HINSTANCE inst = (HINSTANCE)(uintptr_t)ARG(0);
    LPCSTR name = (LPCSTR)(uintptr_t)ARG(1);
    HICON h = NULL;
    if ((uint32_t)(uintptr_t)inst == IMAGE_BASE) { if (game_resources()) h = LoadIconA(game_resources(), name); if (!h) h = LoadIconA(NULL, IDI_APPLICATION); }
    else h = LoadIconA(inst, name);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 2 * 4;
}
static void shim_LoadCursorA(void) {
    HINSTANCE inst = (HINSTANCE)(uintptr_t)ARG(0);
    LPCSTR name = (LPCSTR)(uintptr_t)ARG(1);
    HCURSOR h = NULL;
    if ((uint32_t)(uintptr_t)inst == IMAGE_BASE) { if (game_resources()) h = LoadCursorA(game_resources(), name); if (!h) h = LoadCursorA(NULL, IDC_ARROW); }
    else h = LoadCursorA(inst, name);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 2 * 4;
}

/* ---- DirectPlay: pcrecomp's own (dplay32.c), not Windows' --------------------
 * Windows' DirectPlay is an optional feature that no longer finds sessions
 * between machines; the game gets a TCP one that does. */
static void shim_DirectPlayCreate(void) {       /* (guid, &dp, outer) */
    g_eax = (uint32_t)dplay32_create((void**)(uintptr_t)ARG(1));
    g_esp += 4 + 3 * 4;
}
static void shim_DirectPlayEnumerateA(void) {   /* (callback, context): the callback is the game's */
    uint32_t esp = g_esp;
    HRESULT hr = dplay32_enumerate((void*)(uintptr_t)ARG(0), (void*)(uintptr_t)ARG(1));
    g_esp = esp;
    g_eax = (uint32_t)hr;
    g_esp += 4 + 2 * 4;
}
static void shim_DirectPlayLobbyCreateA(void) { /* (sp, &lobby, outer, data, size) */
    g_eax = (uint32_t)dplay32_lobby_create((void**)(uintptr_t)ARG(1));
    g_esp += 4 + 5 * 4;
}
static void shim_CoCreateInstance(void) {       /* (clsid, outer, context, iid, &obj) */
    const GUID* clsid = (const GUID*)(uintptr_t)ARG(0);
    const GUID* iid = (const GUID*)(uintptr_t)ARG(3);
    void** out = (void**)(uintptr_t)ARG(4);
    HRESULT hr;
    if (dplay32_is_clsid(clsid))
        hr = clsid->Data1 == 0xd1eb6d20u ? dplay32_create_iid(iid, out) : dplay32_lobby_create(out);
    else {
        IUnknown* outer = (IUnknown*)(uintptr_t)ARG(1);
        DWORD ctx = ARG(2);
        UNLOCKED(hr = CoCreateInstance(clsid, outer, ctx, iid, out));
    }
    g_eax = (uint32_t)hr;
    g_esp += 4 + 5 * 4;
}

/* sub_00477DA0, the game's check for DirectPlay 6.1a/7 or Windows 2000's:
 * it asks the dplayx.dll loaded in the process for its file version. The
 * DirectPlay here is dplay32, and there may be no dplayx.dll at all. */
void swgb_dplay_ok(void) {
    g_eax = 1;
    g_esp += 4;
}

static native32_shim_t g_shims[] = {
    { "DPLAYX.dll#1", shim_DirectPlayCreate },
    { "DPLAYX.dll#2", shim_DirectPlayEnumerateA },
    { "DPLAYX.dll#4", shim_DirectPlayLobbyCreateA },
    { "CoCreateInstance", shim_CoCreateInstance },
    { "LoadIconA", shim_LoadIconA },
    { "LoadCursorA", shim_LoadCursorA },
    { "GetCursorPos", shim_GetCursorPos },
    { "SetCursorPos", shim_SetCursorPos },
    { "ScreenToClient", shim_ScreenToClient },
    { "ClientToScreen", shim_ClientToScreen },
    { "GetAsyncKeyState", shim_GetAsyncKeyState },
    { "GetKeyState", shim_GetKeyState },
    { "GetKeyboardState", shim_GetKeyboardState },
    { "DirectDrawCreate", shim_DirectDrawCreate },
    { "GetForegroundWindow", shim_GetForegroundWindow },
    { "GetActiveWindow", shim_GetActiveWindow },
    { "ShowWindow", shim_ShowWindow },
    { "CreateWindowExA", shim_CreateWindowExA },
    { "MCIWndCreateA", shim_MCIWndCreateA },
    { "CreateFileA", shim_CreateFileA },
    { "OpenFile", shim_OpenFile },
    { "MessageBoxA", shim_MessageBoxA },
    { "OutputDebugStringA", shim_OutputDebugStringA },
    { "ExitProcess", shim_ExitProcess },
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
};

/* ---- muted by default -------------------------------------------------------
 * Test runs are silent unless --sound. */
static const GUID kCLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kIID_IAudioSessionManager =
    { 0xBFA971F1, 0x4D5E, 0x40BB, { 0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4 } };

/* 1: muted; 2: there is no audio device; 0: failed. */
static int mute_process(void) {
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    IAudioSessionManager* mgr = NULL;
    ISimpleAudioVolume* vol = NULL;
    HRESULT hr = CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &kIID_IMMDeviceEnumerator, (void**)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) { IMMDeviceEnumerator_Release(en); return 2; }
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(dev, &kIID_IAudioSessionManager, CLSCTX_ALL, NULL, (void**)&mgr);
    if (SUCCEEDED(hr)) hr = IAudioSessionManager_GetSimpleAudioVolume(mgr, &GUID_NULL, FALSE, &vol);
    if (SUCCEEDED(hr)) hr = ISimpleAudioVolume_SetMute(vol, TRUE, NULL);
    if (vol) ISimpleAudioVolume_Release(vol);
    if (mgr) IAudioSessionManager_Release(mgr);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    return SUCCEEDED(hr);
}

/* ---- reports ---------------------------------------------------------------- */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "\n[not-lifted] sub_%08X  (called from 0x%08X)\n", va, g_cur_func);
    recomp_dump_trace("not-lifted");
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
}

/* Added after native32's own handler, so callbacks are resolved first and only
 * real faults get here. */
static char g_crash_buf[4096];
static int g_crash_len;
static void crash_emit(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(g_crash_buf + g_crash_len, sizeof g_crash_buf - 1 - g_crash_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_crash_len += n;
}

static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&once, 1)) TerminateProcess(GetCurrentProcess(), 3);
    crash_emit("\n=== fault 0x%08lX at 0x%p, thread %lu ===\n", r->ExceptionCode,
               r->ExceptionAddress, GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        crash_emit("  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                   native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    crash_emit("  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    crash_emit("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
               g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    crash_emit("last indirect calls (newest first):\n");
    for (int i = 1; i <= 12 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* nm = native32_name(g_icall_trace[k]);
        crash_emit("  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), g_crash_buf, (DWORD)g_crash_len, &w, NULL);
    recomp_trace_flush();
    softdd_finish();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* --profile: which lifted function was entered last, sampled every
 * millisecond; the top 30 when the run ends (the watchdog, or exit). */
static volatile LONG g_prof_on;
static struct { uint32_t va, n; } g_prof[1 << 14];
static DWORD WINAPI profiler(LPVOID unused) {
    (void)unused;
    timeBeginPeriod(1);
    while (g_prof_on) {
        uint32_t va = g_cur_func, h = (va * 2654435761u) >> 18;
        for (int k = 0; k < 64; k++, h = (h + 1) & ((1 << 14) - 1))
            if (g_prof[h].va == va || !g_prof[h].va) { g_prof[h].va = va; g_prof[h].n++; break; }
        Sleep(1);
    }
    return 0;
}
static void profile_dump(void) {
    uint32_t total = 0;
    if (!InterlockedExchange(&g_prof_on, 0)) return;
    for (int i = 0; i < (1 << 14); i++) total += g_prof[i].n;
    fprintf(stderr, "[profile] %u samples, the most entered:\n", total);
    for (int k = 0; k < 30 && total; k++) {
        int best = -1;
        for (int i = 0; i < (1 << 14); i++) if (g_prof[i].n && (best < 0 || g_prof[i].n > g_prof[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, "  sub_%08X %5.1f%%\n", g_prof[best].va, 100.0 * g_prof[best].n / total);
        g_prof[best].n = 0;
    }
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls, %ld frames\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count, softdd_frames);
    native32_dump_icalls(8);
    profile_dump();
    {   /* the game object (0x006A0FD4): its active flag, its mode (2 the menus,
         * 4 a game) and its comm handler's state (4: paused for a player) */
        uint32_t g = MEM32(0x006A0FD4u), c = g ? MEM32(g + 0x1B4) : 0;
        if (g) fprintf(stderr, "[state] active %u, mode %u, comm state %u\n",
                       MEM32(g + 0x30), MEM32(g + 0x1DC), c ? MEM32(c + 0x1E3C) : 0);
    }
    recomp_trace_flush();
    softdd_finish();
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

/* ---- settings: swgbrecomp.ini beside the program, then the command line -------
 *   [swgb]
 *   resolution=1920x1080   the game's size; "screen": the screen's; "game": its own (Options)
 *   lan_host=192.168.1.20  where else to look for LAN games (dplay32)
 * --classic is the original game: 800x600, nothing of the remaster's size. */
static char g_ini[MAX_PATH];

static int parse_res(const char* s, int* w, int* h) {
    int a = 0, b = 0;
    if (!s || !*s || !_stricmp(s, "game")) { *w = *h = 0; return 1; }
    if (!_stricmp(s, "screen")) a = GetSystemMetrics(SM_CXSCREEN), b = GetSystemMetrics(SM_CYSCREEN);   /* the primary screen's */
    else if (sscanf(s, "%dx%d", &a, &b) != 2) return 0;
    if (!swgb_res_valid(a, b)) return 0;
    *w = a, *h = b;
    return 1;
}

static void settings_load(const char* res_arg, int classic) {
    char v[64], *slash;
    GetModuleFileNameA(NULL, g_ini, MAX_PATH);
    if ((slash = strrchr(g_ini, '\\'))) strcpy(slash + 1, "swgbrecomp.ini");
    GetPrivateProfileStringA("swgb", "resolution", "game", v, sizeof v, g_ini);
    if (!parse_res(res_arg ? res_arg : v, &swgb_res_w, &swgb_res_h)) {
        fprintf(stderr, "resolution \"%s\": 800x600, 1024x720 to 4096x4096, screen or game\n", res_arg ? res_arg : v);
        swgb_res_w = swgb_res_h = 0;
    }
    if (GetPrivateProfileStringA("swgb", "lan_host", "", v, sizeof v, g_ini) && !GetEnvironmentVariableA("PCRECOMP_DPLAY_HOST", NULL, 0))
        SetEnvironmentVariableA("PCRECOMP_DPLAY_HOST", v);
    if (classic) swgb_res_w = 800, swgb_res_h = 600;
    if (swgb_res_w) printf("  resolution: %dx%d%s\n", swgb_res_w, swgb_res_h, classic ? " (--classic)" : "");
}

int main(int argc, char** argv) {
    const char* exe = "work\\Battlegrounds.exe";
    const char* game = "game";
    const char* cd = NULL;
    char exe_full[MAX_PATH], game_full[MAX_PATH], guest_args[512] = "";
    int run = 0, sound = 0, classic = 0;
    const char* res_arg = NULL;
    setvbuf(stdout, NULL, _IONBF, 0);   /* a fault ends the run with TerminateProcess */
    /* --headless runs again on a private desktop (native32_headless): there the
     * window can be visible, so it gets WM_PAINT, which the game's dialogs
     * draw from; muted unless PCRECOMP_MUTE says otherwise. */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--headless")) {
            int rc;
            if (!GetEnvironmentVariableA("PCRECOMP_MUTE", NULL, 0)) SetEnvironmentVariableA("PCRECOMP_MUTE", "1");
            /* the game ran in the child: its exit code, a crash's negative */
            if ((rc = native32_headless()) != -1) {
                if (rc < 0) fprintf(stderr, "[headless] the game ended with 0x%08X\n", (unsigned)rc);
                return rc;
            }
            g_hide = !GetEnvironmentVariableA("NATIVE32_HEADLESS", NULL, 0);
        }
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--sound")) sound = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = softdd_headless = 1;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            static char rec[MAX_PATH];   /* the game runs in its own folder */
            GetFullPathNameA(argv[++i], MAX_PATH, rec, NULL);
            softdd_record = rec;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) softdd_stop_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) softdd_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--files")) g_log_files = 1;
        else if (!strcmp(argv[i], "--profile")) g_prof_on = 1;
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) res_arg = argv[++i];
        else if (!strcmp(argv[i], "--classic")) classic = 1;
        else if (!strcmp(argv[i], "--lan-host") && i + 1 < argc) SetEnvironmentVariableA("PCRECOMP_DPLAY_HOST", argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc && g_shots < 16) {
            g_shot_s[g_shots] = strtoul(argv[++i], NULL, 0);
            GetFullPathNameA(argv[++i], MAX_PATH, g_shot_file[g_shots++], NULL);
        }
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--cd") && i + 1 < argc) cd = argv[++i];
        else if (!strcmp(argv[i], "--args") && i + 1 < argc) _snprintf(guest_args, sizeof guest_args - 1, " %s", argv[++i]);
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) parse_keys(argv[++i]);
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else {
            printf("usage: swgbrecomp [--run] [--res WxH|screen|game] [--classic] [--lan-host ADDR]\n"
                   "                  [--headless] [--record out.mp4] [--frames N] [--sound]\n"
                   "                  [--shot S file.bmp] [--keys SCRIPT] [--watchdog S] [--files] [--profile]\n"
                   "                  [--exe work\\Battlegrounds.exe] [--game game] [--cd E]\n"
                   "                  [--args \"<game switches>\"] [--callbacks] [--native-trace]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    settings_load(res_arg, classic);
    GetFullPathNameA(exe, MAX_PATH, exe_full, NULL);
    GetFullPathNameA(game, MAX_PATH, game_full, NULL);
    cd_pick(cd);
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%s\\Battlegrounds.exe", game_full);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"%s", g_guest_exe, guest_args);

    native32_init();
    /* __try/__except frames: RaiseException goes to the guest's own chain
     * (pcrecomp seh32.c); the lift gives each __try function its frame. */
    native32_guest_seh = 1;
    AddVectoredExceptionHandler(0, crash);
    printf("Galactic Battlegrounds recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    uint32_t span = native32_map(exe_full, IMAGE_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, IMAGE_BASE); return 1; }
    printf("  mapped %s: 0x%08X-0x%08X\n", exe, IMAGE_BASE, IMAGE_BASE + span);
    SetDllDirectoryA(game_full);   /* language.dll */
    if (native32_bind(IMAGE_BASE, g_shims, (int)(sizeof g_shims / sizeof g_shims[0]))) return 1;
    printf("  guest command line %s\n", g_guest_cmdline);
    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", swgb_entry_va);
        return 0;
    }
    if (!SetCurrentDirectoryA(game_full)) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    CoInitialize(NULL);
    if (!g_nsteps && !g_watchdog_s && !g_headless) sound = 1;   /* playing: sound on */
    int m = sound ? 1 : mute_process();
    if (!m) {
        fprintf(stderr, "cannot mute the process audio session; pass --sound to run with sound\n");
        return 1;
    }
    printf("  audio: %s\n", sound ? "on" : m == 2 ? "muted (no audio device)" : "muted (a test run; --sound to hear it)");
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    if (g_prof_on) CloseHandle(CreateThread(NULL, 0, profiler, NULL, 0, NULL));
    if (g_shots) CloseHandle(CreateThread(NULL, 0, shot_thread, NULL, 0, NULL));
    g_file_seen = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (g_nsteps) CloseHandle(CreateThread(NULL, 0, keys_thread, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", swgb_entry_va);
    native32_call_guest(swgb_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    softdd_finish();
    return (int)g_eax;
}
