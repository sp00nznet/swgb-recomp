/*
 * Galactic Battlegrounds' own answers on win32hle (Linux, macOS), registered
 * ahead of win32hle's so they win.
 *
 * Two kinds. First, what the Windows host shims for this game too, done the
 * win32hle way: the play disc as a CD-ROM drive, file opens seen by --files
 * and --keys' on:NAME, message boxes answered on a test run. Second, the 21
 * imports win32hle does not answer yet (docs/linux-macos.md, "The import
 * census"), each the smallest answer that lets the game go on. DirectPlay and
 * the movies (MCIWnd) are refused here: multiplayer is out of scope, and the
 * movie player is Track 6's (ROADMAP.md).
 *
 * Every shim is stdcall unless it says otherwise: g_esp -> [ret][arg0]...,
 * and RET pops the return address and its arguments (win32hle.h).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "win32hle.h"
#include "swgb_linux.h"

/* ---- calling win32hle's own answer ------------------------------------------
 * A wrapper runs win32hle's shim with the stack as it found it: the shim
 * reads the same arguments and pops them. */
static recomp_func_t hle_shim(const win32hle_shim* table, const char* name) {
    for (; table->name; table++)
        if (!strcmp(table->name, name)) return table->fn;
    hle_fatal("win32hle has no %s to wrap", name);
    return NULL;
}

/* ---- the play disc --------------------------------------------------------------
 * The game checks the drive letter in CDPath (main.c seeds it) for a CD-ROM
 * with Game\Avi on it. win32hle calls every mapped drive a fixed disk, so the
 * disc's letter is answered here. */
static void s_GetDriveTypeA(void) {
    const char* r = ASTR(0);
    char root[4] = "?:\\", host[1024];
    if (!r || !*r) RET(3u, 1);                            /* DRIVE_FIXED: the current drive */
    char c = (char)(r[0] & ~0x20);
    if (swgb_cd_letter && c == swgb_cd_letter) RET(5u, 1);   /* DRIVE_CDROM */
    root[0] = c;
    RET(hle_host_path(root, host, sizeof host) && host[0] ? 3u : 1u, 1);   /* DRIVE_NO_ROOT_DIR */
}

static void s_GetLogicalDrives(void) {
    uint32_t m = 1u << 2 | 1u << 25;                      /* C: (the game) and Z: (/) */
    if (swgb_cd_letter) m |= 1u << (swgb_cd_letter - 'A');
    RET(m, 0);
}

/* ---- files: --files and on:NAME --------------------------------------------- */
static recomp_func_t g_CreateFileA, g_OpenFile;

static void s_CreateFileA(void) {                         /* 7 arguments */
    char name[512];
    snprintf(name, sizeof name, "%s", ASTR(0) ? ASTR(0) : "(null)");
    g_CreateFileA();
    swgb_file_seen(name, g_eax != 0xFFFFFFFFu);
}

static void s_OpenFile(void) {                            /* 3 arguments */
    char name[512];
    snprintf(name, sizeof name, "%s", ASTR(0) ? ASTR(0) : "(null)");
    g_OpenFile();
    swgb_file_seen(name, g_eax != 0xFFFFFFFFu);
}

/* ---- message boxes ---------------------------------------------------------------
 * Printed; a test run answers No or Cancel, as the Windows host does. */
static recomp_func_t g_MessageBoxA;
static void s_MessageBoxA(void) {                         /* (hwnd, text, caption, type) */
    printf("[host] MessageBoxA \"%s\": %s\n", ASTR(2) ? ASTR(2) : "", ASTR(1) ? ASTR(1) : "");
    if (!swgb_test_run) { g_MessageBoxA(); return; }
    uint32_t b = A32(3) & 0xFu;
    RET(b == 4 || b == 3 ? 7u                             /* MB_YESNO(CANCEL) -> IDNO */
        : b == 5 || b == 1 ? 2u                           /* MB_RETRYCANCEL, MB_OKCANCEL -> IDCANCEL */
        : b == 2 ? 3u : 1u, 4);                           /* MB_ABORTRETRYIGNORE -> IDABORT; IDOK */
}

/* ---- the imports win32hle has no answer for ------------------------------------ */

/* MSVFW32: no video codecs, and no movie window yet. MCIWndCreateA is cdecl:
 * the caller pops its four arguments. NULL is what the game sees on a PC
 * whose movies are missing, and it goes on without them. */
static void s_ICInfo(void) { RET(0, 3); }
static void s_MCIWndCreateA(void) {
    printf("[host] MCIWndCreateA %s: no movie player on this host yet\n", ASTR(3) ? ASTR(3) : "(null)");
    g_eax = 0;
    g_esp += 4;
}

/* WINMM: the CD music. MCI's cdaudio device is not here (the Ogg Vorbis
 * tracks of the Steam release are the plan: docs/linux-macos.md, "Music"). */
static void s_mciSendCommandA(void) {                     /* (device, msg, flags, params) */
    static int once;
    if (!once++) printf("[host] mciSendCommandA(0x%X): no CD audio on this host yet\n", A32(1));
    RET(263u, 4);                                         /* MCIERR_INVALID_DEVICE_NAME */
}

/* KERNEL32 */
static void s_IsDBCSLeadByte(void) { RET(0, 1); }         /* code page 1252: no lead bytes */
static void s_GetPriorityClass(void) { RET(0x20u, 1); }   /* NORMAL_PRIORITY_CLASS */
static void s_WinExec(void) {
    printf("[host] WinExec(%s): not on this host\n", ASTR(0) ? ASTR(0) : "");
    RET(2u, 2);                                           /* ERROR_FILE_NOT_FOUND */
}

/* USER32 */
static recomp_func_t g_DrawTextA;
static void s_GetLastActivePopup(void) { RET(A32(0), 1); }
static void s_GetWindowThreadProcessId(void) {            /* (hwnd, &pid) */
    if (A32(1)) MEM32(A32(1)) = 1;
    RET(hle_thread_id(), 2);
}
static void s_IsClipboardFormatAvailable(void) { RET(0, 1); }
static void s_SystemParametersInfoA(void) {               /* (action, ui, pv, winini) */
    uint32_t pv = A32(2);
    int w = 800, h = 600, bpp;
    switch (A32(0)) {
    case 0x30u:                                           /* SPI_GETWORKAREA: the display mode */
        hle_dd_mode(&w, &h, &bpp);
        if (pv) MEM32(pv) = 0, MEM32(pv + 4) = 0, MEM32(pv + 8) = (uint32_t)w, MEM32(pv + 12) = (uint32_t)h;
        break;
    default:                                              /* a GET answers off/zero, a SET is taken */
        if (pv && (A32(0) & 1u) == 0) MEM32(pv) = 0;
        break;
    }
    RET(1, 4);
}
static void s_SetClassLongA(void) { RET(0, 3); }
static void s_GetCaretBlinkTime(void) { RET(530u, 0); }   /* Windows' default */
static void s_DrawTextExA(void) {                         /* DrawTextA's five, and DRAWTEXTPARAMS */
    g_DrawTextA();                                        /* pops ret + 5 arguments */
    g_esp += 4;                                           /* the sixth */
}
static void s_SetSysColors(void) { RET(1, 3); }
static void s_IsIconic(void) { RET(0, 1); }

/* GDI32: an information context is a DC that answers GetDeviceCaps. */
static void s_CreateICA(void) {
    static uint8_t* px;
    if (!px) px = (uint8_t*)hle_alloc(4);
    RET(hle_gdi_surface_dc(px, 1, 1, 4, 32), 4);
}

/* DPLAYX by ordinal: DirectPlay is out of scope (ROADMAP.md, "Later"). The
 * multiplayer screens find no service providers. */
#define DPERR_UNAVAILABLE 0x887703E8u   /* MAKE_DPHRESULT(1000) */
static void s_DirectPlayCreate(void) {                    /* #1 (guid, &dp, unk) */
    if (A32(1)) MEM32(A32(1)) = 0;
    RET(DPERR_UNAVAILABLE, 3);
}
static void s_DirectPlayEnumerateA(void) { RET(0, 2); }   /* #2 (callback, context): none */
static void s_DirectPlayLobbyCreateA(void) {              /* #4 (guid, &lobby, unk, data, size) */
    if (A32(1)) MEM32(A32(1)) = 0;
    RET(DPERR_UNAVAILABLE, 5);
}

/* IMM32: no input method. */
static void s_ImmGetOpenStatus(void) { RET(0, 1); }
static void s_ImmGetDefaultIMEWnd(void) { RET(0, 1); }

static const win32hle_shim g_shims[] = {
    { "GetDriveTypeA", s_GetDriveTypeA },
    { "GetLogicalDrives", s_GetLogicalDrives },
    { "CreateFileA", s_CreateFileA },
    { "OpenFile", s_OpenFile },
    { "MessageBoxA", s_MessageBoxA },
    { "ICInfo", s_ICInfo },
    { "MCIWndCreateA", s_MCIWndCreateA },
    { "mciSendCommandA", s_mciSendCommandA },
    { "IsDBCSLeadByte", s_IsDBCSLeadByte },
    { "GetPriorityClass", s_GetPriorityClass },
    { "WinExec", s_WinExec },
    { "GetLastActivePopup", s_GetLastActivePopup },
    { "GetWindowThreadProcessId", s_GetWindowThreadProcessId },
    { "IsClipboardFormatAvailable", s_IsClipboardFormatAvailable },
    { "SystemParametersInfoA", s_SystemParametersInfoA },
    { "SetClassLongA", s_SetClassLongA },
    { "GetCaretBlinkTime", s_GetCaretBlinkTime },
    { "DrawTextExA", s_DrawTextExA },
    { "SetSysColors", s_SetSysColors },
    { "IsIconic", s_IsIconic },
    { "CreateICA", s_CreateICA },
    { "dplayx.dll#1", s_DirectPlayCreate },
    { "dplayx.dll#2", s_DirectPlayEnumerateA },
    { "dplayx.dll#4", s_DirectPlayLobbyCreateA },
    { "ImmGetOpenStatus", s_ImmGetOpenStatus },
    { "ImmGetDefaultIMEWnd", s_ImmGetDefaultIMEWnd },
    { 0, 0 }
};

void swgb_register_shims(void) {
    g_CreateFileA = hle_shim(win32hle_kernel32_ext, "CreateFileA");
    g_OpenFile = hle_shim(win32hle_kernel32_ext, "OpenFile");
    g_MessageBoxA = hle_shim(win32hle_user32, "MessageBoxA");
    g_DrawTextA = hle_shim(win32hle_user32, "DrawTextA");
    win32hle_register(g_shims);
}
