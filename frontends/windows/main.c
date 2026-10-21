/*
 * FujiNet Go MS-DOS -- the Windows (Win32 + GDI) frontend.
 *
 * No toolkit: a plain window with a menu bar, a strip of quick buttons
 * (Ctrl+Alt+Del and Reset) under it, the PC's picture, and a status bar
 * with the FujiNet link light, the status line and the drive lights. With
 * no BIOS imported yet the picture's place shows how to import one.
 *
 * Three Windows-specific things are load bearing:
 *
 *   DwmFlush() on a present thread is this platform's frame clock. There is
 *   no GdkFrameClock here, and a plain timer would beat against the panel.
 *
 *   Keys go to the PC by physical key: the scan code and extended bit in
 *   WM_KEYDOWN/WM_SYSKEYDOWN's lParam, through the session's table. F10 and
 *   Alt, which Windows would hand to the menu bar, are the PC's; the app's
 *   own keys are the Ctrl+Shift ones, Ctrl+End (let the mouse go) and
 *   Ctrl+Alt+PgDn (fullscreen), as in 86Box. Alt+F4 and Alt+Space stay the
 *   desktop's, as they are on the other desktops.
 *
 *   WM_ACTIVATE releases every held key and the mouse. Alt-tabbing away and
 *   coming back to a key still down is the classic symptom of not doing
 *   this, and Windows is where it happens most, because the WM eats the
 *   key-up.
 *
 * The mouse, when one is fitted, is captured by a click on the picture: the
 * pointer is hidden and clipped to it, and raw input carries the movement
 * as mickeys. Ctrl+End or the middle button lets it go.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "win_common.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <stdint.h>
#include <string.h>

#include "app.h"
#include "controllers/controller_window.h"
#include "debugger/dbg_window.h"
#include "fujilog.h"
#include "help/help_window.h"
#include "prefs.h"
#include "resource.h"

#ifndef MSDOS_VERSION_STRING
#define MSDOS_VERSION_STRING "dev"   /* CMake sets it from project() */
#endif

#define WIN_CLASS "FujiNetGoMSDOS"
#define APP_TITLE "FujiNet Go MS-DOS"

/* How long a notice (the other frontends' toasts) holds the status line. */
#define NOTICE_MS 4000

/* Sizes at 96 dpi; S() scales them to the window's. */
#define STRIP_H 36              /* the quick buttons' strip */
#define BUTTON_H 26
#define DOT_PART_W 22           /* the status bar: the link light */
#define LIGHT_W 30              /* ... and each drive's light */

#define FB_W MSDOSSESSION_FB_MAX_WIDTH
#define FB_H MSDOSSESSION_FB_MAX_HEIGHT

static msdossession *g_session;
static HINSTANCE g_inst;
static HWND g_hwnd, g_statusbar, g_tip;
static HWND g_cad_btn, g_reset_btn, g_bios_files_btn, g_bios_folder_btn, g_bios_help_btn;
static HMENU g_menubar, g_machine_menu, g_drives_menu, g_edit_menu, g_view_menu, g_fuji_menu;
static int g_dpi = 96;
static HFONT g_ui_font, g_title_font;
static HBRUSH g_accent_brush;
static HICON g_big_icon;

/* the picture: the present thread fills it, WM_PAINT draws it */
static uint32_t *g_fb;
static int g_fw, g_fh, g_par = 1000;
static uint64_t g_serial;
static volatile LONG g_frames;
static BITMAPINFO g_bmi;
static CRITICAL_SECTION g_fb_lock;
static volatile LONG g_running = 1;
static HANDLE g_present_thread;
static RECT g_pic;              /* the picture's area (under the lock) */
static RECT g_shown;            /* where the picture is in it */
static int g_smooth = 1, g_integer, g_fullscreen;
static WINDOWPLACEMENT g_placement;

/* the mouse */
static int g_captured;
static unsigned g_buttons;
static int g_wheel;             /* partial wheel deltas */
static int g_abs_have;
static LONG g_abs_x, g_abs_y;

/* status */
static unsigned g_pad_generation, g_fuji_generation;
static char g_notice[512];
static DWORD g_notice_until;
static char g_status_shown[512];
static int g_dot_shown = -1;
static unsigned g_lights_shown = ~0u;
static int g_page_bios = -1;    /* what the picture's place shows */

/* MSDOS_SCREENSHOT / MSDOS_AUTOQUIT_FRAMES */
static char g_shot_path[MAX_PATH];
static LONG g_shot_frames = 300, g_quit_frames;

static void update_status(void);
static void layout(void);
static void set_captured(int on);
static void load_media(const char *path);

static int S(int v)
{
    return msdos_scale(v, g_dpi);
}

/* ---- the present thread: this platform's frame clock ------------------------- */

static DWORD WINAPI present_thread(LPVOID arg)
{
    LARGE_INTEGER freq, last, now;
    (void)arg;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        int w = 0, h = 0, par = 1000;
        RECT pic;
        /* Blocks until the compositor's next vblank; falls through at once
         * if the DWM is off, and a short sleep paces it instead. A DwmFlush
         * that returns at once (Wine's) is paced the same way, so the loop
         * never spins. */
        if (FAILED(DwmFlush())) Sleep(8);
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - last.QuadPart) * 1000 < freq.QuadPart * 4) {
            Sleep(8);
            QueryPerformanceCounter(&now);
        }
        last = now;
        EnterCriticalSection(&g_fb_lock);
        if (msdossession_copy_frame(g_session, g_fb, &w, &h, &par, &g_serial) && w > 0 && h > 0) {
            g_fw = w;
            g_fh = h;
            g_par = par > 0 ? par : 1000;
            pic = g_pic;
            LeaveCriticalSection(&g_fb_lock);
            InterlockedIncrement(&g_frames);
            InvalidateRect(g_hwnd, &pic, FALSE);
        } else {
            LeaveCriticalSection(&g_fb_lock);
        }
    }
    return 0;
}

/* ---- painting -------------------------------------------------------------------- */

/* The no-BIOS page: the icon, a title, what to do, and the three buttons. */
typedef struct {
    RECT icon, title, desc, files, folder, help;
} nobios_geom;

static const char k_nobios_title[] = "No PC BIOS";
static const char k_nobios_text[] =
    "IBM's and the clone makers' BIOS ROMs are not part of the app. Import the ones "
    "you have -- single images, a zip, or a folder such as an 86Box ROM set -- and "
    "the PC starts.";

static int text_height(HFONT font, const char *utf8, int width)
{
    wchar_t w[1024];
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, font);
    RECT r;
    SetRect(&r, 0, 0, width, 0);
    msdos_wide(utf8, w, 1024);
    DrawTextW(dc, w, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    return r.bottom - r.top;
}

static int text_width(HFONT font, const char *text)
{
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    return sz.cx;
}

static int button_width(const char *text)
{
    const int w = text_width(g_ui_font, text) + S(28);
    return w < S(80) ? S(80) : w;
}

static void nobios_layout(const RECT *area, nobios_geom *g)
{
    const int aw = area->right - area->left, ah = area->bottom - area->top;
    const int width = aw - S(48) < S(560) ? aw - S(48) : S(560);
    const int icon = S(64), title_h = text_height(g_title_font, k_nobios_title, width);
    const int desc_h = text_height(g_ui_font, k_nobios_text, width);
    const int bw1 = button_width("Import BIOS..."), bw2 = button_width("Import a Folder...");
    const int bw3 = button_width("Help"), row_w = bw1 + S(12) + bw2 + S(12) + bw3;
    const int bh = S(32);
    const int total = icon + S(16) + title_h + S(10) + desc_h + S(24) + bh;
    const int x0 = area->left + (aw - width) / 2, cx = area->left + aw / 2;
    int y = area->top + (ah - total) / 2;
    if (y < area->top + S(8)) y = area->top + S(8);
    SetRect(&g->icon, cx - icon / 2, y, cx + icon / 2, y + icon);
    y += icon + S(16);
    SetRect(&g->title, x0, y, x0 + width, y + title_h);
    y += title_h + S(10);
    SetRect(&g->desc, x0, y, x0 + width, y + desc_h);
    y += desc_h + S(24);
    SetRect(&g->files, cx - row_w / 2, y, cx - row_w / 2 + bw1, y + bh);
    SetRect(&g->folder, g->files.right + S(12), y, g->files.right + S(12) + bw2, y + bh);
    SetRect(&g->help, g->folder.right + S(12), y, g->folder.right + S(12) + bw3, y + bh);
}

static void paint_nobios(HDC dc, const RECT *area)
{
    nobios_geom g;
    wchar_t w[1024];
    nobios_layout(area, &g);
    FillRect(dc, area, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    if (g_big_icon)
        DrawIconEx(dc, g.icon.left, g.icon.top, g_big_icon, g.icon.right - g.icon.left,
                   g.icon.bottom - g.icon.top, 0, NULL, DI_NORMAL);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    SelectObject(dc, g_title_font);
    msdos_wide(k_nobios_title, w, 1024);
    DrawTextW(dc, w, -1, &g.title, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, g_ui_font);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    msdos_wide(k_nobios_text, w, 1024);
    DrawTextW(dc, w, -1, &g.desc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
}

/* Black around the picture, the picture over the rest: no fill under it,
 * so no flicker. */
static void fill_around(HDC dc, const RECT *area, const RECT *pic)
{
    RECT r;
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    SetRect(&r, area->left, area->top, area->right, pic->top);
    FillRect(dc, &r, black);
    SetRect(&r, area->left, pic->bottom, area->right, area->bottom);
    FillRect(dc, &r, black);
    SetRect(&r, area->left, pic->top, pic->left, pic->bottom);
    FillRect(dc, &r, black);
    SetRect(&r, pic->right, pic->top, area->right, pic->bottom);
    FillRect(dc, &r, black);
}

static void paint(HDC dc)
{
    RECT client, area;
    double w, h, pw, ph, sw, sh;

    GetClientRect(g_hwnd, &client);
    EnterCriticalSection(&g_fb_lock);
    area = g_pic;
    LeaveCriticalSection(&g_fb_lock);
    if (area.top > 0) {
        RECT strip;
        SetRect(&strip, 0, 0, client.right, area.top);
        FillRect(dc, &strip, GetSysColorBrush(COLOR_BTNFACE));
    }
    w = area.right - area.left;
    h = area.bottom - area.top;
    if (w <= 0 || h <= 0) return;
    if (!msdossession_has_bios(g_session)) {
        paint_nobios(dc, &area);
        return;
    }

    EnterCriticalSection(&g_fb_lock);
    if (g_fw <= 0 || g_fh <= 0) {
        LeaveCriticalSection(&g_fb_lock);
        FillRect(dc, &area, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return;
    }
    /* the frame's pixels at the aspect the session asks for (4:3 on, or
     * square), fitted, or at a whole multiple when that fits */
    pw = g_fw * g_par / 1000.0;
    ph = g_fh;
    if (g_integer && pw <= w && ph <= h) {
        const int k = (int)(w / pw < h / ph ? w / pw : h / ph);
        sw = pw * k;
        sh = ph * k;
    } else if (w / h > pw / ph) {
        sh = h;
        sw = sh * pw / ph;
    } else {
        sw = w;
        sh = sw * ph / pw;
    }
    SetRect(&g_shown, area.left + (int)((w - sw) / 2), area.top + (int)((h - sh) / 2),
            area.left + (int)((w - sw) / 2 + sw), area.top + (int)((h - sh) / 2 + sh));
    fill_around(dc, &area, &g_shown);

    /* The session's pixels are 0x00RRGGBB, packed at the frame's width:
     * exactly a 32-bit BI_RGB DIB. */
    g_bmi.bmiHeader.biWidth = g_fw;
    g_bmi.bmiHeader.biHeight = -g_fh;   /* top-down */
    SetStretchBltMode(dc, g_smooth ? HALFTONE : COLORONCOLOR);
    if (g_smooth) SetBrushOrgEx(dc, 0, 0, NULL);
    StretchDIBits(dc, g_shown.left, g_shown.top, g_shown.right - g_shown.left,
                  g_shown.bottom - g_shown.top, 0, 0, g_fw, g_fh, g_fb, &g_bmi,
                  DIB_RGB_COLORS, SRCCOPY);
    LeaveCriticalSection(&g_fb_lock);
}

/* ---- MSDOS_SCREENSHOT: the last frame as a PNG --------------------------------------
 * Stored (uncompressed) deflate blocks: a test's screenshot needs no zlib. */

static uint32_t crc_update(uint32_t crc, const unsigned char *p, size_t n)
{
    static uint32_t table[256];
    static int made;
    size_t i;
    if (!made) {
        uint32_t c, k;
        for (i = 0; i < 256; i++) {
            for (c = (uint32_t)i, k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = 1;
    }
    for (i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

static void png_chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char hdr[8];
    uint32_t crc = crc_update(0xFFFFFFFFu, (const unsigned char *)type, 4);
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) {
        fwrite(data, 1, len, f);
        crc = crc_update(crc, data, len);
    }
    put32(hdr, crc ^ 0xFFFFFFFFu);
    fwrite(hdr, 1, 4, f);
}

/* A zlib stream of stored blocks around raw; its length in *zlen. */
static unsigned char *zlib_store(const unsigned char *raw, size_t n, size_t *zlen)
{
    const size_t blocks = n / 65535 + 1;
    unsigned char *z = malloc(2 + blocks * 5 + n + 4), *o;
    uint32_t a = 1, b = 0;
    size_t i, done = 0;
    if (!z) return NULL;
    o = z;
    *o++ = 0x78;
    *o++ = 0x01;
    do {
        const size_t len = n - done > 65535 ? 65535 : n - done;
        *o++ = (unsigned char)(done + len == n);     /* BFINAL, stored */
        *o++ = (unsigned char)len;
        *o++ = (unsigned char)(len >> 8);
        *o++ = (unsigned char)~len;
        *o++ = (unsigned char)(~len >> 8);
        memcpy(o, raw + done, len);
        o += len;
        done += len;
    } while (done < n);
    for (i = 0; i < n; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    put32(o, (b << 16) | a);
    o += 4;
    *zlen = (size_t)(o - z);
    return z;
}

static int save_png(const char *path)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    unsigned char ihdr[13];
    unsigned char *raw = NULL, *z;
    size_t zlen = 0;
    int w, h, x, y, ok = 0;
    FILE *f;

    EnterCriticalSection(&g_fb_lock);
    w = g_fw;
    h = g_fh;
    if (w > 0 && h > 0) raw = malloc((size_t)(w * 3 + 1) * (size_t)h);
    if (raw) {
        for (y = 0; y < h; y++) {
            unsigned char *row = raw + (size_t)y * (size_t)(w * 3 + 1);
            *row++ = 0;   /* no filter */
            for (x = 0; x < w; x++) {
                const uint32_t p = g_fb[(size_t)y * (size_t)w + (size_t)x];
                *row++ = (unsigned char)(p >> 16);
                *row++ = (unsigned char)(p >> 8);
                *row++ = (unsigned char)p;
            }
        }
    }
    LeaveCriticalSection(&g_fb_lock);
    if (!raw) return 0;
    z = zlib_store(raw, (size_t)(w * 3 + 1) * (size_t)h, &zlen);
    if (z && (f = fopen(path, "wb"))) {
        put32(ihdr, (uint32_t)w);
        put32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;   /* 8-bit RGB */
        fwrite(sig, 1, 8, f);
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (uint32_t)zlen);
        png_chunk(f, "IEND", NULL, 0);
        ok = fclose(f) == 0;
    }
    free(z);
    free(raw);
    return ok;
}

/* ---- small helpers ---------------------------------------------------------------- */

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '\\');
    const char *t = strrchr(p, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : p;
}

static void show_message(const char *title, const char *text)
{
    msdos_message(g_hwnd, text && *text ? text : title, title, MB_ICONWARNING | MB_OK);
}

/* The other frontends' toasts: the message holds the status line a while. */
static void notice(const char *text)
{
    snprintf(g_notice, sizeof g_notice, "%.*s", (int)sizeof g_notice - 1, text);
    g_notice_until = GetTickCount() + (strlen(text) > 80 ? 2 * NOTICE_MS : NOTICE_MS);
    update_status();
}

static int dir_exists(const char *path)
{
    const DWORD a = path && *path ? GetFileAttributesA(path) : INVALID_FILE_ATTRIBUTES;
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Where to look for media: the folder the last file came from. */
static const char *media_folder(void)
{
    const char *last = msdossession_get_str(g_session, "last_dir", "");
    return dir_exists(last) ? last : msdossession_media_path(g_session);
}

static void remember_folder(const char *path)
{
    char dir[MAX_PATH];
    char *s, *t;
    snprintf(dir, sizeof dir, "%s", path);
    s = strrchr(dir, '\\');
    t = strrchr(dir, '/');
    if (t && (!s || t > s)) s = t;
    if (s) {
        *s = '\0';
        msdossession_set_str(g_session, "last_dir", dir);
    }
}

static void open_folder(const char *path)
{
    char buf[MAX_PATH];
    char *p;
    /* the session's paths may use either separator */
    snprintf(buf, sizeof buf, "%s", path);
    for (p = buf; *p; p++)
        if (*p == '/') *p = '\\';
    SHCreateDirectoryExA(NULL, buf, NULL);
    ShellExecuteA(g_hwnd, "open", buf, NULL, NULL, SW_SHOWNORMAL);
}

static const char kAnyFilter[] =
    "Disk images and programs\0*.img;*.ima;*.vfd;*.flp;*.dsk;*.86f;*.imd;*.td0;*.fdi;*.vhd;*.hdi;"
    "*.hdx;*.zip;*.rom;*.bin;*.exe;*.com\0All files\0*.*\0\0";
static const char kFloppyFilter[] =
    "Floppy images\0*.img;*.ima;*.vfd;*.flp;*.dsk;*.86f;*.imd;*.td0;*.fdi\0All files\0*.*\0\0";
static const char kHdFilter[] = "Hard disk images\0*.vhd;*.img;*.hdi;*.hdx\0All files\0*.*\0\0";
static const char kRomFilter[] =
    "BIOS images and ROM sets\0*.zip;*.rom;*.bin;*.awd;*.bio;*.vbi;*.raw\0All files\0*.*\0\0";

static int pick_file(const char *title, const char *filter, const char *folder, char *path,
                     DWORD pathsz, int save, const char *def_name, const char *def_ext)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    snprintf(path, pathsz, "%s", def_name ? def_name : "");
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.lpstrInitialDir = folder;
    ofn.lpstrDefExt = def_ext;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

/* Several files: each full path to fn; the number chosen. */
static int pick_files(const char *title, const char *filter, const char *folder,
                      void (*fn)(const char *path, void *ud), void *ud)
{
    static char buf[64 * 1024];
    OPENFILENAMEA ofn;
    const char *p;
    int n = 0;
    memset(&ofn, 0, sizeof ofn);
    buf[0] = buf[1] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof buf;
    ofn.lpstrTitle = title;
    ofn.lpstrInitialDir = folder;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER |
                OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameA(&ofn)) return 0;
    p = buf + strlen(buf) + 1;
    if (!*p) {
        /* one file: the whole path */
        fn(buf, ud);
        return 1;
    }
    /* the folder, then each name */
    for (; *p; p += strlen(p) + 1) {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s\\%s", buf, p);
        fn(path, ud);
        n++;
    }
    return n;
}

static int pick_folder(const char *title, char *path, DWORD pathsz)
{
    BROWSEINFOA bi;
    PIDLIST_ABSOLUTE pidl;
    char dir[MAX_PATH] = "";
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return 0;
    if (!SHGetPathFromIDListA(pidl, dir)) dir[0] = '\0';
    CoTaskMemFree(pidl);
    snprintf(path, pathsz, "%s", dir);
    return path[0] != '\0';
}

/* ---- a question with its own answers ----------------------------------------------- */

static int g_ask_result;

static LRESULT CALLBACK ask_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) g_ask_result = 0;
        else if (LOWORD(wp) > 100 && LOWORD(wp) < 110) g_ask_result = LOWORD(wp) - 100;
        else if (LOWORD(wp) != IDCANCEL) return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* A modal question: answers[0] is the default (Enter), the last is Cancel
 * (Escape). The answer's index, or -1. */
static int ask(const char *title, const char *text, const char *const *answers, int n)
{
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    static int registered;
    wchar_t wtitle[128], wtext[1024];
    const int width = S(400), bh = S(28), gap = S(8), margin = S(14);
    int text_h, bx, i;
    RECT want, ro;
    HWND dlg, h;
    MSG msg;

    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = ask_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MsdosAskWindow";
        RegisterClassW(&wc);
        registered = 1;
    }
    text_h = text_height(g_ui_font, text, width);
    SetRect(&want, 0, 0, width + 2 * margin, margin + text_h + margin + bh + margin);
    AdjustWindowRectEx(&want, style, FALSE, WS_EX_DLGMODALFRAME);
    GetWindowRect(g_hwnd, &ro);
    msdos_wide(title, wtitle, 128);
    msdos_wide(text, wtext, 1024);
    dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"MsdosAskWindow", wtitle, style,
                          ro.left + ((ro.right - ro.left) - (want.right - want.left)) / 2,
                          ro.top + ((ro.bottom - ro.top) - (want.bottom - want.top)) / 3,
                          want.right - want.left, want.bottom - want.top, g_hwnd, NULL, g_inst, NULL);
    if (!dlg) return -1;
    h = CreateWindowExW(0, L"STATIC", wtext, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, margin, margin,
                        width, text_h, dlg, NULL, g_inst, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    bx = margin + width;
    for (i = n - 1; i >= 0; i--) {
        const int bw = button_width(answers[i]) < S(72) ? S(72) : button_width(answers[i]);
        const int id = i == 0 ? IDOK : i == n - 1 ? IDCANCEL : 100 + i;
        bx -= bw;
        h = CreateWindowExA(0, "BUTTON", answers[i],
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == 0 ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                            bx, margin + text_h + margin, bw, bh, dlg, (HMENU)(INT_PTR)id, g_inst, NULL);
        SendMessageA(h, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        if (i == 0) SetFocus(h);
        bx -= gap;
    }

    g_ask_result = -1;
    EnableWindow(g_hwnd, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(GetDlgItem(dlg, IDOK));
    while (IsWindow(dlg) && GetMessage(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
    return g_ask_result;
}

/* ---- the page: the picture, or how to import a BIOS --------------------------------- */

static void show_page(void)
{
    const int bios = msdossession_has_bios(g_session) != 0;
    const int up = msdossession_is_running(g_session) != 0;
    EnableWindow(g_cad_btn, up);
    EnableWindow(g_reset_btn, up);
    if (bios != g_page_bios) {
        g_page_bios = bios;
        ShowWindow(g_bios_files_btn, bios ? SW_HIDE : SW_SHOW);
        ShowWindow(g_bios_folder_btn, bios ? SW_HIDE : SW_SHOW);
        ShowWindow(g_bios_help_btn, bios ? SW_HIDE : SW_SHOW);
        msdos_help_set_context(g_hwnd, bios ? "main" : "nobios", 0);
        if (bios) SetFocus(g_hwnd);
        layout();
    }
}

static void apply_view(void)
{
    g_smooth = msdossession_smooth(g_session) != 0;
    g_integer = msdossession_integer_scale(g_session) != 0;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void settings_changed(void)
{
    apply_view();
    msdos_controllers_refresh();
    update_status();
    show_page();
}

/* Starts the PC (FujiNet first), or shows the no-BIOS page. */
static void start_machine(void)
{
    msdossession_start_opts opts;
    show_page();
    if (msdossession_is_running(g_session) || !msdossession_has_bios(g_session)) return;
    msdossession_default_opts(g_session, &opts);
    if (msdossession_start(g_session, &opts) != 0) {
        show_message("Cannot Start the PC", msdossession_last_error(g_session));
        update_status();
        return;
    }
    update_status();
    show_page();
}

/* ---- status, and news from the other threads ----------------------------------------- */

static void update_status(void)
{
    const int up = msdossession_is_running(g_session) != 0;
    const int dot = up && msdossession_fujinet_link(g_session);
    char line[512];
    if (!g_statusbar) return;
    if (dot != g_dot_shown) {
        /* part 0 is the link light, drawn in WM_DRAWITEM from this */
        SendMessageA(g_statusbar, SB_SETTEXTA, SBT_OWNERDRAW | 0, (LPARAM)dot);
        g_dot_shown = dot;
    }
    if (g_notice[0] && (LONG)(GetTickCount() - g_notice_until) < 0) {
        snprintf(line, sizeof line, "%s", g_notice);
    } else {
        g_notice[0] = '\0';
        if (up)
            msdossession_status(g_session, line, sizeof line);
        else
            snprintf(line, sizeof line, "%s", msdossession_has_bios(g_session) ? "Off" : "No BIOS");
    }
    if (strcmp(line, g_status_shown) != 0) {
        wchar_t w[512];
        snprintf(g_status_shown, sizeof g_status_shown, "%s", line);
        msdos_wide(line, w, 512);
        SendMessageW(g_statusbar, SB_SETTEXTW, 1, (LPARAM)w);
    }
}

/* The drives' lights: bits 0-3 present, 4-7 lit. */
static void update_lights(void)
{
    const int up = msdossession_is_running(g_session) != 0;
    unsigned bits = 0;
    int i;
    for (i = 0; i < 4 && up; i++) {
        int present, on;
        if (i < 2) {
            msdos_floppy_info fi;
            memset(&fi, 0, sizeof fi);
            msdossession_floppy_info(g_session, i, &fi);
            present = fi.present;
            on = fi.active;
        } else {
            msdos_hd_info hi;
            memset(&hi, 0, sizeof hi);
            msdossession_hd_info(g_session, i - 2, &hi);
            present = hi.path[0] != 0;
            on = hi.active;
        }
        if (present) bits |= 1u << i;
        if (present && on) bits |= 16u << i;
    }
    if (bits != g_lights_shown && g_statusbar) {
        g_lights_shown = bits;
        SendMessageA(g_statusbar, SB_SETTEXTA, SBT_OWNERDRAW | 2, (LPARAM)bits);
    }
}

static void draw_status_part(const DRAWITEMSTRUCT *di)
{
    RECT r = di->rcItem;
    if (di->itemID == 0) {
        /* the link light: the accent while 86Box's COM device is connected */
        const int up = di->itemData != 0, size = S(10);
        const int x = r.left + ((r.right - r.left) - size) / 2;
        const int y = r.top + ((r.bottom - r.top) - size) / 2;
        HGDIOBJ oldb = SelectObject(di->hDC, up ? (HGDIOBJ)g_accent_brush : GetStockObject(GRAY_BRUSH));
        HGDIOBJ oldp = SelectObject(di->hDC, GetStockObject(up ? BLACK_PEN : NULL_PEN));
        Ellipse(di->hDC, x, y, x + size + 1, y + size + 1);
        SelectObject(di->hDC, oldp);
        SelectObject(di->hDC, oldb);
    } else if (di->itemID == 2) {
        static const char *const names[4] = { "A:", "B:", "C:", "D:" };
        const unsigned bits = (unsigned)di->itemData;
        HGDIOBJ oldf = SelectObject(di->hDC, g_ui_font);
        int i, x = r.left + S(2);
        SetBkMode(di->hDC, TRANSPARENT);
        for (i = 0; i < 4; i++) {
            RECT l;
            if (!(bits & (1u << i))) continue;
            SetRect(&l, x, r.top + S(2), x + S(LIGHT_W) - S(4), r.bottom - S(2));
            if (bits & (16u << i)) {
                FillRect(di->hDC, &l, g_accent_brush);
                SetTextColor(di->hDC, MSDOS_INK_RGB);
            } else {
                SetTextColor(di->hDC, GetSysColor(COLOR_GRAYTEXT));
            }
            DrawTextA(di->hDC, names[i], 2, &l, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            x += S(LIGHT_W);
        }
        SelectObject(di->hDC, oldf);
    }
}

static void status_tick(void)
{
    update_status();
}

/* Gamepads come and go, FujiNet restarts itself, 86Box has something to
 * say: the other threads cannot touch the windows, so this timer asks. */
static void fast_tick(void)
{
    char text[1024];
    unsigned gen;

    gen = msdossession_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        if (msdossession_gamepad_last_event(g_session, text, sizeof text) > 0) notice(text);
        msdos_controllers_refresh();
    }
    gen = msdossession_fujinet_generation(g_session);
    if (gen != g_fuji_generation) {
        g_fuji_generation = gen;
        if (msdossession_fujinet_last_event(g_session, text, sizeof text) > 0) notice(text);
        update_status();
    }
    if (msdossession_take_notice(g_session, text, sizeof text) > 0) notice(text);
    if (g_notice[0] && (LONG)(GetTickCount() - g_notice_until) >= 0) update_status();
    update_lights();

    if (g_shot_path[0] && InterlockedCompareExchange(&g_frames, 0, 0) >= g_shot_frames) {
        if (!save_png(g_shot_path)) {
            fprintf(stderr, "MSDOS_SCREENSHOT: could not write %s\n", g_shot_path);
            OutputDebugStringA("MSDOS_SCREENSHOT: could not write the picture\n");
        }
        g_shot_path[0] = '\0';
    }
    if (g_quit_frames && InterlockedCompareExchange(&g_frames, 0, 0) >= g_quit_frames) {
        g_quit_frames = 0;
        PostMessage(g_hwnd, WM_CLOSE, 0, 0);
    }
}

/* ---- opening things ------------------------------------------------------------------ */

static void attach_hd(int drive, const char *path)
{
    char msg[1024];
    if (msdossession_hd_attach(g_session, drive, path, msg, sizeof msg) != 0) {
        show_message("Cannot Attach the Disk", msg);
        return;
    }
    remember_folder(path);
    notice(msg[0] ? msg : base_name(path));
}

static void ask_hd_drive(const char *path)
{
    static const char *const answers[3] = { "C:", "D:", "Cancel" };
    char body[700];
    int a;
    snprintf(body, sizeof body, "%s is a hard disk image. It takes effect from the next reset.",
             base_name(path));
    a = ask("Which Drive?", body, answers, 3);
    if (a == 0 || a == 1) attach_hd(a, path);
}

/* A dropped file, one named on the command line, or Open: a floppy goes in
 * A:, a hard disk asks which drive, a BIOS is imported, anything else goes
 * on FujiNet's SD card. */
static void load_media(const char *path)
{
    char msg[2048];
    int rc;
    msg[0] = '\0';
    rc = msdossession_open(g_session, path, msg, sizeof msg);
    if (rc == 1) {
        ask_hd_drive(path);
        return;
    }
    if (rc != 0) {
        show_message("Cannot Open It", msg[0] ? msg : msdossession_last_error(g_session));
        return;
    }
    remember_folder(path);
    notice(msg[0] ? msg : base_name(path));
    settings_changed();
    start_machine();
}

/* ---- the BIOS ------------------------------------------------------------------------ */

typedef struct {
    char log[4096];
    int imported;
} import_log;

static void import_one(const char *path, void *ud)
{
    import_log *l = ud;
    char msg[2048];
    size_t len = strlen(l->log);
    msg[0] = '\0';
    if (msdossession_rom_import(g_session, path, msg, sizeof msg) == 0) l->imported++;
    snprintf(l->log + len, sizeof l->log - len, "%s%s", len ? "\n" : "", msg[0] ? msg : base_name(path));
}

static void import_bios(int folder)
{
    static import_log l;
    memset(&l, 0, sizeof l);
    if (folder) {
        char dir[MAX_PATH];
        if (!pick_folder("Import BIOS from a Folder (an 86Box ROM set works)", dir, sizeof dir)) return;
        import_one(dir, &l);
    } else if (!pick_files("Import BIOS", kRomFilter, NULL, import_one, &l)) {
        return;
    }
    if (l.log[0])
        msdos_message(g_hwnd, l.log, l.imported ? "BIOS Imported" : "Nothing Imported",
                      (l.imported ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_OK);
    settings_changed();
    start_machine();
}

/* ---- floppies and hard disks ---------------------------------------------------------- */

static void floppy_insert(int d)
{
    char path[MAX_PATH], msg[1024];
    if (!pick_file(d ? "Insert in B:" : "Insert in A:", kFloppyFilter, media_folder(), path, sizeof path,
                   0, NULL, NULL))
        return;
    msg[0] = '\0';
    if (msdossession_floppy_insert(g_session, d, path, 0, msg, sizeof msg) == 0) {
        remember_folder(path);
        notice(msg[0] ? msg : base_name(path));
    } else {
        show_message("Cannot Insert the Disk", msg);
    }
}

static void floppy_blank(int d)
{
    char path[MAX_PATH], msg[512], ins[512];
    if (!pick_file("New Blank Floppy", "Floppy images (*.img)\0*.img\0All files\0*.*\0\0", media_folder(),
                   path, sizeof path, 1, "blank.img", "img"))
        return;
    msg[0] = ins[0] = '\0';
    if (msdossession_new_blank_floppy(g_session, d, path, msg, sizeof msg) == 0
        && msdossession_floppy_insert(g_session, d, path, 0, ins, sizeof ins) == 0) {
        remember_folder(path);
        notice(ins[0] ? ins : base_name(path));
    } else {
        show_message("Cannot Make the Disk", msg[0] ? msg : ins);
    }
}

static void hd_attach_pick(int d)
{
    char path[MAX_PATH];
    if (pick_file(d ? "Attach as D:" : "Attach as C:", kHdFilter, media_folder(), path, sizeof path, 0,
                  NULL, NULL))
        attach_hd(d, path);
}

static void hd_blank(int d)
{
    char path[MAX_PATH], msg[512];
    if (!pick_file("New Blank Hard Disk", "Hard disk images (*.vhd;*.img)\0*.vhd;*.img\0All files\0*.*\0\0",
                   media_folder(), path, sizeof path, 1, d ? "drive-d.vhd" : "drive-c.vhd", "vhd"))
        return;
    msg[0] = '\0';
    if (msdossession_new_blank_hd(g_session, d, path, msg, sizeof msg) == 0) {
        attach_hd(d, path);
        msdos_message(g_hwnd, msg, "New Hard Disk", MB_ICONINFORMATION | MB_OK);
    } else {
        show_message("Cannot Make the Disk", msg);
    }
}

/* ---- FujiNet's SD card ------------------------------------------------------------------ */

static void import_sd_one(const char *path, void *ud)
{
    char dest[1024];
    if (msdossession_import_to_sd(g_session, path, dest, sizeof dest) == 0) (*(int *)ud)++;
}

static void import_sd(void)
{
    int n = 0;
    if (!pick_files("Import to FujiNet's SD Card", kAnyFilter, media_folder(), import_sd_one, &n)) return;
    if (n) {
        char t[200];
        snprintf(t, sizeof t, "%d file%s on FujiNet's SD card: mount %s in CONFIG", n,
                 n == 1 ? " is" : "s are", n == 1 ? "it" : "them");
        notice(t);
    } else {
        show_message("Nothing Imported", "The files could not be copied.");
    }
}

/* ---- text in and out ------------------------------------------------------------------- */

static void copy_screen(void)
{
    static char buf[16384];
    wchar_t *w;
    char *crlf, *o;
    const char *s;
    int n;
    HGLOBAL mem;
    if (msdossession_copy_screen_text(g_session, buf, sizeof buf) <= 0) {
        notice("No text screen to copy");
        return;
    }
    /* the clipboard's lines end in CR LF */
    crlf = malloc(strlen(buf) * 2 + 1);
    if (!crlf) return;
    for (s = buf, o = crlf; *s; s++) {
        if (*s == '\n') *o++ = '\r';
        *o++ = *s;
    }
    *o = '\0';
    n = MultiByteToWideChar(CP_UTF8, 0, crlf, -1, NULL, 0);
    mem = n > 0 ? GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t)) : NULL;
    if (!mem || !OpenClipboard(g_hwnd)) {
        if (mem) GlobalFree(mem);
        free(crlf);
        return;
    }
    w = GlobalLock(mem);
    MultiByteToWideChar(CP_UTF8, 0, crlf, -1, w, n);
    GlobalUnlock(mem);
    free(crlf);
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, mem);
    CloseClipboard();
    notice("The screen's text is on the clipboard");
}

static void paste(void)
{
    HANDLE h;
    if (!OpenClipboard(g_hwnd)) return;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *w = GlobalLock(h);
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            char *u = n > 0 ? malloc((size_t)n) : NULL;
            if (u) {
                WideCharToMultiByte(CP_UTF8, 0, w, -1, u, n, NULL, NULL);
                if (*u) msdossession_paste(g_session, u);
                free(u);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

/* ---- the view ------------------------------------------------------------------------------ */

static void toggle_pref(const char *key)
{
    const int i = msdossession_pref_find(key);
    if (i >= 0) msdossession_pref_set_int(g_session, i, !msdossession_pref_get_int(g_session, i));
    msdossession_settings_flush(g_session);
    apply_view();
}

static void toggle_fullscreen(void)
{
    const DWORD style = (DWORD)GetWindowLongA(g_hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        g_placement.length = sizeof g_placement;
        GetWindowPlacement(g_hwnd, &g_placement);
        if (GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            g_fullscreen = 1;
            SetWindowLongA(g_hwnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
            SetMenu(g_hwnd, NULL);
            ShowWindow(g_statusbar, SW_HIDE);
            ShowWindow(g_cad_btn, SW_HIDE);
            ShowWindow(g_reset_btn, SW_HIDE);
            SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        g_fullscreen = 0;
        SetWindowLongA(g_hwnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetMenu(g_hwnd, g_menubar);
        ShowWindow(g_statusbar, SW_SHOW);
        ShowWindow(g_cad_btn, SW_SHOW);
        ShowWindow(g_reset_btn, SW_SHOW);
        SetWindowPlacement(g_hwnd, &g_placement);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    layout();
}

/* ---- the menus -------------------------------------------------------------------------- */

/* A menu label takes & as a mnemonic: a file name keeps its own. */
static void menu_escape(const char *s, char *out, size_t outsz)
{
    size_t o = 0;
    for (; *s && o + 2 < outsz; s++) {
        if (*s == '&') out[o++] = '&';
        out[o++] = *s;
    }
    out[o] = '\0';
}

static void fill_drives_menu(void)
{
    char label[1200], name[1100];
    int d, any = 0;
    while (GetMenuItemCount(g_drives_menu) > 0) DeleteMenu(g_drives_menu, 0, MF_BYPOSITION);
    for (d = 0; d < MSDOS_FLOPPIES; d++) {
        msdos_floppy_info fi;
        HMENU sub;
        memset(&fi, 0, sizeof fi);
        if (msdossession_floppy_info(g_session, d, &fi) != 0 || !fi.present) continue;
        sub = CreatePopupMenu();
        menu_escape(fi.boot_disk ? "FujiNet boot floppy" : fi.path[0] ? base_name(fi.path) : "Empty",
                    name, sizeof name);
        snprintf(label, sizeof label, "%s%s", name, fi.read_only ? " (write-protected)" : "");
        AppendMenuA(sub, MF_STRING | MF_GRAYED, IDM_DRIVE_INFO, label);
        AppendMenuA(sub, MF_SEPARATOR, 0, NULL);
        AppendMenuA(sub, MF_STRING, IDM_FLOPPY_INSERT + d, "&Insert...");
        AppendMenuA(sub, MF_STRING | (fi.path[0] ? 0 : MF_GRAYED), IDM_FLOPPY_EJECT + d, "&Eject");
        AppendMenuA(sub, MF_STRING | (fi.read_only ? MF_CHECKED : 0), IDM_FLOPPY_PROTECT + d,
                    "&Write-Protect");
        AppendMenuA(sub, MF_STRING, IDM_FLOPPY_BOOT + d, "FujiNet &Boot Floppy");
        AppendMenuA(sub, MF_STRING, IDM_FLOPPY_BLANK + d, "&New Blank Disk...");
        menu_escape(fi.type, name, sizeof name);
        snprintf(label, sizeof label, "%s %s", d ? "&B:" : "&A:", name);
        AppendMenuA(g_drives_menu, MF_POPUP, (UINT_PTR)sub, label);
        any = 1;
    }
    for (d = 0; d < MSDOS_HARD_DISKS; d++) {
        msdos_hd_info hi;
        HMENU sub;
        memset(&hi, 0, sizeof hi);
        if (msdossession_hd_info(g_session, d, &hi) != 0 || !hi.present) continue;
        sub = CreatePopupMenu();
        menu_escape(hi.path[0] ? base_name(hi.path) : "None", name, sizeof name);
        AppendMenuA(sub, MF_STRING | MF_GRAYED, IDM_DRIVE_INFO, name);
        AppendMenuA(sub, MF_SEPARATOR, 0, NULL);
        AppendMenuA(sub, MF_STRING, IDM_HD_ATTACH + d, "&Attach...");
        AppendMenuA(sub, MF_STRING | (hi.path[0] ? 0 : MF_GRAYED), IDM_HD_DETACH + d, "&Detach");
        AppendMenuA(sub, MF_STRING, IDM_HD_BLANK + d, "&New Blank Disk...");
        AppendMenuA(g_drives_menu, MF_POPUP, (UINT_PTR)sub, d ? "&D: hard disk" : "&C: hard disk");
        any = 1;
    }
    if (!any) AppendMenuA(g_drives_menu, MF_STRING | MF_GRAYED, IDM_DRIVE_INFO, "(the PC's drives)");
    AppendMenuA(g_drives_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_drives_menu, MF_STRING, IDM_IMPORT_SD, "&Import to SD Card...");
    AppendMenuA(g_drives_menu, MF_STRING, IDM_SD_FOLDER, "Open &SD Folder");
}

static void enable(HMENU m, UINT id, int on)
{
    EnableMenuItem(m, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
}

static void check(HMENU m, UINT id, int on)
{
    CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
}

static void sync_menus(void)
{
    const int up = msdossession_is_running(g_session) != 0;
    enable(g_machine_menu, IDM_CAD, up);
    enable(g_machine_menu, IDM_PAUSE, up);
    enable(g_machine_menu, IDM_WARP, up);
    check(g_machine_menu, IDM_PAUSE, up && msdossession_paused(g_session));
    check(g_machine_menu, IDM_WARP, up && msdossession_warp(g_session));
    enable(g_edit_menu, IDM_COPY_SCREEN, up);
    enable(g_edit_menu, IDM_PASTE, up);
    check(g_view_menu, IDM_ASPECT, msdossession_get_int(g_session, "aspect43", 1) != 0);
    check(g_view_menu, IDM_INTEGER, msdossession_integer_scale(g_session) != 0);
    check(g_view_menu, IDM_SMOOTH, msdossession_smooth(g_session) != 0);
    check(g_view_menu, IDM_CONTROLLERS, msdos_controllers_visible());
    check(g_view_menu, IDM_DEBUGGER, msdos_debugger_visible());
    enable(g_fuji_menu, IDM_FUJINET_CONFIG, msdossession_fujinet_running(g_session));
    enable(g_fuji_menu, IDM_FUJINET_RESTART, up && msdossession_get_int(g_session, "fujinet", 1));
}

static void build_menu(void)
{
    HMENU help = CreatePopupMenu();

    g_menubar = CreateMenu();
    g_machine_menu = CreatePopupMenu();
    AppendMenuA(g_machine_menu, MF_STRING, IDM_OPEN, "&Open...\tCtrl+Shift+O");
    AppendMenuA(g_machine_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_machine_menu, MF_STRING, IDM_CAD, "Ctrl+&Alt+Del");
    AppendMenuA(g_machine_menu, MF_STRING, IDM_RESET, "&Reset\tCtrl+Shift+R");
    AppendMenuA(g_machine_menu, MF_STRING, IDM_PAUSE, "&Pause");
    AppendMenuA(g_machine_menu, MF_STRING, IDM_WARP, "&Warp Speed\tCtrl+Shift+W");
    AppendMenuA(g_machine_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_machine_menu, MF_STRING, IDM_IMPORT_BIOS, "Import &BIOS...");
    AppendMenuA(g_machine_menu, MF_STRING, IDM_IMPORT_BIOS_DIR, "Import BIOS from a &Folder...");
    AppendMenuA(g_machine_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_machine_menu, MF_STRING, IDM_PREFS, "Pr&eferences...\tCtrl+Shift+P");
    AppendMenuA(g_machine_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_machine_menu, MF_STRING, IDM_EXIT, "E&xit\tCtrl+Shift+Q");

    g_drives_menu = CreatePopupMenu();
    fill_drives_menu();

    g_edit_menu = CreatePopupMenu();
    AppendMenuA(g_edit_menu, MF_STRING, IDM_COPY_SCREEN, "&Copy Screen Text\tCtrl+Shift+C");
    AppendMenuA(g_edit_menu, MF_STRING, IDM_PASTE, "P&aste\tCtrl+Shift+V");

    g_view_menu = CreatePopupMenu();
    AppendMenuA(g_view_menu, MF_STRING, IDM_CONTROLLERS, "C&ontrollers\tCtrl+Shift+K");
    AppendMenuA(g_view_menu, MF_STRING, IDM_DEBUGGER, "De&bugger\tCtrl+Shift+D");
    AppendMenuA(g_view_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_view_menu, MF_STRING, IDM_ASPECT, "&4:3 Picture");
    AppendMenuA(g_view_menu, MF_STRING, IDM_INTEGER, "&Whole-Number Scaling");
    AppendMenuA(g_view_menu, MF_STRING, IDM_SMOOTH, "S&mooth Scaling");
    AppendMenuA(g_view_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_view_menu, MF_STRING, IDM_FULLSCREEN, "&Fullscreen\tCtrl+Alt+PgDn");

    g_fuji_menu = CreatePopupMenu();
    AppendMenuA(g_fuji_menu, MF_STRING, IDM_FUJINET_CONFIG, "FujiNet &Web UI");
    AppendMenuA(g_fuji_menu, MF_STRING, IDM_FUJINET_LOG, "Console &Log");
    AppendMenuA(g_fuji_menu, MF_STRING, IDM_FUJINET_RESTART, "R&estart FujiNet");

    AppendMenuA(help, MF_STRING, IDM_HELP, APP_TITLE " &Help\tCtrl+Shift+H");
    AppendMenuA(help, MF_SEPARATOR, 0, NULL);
    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About " APP_TITLE);

    AppendMenuA(g_menubar, MF_POPUP, (UINT_PTR)g_machine_menu, "&Machine");
    AppendMenuA(g_menubar, MF_POPUP, (UINT_PTR)g_drives_menu, "&Drives");
    AppendMenuA(g_menubar, MF_POPUP, (UINT_PTR)g_edit_menu, "&Edit");
    AppendMenuA(g_menubar, MF_POPUP, (UINT_PTR)g_view_menu, "&View");
    AppendMenuA(g_menubar, MF_POPUP, (UINT_PTR)g_fuji_menu, "&FujiNet");
    AppendMenuA(g_menubar, MF_POPUP, (UINT_PTR)help, "&Help");
    SetMenu(g_hwnd, g_menubar);
    sync_menus();
}

/* ---- Preferences ------------------------------------------------------------------------- */

/* A live row changed (the picture, the volume), or a ROM came in on the
 * ROMs page: the view, the status and the page follow. */
static void prefs_live(void)
{
    apply_view();
    update_status();
    show_page();
}

static void prefs_closed(int applied)
{
    settings_changed();
    if (applied == MSDOS_APPLY_RESET) notice("Reset with the new configuration");
    else if (applied == MSDOS_APPLY_RESTART) notice("FujiNet restarted");
    /* a BIOS imported on the ROMs page: the PC can start now */
    start_machine();
}

static void show_prefs(void)
{
    msdos_prefs_show(g_hwnd, g_session, prefs_live, prefs_closed);
}

static void about(void)
{
    MSGBOXPARAMSW mb;
    wchar_t text[2048];
    msdos_wide(APP_TITLE " " MSDOS_VERSION_STRING "\n\n"
               "IBM PCs and compatibles, from the 5150 to a Pentium, with a FujiNet on a COM port. "
               "The emulator is 86Box (GPL-2.0-or-later, the 86Box team), with the FujiNet firmware "
               "(GPL-3.0-or-later) talking to FUJINET.SYS. The boot floppy is FreeDOS (GPL). The BIOS "
               "ROMs are imported.\n\n"
               "Copyright (C) 2026 Thomas Cherryhomes -- GPL-3.0-or-later\n"
               "https://fujinet.online/\n"
               "Issues: https://github.com/FujiNetWIFI/fujinet-go-msdos-desktop/issues",
               text, 2048);
    memset(&mb, 0, sizeof mb);
    mb.cbSize = sizeof mb;
    mb.hwndOwner = g_hwnd;
    mb.hInstance = g_inst;
    mb.lpszText = text;
    mb.lpszCaption = L"About " APP_TITLE;
    mb.dwStyle = MB_OK | MB_USERICON;
    mb.lpszIcon = MAKEINTRESOURCEW(IDI_APPICON);
    MessageBoxIndirectW(&mb);
}

/* ---- commands ------------------------------------------------------------------------------- */

static void command(int id)
{
    if (id >= IDM_FLOPPY_INSERT && id < IDM_FLOPPY_INSERT + MSDOS_FLOPPIES) {
        floppy_insert(id - IDM_FLOPPY_INSERT);
        return;
    }
    if (id >= IDM_FLOPPY_EJECT && id < IDM_FLOPPY_EJECT + MSDOS_FLOPPIES) {
        msdossession_floppy_eject(g_session, id - IDM_FLOPPY_EJECT);
        return;
    }
    if (id >= IDM_FLOPPY_PROTECT && id < IDM_FLOPPY_PROTECT + MSDOS_FLOPPIES) {
        msdos_floppy_info fi;
        const int d = id - IDM_FLOPPY_PROTECT;
        memset(&fi, 0, sizeof fi);
        if (msdossession_floppy_info(g_session, d, &fi) == 0)
            msdossession_floppy_set_read_only(g_session, d, !fi.read_only);
        return;
    }
    if (id >= IDM_FLOPPY_BOOT && id < IDM_FLOPPY_BOOT + MSDOS_FLOPPIES) {
        msdossession_floppy_boot_disk(g_session, id - IDM_FLOPPY_BOOT);
        notice("The FujiNet boot floppy is in the drive");
        return;
    }
    if (id >= IDM_FLOPPY_BLANK && id < IDM_FLOPPY_BLANK + MSDOS_FLOPPIES) {
        floppy_blank(id - IDM_FLOPPY_BLANK);
        return;
    }
    if (id >= IDM_HD_ATTACH && id < IDM_HD_ATTACH + MSDOS_HARD_DISKS) {
        hd_attach_pick(id - IDM_HD_ATTACH);
        return;
    }
    if (id >= IDM_HD_DETACH && id < IDM_HD_DETACH + MSDOS_HARD_DISKS) {
        msdossession_hd_detach(g_session, id - IDM_HD_DETACH);
        notice("Detached from the next reset");
        return;
    }
    if (id >= IDM_HD_BLANK && id < IDM_HD_BLANK + MSDOS_HARD_DISKS) {
        hd_blank(id - IDM_HD_BLANK);
        return;
    }

    switch (id) {
    case IDM_OPEN: {
        char path[MAX_PATH];
        if (pick_file("Open", kAnyFilter, media_folder(), path, sizeof path, 0, NULL, NULL))
            load_media(path);
        break;
    }
    case IDM_CAD:
    case IDC_CAD_BUTTON:
        msdossession_ctrl_alt_del(g_session);
        break;
    case IDM_RESET:
    case IDC_RESET_BUTTON:
        if (!msdossession_is_running(g_session)) {
            start_machine();
            break;
        }
        msdossession_reset(g_session);
        update_status();
        break;
    case IDM_PAUSE:
        msdossession_set_paused(g_session, !msdossession_paused(g_session));
        update_status();
        break;
    case IDM_WARP:
        msdossession_set_warp(g_session, !msdossession_warp(g_session));
        update_status();
        break;
    case IDM_IMPORT_BIOS:
    case IDC_BIOS_FILES:
        import_bios(0);
        break;
    case IDM_IMPORT_BIOS_DIR:
    case IDC_BIOS_FOLDER:
        import_bios(1);
        break;
    case IDC_BIOS_HELP: msdos_help_show(g_hwnd, "nobios"); break;
    case IDM_PREFS: show_prefs(); break;
    case IDM_EXIT: PostMessage(g_hwnd, WM_CLOSE, 0, 0); break;

    case IDM_IMPORT_SD: import_sd(); break;
    case IDM_SD_FOLDER: open_folder(msdossession_sd_path(g_session)); break;

    case IDM_COPY_SCREEN: copy_screen(); break;
    case IDM_PASTE: paste(); break;

    case IDM_CONTROLLERS: msdos_controllers_toggle(g_hwnd, g_session); break;
    case IDM_DEBUGGER: msdos_debugger_toggle(g_hwnd, g_session); break;
    case IDM_ASPECT: toggle_pref("aspect43"); break;
    case IDM_INTEGER: toggle_pref("integer_scale"); break;
    case IDM_SMOOTH: toggle_pref("smooth"); break;
    case IDM_FULLSCREEN: toggle_fullscreen(); break;

    case IDM_FUJINET_CONFIG:
        /* the system browser, not an embedded view: the web UI's Google and
         * OneDrive Authorize buttons need a real one */
        if (!msdossession_fujinet_running(g_session)) {
            notice("FujiNet is off (Preferences \xe2\x96\xb8 FujiNet)");
            break;
        }
        ShellExecuteA(g_hwnd, "open", msdossession_fujinet_webui_url(g_session), NULL, NULL, SW_SHOWNORMAL);
        break;
    case IDM_FUJINET_LOG: msdos_fujilog_show(g_hwnd, g_session); break;
    case IDM_FUJINET_RESTART:
        if (msdossession_fujinet_restart(g_session) != 0)
            show_message("Cannot Restart FujiNet", msdossession_last_error(g_session));
        else
            notice("FujiNet restarted");
        break;
    case IDM_ABOUT: about(); break;
    case IDM_HELP: msdos_help_for(g_hwnd); break;
    default: break;
    }
}

/* ---- the keyboard ---------------------------------------------------------------------- */

/* The HID usage of a key message: the scan code and its E0 bit through the
 * session's table, then the virtual key for what that cannot say (Pause and
 * Num Lock share 45h; synthesized keys may carry no scan code). */
static uint32_t hid_from_msg(WPARAM vk, LPARAM lp)
{
    const unsigned sc = (unsigned)((lp >> 16) & 0xff);
    const int ext = (int)((lp >> 24) & 1);
    uint32_t hid;
    if (vk == VK_PAUSE) return 0x48;
    if (vk == VK_NUMLOCK) return 0x53;
    hid = msdossession_hid_from_win_scancode(sc, ext);
    if (hid) return hid;
    if (vk >= 'A' && vk <= 'Z') return 0x04 + (uint32_t)(vk - 'A');
    if (vk >= '1' && vk <= '9') return 0x1E + (uint32_t)(vk - '1');
    if (vk == '0') return 0x27;
    if (vk >= VK_F1 && vk <= VK_F12) return 0x3A + (uint32_t)(vk - VK_F1);
    switch (vk) {
    case VK_RETURN: return 0x28;
    case VK_ESCAPE: return 0x29;
    case VK_BACK: return 0x2A;
    case VK_TAB: return 0x2B;
    case VK_SPACE: return 0x2C;
    case VK_CAPITAL: return 0x39;
    case VK_SCROLL: return 0x47;
    case VK_INSERT: return 0x49;
    case VK_HOME: return 0x4A;
    case VK_PRIOR: return 0x4B;
    case VK_DELETE: return 0x4C;
    case VK_END: return 0x4D;
    case VK_NEXT: return 0x4E;
    case VK_RIGHT: return 0x4F;
    case VK_LEFT: return 0x50;
    case VK_DOWN: return 0x51;
    case VK_UP: return 0x52;
    case VK_SHIFT: case VK_LSHIFT: return 0xE1;
    case VK_RSHIFT: return 0xE5;
    case VK_CONTROL: case VK_LCONTROL: return 0xE0;
    case VK_RCONTROL: return 0xE4;
    case VK_MENU: case VK_LMENU: return 0xE2;
    case VK_RMENU: return 0xE6;
    default: return 0;
    }
}

/* Ctrl+Shift+letter: the app's (the others swallowed, as the other
 * frontends keep the whole Ctrl+Shift alphabet). */
static void shortcut(WPARAM vk)
{
    switch (vk) {
    case 'P': command(IDM_PREFS); break;
    case 'Q': command(IDM_EXIT); break;
    case 'O': command(IDM_OPEN); break;
    case 'C': command(IDM_COPY_SCREEN); break;
    case 'V': command(IDM_PASTE); break;
    case 'R': command(IDM_RESET); break;
    case 'K': command(IDM_CONTROLLERS); break;
    case 'D': command(IDM_DEBUGGER); break;
    case 'F': command(IDM_FULLSCREEN); break;
    case 'W': command(IDM_WARP); break;
    case 'H': command(IDM_HELP); break;
    default: break;
    }
}

/* A key message for this window (or forwarded from the Controllers panel):
 * the window's own keys, else the PC's. 1 when it was used. */
static int handle_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const int repeat = (lp & (1L << 30)) != 0 && down;
    const int ctrl = GetKeyState(VK_CONTROL) < 0;
    const int shift = GetKeyState(VK_SHIFT) < 0;
    const int alt = GetKeyState(VK_MENU) < 0;
    uint32_t hid;

    if (down) {
        /* Ctrl+End lets the mouse go (86Box's own key for it) */
        if (ctrl && !alt && vk == VK_END && g_captured) {
            set_captured(0);
            return 1;
        }
        if (ctrl && alt && vk == VK_NEXT) {
            if (!repeat) toggle_fullscreen();
            return 1;
        }
        if (ctrl && shift && vk >= 'A' && vk <= 'Z') {
            if (!repeat) shortcut(vk);
            return 1;
        }
        /* the Windows keys are the desktop's; a PC/AT had none */
        if (vk == VK_LWIN || vk == VK_RWIN || GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0)
            return 0;
    }
    /* Alt+F4 and Alt+Space stay the desktop's (close, the window menu) */
    if (alt && !ctrl && (vk == VK_F4 || vk == VK_SPACE)) return 0;
    hid = hid_from_msg(vk, lp);
    if (!hid) return 0;
    /* a release always goes: a key let go while a shortcut was held must
     * not stick */
    return msdossession_key(g_session, hid, down) != 0;
}

int msdos_main_key(UINT msg, WPARAM vk, LPARAM lp)
{
    return g_hwnd ? handle_key(msg, vk, lp) : 0;
}

/* ---- the mouse ---------------------------------------------------------------------------- */

static void clip_to_picture(void)
{
    RECT r = g_shown;
    if (r.right <= r.left || r.bottom <= r.top) GetClientRect(g_hwnd, &r);
    MapWindowPoints(g_hwnd, NULL, (POINT *)&r, 2);
    ClipCursor(&r);
}

/* The mouse belongs to the PC: hidden, clipped to the picture, its
 * movement sent as mickeys from raw input. */
static void set_captured(int on)
{
    RAWINPUTDEVICE rid;
    on = on && msdossession_mouse_fitted(g_session) && msdossession_is_running(g_session)
         && msdossession_has_bios(g_session);
    if (on == g_captured) return;
    g_captured = on;
    g_abs_have = 0;
    g_wheel = 0;
    memset(&rid, 0, sizeof rid);
    rid.usUsagePage = 0x01;     /* generic desktop */
    rid.usUsage = 0x02;         /* mouse */
    if (on) {
        rid.hwndTarget = g_hwnd;
        RegisterRawInputDevices(&rid, 1, sizeof rid);
        clip_to_picture();
        SetCursor(NULL);
        notice("Mouse captured: Ctrl+End or the middle button lets it go");
    } else {
        rid.dwFlags = RIDEV_REMOVE;
        RegisterRawInputDevices(&rid, 1, sizeof rid);
        ClipCursor(NULL);
        if (g_buttons) {
            g_buttons = 0;
            msdossession_mouse_buttons(g_session, 0);
        }
        SetCursor(LoadCursor(NULL, IDC_ARROW));
    }
}

static void raw_input(HRAWINPUT h)
{
    RAWINPUT ri;
    UINT sz = sizeof ri;
    const RAWMOUSE *m;
    if (GetRawInputData(h, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    if (ri.header.dwType != RIM_TYPEMOUSE) return;
    m = &ri.data.mouse;
    if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {
        /* a tablet or a remote desktop: positions on the (virtual) screen */
        const int virt = (m->usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const LONG w = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        const LONG hgt = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        const LONG x = MulDiv(m->lLastX, w, 65535), y = MulDiv(m->lLastY, hgt, 65535);
        if (g_abs_have && (x != g_abs_x || y != g_abs_y))
            msdossession_mouse_motion(g_session, (int)(x - g_abs_x), (int)(y - g_abs_y));
        g_abs_x = x;
        g_abs_y = y;
        g_abs_have = 1;
    } else if (m->lLastX || m->lLastY) {
        msdossession_mouse_motion(g_session, (int)m->lLastX, (int)m->lLastY);
    }
}

static void mouse_button(UINT msg, LPARAM lp)
{
    POINT p;
    p.x = GET_X_LPARAM(lp);
    p.y = GET_Y_LPARAM(lp);
    SetFocus(g_hwnd);
    if (!g_captured) {
        if (msg == WM_LBUTTONDOWN && PtInRect(&g_shown, p)) set_captured(1);
        return;
    }
    switch (msg) {
    case WM_MBUTTONDOWN: set_captured(0); return;
    case WM_LBUTTONDOWN: g_buttons |= 1u; break;
    case WM_LBUTTONUP: g_buttons &= ~1u; break;
    case WM_RBUTTONDOWN: g_buttons |= 2u; break;
    case WM_RBUTTONUP: g_buttons &= ~2u; break;
    default: return;
    }
    msdossession_mouse_buttons(g_session, g_buttons);
}

/* ---- the window ------------------------------------------------------------------------ */

static int statusbar_height(void)
{
    RECT r;
    if (!g_statusbar || !IsWindowVisible(g_statusbar)) return 0;
    GetWindowRect(g_statusbar, &r);
    return r.bottom - r.top;
}

static void layout(void)
{
    RECT c, pic;
    int top, bottom, x, w;
    if (!g_hwnd) return;
    GetClientRect(g_hwnd, &c);
    if (g_statusbar) {
        int parts[3];
        SendMessageA(g_statusbar, WM_SIZE, 0, 0);
        parts[0] = S(DOT_PART_W);
        parts[1] = c.right - 4 * S(LIGHT_W) - S(24) > parts[0] ? c.right - 4 * S(LIGHT_W) - S(24) : parts[0];
        parts[2] = -1;
        SendMessageA(g_statusbar, SB_SETPARTS, 3, (LPARAM)parts);
        SendMessageA(g_statusbar, SB_SETTEXTA, SBT_OWNERDRAW | 0, (LPARAM)(g_dot_shown > 0));
        SendMessageA(g_statusbar, SB_SETTEXTA, SBT_OWNERDRAW | 2, (LPARAM)(g_lights_shown == ~0u ? 0 : g_lights_shown));
    }
    top = g_fullscreen ? 0 : S(STRIP_H);
    bottom = c.bottom - statusbar_height();
    if (bottom < top) bottom = top;

    /* the quick buttons, at the strip's left */
    x = S(8);
    w = button_width("Ctrl+Alt+Del");
    MoveWindow(g_cad_btn, x, (S(STRIP_H) - S(BUTTON_H)) / 2, w, S(BUTTON_H), TRUE);
    x += w + S(6);
    w = button_width("Reset");
    MoveWindow(g_reset_btn, x, (S(STRIP_H) - S(BUTTON_H)) / 2, w, S(BUTTON_H), TRUE);

    SetRect(&pic, 0, top, c.right, bottom);
    EnterCriticalSection(&g_fb_lock);
    g_pic = pic;
    LeaveCriticalSection(&g_fb_lock);

    if (!msdossession_has_bios(g_session)) {
        nobios_geom g;
        nobios_layout(&pic, &g);
        MoveWindow(g_bios_files_btn, g.files.left, g.files.top, g.files.right - g.files.left,
                   g.files.bottom - g.files.top, TRUE);
        MoveWindow(g_bios_folder_btn, g.folder.left, g.folder.top, g.folder.right - g.folder.left,
                   g.folder.bottom - g.folder.top, TRUE);
        MoveWindow(g_bios_help_btn, g.help.left, g.help.top, g.help.right - g.help.left,
                   g.help.bottom - g.help.top, TRUE);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
    if (g_captured) {
        /* g_shown follows at the next paint; clip to the area meanwhile */
        UpdateWindow(g_hwnd);
        clip_to_picture();
    }
}

static void make_fonts(void)
{
    HWND controls[6] = { g_cad_btn, g_reset_btn, g_bios_files_btn, g_bios_folder_btn, g_bios_help_btn,
                         g_statusbar };
    int i;
    if (g_ui_font) DeleteObject(g_ui_font);
    if (g_title_font) DeleteObject(g_title_font);
    g_ui_font = msdos_make_font(g_dpi, 0, FW_NORMAL, 0, NULL);
    g_title_font = msdos_make_font(g_dpi, 22, FW_BOLD, 0, NULL);
    for (i = 0; i < 6; i++)
        if (controls[i]) SendMessageA(controls[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    if (g_big_icon) DestroyIcon(g_big_icon);
    g_big_icon = (HICON)LoadImageA(g_inst, MAKEINTRESOURCEA(IDI_APPICON), IMAGE_ICON, S(64), S(64), 0);
}

static void add_tip(HWND control, const char *text)
{
    TTTOOLINFOA ti;
    if (!g_tip) return;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = TTTOOLINFOA_V2_SIZE;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_hwnd;
    ti.uId = (UINT_PTR)control;
    ti.lpszText = (char *)text;
    SendMessageA(g_tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

static HWND make_button(const char *text, int id)
{
    return CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_hwnd,
                           (HMENU)(INT_PTR)id, g_inst, NULL);
}

static void create_controls(void)
{
    g_cad_btn = make_button("Ctrl+Alt+Del", IDC_CAD_BUTTON);
    g_reset_btn = make_button("Reset", IDC_RESET_BUTTON);
    g_bios_files_btn = CreateWindowExA(0, "BUTTON", "Import BIOS...",
                                       WS_CHILD | BS_DEFPUSHBUTTON, 0, 0, 10, 10, g_hwnd,
                                       (HMENU)(INT_PTR)IDC_BIOS_FILES, g_inst, NULL);
    g_bios_folder_btn = CreateWindowExA(0, "BUTTON", "Import a Folder...", WS_CHILD | BS_PUSHBUTTON, 0, 0,
                                        10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_BIOS_FOLDER, g_inst, NULL);
    g_bios_help_btn = CreateWindowExA(0, "BUTTON", "Help", WS_CHILD | BS_PUSHBUTTON, 0, 0, 10, 10, g_hwnd,
                                      (HMENU)(INT_PTR)IDC_BIOS_HELP, g_inst, NULL);
    g_statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                                  g_hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR, g_inst, NULL);
    g_tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_hwnd, NULL, g_inst,
                            NULL);
    if (g_tip) SendMessageA(g_tip, TTM_SETMAXTIPWIDTH, 0, S(360));
    add_tip(g_cad_btn, "Send Ctrl+Alt+Del: DOS restarts (a warm boot)");
    add_tip(g_reset_btn, "Press the reset button: the PC starts again from cold (Ctrl+Shift+R)");
    make_fonts();
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        layout();
        return 0;
    case WM_MOVE:
        if (g_captured) clip_to_picture();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(480);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(360);
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        g_dpi = HIWORD(wp);
        make_fonts();
        if (g_tip) SendMessageA(g_tip, TTM_SETMAXTIPWIDTH, 0, S(360));
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_DRAWITEM:
        if (wp == IDC_STATUSBAR) {
            draw_status_part((const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_INITMENUPOPUP:
        if ((HMENU)wp == g_drives_menu) fill_drives_menu();
        sync_menus();
        break;
    case WM_ENTERMENULOOP:
        /* the menu has the keyboard and the mouse */
        set_captured(0);
        break;

    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_KEYUP: case WM_SYSKEYUP:
        if (handle_key(msg, wp, lp)) return 0;
        break;
    case WM_SYSCHAR:
        /* Alt+letter went to the PC: no menu mnemonic, no beep. Alt+Space
         * opens the window menu. */
        if (wp != ' ') return 0;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            const HWND other = (HWND)lp;
            const int ours = other && GetWindowThreadProcessId(other, NULL) == GetCurrentThreadId();
            /* typing on the Controllers panel keeps what is held here */
            if (!(ours && msdos_controllers_visible())) msdossession_release_keys(g_session);
            set_captured(0);
        }
        break;
    case WM_CANCELMODE:
        set_captured(0);
        break;

    case WM_SETCURSOR:
        if (g_captured && LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_INPUT:
        if (g_captured) raw_input((HRAWINPUT)lp);
        break;                          /* DefWindowProc cleans up */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
        mouse_button(msg, lp);
        return 0;
    case WM_MOUSEWHEEL:
        if (g_captured) {
            g_wheel += GET_WHEEL_DELTA_WPARAM(wp);
            while (g_wheel >= WHEEL_DELTA) {
                msdossession_mouse_wheel(g_session, 1);
                g_wheel -= WHEEL_DELTA;
            }
            while (g_wheel <= -WHEEL_DELTA) {
                msdossession_mouse_wheel(g_session, -1);
                g_wheel += WHEEL_DELTA;
            }
            return 0;
        }
        break;

    case WM_TIMER:
        if (wp == IDT_STATUS) status_tick();
        else if (wp == IDT_FAST) fast_tick();
        return 0;

    case WM_COMMAND:
        command(LOWORD(wp));
        /* a quick button keeps no focus: typing is the PC's */
        if (lp && (HWND)lp != g_hwnd && GetParent((HWND)lp) == hwnd && IsWindow(hwnd)) SetFocus(hwnd);
        return 0;

    case WM_DROPFILES: {
        char path[MAX_PATH];
        HDROP drop = (HDROP)wp;
        path[0] = '\0';
        DragQueryFileA(drop, 0, path, sizeof path);
        DragFinish(drop);
        if (path[0]) {
            SetForegroundWindow(hwnd);
            load_media(path);
        }
        return 0;
    }

    case WM_CLOSE:
        /* Preferences open while quitting: nothing is reset on the way out */
        msdos_prefs_abandon();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_STATUS);
        KillTimer(hwnd, IDT_FAST);
        set_captured(0);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int env_on(const char *name)
{
    const char *env = getenv(name);
    return env && *env && *env != '0';
}

/* Keys sent to one of this window's controls (a quick button just clicked,
 * the status bar) are the window's own. */
static int main_pretranslate(MSG *msg)
{
    if (!g_hwnd || !msg->hwnd || msg->hwnd == g_hwnd || GetParent(msg->hwnd) != g_hwnd) return 0;
    switch (msg->message) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        SendMessageA(g_hwnd, msg->message, msg->wParam, msg->lParam);
        return 1;
    default:
        return 0;
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEXA wc;
    MSG msg;
    RECT want, work;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES |
                                             ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS };
    char media[MAX_PATH] = "";
    int ww, wh;
    (void)prev;

    g_inst = inst;
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* the folder picker */
    InitializeCriticalSection(&g_fb_lock);

    g_session = msdossession_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session (unusable config or data directories?)",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    g_fb = calloc((size_t)FB_W * FB_H, sizeof *g_fb);
    if (!g_fb) return 1;
    g_accent_brush = CreateSolidBrush(msdos_accent_rgb());

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof g_bmi.bmiHeader;
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = WIN_CLASS;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExA(&wc);

    /* 640 x 480 at one and a half, plus the strip and the status bar */
    g_dpi = msdos_system_dpi();
    SetRect(&want, 0, 0, S(960), S(720) + S(STRIP_H) + S(24));
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_ACCEPTFILES);
    ww = want.right - want.left;
    wh = want.bottom - want.top;
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
        if (ww > work.right - work.left) ww = work.right - work.left;
        if (wh > work.bottom - work.top) wh = work.bottom - work.top;
    }
    g_hwnd = CreateWindowExA(WS_EX_ACCEPTFILES, WIN_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, ww, wh, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    if (msdos_window_dpi(g_hwnd) != g_dpi) {
        /* placed on a monitor of another DPI than the system's */
        const int old = g_dpi;
        g_dpi = msdos_window_dpi(g_hwnd);
        SetWindowPos(g_hwnd, NULL, 0, 0, MulDiv(ww, g_dpi, old), MulDiv(wh, g_dpi, old),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    create_controls();
    build_menu();
    apply_view();
    layout();
    ShowWindow(g_hwnd, show);

    if (cmdline && *cmdline) {
        /* a path on the command line, quotes and all, goes where Open would
         * put it once the machine is up */
        const char *p = cmdline;
        size_t n;
        while (*p == ' ') p++;
        if (*p == '"') p++;
        snprintf(media, sizeof media, "%s", p);
        n = strlen(media);
        while (n && (media[n - 1] == '"' || media[n - 1] == ' ')) media[--n] = '\0';
    }

    /* the current pads and FujiNet state are the baseline: no notices */
    g_pad_generation = msdossession_gamepad_generation(g_session);
    g_fuji_generation = msdossession_fujinet_generation(g_session);
    update_status();
    show_page();

    /* FujiNet first, then the machine */
    start_machine();
    if (media[0]) load_media(media);
    g_fuji_generation = msdossession_fujinet_generation(g_session);
    SetTimer(g_hwnd, IDT_STATUS, 1000, NULL);
    SetTimer(g_hwnd, IDT_FAST, 100, NULL);
    update_status();

    /* MSDOS_OPEN_CONTROLLERS / _DEBUGGER / _SETTINGS open those windows at
     * launch (MSDOS_DEBUGGER_TAB picks the debugger's tab), MSDOS_OPEN_HELP=<slug,
     * context or ?words> the Help ("1": its start).
     * MSDOS_SCREENSHOT=file.png saves the picture after
     * MSDOS_SCREENSHOT_FRAMES frames (300); MSDOS_AUTOQUIT_FRAMES quits after
     * that many. */
    if (env_on("MSDOS_OPEN_CONTROLLERS")) msdos_controllers_toggle(g_hwnd, g_session);
    if (env_on("MSDOS_OPEN_DEBUGGER")) msdos_debugger_show(g_hwnd, g_session);
    if (env_on("MSDOS_OPEN_SETTINGS")) show_prefs();
    if (env_on("MSDOS_OPEN_HELP"))
        msdos_help_show(g_hwnd, strcmp(getenv("MSDOS_OPEN_HELP"), "1") ? getenv("MSDOS_OPEN_HELP") : NULL);
    if (getenv("MSDOS_SCREENSHOT") && *getenv("MSDOS_SCREENSHOT"))
        snprintf(g_shot_path, sizeof g_shot_path, "%s", getenv("MSDOS_SCREENSHOT"));
    if (getenv("MSDOS_SCREENSHOT_FRAMES")) g_shot_frames = atol(getenv("MSDOS_SCREENSHOT_FRAMES"));
    if (getenv("MSDOS_AUTOQUIT_FRAMES")) g_quit_frames = atol(getenv("MSDOS_AUTOQUIT_FRAMES"));

    g_present_thread = CreateThread(NULL, 0, present_thread, NULL, 0, NULL);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* Before TranslateMessage, so the other windows' keys reach them
         * whichever of their controls has the focus. */
        if (msdos_help_pretranslate(&msg)) continue;
        if (msdos_debugger_pretranslate(&msg)) continue;
        if (msdos_controllers_pretranslate(&msg)) continue;
        if (main_pretranslate(&msg)) continue;
        if (msdos_prefs_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    InterlockedExchange(&g_running, 0);
    if (g_present_thread) {
        WaitForSingleObject(g_present_thread, 2000);
        CloseHandle(g_present_thread);
    }
    msdossession_stop(g_session);
    msdossession_free(g_session);
    free(g_fb);
    DeleteObject(g_accent_brush);
    if (g_ui_font) DeleteObject(g_ui_font);
    if (g_title_font) DeleteObject(g_title_font);
    if (g_big_icon) DestroyIcon(g_big_icon);
    DeleteCriticalSection(&g_fb_lock);
    CoUninitialize();
    return (int)msg.wParam;
}
