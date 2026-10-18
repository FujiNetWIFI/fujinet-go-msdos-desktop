/*
 * Helpers every window of the Win32 frontend shares: DPI, the UI font at a
 * window's DPI, UTF-8 text into wide controls, and the accent colour.
 *
 * The manifest makes the process per-monitor DPI aware (v2), so nothing is
 * scaled for us: CreateFont(-13) is 13 physical pixels, tiny at 200 %.
 * Every window lays itself out in 96-dpi units through msdos_scale() and
 * remakes its fonts on WM_DPICHANGED.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

#include "msdossession.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

/* Text on an accent fill: the IBM badge's ink, not white. */
#define MSDOS_INK_RGB RGB(0x1E, 0x1E, 0x1E)

static inline COLORREF msdos_accent_rgb(void)
{
    return RGB((MSDOSSESSION_ACCENT_RGB >> 16) & 0xff, (MSDOSSESSION_ACCENT_RGB >> 8) & 0xff,
               MSDOSSESSION_ACCENT_RGB & 0xff);
}

static inline int msdos_system_dpi(void)
{
    HDC dc = GetDC(NULL);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return d > 0 ? d : 96;
}

/* GetDpiForWindow is Windows 10 1607 and later: looked up, not imported. */
static inline int msdos_window_dpi(HWND hwnd)
{
    typedef UINT (WINAPI *dpi_fn)(HWND);
    static dpi_fn fn;
    static int looked;
    if (!looked) {
        HMODULE user = GetModuleHandleA("user32.dll");
        fn = user ? (dpi_fn)(void (*)(void))GetProcAddress(user, "GetDpiForWindow") : NULL;
        looked = 1;
    }
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d) return (int)d;
    }
    return msdos_system_dpi();
}

static inline int msdos_scale(int v, int dpi)
{
    return MulDiv(v, dpi, 96);
}

/* The message font at dpi (system metrics come at the system DPI), at its
 * own size (px 0) or px pixels at 96 dpi; face NULL keeps the message
 * font's, otherwise a fixed-pitch face (Consolas). */
static inline HFONT msdos_make_font(int dpi, int px, int weight, int underline, const char *face)
{
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        lf = ncm.lfMessageFont;
    else
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
    if (px > 0)
        lf.lfHeight = -MulDiv(px, dpi, 96);
    else
        lf.lfHeight = -MulDiv(abs(lf.lfHeight ? lf.lfHeight : 12), dpi, msdos_system_dpi());
    lf.lfWidth = 0;
    lf.lfWeight = weight;
    lf.lfUnderline = (BYTE)(underline != 0);
    if (face) {
        snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", face);
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    }
    return CreateFontIndirectA(&lf);
}

/* UTF-8 (the session's text) into a wide buffer. */
static inline void msdos_wide(const char *utf8, wchar_t *out, int outn)
{
    if (outn <= 0) return;
    out[0] = 0;
    if (utf8 && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outn) <= 0)
        out[0] = 0;
    out[outn - 1] = 0;
}

static inline void msdos_set_text_utf8(HWND h, const char *utf8)
{
    wchar_t w[1024];
    msdos_wide(utf8, w, 1024);
    SetWindowTextW(h, w);
}

static inline int msdos_message(HWND owner, const char *text, const char *title, UINT flags)
{
    wchar_t wt[4096], wc[256];
    msdos_wide(text, wt, 4096);
    msdos_wide(title, wc, 256);
    return MessageBoxW(owner, wt, wc, flags);
}
