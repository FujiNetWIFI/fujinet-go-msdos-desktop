/*
 * prefs.c -- the Win32 Preferences window, built from msdossession_pref like
 * the other frontends' Preferences: a tab per page, a heading per group, a
 * row per preference, and a ROMs tab listing what each machine and card
 * needs, imported or not.
 *
 * Rows that apply LIVE (the picture, the volume) take effect at once. The
 * machine -- model, processor, memory, cards, drives, ports -- is the
 * computer you would switch off to change: those rows wait, and closing the
 * window applies them together with one reset. Rows follow each other as
 * they change: processors and memory by machine, cards by what the machine
 * can take and what has been imported.
 *
 * The tab control is its strip only; the pages sit on the window's own face
 * below it (a themed tab pane is lighter than the dialog face, and captions
 * drawn over it show bands). Everything is laid out in 96-dpi units and
 * rebuilt at a new DPI.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include <commctrl.h>
#include <shlobj.h>

#include <string.h>

#include "controllers/controller_window.h"
#include "help/help_window.h"

#define MAX_ROWS 128
#define MAX_PAGES 10
#define MAX_ROM_ROWS 64

/* Sizes at 96 dpi */
#define PAGE_W 600
#define MARGIN 12
#define ROW_H 30
#define HELP_H 18
#define HEAD_H 30
#define LABEL_X 24
#define CTRL_X 230
#define CTRL_W 340
#define MIN_PAGE_H 420
#define BUTTONS_H 40            /* the Help button's row */

#define ID_TABS 3000
#define ID_ROM_LIST 3001
#define ID_ROM_IMPORT 3002
#define ID_ROM_FOLDER 3003
#define ID_HELP 3004
#define ID_ROW 3100             /* + pref */
#define DIM_TAG 1               /* GWLP_USERDATA of a grey caption */

#define PREFS_CLASS "FujiNetGoMSDOSPrefs"
#define PAGE_CLASS "FujiNetGoMSDOSPrefsPage"
#define PREFS_STYLE (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN)

typedef struct {
    HWND label, ctrl, updown, help;
    char labels[4096];          /* a choice row's labels, as shown */
} Row;

static HWND g_win, g_parent, g_tabs, g_pages[MAX_PAGES], g_rom_list;
static const char *g_page_names[MAX_PAGES];
static int g_page_count, g_page;
static Row g_rows[MAX_ROWS];
static msdossession *g_s;
static int g_syncing, g_pending, g_abandon, g_dpi = 96;
static void (*g_on_live)(void);
static void (*g_on_closed)(int);
static HFONT g_font, g_bold;

static void refresh(void);

static int S(int v)
{
    return msdos_scale(v, g_dpi);
}

/* A change to pref i: LIVE ones apply now unless something heavier waits
 * (applying then would reset mid-edit). */
static void changed(int i)
{
    const msdos_pref *p = msdossession_pref(i);
    if (p->applies > g_pending) g_pending = p->applies;
    if (g_pending == MSDOS_APPLY_LIVE) msdossession_apply_settings(g_s);
    if (p->applies == MSDOS_APPLY_LIVE && g_on_live) g_on_live();
    if (!strcmp(p->key, "machine")) msdos_controllers_refresh();
    refresh();
}

/* A choice row's labels joined, to see when they change. */
static void choice_labels(int i, char *dst, size_t dstsz)
{
    const char *l;
    size_t len = 0;
    int c;
    dst[0] = '\0';
    for (c = 0; (l = msdossession_pref_choice(g_s, i, c)) != NULL && len + 1 < dstsz; c++) {
        int n = snprintf(dst + len, dstsz - len, "%s%s\n", l,
                         msdossession_pref_choice_enabled(g_s, i, c) ? "" : " (not available)");
        if (n < 0) break;
        len += (size_t)n < dstsz - len ? (size_t)n : dstsz - len - 1;
    }
}

/* UTF-8 into a combo box (the labels have dashes and such in them). */
static void combo_add_utf8(HWND combo, const char *text)
{
    wchar_t w[256];
    msdos_wide(text, w, 256);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)w);
}

static void lv_set(HWND lv, int row, int col, const char *text)
{
    LVITEMA it;
    memset(&it, 0, sizeof it);
    it.iSubItem = col;
    it.pszText = (char *)text;
    SendMessageA(lv, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&it);
}

/* Every row from the store: values, labels that depend on others, and which
 * rows apply to this machine. */
static void refresh(void)
{
    int i, r;
    if (!g_win) return;
    g_syncing = 1;
    for (i = 0; i < msdossession_pref_count() && i < MAX_ROWS; i++) {
        const msdos_pref *p = msdossession_pref(i);
        Row *row = &g_rows[i];
        const int on = msdossession_pref_enabled(g_s, i) != 0;
        if (!row->ctrl) continue;
        EnableWindow(row->ctrl, on);
        if (row->label) EnableWindow(row->label, on);
        if (row->updown) EnableWindow(row->updown, on);
        switch (p->type) {
        case MSDOS_PREF_BOOL:
            SendMessageA(row->ctrl, BM_SETCHECK,
                         msdossession_pref_get_int(g_s, i) ? BST_CHECKED : BST_UNCHECKED, 0);
            break;
        case MSDOS_PREF_INT: {
            int lo = p->min, hi = p->max, step = p->step;
            char v[24], cur[24];
            UDACCEL acc;
            msdossession_pref_int_range(g_s, i, &lo, &hi, &step);
            SendMessageA(row->updown, UDM_SETRANGE32, (WPARAM)lo, (LPARAM)hi);
            acc.nSec = 0;
            acc.nInc = (UINT)(step > 0 ? step : 1);
            SendMessageA(row->updown, UDM_SETACCEL, 1, (LPARAM)&acc);
            /* not while it is being typed into */
            if (GetFocus() != row->ctrl) {
                snprintf(v, sizeof v, "%d", msdossession_pref_get_int(g_s, i));
                GetWindowTextA(row->ctrl, cur, sizeof cur);
                if (strcmp(v, cur) != 0) SetWindowTextA(row->ctrl, v);
            }
            break;
        }
        case MSDOS_PREF_CHOICE: {
            static char labels[4096];
            const int sel = msdossession_pref_get_choice(g_s, i);
            choice_labels(i, labels, sizeof labels);
            if (strcmp(labels, row->labels) != 0) {
                char *line = labels, *nl;
                SendMessageA(row->ctrl, CB_RESETCONTENT, 0, 0);
                snprintf(row->labels, sizeof row->labels, "%s", labels);
                while ((nl = strchr(line, '\n')) != NULL) {
                    *nl = '\0';
                    combo_add_utf8(row->ctrl, line);
                    line = nl + 1;
                }
            }
            SendMessageA(row->ctrl, CB_SETCURSEL, (WPARAM)(sel >= 0 ? sel : 0), 0);
            break;
        }
        default:
            break;
        }
    }
    for (r = 0; g_rom_list && r < msdossession_rom_item_count() && r < MAX_ROM_ROWS; r++)
        lv_set(g_rom_list, r, 2, msdossession_rom_item_available(g_s, r) ? "Imported" : "Not imported");
    g_syncing = 0;
}

/* ---- the ROMs page --------------------------------------------------------------------- */

static void import_roms(int folder)
{
    char path[MAX_PATH] = "", msg[2048];
    if (folder) {
        BROWSEINFOA bi;
        PIDLIST_ABSOLUTE pidl;
        memset(&bi, 0, sizeof bi);
        bi.hwndOwner = g_win;
        bi.lpszTitle = "Import BIOS from a Folder (an 86Box ROM set works)";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        pidl = SHBrowseForFolderA(&bi);
        if (!pidl) return;
        if (!SHGetPathFromIDListA(pidl, path)) path[0] = '\0';
        CoTaskMemFree(pidl);
    } else {
        OPENFILENAMEA ofn;
        memset(&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = g_win;
        ofn.lpstrFilter = "BIOS images and ROM sets\0*.zip;*.rom;*.bin;*.awd;*.bio;*.vbi;*.raw\0"
                          "All files\0*.*\0\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = sizeof path;
        ofn.lpstrTitle = "Import BIOS";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameA(&ofn)) return;
    }
    if (!path[0]) return;
    msg[0] = '\0';
    if (msdossession_rom_import(g_s, path, msg, sizeof msg) == 0)
        msdos_message(g_win, msg[0] ? msg : "Imported", "BIOS Imported", MB_ICONINFORMATION | MB_OK);
    else
        msdos_message(g_win, msg[0] ? msg : "Nothing in it is a ROM this app knows.", "Nothing Imported",
                      MB_ICONWARNING | MB_OK);
    /* the machines and cards it unlocks become choosable; the main window's
     * page and menus follow */
    if (g_on_live) g_on_live();
    refresh();
}

/* ---- the window -------------------------------------------------------------------------- */

static void show_page(int n)
{
    char ctx[48];
    int i;
    g_page = n;
    for (i = 0; i < g_page_count; i++)
        ShowWindow(g_pages[i], i == n ? SW_SHOW : SW_HIDE);
    /* Help, F1 and Ctrl+Shift+H open the page's own */
    snprintf(ctx, sizeof ctx, "prefs/%s", n >= 0 && n < MAX_PAGES && g_page_names[n] ? g_page_names[n] : "Machine");
    msdos_help_set_context(g_win, ctx, 1);
}

/* The pages are children of the window (not of the tab control), so their
 * controls' notifications go up to it. */
static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND || msg == WM_NOTIFY || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN ||
        msg == WM_VSCROLL)
        return SendMessageA(g_win, msg, wp, lp);
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void on_row(int i, int code)
{
    const msdos_pref *p = msdossession_pref(i);
    Row *row = &g_rows[i];
    switch (p->type) {
    case MSDOS_PREF_BOOL:
        if (code == BN_CLICKED) {
            msdossession_pref_set_int(g_s, i, SendMessageA(row->ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED);
            changed(i);
        }
        break;
    case MSDOS_PREF_INT:
        if (code == EN_CHANGE) {
            int lo = p->min, hi = p->max, step = p->step;
            char v[24], *end;
            long n;
            msdossession_pref_int_range(g_s, i, &lo, &hi, &step);
            GetWindowTextA(row->ctrl, v, sizeof v);
            n = strtol(v, &end, 10);
            if (end != v && *end == '\0' && n >= lo && n <= hi && n != msdossession_pref_get_int(g_s, i)) {
                msdossession_pref_set_int(g_s, i, (int)n);
                changed(i);
            }
        } else if (code == EN_KILLFOCUS) {
            /* what the store took, back in the box */
            refresh();
        }
        break;
    case MSDOS_PREF_CHOICE:
        if (code == CBN_SELCHANGE) {
            const int c = (int)SendMessageA(row->ctrl, CB_GETCURSEL, 0, 0);
            if (c < 0) break;
            if (!msdossession_pref_choice_enabled(g_s, i, c)) {
                /* not imported, or not on this machine: put it back */
                msdos_message(g_win, "Not available \xe2\x80\x94 import its ROM first (the ROMs tab), or "
                              "choose a machine that takes it.", "Preferences", MB_ICONINFORMATION | MB_OK);
                refresh();
                break;
            }
            msdossession_pref_set_choice(g_s, i, c);
            changed(i);
        }
        break;
    default:
        break;
    }
}

static void build(void);

static LRESULT CALLBACK prefs_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NOTIFY: {
        const NMHDR *n = (const NMHDR *)lp;
        if (n->idFrom == ID_TABS && n->code == (UINT)TCN_SELCHANGE)
            show_page((int)SendMessageA(g_tabs, TCM_GETCURSEL, 0, 0));
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        /* the pages are plain windows on the dialog face */
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        if (GetWindowLongPtrA((HWND)lp, GWLP_USERDATA) == DIM_TAG)
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_ROM_IMPORT) { import_roms(0); return 0; }
        if (id == ID_ROM_FOLDER) { import_roms(1); return 0; }
        if (id == ID_HELP) { msdos_help_for(hwnd); return 0; }
        if (id == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        if (id < ID_ROW || g_syncing) return 0;
        if (id - ID_ROW < msdossession_pref_count() && id - ID_ROW < MAX_ROWS) on_row(id - ID_ROW, code);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        g_dpi = HIWORD(wp);
        SetWindowPos(hwnd, NULL, r->left, r->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        build();    /* sizes the window itself */
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        int did = 0;
        g_win = NULL;
        g_tabs = g_rom_list = NULL;
        msdossession_settings_flush(g_s);
        if (!g_abandon) {
            did = g_pending > MSDOS_APPLY_LIVE ? msdossession_apply_settings(g_s) : 0;
            if (g_on_closed) g_on_closed(did);
        }
        if (g_font) DeleteObject(g_font);
        if (g_bold) DeleteObject(g_bold);
        g_font = g_bold = NULL;
        if (g_parent && IsWindow(g_parent) && !g_abandon) SetForegroundWindow(g_parent);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND child(HWND parent, DWORD ex, const char *cls, const char *text, DWORD style, int x, int y,
                  int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

/* UTF-8 text (the table's help has dashes): a wide STATIC. */
static HWND label_utf8(HWND parent, const char *text, DWORD style, int x, int y, int w, int h, HFONT font,
                       int dim)
{
    wchar_t wt[512];
    HWND c;
    msdos_wide(text, wt, 512);
    c = CreateWindowExW(0, L"STATIC", wt, WS_CHILD | WS_VISIBLE | SS_NOPREFIX | style, x, y, w, h, parent,
                        NULL, GetModuleHandleA(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    if (dim) SetWindowLongPtrA(c, GWLP_USERDATA, DIM_TAG);
    return c;
}

static HWND make_page(const RECT *area)
{
    return CreateWindowExA(WS_EX_CONTROLPARENT, PAGE_CLASS, "", WS_CHILD | WS_CLIPCHILDREN, area->left,
                           area->top, area->right - area->left, area->bottom - area->top, g_win, NULL,
                           GetModuleHandleA(NULL), NULL);
}

static void add_tab(const char *text)
{
    TCITEMA tab;
    memset(&tab, 0, sizeof tab);
    tab.mask = TCIF_TEXT;
    tab.pszText = (char *)text;
    SendMessageA(g_tabs, TCM_INSERTITEMA, (WPARAM)g_page_count, (LPARAM)&tab);
}

static void build_roms_page(HWND page, int h)
{
    LVCOLUMNA col;
    char note[1400];
    const int bw = S(140), bh = S(28);
    int r, y = S(MARGIN);
    snprintf(note, sizeof note,
             "The machines' BIOS chips and the cards' ROMs are not part of the app. Import them from "
             "files, a zip or a folder (an 86Box ROM set works); they are recognised by their "
             "checksums. Kept in %s.", msdossession_roms_path(g_s));
    label_utf8(page, note, SS_LEFT, S(MARGIN), y, S(PAGE_W) - 2 * S(MARGIN), S(64), g_font, 1);
    y += S(70);
    child(page, 0, "BUTTON", "Import...", BS_PUSHBUTTON | WS_TABSTOP, S(MARGIN), y, bw, bh, ID_ROM_IMPORT,
          g_font);
    child(page, 0, "BUTTON", "Import a Folder...", BS_PUSHBUTTON | WS_TABSTOP, S(MARGIN) + bw + S(8), y, bw,
          bh, ID_ROM_FOLDER, g_font);
    y += bh + S(10);
    g_rom_list = child(page, WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                       LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP,
                       S(MARGIN), y, S(PAGE_W) - 2 * S(MARGIN), h - y - S(MARGIN), ID_ROM_LIST, g_font);
    SendMessageA(g_rom_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = S(300);
    col.pszText = "ROM";
    SendMessageA(g_rom_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
    col.cx = S(130);
    col.pszText = "Group";
    SendMessageA(g_rom_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
    col.cx = S(110);
    col.pszText = "State";
    SendMessageA(g_rom_list, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);
    for (r = 0; r < msdossession_rom_item_count() && r < MAX_ROM_ROWS; r++) {
        const msdos_rom_item *item = msdossession_rom_item(r);
        LVITEMA it;
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = r;
        it.pszText = (char *)item->name;
        SendMessageA(g_rom_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        lv_set(g_rom_list, r, 1, item->group);
    }
}

/* One preference's row from y: its height. */
static int build_row(HWND pg, int i, int y)
{
    const msdos_pref *p = msdossession_pref(i);
    Row *r = &g_rows[i];
    const int id = ID_ROW + i;
    const int cx = S(CTRL_X), cw = S(CTRL_W);
    int h = S(ROW_H);

    switch (p->type) {
    case MSDOS_PREF_BOOL: {
        wchar_t wt[256];
        msdos_wide(p->label, wt, 256);
        r->ctrl = CreateWindowExW(0, L"BUTTON", wt, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                  S(LABEL_X), y + S(4), S(PAGE_W) - S(LABEL_X) - S(MARGIN), S(22), pg,
                                  (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
        SendMessageW(r->ctrl, WM_SETFONT, (WPARAM)g_font, TRUE);
        if (p->help) {
            r->help = label_utf8(pg, p->help, SS_LEFT, S(LABEL_X) + S(20), y + S(26),
                                 S(PAGE_W) - S(LABEL_X) - S(20) - S(MARGIN), S(HELP_H), g_font, 1);
            h += S(HELP_H);
        }
        break;
    }
    case MSDOS_PREF_INT:
        r->label = label_utf8(pg, p->label, SS_LEFT, S(LABEL_X), y + S(7), cx - S(LABEL_X) - S(8), S(20),
                              g_font, 0);
        r->ctrl = child(pg, WS_EX_CLIENTEDGE, "EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, cx, y + S(3), S(110),
                        S(24), id, g_font);
        r->updown = child(pg, 0, UPDOWN_CLASSA, "", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS |
                          UDS_NOTHOUSANDS, 0, 0, 0, 0, 0, g_font);
        SendMessageA(r->updown, UDM_SETBUDDY, (WPARAM)r->ctrl, 0);
        if (p->help) {
            r->help = label_utf8(pg, p->help, SS_LEFT, cx, y + S(30), cw, S(HELP_H), g_font, 1);
            h += S(HELP_H);
        }
        break;
    case MSDOS_PREF_CHOICE:
        r->label = label_utf8(pg, p->label, SS_LEFT, S(LABEL_X), y + S(7), cx - S(LABEL_X) - S(8), S(20),
                              g_font, 0);
        r->ctrl = child(pg, 0, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y + S(3), cw,
                        S(320), id, g_font);
        SendMessageA(r->ctrl, CB_SETDROPPEDWIDTH, (WPARAM)S(420), 0);
        if (p->help) {
            r->help = label_utf8(pg, p->help, SS_LEFT, cx, y + S(30), cw, S(HELP_H), g_font, 1);
            h += S(HELP_H);
        }
        break;
    default:
        /* MSDOS_PREF_FILE: nothing in the table uses one yet */
        r->label = label_utf8(pg, p->label, SS_LEFT, S(LABEL_X), y + S(7), cx - S(LABEL_X) - S(8), S(20),
                              g_font, 0);
        r->ctrl = label_utf8(pg, msdossession_get_str(g_s, p->key, ""), SS_LEFT | SS_PATHELLIPSIS, cx,
                             y + S(7), cw, S(20), g_font, 1);
        break;
    }
    return h;
}

/* The heights the pages need: the tallest decides the window's. */
static int page_height(void)
{
    const char *last_page = NULL, *last_group = NULL;
    int i, h = 0, best = S(MIN_PAGE_H);
    for (i = 0; i < msdossession_pref_count() && i < MAX_ROWS; i++) {
        const msdos_pref *p = msdossession_pref(i);
        if (!last_page || strcmp(last_page, p->page)) {
            if (h > best) best = h;
            h = S(MARGIN) + (!strcmp(p->page, "Machine") ? S(40) : 0);
            last_page = p->page;
            last_group = NULL;
        }
        if (!last_group || strcmp(last_group, p->group)) {
            h += S(HEAD_H);
            last_group = p->group;
        }
        h += S(ROW_H) + (p->help ? S(HELP_H) : 0);
    }
    if (h + S(MARGIN) > best) best = h + S(MARGIN);
    return best;
}

/* Every control, at the window's DPI (again after a DPI change). */
static void build(void)
{
    const char *last_page = NULL, *last_group = NULL;
    HWND c, pg = NULL;
    RECT want, strip, area, wr;
    int i, y = 0, page_h, strip_h, sel = g_page;

    /* whatever was there, at the old size */
    while ((c = GetWindow(g_win, GW_CHILD)) != NULL) DestroyWindow(c);
    memset(g_rows, 0, sizeof g_rows);
    memset(g_pages, 0, sizeof g_pages);
    g_page_count = 0;
    g_rom_list = NULL;
    if (g_font) DeleteObject(g_font);
    if (g_bold) DeleteObject(g_bold);
    g_font = msdos_make_font(g_dpi, 0, FW_NORMAL, 0, NULL);
    g_bold = msdos_make_font(g_dpi, 0, FW_BOLD, 0, NULL);

    g_tabs = child(g_win, 0, WC_TABCONTROLA, "", WS_CLIPSIBLINGS | WS_TABSTOP, S(8), S(8), S(PAGE_W),
                   S(30), ID_TABS, g_font);
    /* every tab first: the strip's height comes from its items */
    for (i = 0; i < msdossession_pref_count() && i < MAX_ROWS && g_page_count < MAX_PAGES - 1; i++) {
        const msdos_pref *p = msdossession_pref(i);
        if (!last_page || strcmp(last_page, p->page)) {
            add_tab(p->page);
            g_page_names[g_page_count] = p->page;
            g_page_count++;
            last_page = p->page;
        }
    }
    add_tab("ROMs");
    g_page_names[g_page_count] = "ROMs";
    g_page_count = 0;
    last_page = NULL;
    /* the strip's height, from the control itself */
    SetRect(&strip, 0, 0, S(PAGE_W), S(400));
    SendMessageA(g_tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&strip);
    strip_h = strip.top > 0 ? strip.top : S(28);
    page_h = page_height();
    SetRect(&area, S(8), S(8) + strip_h + S(4), S(8) + S(PAGE_W), S(8) + strip_h + S(4) + page_h);
    MoveWindow(g_tabs, S(8), S(8), S(PAGE_W), strip_h, FALSE);

    for (i = 0; i < msdossession_pref_count() && i < MAX_ROWS && g_page_count < MAX_PAGES - 1; i++) {
        const msdos_pref *p = msdossession_pref(i);
        if (!last_page || strcmp(last_page, p->page)) {
            pg = g_pages[g_page_count++] = make_page(&area);
            last_page = p->page;
            last_group = NULL;
            y = S(MARGIN);
            if (!strcmp(p->page, "Machine")) {
                label_utf8(pg, "The machine's settings apply together when Preferences closes: the PC "
                           "resets once.", SS_LEFT, S(MARGIN), y, S(PAGE_W) - 2 * S(MARGIN), S(36), g_font, 1);
                y += S(40);
            }
        }
        if (!last_group || strcmp(last_group, p->group)) {
            label_utf8(pg, p->group, SS_LEFT, S(MARGIN), y + S(8), S(PAGE_W) - 2 * S(MARGIN), S(20), g_bold, 0);
            y += S(HEAD_H);
            last_group = p->group;
        }
        y += build_row(pg, i, y);
    }
    g_pages[g_page_count] = make_page(&area);
    build_roms_page(g_pages[g_page_count], page_h);
    g_page_count++;

    child(g_win, 0, "BUTTON", "Help", WS_TABSTOP | BS_PUSHBUTTON, S(PAGE_W) + S(8) - S(90),
          area.bottom + S(8), S(90), S(BUTTONS_H) - S(14), ID_HELP, g_font);

    /* exactly the client the pages need, whatever the frame is at this DPI */
    SetRect(&want, 0, 0, S(PAGE_W) + S(16), area.bottom + S(BUTTONS_H));
    AdjustWindowRectEx(&want, PREFS_STYLE, FALSE, 0);
    GetWindowRect(g_win, &wr);
    SetWindowPos(g_win, NULL, 0, 0, want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    {
        RECT cr;
        GetClientRect(g_win, &cr);
        if (cr.right != S(PAGE_W) + S(16) || cr.bottom != area.bottom + S(BUTTONS_H)) {
            /* the frame differs from what AdjustWindowRectEx assumed */
            GetWindowRect(g_win, &wr);
            SetWindowPos(g_win, NULL, 0, 0, (wr.right - wr.left) - cr.right + S(PAGE_W) + S(16),
                         (wr.bottom - wr.top) - cr.bottom + area.bottom + S(BUTTONS_H),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    refresh();
    if (sel < 0 || sel >= g_page_count) sel = 0;
    SendMessageA(g_tabs, TCM_SETCURSEL, (WPARAM)sel, 0);
    show_page(sel);
    InvalidateRect(g_win, NULL, TRUE);
}

static void register_classes(void)
{
    static int registered;
    WNDCLASSEXA wc;
    if (registered) return;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = page_proc;
    wc.lpszClassName = PAGE_CLASS;
    RegisterClassExA(&wc);
    wc.lpfnWndProc = prefs_proc;
    wc.lpszClassName = PREFS_CLASS;
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(101));
    RegisterClassExA(&wc);
    registered = 1;
}

void msdos_prefs_show(HWND parent, msdossession *session, void (*on_live)(void),
                      void (*on_closed)(int applied))
{
    RECT pr;
    if (g_win) {
        if (IsIconic(g_win)) ShowWindow(g_win, SW_RESTORE);
        SetForegroundWindow(g_win);
        return;
    }
    g_s = session;
    g_parent = parent;
    g_on_live = on_live;
    g_on_closed = on_closed;
    g_pending = MSDOS_APPLY_LIVE;
    g_abandon = 0;
    g_page = 0;
    register_classes();

    g_dpi = msdos_window_dpi(parent);
    GetWindowRect(parent, &pr);
    /* owned by the main window: over it, and gone with it */
    g_win = CreateWindowExA(WS_EX_CONTROLPARENT, PREFS_CLASS, "Preferences", PREFS_STYLE,
                            pr.left + S(40), pr.top + S(40), S(PAGE_W), S(MIN_PAGE_H), parent, NULL,
                            GetModuleHandleA(NULL), NULL);
    if (!g_win) return;
    if (msdos_window_dpi(g_win) != g_dpi) g_dpi = msdos_window_dpi(g_win);
    build();
    ShowWindow(g_win, SW_SHOW);
    SetFocus(g_tabs);
}

void msdos_prefs_abandon(void)
{
    if (!g_win) return;
    g_abandon = 1;
    DestroyWindow(g_win);
}

int msdos_prefs_pretranslate(MSG *msg)
{
    if (!g_win || !msg->hwnd || (msg->hwnd != g_win && !IsChild(g_win, msg->hwnd))) return 0;
    /* Ctrl+Tab and Ctrl+Shift+Tab move between the tabs, as in a dialog */
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_TAB && GetKeyState(VK_CONTROL) < 0) {
        const int n = g_page_count, dir = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
        const int page = n > 0 ? (g_page + dir + n) % n : 0;
        SendMessageA(g_tabs, TCM_SETCURSEL, (WPARAM)page, 0);
        show_page(page);
        return 1;
    }
    /* Escape closes it */
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE) {
        HWND focus = GetFocus();
        char cls[32] = "";
        if (focus) GetClassNameA(focus, cls, sizeof cls);
        if (!(focus && !_stricmp(cls, "ComboBox") && SendMessageA(focus, CB_GETDROPPEDSTATE, 0, 0))) {
            DestroyWindow(g_win);
            return 1;
        }
    }
    return IsDialogMessageA(g_win, msg) ? 1 : 0;
}
