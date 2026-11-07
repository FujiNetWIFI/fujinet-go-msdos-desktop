/*
 * The debugger's Video tab (Win32).
 *
 * Everything that decides what to show lives in the core (msdosvidtab.h): this file is
 * controls and a GDI picture. The picture is the controller's cached 0xFFRRGGBB buffer drawn
 * with StretchDIBits (BI_RGB ignores the alpha byte), double buffered; a mouse move redraws
 * only the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <commctrl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "msdosvidtab.h"

#define PAGE_CLASS L"FujiNetGoMSDOSVideoPage"
#define PIC_CLASS L"FujiNetGoMSDOSVideoPic"
#define PAL_CLASS L"FujiNetGoMSDOSVideoPal"

#define PAL_CELL 18

enum {
    IDV_VIEW = 3000, IDV_ZOOM, IDV_STATUS, IDV_NOTE, IDV_SEC, IDV_PIXEL, IDV_BREAK, IDV_UNPIN,
    IDV_REGS, IDV_PALD, IDV_MEM, IDV_BEAM, IDV_TIMING, IDV_PIC, IDV_PAL, IDV_LABEL_VIEW,
    IDV_LABEL_ZOOM, IDV_OV0
};

enum { SEC_PIXEL, SEC_REGS, SEC_PALETTE, SEC_MEMORY, SEC_TIMING, SEC_COUNT };

static const int k_zoom_values[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };
static const wchar_t *const k_zoom_labels[] = { L"Fit", L"1×", L"2×", L"3×", L"4×",
                                                L"6×", L"8×", L"12×", L"16×" };
#define NZOOM 9
static const wchar_t *const k_sections[SEC_COUNT] = { L"Pixel", L"Registers", L"Palette", L"Memory", L"Timing" };

typedef struct {
    HWND page, view, zoom, status, note, pic, sec;
    HWND ov[MSDOSVID_O_COUNT];
    HWND label_view, label_zoom;
    HWND pixel, brk, unpin, regs, pal, pal_detail, mem, beam, timing;
    HFONT ui, mono;
    int dpi;
    msdosvid_tab *tab;
    int updating;
    int scroll_x, scroll_y;
    int pal_hover;
    char *regs_sig;
    unsigned char hot[512];     /* per register row: changed lately */
    int nhot;
    BITMAPINFO bmi;
    int tracking;
} vidwin;

#define S(v, n) MulDiv((n), (v)->dpi, 96)

/* ---- text ------------------------------------------------------------------------ */

static wchar_t *wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *w = malloc(((size_t)n * 2 + 2) * sizeof *w), *o;
    const wchar_t *p;
    if (!w) return NULL;
    o = malloc(((size_t)n * 2 + 2) * sizeof *w);
    if (!o) { free(w); return NULL; }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    /* multi-line controls want CR LF */
    {
        wchar_t *d = o;
        for (p = w; *p; p++) {
            if (*p == L'\n') *d++ = L'\r';
            *d++ = *p;
        }
        *d = 0;
    }
    free(w);
    return o;
}

/* a control's text, set only when it changed (no flicker on the live refresh) */
static void set_text(HWND h, const char *utf8)
{
    wchar_t *w = wide(utf8), cur[2048];
    if (!w) return;
    GetWindowTextW(h, cur, 2048);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
    free(w);
}

/* ---- the picture --------------------------------------------------------------------- */

static void pic_update_scroll(vidwin *v)
{
    RECT rc;
    SCROLLINFO si;
    int pw = 0, ph = 0;
    GetClientRect(v->pic, &rc);
    msdosvid_tab_picture(v->tab, &pw, &ph);
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = pw > 0 ? pw - 1 : 0;
    si.nPage = (UINT)rc.right;
    if (v->scroll_x > pw - rc.right) v->scroll_x = pw > rc.right ? pw - rc.right : 0;
    si.nPos = v->scroll_x;
    SetScrollInfo(v->pic, SB_HORZ, &si, TRUE);
    si.nMax = ph > 0 ? ph - 1 : 0;
    si.nPage = (UINT)rc.bottom;
    if (v->scroll_y > ph - rc.bottom) v->scroll_y = ph > rc.bottom ? ph - rc.bottom : 0;
    si.nPos = v->scroll_y;
    SetScrollInfo(v->pic, SB_VERT, &si, TRUE);
}

static void pic_paint(vidwin *v, HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem;
    RECT rc;
    HBITMAP bmp, old;
    int pw = 0, ph = 0, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *px;
    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 24));
    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
    old = SelectObject(mem, bmp);
    FillRect(mem, &rc, bg);
    px = msdosvid_tab_picture(v->tab, &pw, &ph);
    if (!px) {
        const char *why = msdosvid_tab_why(v->tab);
        wchar_t *w = wide(why ? why : "Nothing to show");
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(140, 140, 148));
        SelectObject(mem, v->ui);
        if (w) DrawTextW(mem, w, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        free(w);
    } else {
        HPEN pen_h, pen_p, oldpen;
        HGDIOBJ oldbr;
        v->bmi.bmiHeader.biWidth = pw;
        v->bmi.bmiHeader.biHeight = -ph;     /* top-down */
        StretchDIBits(mem, -v->scroll_x, -v->scroll_y, pw, ph, 0, 0, pw, ph, px, &v->bmi, DIB_RGB_COLORS, SRCCOPY);
        marks = msdosvid_tab_marks(v->tab, hover, pin);
        oldbr = SelectObject(mem, GetStockObject(NULL_BRUSH));
        pen_h = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        pen_p = CreatePen(PS_SOLID, 2, RGB(255, 217, 26));
        if (marks & 1) {
            oldpen = SelectObject(mem, pen_h);
            Rectangle(mem, hover[0] - v->scroll_x, hover[1] - v->scroll_y, hover[0] - v->scroll_x + hover[2],
                      hover[1] - v->scroll_y + hover[3]);
            SelectObject(mem, oldpen);
        }
        if (marks & 2) {
            oldpen = SelectObject(mem, pen_p);
            Rectangle(mem, pin[0] - v->scroll_x + 1, pin[1] - v->scroll_y + 1, pin[0] - v->scroll_x + pin[2] - 1,
                      pin[1] - v->scroll_y + pin[3] - 1);
            SelectObject(mem, oldpen);
        }
        SelectObject(mem, oldbr);
        DeleteObject(pen_h);
        DeleteObject(pen_p);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    DeleteObject(bg);
    EndPaint(h, &ps);
}

static void update_pixel(vidwin *v)
{
    const msdosvid_pixel *p = msdosvid_tab_pixel(v->tab);
    set_text(v->pixel, p ? p->text : "Move over the picture to inspect a pixel.\nClick to pin it; double-click to break on writes to it.");
    if (v->pal) InvalidateRect(v->pal, NULL, FALSE);
}

static void set_note(vidwin *v, const char *text)
{
    set_text(v->note, text);
}

static LRESULT CALLBACK pic_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        pic_paint(v, h);
        return 0;
    case WM_SIZE:
        pic_update_scroll(v);
        return 0;
    case WM_HSCROLL:
    case WM_VSCROLL: {
        SCROLLINFO si;
        int bar = msg == WM_HSCROLL ? SB_HORZ : SB_VERT, pos;
        memset(&si, 0, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = SIF_ALL;
        GetScrollInfo(h, bar, &si);
        pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: pos -= 16; break;
        case SB_LINERIGHT: pos += 16; break;
        case SB_PAGELEFT: pos -= (int)si.nPage; break;
        case SB_PAGERIGHT: pos += (int)si.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: break;
        }
        if (pos < 0) pos = 0;
        if (bar == SB_HORZ) v->scroll_x = pos; else v->scroll_y = pos;
        pic_update_scroll(v);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        v->scroll_y -= GET_WHEEL_DELTA_WPARAM(wp) / 3;
        if (v->scroll_y < 0) v->scroll_y = 0;
        pic_update_scroll(v);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp) + v->scroll_x, y = (short)HIWORD(lp) + v->scroll_y;
        if (!v->tracking) {
            TRACKMOUSEEVENT t;
            memset(&t, 0, sizeof t);
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            TrackMouseEvent(&t);
            v->tracking = 1;
        }
        if (msdosvid_tab_hover(v->tab, x, y)) {
            InvalidateRect(h, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        v->tracking = 0;
        if (msdosvid_tab_hover(v->tab, -1, -1)) {
            InvalidateRect(h, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        msdosvid_tab_pin(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        InvalidateRect(h, NULL, FALSE);
        update_pixel(v);
        return 0;
    case WM_LBUTTONDBLCLK: {
        int id;
        msdosvid_tab_hover(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        id = msdosvid_tab_break_on_pixel(v->tab);
        set_note(v, id > 0 ? "Breakpoint added: break on writes to that address (see Breakpoints)"
                           : "That pixel has no CPU address to break on");
        InvalidateRect(h, NULL, FALSE);
        update_pixel(v);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- the palette ---------------------------------------------------------------------- */

static LRESULT CALLBACK pal_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        const msdosvid_pal_entry *e;
        int n = msdosvid_tab_palette(v->tab, &e), i, cols = n > 16 ? 16 : 8, sel = msdosvid_tab_pixel_palette_index(v->tab);
        HBRUSH bg = GetSysColorBrush(COLOR_BTNFACE);
        GetClientRect(h, &rc);
        FillRect(dc, &rc, bg);
        for (i = 0; i < n; i++) {
            RECT r;
            HBRUSH b = CreateSolidBrush(RGB((e[i].rgb >> 16) & 255, (e[i].rgb >> 8) & 255, e[i].rgb & 255));
            r.left = S(v, (i % cols) * (PAL_CELL + 2));
            r.top = S(v, (i / cols) * (PAL_CELL + 2));
            r.right = r.left + S(v, PAL_CELL);
            r.bottom = r.top + S(v, PAL_CELL);
            FillRect(dc, &r, b);
            DeleteObject(b);
            if (!e[i].active) {
                HBRUSH dim = CreateHatchBrush(HS_DIAGCROSS, RGB(0, 0, 0));
                SetBkMode(dc, TRANSPARENT);
                FillRect(dc, &r, dim);
                DeleteObject(dim);
            }
            if (e[i].age > 0 && e[i].age < MSDOSVID_REG_FADE) {
                HBRUSH hb = CreateSolidBrush(RGB(255, 77, 230));
                FrameRect(dc, &r, hb);
                DeleteObject(hb);
            }
            if (i == sel || i == v->pal_hover) {
                RECT o = r;
                HBRUSH wb = CreateSolidBrush(RGB(0, 0, 0));
                InflateRect(&o, 1, 1);
                FrameRect(dc, &o, wb);
                DeleteObject(wb);
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const msdosvid_pal_entry *e;
        int n = msdosvid_tab_palette(v->tab, &e), cols = n > 16 ? 16 : 8, cw = S(v, PAL_CELL + 2);
        int i = ((short)HIWORD(lp) / cw) * cols + (short)LOWORD(lp) / cw;
        char buf[256];
        if (i < 0 || i >= n || (short)LOWORD(lp) < 0 || (short)HIWORD(lp) < 0) i = -1;
        if (i != v->pal_hover) {
            v->pal_hover = i;
            InvalidateRect(h, NULL, FALSE);
        }
        if (i < 0) {
            set_text(v->pal_detail, "");
            return 0;
        }
        if (e[i].used >= 0)
            snprintf(buf, sizeof buf, "#%06X  %s\nused by %d pixels%s", e[i].rgb, e[i].detail, e[i].used,
                     e[i].active ? "" : "  (this mode cannot reach it)");
        else
            snprintf(buf, sizeof buf, "#%06X  %s\nused by ?%s", e[i].rgb, e[i].detail,
                     e[i].active ? "" : "  (this mode cannot reach it)");
        set_text(v->pal_detail, buf);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- sections ----------------------------------------------------------------------- */

static void fill_regs(vidwin *v)
{
    const msdosvid_reg *r;
    int n = msdosvid_tab_regs(v->tab, &r), i;
    size_t cap = (size_t)n * 24 + 16, len = 0;
    char *sig = malloc(cap + 1);
    if (!sig) return;
    for (i = 0; i < n; i++)
        len += (size_t)snprintf(sig + len, cap - len, "%x%c ", r[i].value, (r[i].age > 0 && r[i].age < MSDOSVID_REG_FADE) ? '*' : '.');
    if (v->regs_sig && !strcmp(v->regs_sig, sig)) {
        free(sig);
        return;
    }
    free(v->regs_sig);
    v->regs_sig = sig;
    SendMessageW(v->regs, WM_SETREDRAW, FALSE, 0);
    SendMessageW(v->regs, LVM_DELETEALLITEMS, 0, 0);
    v->nhot = n < (int)sizeof v->hot ? n : (int)sizeof v->hot;
    for (i = 0; i < n; i++) {
        char id[48], val[16];
        LVITEMW it;
        wchar_t *w;
        if (r[i].port && r[i].index < 0) snprintf(id, sizeof id, "%s %03X", r[i].group, r[i].port);
        else snprintf(id, sizeof id, "%s %02X", r[i].group, r[i].index);
        snprintf(val, sizeof val, "%0*X", r[i].digits, r[i].value);
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = i;
        w = wide(id);
        it.pszText = w;
        SendMessageW(v->regs, LVM_INSERTITEMW, 0, (LPARAM)&it);
        free(w);
        {
            const char *cols[3] = { r[i].name, val, r[i].meaning };
            int c;
            for (c = 0; c < 3; c++) {
                LVITEMW sub;
                memset(&sub, 0, sizeof sub);
                w = wide(cols[c]);
                sub.iSubItem = c + 1;
                sub.pszText = w;
                SendMessageW(v->regs, LVM_SETITEMTEXTW, (WPARAM)i, (LPARAM)&sub);
                free(w);
            }
        }
        if (i < (int)sizeof v->hot) v->hot[i] = (r[i].age > 0 && r[i].age < MSDOSVID_REG_FADE);
    }
    SendMessageW(v->regs, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(v->regs, NULL, TRUE);
}

static void fill_memory(vidwin *v)
{
    const msdosvid_region *r;
    int n = msdosvid_tab_regions(v->tab, &r), i;
    const msdos_vid_snap *sn = msdosvid_tab_snap(v->tab);
    char buf[2048];
    size_t len = 0;
    if (!n) len += (size_t)snprintf(buf + len, sizeof buf - len, "No video memory");
    for (i = 0; i < n; i++) {
        len += (size_t)snprintf(buf + len, sizeof buf - len, "%s\n     VRAM %05X + %05X", r[i].name, r[i].vram_off, r[i].size);
        if (r[i].cpu_addr != MSDOS_VID_NONE) len += (size_t)snprintf(buf + len, sizeof buf - len, "   CPU %05X", r[i].cpu_addr);
        len += (size_t)snprintf(buf + len, sizeof buf - len, "\n");
    }
    if (n)
        snprintf(buf + len, sizeof buf - len, "\n%u KB of VRAM%s; the CPU sees it at %05X-%05X.\nSelect the Memory map view to see it all.",
                 sn->vram_size / 1024, sn->planar ? ", four planes" : "", sn->cpu_base,
                 sn->cpu_base + (sn->cpu_size ? sn->cpu_size - 1 : 0));
    set_text(v->mem, buf);
}

static void fill_timing(vidwin *v)
{
    const char *const *lines;
    int n = msdosvid_tab_timing(v->tab, &lines), i;
    char buf[2048];
    size_t len = 0;
    for (i = 0; i < n; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab) len += (size_t)snprintf(buf + len, sizeof buf - len, "%-13.*s %s\n", (int)(tab - lines[i]), lines[i], tab + 1);
        else len += (size_t)snprintf(buf + len, sizeof buf - len, "%s\n", lines[i]);
    }
    if (!n) snprintf(buf, sizeof buf, "The PC is off");
    set_text(v->timing, buf);
    SendMessageW(v->beam, PBM_SETPOS, (WPARAM)msdosvid_tab_beam_permille(v->tab), 0);
}

static void show_section(vidwin *v)
{
    int s = (int)SendMessageW(v->sec, TCM_GETCURSEL, 0, 0);
    ShowWindow(v->pixel, s == SEC_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->brk, s == SEC_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->unpin, s == SEC_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->regs, s == SEC_REGS ? SW_SHOW : SW_HIDE);
    ShowWindow(v->pal, s == SEC_PALETTE ? SW_SHOW : SW_HIDE);
    ShowWindow(v->pal_detail, s == SEC_PALETTE ? SW_SHOW : SW_HIDE);
    ShowWindow(v->mem, s == SEC_MEMORY ? SW_SHOW : SW_HIDE);
    ShowWindow(v->beam, s == SEC_TIMING ? SW_SHOW : SW_HIDE);
    ShowWindow(v->timing, s == SEC_TIMING ? SW_SHOW : SW_HIDE);
}

/* ---- refresh -------------------------------------------------------------------------- */

static void refresh_view(vidwin *v, unsigned what)
{
    if (what & MSDOSVID_TAB_PICTURE) {
        RECT rc;
        GetClientRect(v->pic, &rc);
        if (rc.right > 4 && rc.bottom > 4) msdosvid_tab_set_viewport(v->tab, rc.right - 2, rc.bottom - 2);
        v->scroll_x = v->scroll_y = (what & 0x8000) ? 0 : v->scroll_x;
        pic_update_scroll(v);
        InvalidateRect(v->pic, NULL, FALSE);
    }
    if (what & MSDOSVID_TAB_TABLES) {
        int s = (int)SendMessageW(v->sec, TCM_GETCURSEL, 0, 0);
        set_text(v->status, msdosvid_tab_status(v->tab));
        switch (s) {
        case SEC_REGS: fill_regs(v); break;
        case SEC_PALETTE:
            InvalidateRect(v->pal, NULL, FALSE);
            break;
        case SEC_MEMORY: fill_memory(v); break;
        case SEC_TIMING: fill_timing(v); break;
        default: break;
        }
        update_pixel(v);
    }
}

void dbg_video_refresh(HWND page, int force)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    unsigned what;
    if (!v) return;
    what = msdosvid_tab_refresh(v->tab, force);
    if (force) what |= MSDOSVID_TAB_PICTURE | MSDOSVID_TAB_TABLES;
    refresh_view(v, what);
}

/* ---- layout --------------------------------------------------------------------------- */

static int text_w(vidwin *v, HFONT f, const wchar_t *s)
{
    HDC dc = GetDC(v->page);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz;
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, old);
    ReleaseDC(v->page, dc);
    return sz.cx;
}

static void layout(vidwin *v)
{
    RECT rc;
    int W, H, rw, lw, x, y, i, n, row_h = S(v, 24), lh = S(v, 18), gap = S(v, 6);
    const msdosvid_overlay_def *od = msdosvid_overlays(&n);
    int right_x, tab_h;
    RECT tr;
    GetClientRect(v->page, &rc);
    W = rc.right;
    H = rc.bottom;
    if (W < 100 || H < 100) return;
    rw = S(v, 390);
    if (rw > W * 45 / 100) rw = W * 45 / 100;
    lw = W - rw - gap;
    right_x = lw + gap;

    /* left: View/Zoom, overlays (flowing), status, picture, note */
    x = 0;
    y = 0;
    MoveWindow(v->label_view, x, y + (row_h - lh) / 2, S(v, 36), lh, TRUE);
    x += S(v, 36);
    MoveWindow(v->view, x, y, S(v, 210), S(v, 300), TRUE);
    x += S(v, 210) + gap * 2;
    MoveWindow(v->label_zoom, x, y + (row_h - lh) / 2, S(v, 40), lh, TRUE);
    x += S(v, 40);
    MoveWindow(v->zoom, x, y, S(v, 80), S(v, 300), TRUE);
    y += row_h + gap;
    x = 0;
    for (i = 0; i < n; i++) {
        wchar_t *w = wide(od[i].label);
        int bw = text_w(v, v->ui, w ? w : L"") + S(v, 26);
        free(w);
        if (x + bw > lw && x > 0) {
            x = 0;
            y += row_h - S(v, 2);
        }
        MoveWindow(v->ov[i], x, y, bw, row_h - S(v, 4), TRUE);
        x += bw + gap;
    }
    y += row_h;
    MoveWindow(v->status, 0, y, lw, lh * 2 + S(v, 4), TRUE);
    y += lh * 2 + S(v, 8);
    MoveWindow(v->pic, 0, y, lw, H - y - lh - gap > 20 ? H - y - lh - gap : 20, TRUE);
    MoveWindow(v->note, 0, H - lh, lw, lh, TRUE);

    /* right: the section strip and its page */
    MoveWindow(v->sec, right_x, 0, rw, H, TRUE);
    SetRect(&tr, 0, 0, rw, H);
    SendMessageW(v->sec, TCM_ADJUSTRECT, FALSE, (LPARAM)&tr);
    tab_h = tr.top;
    {
        int cx = right_x + S(v, 8), cy = tab_h + S(v, 8), cw = rw - S(v, 16), ch = H - tab_h - S(v, 16);
        MoveWindow(v->pixel, cx, cy, cw, S(v, 150), TRUE);
        MoveWindow(v->brk, cx, cy + S(v, 160), S(v, 150), S(v, 26), TRUE);
        MoveWindow(v->unpin, cx + S(v, 160), cy + S(v, 160), S(v, 80), S(v, 26), TRUE);
        MoveWindow(v->regs, cx, cy, cw, ch, TRUE);
        MoveWindow(v->pal, cx, cy, S(v, 16 * (PAL_CELL + 2)), S(v, 16 * (PAL_CELL + 2)), TRUE);
        MoveWindow(v->pal_detail, cx, cy + S(v, 16 * (PAL_CELL + 2) + 8), cw, S(v, 48), TRUE);
        MoveWindow(v->mem, cx, cy, cw, ch, TRUE);
        MoveWindow(v->beam, cx, cy, cw, S(v, 20), TRUE);
        MoveWindow(v->timing, cx, cy + S(v, 30), cw, ch - S(v, 30), TRUE);
        {
            HWND hdr = (HWND)SendMessageW(v->regs, LVM_GETHEADER, 0, 0);
            (void)hdr;
            SendMessageW(v->regs, LVM_SETCOLUMNWIDTH, 0, S(v, 84));
            SendMessageW(v->regs, LVM_SETCOLUMNWIDTH, 1, S(v, 150));
            SendMessageW(v->regs, LVM_SETCOLUMNWIDTH, 2, S(v, 50));
            SendMessageW(v->regs, LVM_SETCOLUMNWIDTH, 3, S(v, 300));
        }
    }
    pic_update_scroll(v);
}

/* ---- the page -------------------------------------------------------------------------- */

static HWND mk(vidwin *v, const wchar_t *cls, const wchar_t *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, v->page, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrW(v->page, GWLP_HINSTANCE), NULL);
    if (font) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static void on_overlay(vidwin *v)
{
    unsigned m = 0;
    int n, i;
    const msdosvid_overlay_def *od = msdosvid_overlays(&n);
    for (i = 0; i < n; i++)
        if (SendMessageW(v->ov[i], BM_GETCHECK, 0, 0) == BST_CHECKED) m |= od[i].bit;
    msdosvid_tab_set_overlays(v->tab, m);
    refresh_view(v, MSDOSVID_TAB_PICTURE);
}

static LRESULT CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_SIZE:
        layout(v);
        refresh_view(v, MSDOSVID_TAB_PICTURE);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (v->updating) return 0;
        if (id == IDV_VIEW && code == CBN_SELCHANGE) {
            msdosvid_tab_set_view(v->tab, (int)SendMessageW(v->view, CB_GETCURSEL, 0, 0));
            refresh_view(v, MSDOSVID_TAB_PICTURE | MSDOSVID_TAB_TABLES | 0x8000);
        } else if (id == IDV_ZOOM && code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(v->zoom, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < NZOOM) msdosvid_tab_set_zoom(v->tab, k_zoom_values[i]);
            refresh_view(v, MSDOSVID_TAB_PICTURE);
        } else if (id >= IDV_OV0 && id < IDV_OV0 + MSDOSVID_O_COUNT && code == BN_CLICKED) {
            on_overlay(v);
        } else if (id == IDV_BREAK && code == BN_CLICKED) {
            int b = msdosvid_tab_break_on_pixel(v->tab);
            set_note(v, b > 0 ? "Breakpoint added: break on writes to that address (see Breakpoints)"
                              : "Pin a pixel that has a CPU address first");
        } else if (id == IDV_UNPIN && code == BN_CLICKED) {
            msdosvid_tab_unpin(v->tab);
            InvalidateRect(v->pic, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDV_SEC && nh->code == TCN_SELCHANGE) {
            show_section(v);
            refresh_view(v, MSDOSVID_TAB_TABLES);
        } else if (nh->idFrom == IDV_REGS && nh->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int i = (int)cd->nmcd.dwItemSpec;
                if (i >= 0 && i < v->nhot && v->hot[i]) {
                    cd->clrTextBk = RGB(122, 42, 106);
                    cd->clrText = RGB(255, 255, 255);
                }
                return CDRF_DODEFAULT;
            }
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        /* the read-only labels sit on the page colour */
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_DESTROY:
        msdosvid_tab_free(v->tab);
        free(v->regs_sig);
        free(v);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void register_classes(HINSTANCE inst)
{
    static int done;
    WNDCLASSW wc;
    if (done) return;
    done = 1;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = page_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = PAGE_CLASS;
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = pic_proc;
    wc.lpszClassName = PIC_CLASS;
    wc.hbrBackground = NULL;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    RegisterClassW(&wc);
    wc.lpfnWndProc = pal_proc;
    wc.lpszClassName = PAL_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
}

HWND dbg_video_create(HWND parent, HFONT ui, HFONT mono, int dpi, msdossession *session)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    vidwin *v = calloc(1, sizeof *v);
    int n, i;
    const msdosvid_overlay_def *od;
    LVCOLUMNW col;
    TCITEMW item;
    if (!v) return NULL;
    register_classes(inst);
    v->ui = ui;
    v->mono = mono;
    v->dpi = dpi > 0 ? dpi : 96;
    v->pal_hover = -1;
    v->tab = msdosvid_tab_new(session);
    memset(&v->bmi, 0, sizeof v->bmi);
    v->bmi.bmiHeader.biSize = sizeof v->bmi.bmiHeader;
    v->bmi.bmiHeader.biPlanes = 1;
    v->bmi.bmiHeader.biBitCount = 32;
    v->bmi.bmiHeader.biCompression = BI_RGB;
    v->page = CreateWindowExW(0, PAGE_CLASS, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, NULL, inst, NULL);
    SetWindowLongPtrW(v->page, GWLP_USERDATA, (LONG_PTR)v);

    v->label_view = mk(v, L"STATIC", L"View", SS_LEFT, IDV_LABEL_VIEW, ui);
    v->view = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_VIEW, ui);
    for (i = 0; i < MSDOSVID_V_COUNT; i++) {
        wchar_t *w = wide(msdosvid_view_name(i));
        SendMessageW(v->view, CB_ADDSTRING, 0, (LPARAM)w);
        free(w);
    }
    v->label_zoom = mk(v, L"STATIC", L"Zoom", SS_LEFT, IDV_LABEL_ZOOM, ui);
    v->zoom = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_ZOOM, ui);
    for (i = 0; i < NZOOM; i++) SendMessageW(v->zoom, CB_ADDSTRING, 0, (LPARAM)k_zoom_labels[i]);
    od = msdosvid_overlays(&n);
    for (i = 0; i < n; i++) {
        wchar_t *w = wide(od[i].label);
        v->ov[i] = mk(v, L"BUTTON", w, BS_AUTOCHECKBOX | WS_TABSTOP, IDV_OV0 + i, ui);
        free(w);
    }
    v->status = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_STATUS, mono);
    v->pic = CreateWindowExW(WS_EX_CLIENTEDGE, PIC_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL, 0, 0, 10, 10,
                             v->page, (HMENU)(INT_PTR)IDV_PIC, inst, NULL);
    SetWindowLongPtrW(v->pic, GWLP_USERDATA, (LONG_PTR)v);
    v->note = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_NOTE, ui);

    v->sec = mk(v, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, IDV_SEC, ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < SEC_COUNT; i++) {
        item.pszText = (LPWSTR)k_sections[i];
        SendMessageW(v->sec, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
    }
    SetWindowPos(v->sec, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   /* under the section controls */
    v->pixel = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_PIXEL, mono);
    v->brk = mk(v, L"BUTTON", L"Break on its writes", BS_PUSHBUTTON | WS_TABSTOP, IDV_BREAK, ui);
    v->unpin = mk(v, L"BUTTON", L"Unpin", BS_PUSHBUTTON | WS_TABSTOP, IDV_UNPIN, ui);
    v->regs = mk(v, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                 IDV_REGS, mono);
    SendMessageW(v->regs, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    {
        static const wchar_t *const names[4] = { L"Reg", L"Name", L"Value", L"Meaning" };
        for (i = 0; i < 4; i++) {
            col.pszText = (LPWSTR)names[i];
            col.cx = 100;
            SendMessageW(v->regs, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
        }
    }
    v->pal = CreateWindowExW(0, PAL_CLASS, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, v->page, (HMENU)(INT_PTR)IDV_PAL, inst, NULL);
    SetWindowLongPtrW(v->pal, GWLP_USERDATA, (LONG_PTR)v);
    v->pal_detail = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_PALD, mono);
    v->mem = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_MEM, mono);
    v->beam = mk(v, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDV_BEAM, NULL);
    SendMessageW(v->beam, PBM_SETRANGE32, 0, 1000);
    v->timing = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_TIMING, mono);

    /* start from the controller's state (the MSDOS_DEBUGGER_VIDEO_* variables) */
    v->updating = 1;
    SendMessageW(v->view, CB_SETCURSEL, (WPARAM)msdosvid_tab_view(v->tab), 0);
    for (i = 0; i < NZOOM; i++)
        if (k_zoom_values[i] == msdosvid_tab_zoom(v->tab)) SendMessageW(v->zoom, CB_SETCURSEL, (WPARAM)i, 0);
    for (i = 0; i < n; i++)
        SendMessageW(v->ov[i], BM_SETCHECK, (msdosvid_tab_overlays(v->tab) & od[i].bit) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(v->sec, TCM_SETCURSEL, (WPARAM)msdosvid_tab_env_section(v->tab), 0);
    v->updating = 0;
    show_section(v);
    return v->page;
}

void dbg_video_place(HWND page, int x, int y, int w, int h)
{
    MoveWindow(page, x, y, w > 1 ? w : 1, h > 1 ? h : 1, TRUE);
}
