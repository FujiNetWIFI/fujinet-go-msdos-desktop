/*
 * Debugger window (Win32) over the session's debugger engine, via
 * core/include/msdosdebug.h. Thirteen tabs, the family's plus the PC's
 * Interrupts: Console (DEBUG.COM's commands and the family's, a line at a
 * time, with a command history and Tab completion), Disassembly, Memory
 * (linear bytes through the memory map, with Go to, a search and a Poke bar),
 * Breakpoints (Execute / Memory / Port / INT, expression conditions, hits),
 * Watches, Machine & I/O, Trace, History, Video (the card's picture decoded
 * from its memory, registers, palette and beam; see msdosvidtab.h), Sound
 * (PC speaker and FM chip), Symbols (linker maps), Interrupts (the vector
 * table and a DOS/BIOS call trace) and FujiNet (its UART and the FujiBus
 * packets on its COM port). An editable register strip sits under every tab
 * (Enter applies, only while stopped). Built from plain common controls, with
 * the disassembly drawn by hand.
 *
 * Showing this window attaches (which stops the machine, as on every
 * sibling); hiding it (the close box, Ctrl+Shift+D, the View menu item)
 * turns the trace and history rings off and detaches, which lets the machine
 * run on at full speed. The toolbar carries the stepping controls, Save and
 * Pop Out; F5 F7 F8 Shift+F8 F9 and Ctrl+Shift+D work wherever the focus is
 * inside the window (msdos_debugger_pretranslate).
 *
 * Each tab's controls are children of a container of their own, so Pop Out
 * can peel the tab off into a window (SetParent) and Dock put it back.
 *
 * A 100 ms timer polls the engine (the generation and the stop snapshot,
 * never a stop callback, which fires off the UI thread) and refreshes the
 * tabs showing, and only those, when something changed; while the machine
 * runs it refreshes the live values twice a second. The engine's reads that
 * mean something only for a stopped PC (the memory map, the machine text,
 * the sound state) are made once a stop, never while running.
 *
 * Environment hooks, for captures and tests: MSDOS_DEBUGGER_TAB (a tab number,
 * or the start of a tab's name or label, in any case; the old "cpu" and
 * "registers" still work), MSDOS_DEBUGGER_WATCH ("x,y": watches),
 * MSDOS_DEBUGGER_WATCH_BREAK (a write-when-changed breakpoint on a range),
 * MSDOS_DEBUGGER_MEM_GOTO (show a range in Memory), MSDOS_DEBUGGER_HISTORY
 * (record from the start), MSDOS_DEBUGGER_POPOUT (a tab, as for
 * MSDOS_DEBUGGER_TAB) and the Video tab's MSDOS_DEBUGGER_VIDEO_*.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "msdosdebug.h"
#include "msdosmemtools.h"

#include "accent.h"
#include "dbg_video.h"
#include "../help/help_window.h"

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define CMD_HISTORY 100         /* console lines Up/Down recall */
#define TRACE_MAX 512           /* INT calls and FujiNet packets shown */
#define BP_MAX MSDOS_MAX_BREAKPOINTS
#define MAP_MAX 16              /* memory map regions (the PC makes at most 10) */
#define HIST_SHOWN 400          /* lines on the Trace and History tabs */
#define HIST_COPY 4000          /* entries read to find them */
#define SYM_MAX 4096            /* symbols listed at most */
#define CONSOLE_KEEP (256 * 1024)
#define NOTICE_MS 4000          /* how long a notice holds the status line */
#define SLOW_DEBOUNCE_MS 400    /* the machine reads are for a stopped PC: never more often than this */

/* Sizes at 96 dpi; scaled() scales them to the window's. */
#define GUTTER_W 18             /* the disassembly's breakpoint column */
#define MONO_PX 13              /* the monospace font */

#define WM_DBG_ACCEPT  (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB     (WM_APP + 3)   /* Tab in the console: complete */
#define WM_DBG_HISTORY (WM_APP + 4)   /* wp: 0 Up (older), 1 Down (newer) */
#define WM_DBG_SYNC    (WM_APP + 5)   /* resync once a list's notification is over */

#define TIMER_REFRESH 1

#define DBG_CLASS "FujiNetGoMSDOSDebugger"
#define DISASM_CLASS "FujiNetGoMSDOSDisasm"
#define PAGE_CLASS "FujiNetGoMSDOSDbgPage"
#define POP_CLASS "FujiNetGoMSDOSDbgPop"

enum {
    TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_MACHINE, TAB_TRACE, TAB_HISTORY,
    TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_INTS, TAB_FUJINET, TAB_COUNT
};

/* The tabs, in order: the names MSDOS_DEBUGGER_TAB takes (they are also the
 * Help's contexts, debugger/<name>), and their labels (which it takes too). */
static const struct { const char *name, *label; } k_tabs[TAB_COUNT] = {
    { "console", "Console" }, { "disassembly", "Disassembly" }, { "memory", "Memory" },
    { "breakpoints", "Breakpoints" }, { "watches", "Watches" }, { "machine", "Machine & I/O" },
    { "trace", "Trace" }, { "history", "History" }, { "video", "Video" }, { "sound", "Sound" },
    { "symbols", "Symbols" }, { "interrupts", "Interrupts" }, { "fujinet", "FujiNet" },
};

/* The tabs before there were thirteen: what they became. */
static const struct { const char *name; int tab; } k_tab_aliases[] = {
    { "cpu", TAB_DISASM }, { "registers", TAB_MEMORY }, { "io", TAB_MACHINE }, { "i/o", TAB_MACHINE },
    { "ports", TAB_MACHINE }, { "ivt", TAB_INTS }, { "int", TAB_INTS },
};

/* the general registers, as msdosdebug_register_set names them */
static const char *const k_regs16[] = { "ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "ip" };
static const char *const k_regs32[] = { "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip" };
#define NREGS 9
static const char *const k_sregs[] = { "cs", "ds", "es", "ss", "fs", "gs" };
#define NSREGS 6
/* the flags shown, with their EFLAGS bits */
static const struct { const char *name; int bit; } k_flags[] = {
    { "of", 11 }, { "df", 10 }, { "if", 9 }, { "tf", 8 }, { "sf", 7 }, { "zf", 6 },
    { "af", 4 }, { "pf", 2 }, { "cf", 0 },
};
#define NFLAGS 9

static const char *const k_bp_types[MSDOS_BP_TYPES] = {
    "Execute", "Memory read", "Memory write", "Memory read/write", "Port IN", "Port OUT",
    "Port IN/OUT", "INT" };

#define NSAVE 3
static const char *const k_save_kinds[NSAVE] = { "dis", "mem", "regs" };
static const char *const k_save_titles[NSAVE] = { "Disassembly from CS:IP...", "Memory (the first megabyte)...",
                                                  "Registers (text)..." };

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_RUN_TO, IDC_STATUS, IDC_SAVE, IDC_POPOUT, IDC_DOCK,
    IDC_HELP_BTN, IDC_TABS, IDC_TOGGLE_BP,
    IDC_CON_OUT, IDC_CON_PROMPT, IDC_CON_IN,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + NREGS - 1,
    IDC_SREG0, IDC_SREG_LAST = IDC_SREG0 + NSREGS - 1,
    IDC_FLAG0, IDC_FLAG_LAST = IDC_FLAG0 + NFLAGS - 1,
    IDC_MEM_VIEW, IDC_MEM_GOTO, IDC_MEM_BACK, IDC_MEM_ON, IDC_MEM_POKE,
    IDC_SRCH_KIND, IDC_SRCH_ENTRY, IDC_SRCH_NEXT, IDC_SRCH_PREV,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_BREAK_AT, IDC_DISASM,
    IDC_BP_LIST, IDC_BP_REMOVE, IDC_BP_HITS, IDC_BP_CLEAR, IDC_BP_TYPE, IDC_BP_START, IDC_BP_END, IDC_BP_AH,
    IDC_BP_COND, IDC_BP_ADD, IDC_BP_EDIT, IDC_BP_SET,
    IDC_WT_ENTRY, IDC_WT_ADD, IDC_WT_LIST, IDC_WT_MEM, IDC_WT_BRK, IDC_WT_REMOVE,
    IDC_MAP_TEXT, IDC_IO_TEXT, IDC_SOUND_TEXT,
    IDC_H_REC0, IDC_H_REC1, IDC_H_FILTER0, IDC_H_FILTER1, IDC_H_RANGE0, IDC_H_RANGE1,
    IDC_H_CLEAR0, IDC_H_CLEAR1, IDC_H_NOTE0, IDC_H_NOTE1, IDC_H_TEXT0, IDC_H_TEXT1,
    IDC_SYM_FILTER, IDC_SYM_LIST, IDC_SYM_SEG, IDC_SYM_LOAD, IDC_SYM_CLEAR, IDC_SYM_BREAK,
    IDC_INT_ON, IDC_INT_CLEAR, IDC_INT_TEXT, IDC_IVT_TEXT,
    IDC_FUJI_CLEAR, IDC_FUJI_TEXT, IDC_UART_TEXT,
    IDC_SAVE_KIND0, IDC_SAVE_KIND_LAST = IDC_SAVE_KIND0 + NSAVE - 1,
    IDC_LABEL_FIRST
};

typedef struct {
    HWND hwnd, tabs, tip;
    HWND run_btn, step_btn, over_btn, out_btn, run_to_btn, save_btn, popout_btn, help_btn, status;

    /* a container per tab: its controls are its children, so it can be peeled off into a window */
    HWND pages[TAB_COUNT];
    HWND popped[TAB_COUNT];     /* the window a tab lives in, or NULL */
    int tab_page[TAB_COUNT];    /* the tab at each tab-control index */
    int ntabs;
    HWND new_parent;            /* where child() puts the next control; NULL: the window */

    /* the register strip, under every tab */
    HWND reg_label[NREGS], reg[NREGS];
    HWND sreg_label[NSREGS], sreg[NSREGS], sbase[NSREGS];
    HWND flag[NFLAGS], cpu_info;
    int is386;

    /* console */
    HWND con_out, con_prompt, con_in;
    char history[CMD_HISTORY][256];
    int nhistory, history_pos;
    uint64_t con_serial;        /* the stop the console last reported */

    /* memory */
    HWND mem_view, goto_label, mem_goto, mem_back, mem_on, poke_label, mem_poke;
    HWND srch_label, srch_kind, srch_entry, srch_next, srch_prev, srch_note;
    uint32_t mem_top;           /* linear: the dump's first address */
    int mem_digits;             /* address digits in the dump */
    int mem_hl;                 /* a range is marked */
    uint32_t mem_hl_low, mem_hl_high;

    /* disassembly */
    HWND follow_pc, jump_label, jump, break_label, break_at, disasm_hint, disasm;
    uint16_t top_seg;           /* the first line */
    uint32_t top_off;
    int64_t selected;           /* the selected line's linear address, or -1 */
    msdos_dis_line lines[DISASM_WINDOW];
    int bp_state[DISASM_WINDOW];      /* each line's breakpoint: 1 on, 2 off */
    int line_count, pc_line;

    /* breakpoints */
    HWND bp_hint, bp_remove, bp_hits, bp_clear, bp_list, bp_empty;
    HWND bp_type, bp_start, bp_end, bp_ah, bp_cond, bp_add, bp_msg, bp_stop;
    HWND edit_label, bp_edit, bp_set;
    msdos_breakpoint bps[BP_MAX];
    int nbps;
    int edit_id;                /* the breakpoint whose condition the edit box shows */
    char bp_sig[BP_MAX * 260];  /* what the list shows, to rebuild on change */
    int bp_sig_ok;

    /* watches */
    HWND wt_label, wt_entry, wt_add, wt_list, wt_mem, wt_brk, wt_remove;

    /* machine & i/o: the slow reads are cached (see cache_due) */
    HWND map_text, io_text;
    msdos_region map[MAP_MAX];
    int map_count;
    char io_buf[16384];
    int machine_valid, machine_dirty;
    uint64_t machine_serial;
    DWORD machine_stamp;

    /* sound */
    HWND sound_text;

    /* trace (0, live) and history (1, when stopped) */
    HWND h_rec[2], h_filter[2], h_range[2], h_clear[2], h_note[2], h_text[2];
    uint64_t h_seen[2];

    /* video: one child window holding its own controls (dbg_video.c) */
    HWND video;

    /* symbols */
    HWND sym_filter_label, sym_filter, sym_count, sym_seg_label, sym_seg, sym_load, sym_clear, sym_break, sym_list;
    uint32_t sym_addr[SYM_MAX]; /* the address of each row shown, for an activate */
    int sym_shown;
    unsigned sym_generation;
    char sym_filter_text[64];
    int sym_ok;

    /* interrupts */
    HWND int_title, int_on, int_clear, int_text, ivt_title, ivt_text;
    uint64_t int_last;
    int int_count;

    /* fujinet */
    HWND fuji_title, fuji_note, fuji_clear, fuji_text, uart_title, uart_text;
    uint64_t fuji_last;
    int fuji_count, fuji_ticks;

    HWND views[40];             /* the read-only text views, drawn on the window colour */
    int nviews;
    HWND dim[96];               /* captions and hints, drawn grey */
    int ndim, nlabels;

    HFONT ui_font, ui_bold, mono;
    int dpi;
    int mono_w, mono_h, line_h;
    HACCEL accel;
    HBRUSH accent, bp_on, bp_off;

    msdossession *session;
    msdosdebug *dbg;

    int page;
    unsigned seen_generation;
    uint64_t seen_serial;
    int was_stopped;
    int attached_running;       /* attached while the machine was running */
    int running_ticks;
    int updating;               /* a refresh is setting controls */

    char notice[200];           /* a message holding the status line a moment */
    DWORD notice_until;
} debugger;

static debugger *g_dbg;

/* ---- helpers ---------------------------------------------------------------- */

static void edit_text(HWND hwnd, char *out, int outsz) { GetWindowTextA(hwnd, out, outsz); }

/* A label set only when it changed (no flicker on the live refresh). */
static void set_label(HWND hwnd, const char *utf8)
{
    wchar_t cur[512], wtext[512];
    msdos_wide(utf8, wtext, 512);
    GetWindowTextW(hwnd, cur, 512);
    if (wcscmp(cur, wtext) != 0) SetWindowTextW(hwnd, wtext);
}

/* A right-aligned label: its last glyph is drawn clipped by the label's edge,
 * so two no-break spaces follow the text. */
static void set_label_right(HWND hwnd, const char *utf8)
{
    char padded[600];
    snprintf(padded, sizeof padded, "%s\xc2\xa0\xc2\xa0", utf8);
    set_label(hwnd, padded);
}

/* EDIT controls want CR LF and draw control characters as boxes: engine
 * text (UTF-8) as the wide text a view shows. */
static wchar_t *wide_view_text(const char *text)
{
    const size_t length = strlen(text);
    size_t idx, used = 0;
    char *buf = malloc(length * 2 + 1);
    wchar_t *wtext = NULL;
    int wide_len;
    if (!buf) return NULL;
    for (idx = 0; idx < length; idx++) {
        const unsigned char letter = (unsigned char)text[idx];
        if (letter == '\n') { buf[used++] = '\r'; buf[used++] = '\n'; }
        else if (letter >= 0x20 || letter == '\t') buf[used++] = (char)letter;
    }
    buf[used] = '\0';
    wide_len = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    if (wide_len > 0 && (wtext = malloc((size_t)wide_len * sizeof *wtext)) != NULL)
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, wtext, wide_len);
    free(buf);
    return wtext;
}

static size_t addf(char *str, size_t len, size_t cap, const char *fmt, ...)
{
    va_list args;
    int written;
    if (len >= cap) return len;
    va_start(args, fmt);
    written = vsnprintf(str + len, cap - len, fmt, args);
    va_end(args);
    if (written < 0) return len;
    return len + (size_t)written < cap ? len + (size_t)written : cap - 1;
}

/* Blanks off both ends, in place. */
static char *strip(char *str)
{
    char *last;
    while (*str == ' ' || *str == '\t') str++;
    last = str + strlen(str);
    while (last > str && (last[-1] == ' ' || last[-1] == '\t' || last[-1] == '\r' || last[-1] == '\n')) *--last = '\0';
    return str;
}

/* Hex, as DEBUG.COM takes it ("2F8", "0x2F8", "$2F8", "2F8h"); # for decimal. */
static int parse_num(const char *text, long long *out)
{
    char *end;
    long long val;
    while (*text == ' ' || *text == '\t') text++;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) val = strtoll(text + 2, &end, 16);
    else if (*text == '$') val = strtoll(text + 1, &end, 16);
    else if (*text == '#') val = strtoll(text + 1, &end, 10);
    else val = strtoll(text, &end, 16);
    if (*end == 'h' || *end == 'H') end++;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (end == text || *end) return 0;
    *out = val;
    return 1;
}

/* seg:off, a register pair, a linear address or a symbol. */
static int parse_addr(debugger *win, const char *text, uint16_t *seg, uint32_t *off, uint32_t *lin)
{
    while (*text == ' ') text++;
    if (!*text) return 0;
    return msdosdebug_parse_address(win->dbg, text, seg, off, lin) == 0;
}

static const char *base_name(const char *path)
{
    const char *back = strrchr(path, '\\'), *fwd = strrchr(path, '/');
    if (fwd && (!back || fwd > back)) back = fwd;
    return back ? back + 1 : path;
}

/* ---- scale and fonts --------------------------------------------------------- */

static int scaled(const debugger *win, int val)
{
    return msdos_scale(val, win->dpi);
}

static BOOL CALLBACK refont_child(HWND hwnd, LPARAM lparam)
{
    const HFONT *map = (const HFONT *)lparam;     /* old ui, bold, mono; new ui, bold, mono */
    const HFONT cur = (HFONT)SendMessageA(hwnd, WM_GETFONT, 0, 0);
    int idx;
    for (idx = 0; idx < 3; idx++)
        if (cur && cur == map[idx]) SendMessageA(hwnd, WM_SETFONT, (WPARAM)map[idx + 3], TRUE);
    return TRUE;
}

/* The message font and Consolas at the window's DPI; the controls already
 * made take the new ones. */
static void make_fonts(debugger *win)
{
    HFONT old[3] = { win->ui_font, win->ui_bold, win->mono };
    HDC hdc;
    TEXTMETRICA metrics;
    HGDIOBJ prev;
    int idx;

    win->ui_font = msdos_make_font(win->dpi, 0, FW_NORMAL, 0, NULL);
    win->ui_bold = msdos_make_font(win->dpi, 0, FW_BOLD, 0, NULL);
    win->mono = msdos_make_font(win->dpi, MONO_PX, FW_NORMAL, 0, "Consolas");

    /* the monospace cell, for the hand-drawn disassembly */
    hdc = GetDC(win->hwnd);
    prev = SelectObject(hdc, win->mono);
    win->mono_w = scaled(win, 7);
    win->mono_h = scaled(win, 15);
    if (GetTextMetricsA(hdc, &metrics)) {
        win->mono_w = metrics.tmAveCharWidth;
        win->mono_h = metrics.tmHeight;
    }
    win->line_h = win->mono_h + scaled(win, 2);
    SelectObject(hdc, prev);
    ReleaseDC(win->hwnd, hdc);

    if (old[0]) {
        HFONT map[6] = { old[0], old[1], old[2], win->ui_font, win->ui_bold, win->mono };
        EnumChildWindows(win->hwnd, refont_child, (LPARAM)map);
    }
    for (idx = 0; idx < 3; idx++)
        if (old[idx]) DeleteObject(old[idx]);
}

static int text_w(debugger *win, HFONT font, const char *text)
{
    HDC hdc = GetDC(win->hwnd);
    HGDIOBJ old = SelectObject(hdc, font);
    SIZE size = { 0, 0 };
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &size);
    SelectObject(hdc, old);
    ReleaseDC(win->hwnd, hdc);
    return size.cx;
}

static int btn_w(debugger *win, const char *text)
{
    const int width = text_w(win, win->ui_font, text) + scaled(win, 24);
    return width < scaled(win, 64) ? scaled(win, 64) : width;
}

static int in_list(HWND hwnd, const HWND *list, int count)
{
    int idx;
    for (idx = 0; idx < count; idx++)
        if (list[idx] == hwnd) return 1;
    return 0;
}

/* High-contrast themes: the accent fills give way to the system's selection colours. */
static int high_contrast(void)
{
    HIGHCONTRASTA contrast;
    memset(&contrast, 0, sizeof contrast);
    contrast.cbSize = sizeof contrast;
    return SystemParametersInfoA(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0) &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON);
}

/* ---- the text views ------------------------------------------------------------ */

/* The rows an EDIT view shows. */
static int edit_rows(debugger *win, HWND hwnd)
{
    RECT area;
    GetClientRect(hwnd, &area);
    return win->mono_h > 0 && area.bottom > win->mono_h ? area.bottom / win->mono_h : 1;
}

/* Logs (the console, the traces) follow their end while their reader is
 * there. */
static int edit_at_end(debugger *win, HWND hwnd)
{
    const int first = (int)SendMessageW(hwnd, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageW(hwnd, EM_GETLINECOUNT, 0, 0);
    return first + edit_rows(win, hwnd) >= count;
}

static void edit_to_end(HWND hwnd)
{
    SendMessageW(hwnd, EM_LINESCROLL, 0, SendMessageW(hwnd, EM_GETLINECOUNT, 0, 0));
}

static void edit_scroll_to(HWND hwnd, int first)
{
    SendMessageW(hwnd, EM_LINESCROLL, 0, first - (int)SendMessageW(hwnd, EM_GETFIRSTVISIBLELINE, 0, 0));
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(HWND hwnd, const char *text, int follow)
{
    const int first = (int)SendMessageW(hwnd, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *wtext = wide_view_text(text);
    if (!wtext) return;
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(hwnd, wtext);
    if (follow) edit_to_end(hwnd);
    else edit_scroll_to(hwnd, first);
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hwnd, NULL, TRUE);
    free(wtext);
}

/* A view's text, replaced only when it changed (an unchanged one keeps its
 * selection and where the reader scrolled it to). */
static void set_view(HWND hwnd, const char *text)
{
    wchar_t *wtext = wide_view_text(text), *cur;
    int length;
    if (!wtext) return;
    length = GetWindowTextLengthW(hwnd);
    cur = malloc(((size_t)length + 2) * sizeof *cur);
    if (cur) {
        GetWindowTextW(hwnd, cur, length + 1);
        if (wcscmp(cur, wtext) == 0) {
            free(cur);
            free(wtext);
            return;
        }
        free(cur);
    }
    free(wtext);
    set_text_at(hwnd, text, 0);
}

/* The live refresh does not overwrite a value being typed: while the
 * machine runs, a focused field keeps what is in it. */
static void set_entry(debugger *win, HWND hwnd, const char *text)
{
    if (GetFocus() == hwnd && !msdosdebug_stopped(win->dbg)) return;
    set_label(hwnd, text);
}

static void refresh_status(debugger *win);
static int page_visible(debugger *win, int page);

static int notice_showing(debugger *win)
{
    return win->notice[0] && (LONG)(GetTickCount() - win->notice_until) < 0;
}

/* The other frontends' toasts: the message holds the status line a moment. */
static void toast(debugger *win, const char *msg)
{
    snprintf(win->notice, sizeof win->notice, "%s", msg);
    win->notice_until = GetTickCount() + NOTICE_MS;
    refresh_status(win);
}

/* The PC is running and the engine says it is stopped. */
static int machine_stopped(debugger *win)
{
    return msdossession_is_running(win->session) && msdosdebug_stopped(win->dbg);
}

/* ---- the console ---------------------------------------------------------------- */

/* Long sessions keep the last CONSOLE_KEEP characters, whole lines. */
static void console_trim(debugger *win)
{
    const int length = GetWindowTextLengthW(win->con_out);
    wchar_t *all, *newline;
    int cut;
    if (length <= CONSOLE_KEEP) return;
    all = malloc(((size_t)length + 1) * sizeof *all);
    if (!all) return;
    GetWindowTextW(win->con_out, all, length + 1);
    cut = length - CONSOLE_KEEP * 3 / 4;
    newline = wcschr(all + cut, L'\n');
    if (newline) cut = (int)(newline - all) + 1;
    free(all);
    SendMessageW(win->con_out, EM_SETSEL, 0, (LPARAM)cut);
    SendMessageW(win->con_out, EM_REPLACESEL, FALSE, (LPARAM)L"");
}

/* Onto the console's transcript, which follows its end only while the
 * reader is there. */
static void console_append(debugger *win, const char *text)
{
    const int follow = edit_at_end(win, win->con_out);
    const int first = (int)SendMessageW(win->con_out, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *wtext = wide_view_text(text);
    int length;
    if (!wtext) return;
    length = GetWindowTextLengthW(win->con_out);
    SendMessageW(win->con_out, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    SendMessageW(win->con_out, EM_REPLACESEL, FALSE, (LPARAM)wtext);
    free(wtext);
    console_trim(win);
    if (follow) edit_to_end(win->con_out);
    else edit_scroll_to(win->con_out, first);
}

/* Command output, ended with a newline. */
static void console_output(debugger *win, const char *text)
{
    const size_t length = strlen(text);
    if (!length) return;
    console_append(win, text);
    if (text[length - 1] != '\n') console_append(win, "\n");
}

/* Messages from Load Symbols and Save land in the console and, since it may
 * not be the tab showing, on the status line. */
static void say(debugger *win, const char *msg)
{
    console_output(win, msg);
    toast(win, msg);
}

/* At a new stop the console says why and where, as DEBUG's R does. */
static void console_stop(debugger *win, const msdos_stop_snapshot *snap)
{
    static char out[4096];
    char line[260];
    if (!snap->paused || snap->serial == win->con_serial) return;
    win->con_serial = snap->serial;
    snprintf(line, sizeof line, "\xe2\x80\x94 %s", snap->text);
    console_output(win, line);
    out[0] = '\0';
    msdosdebug_command(win->dbg, "r", out, sizeof out);
    console_output(win, out);
}

/* ---- refresh ---------------------------------------------------------------------- */

static void refresh_status(debugger *win)
{
    msdos_stop_snapshot snap;
    char text[300];
    const int running = msdossession_is_running(win->session) != 0;
    const int stopped = running && msdosdebug_stop_snapshot(win->dbg, &snap) != 0;

    if (notice_showing(win)) {
        snprintf(text, sizeof text, "%s", win->notice);
    } else if (!running) {
        snprintf(text, sizeof text, "The PC is not running");
    } else if (stopped) {
        snprintf(text, sizeof text, "Stopped: %s", snap.text);
    } else {
        snprintf(text, sizeof text, "Running");
    }
    set_label_right(win->status, text);
    if (win->tip) {
        TTTOOLINFOA info;
        memset(&info, 0, sizeof info);
        info.cbSize = TTTOOLINFOA_V2_SIZE;
        info.hwnd = win->hwnd;
        info.uId = (UINT_PTR)win->status;
        info.lpszText = text;
        SendMessageA(win->tip, TTM_UPDATETIPTEXTA, 0, (LPARAM)&info);
    }
    set_label(win->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    InvalidateRect(win->run_btn, NULL, FALSE);
    InvalidateRect(win->status, NULL, TRUE);
}

/* The register strip: the machine's values (the stale ones while it runs, a
 * box being typed in is left alone). */
static void refresh_cpu(debugger *win)
{
    static const char *const modes[] = { "real mode", "protected mode", "virtual 8086 mode" };
    msdos_cpu_regs regs;
    char buf[200], video[160];
    int idx;
    memset(&regs, 0, sizeof regs);
    if (msdosdebug_regs(win->dbg, &regs) != 0) return;
    {
        const uint32_t vals[NREGS] = { regs.eax, regs.ebx, regs.ecx, regs.edx, regs.esi, regs.edi, regs.ebp,
                                       regs.esp, regs.eip };
        const uint16_t svals[NSREGS] = { regs.cs, regs.ds, regs.es, regs.ss, regs.fs, regs.gs };
        const uint32_t bases[NSREGS] = { regs.cs_base, regs.ds_base, regs.es_base, regs.ss_base, regs.fs_base,
                                         regs.gs_base };
        win->is386 = regs.is386 != 0;
        for (idx = 0; idx < NREGS; idx++) {
            char name[8];
            snprintf(name, sizeof name, "%s", regs.is386 ? k_regs32[idx] : k_regs16[idx]);
            CharUpperA(name);
            set_label(win->reg_label[idx], name);
            snprintf(buf, sizeof buf, regs.is386 ? "%08X" : "%04X", regs.is386 ? vals[idx] : vals[idx] & 0xFFFFu);
            set_entry(win, win->reg[idx], buf);
        }
        for (idx = 0; idx < NSREGS; idx++) {
            const int shown = idx < 4 || win->is386;      /* FS and GS come with the 386 */
            snprintf(buf, sizeof buf, "%04X", svals[idx]);
            set_entry(win, win->sreg[idx], buf);
            snprintf(buf, sizeof buf, "base %05X", bases[idx]);
            set_label(win->sbase[idx], buf);
            ShowWindow(win->sreg_label[idx], shown ? SW_SHOW : SW_HIDE);
            ShowWindow(win->sreg[idx], shown ? SW_SHOW : SW_HIDE);
            ShowWindow(win->sbase[idx], shown ? SW_SHOW : SW_HIDE);
        }
    }
    win->updating = 1;
    for (idx = 0; idx < NFLAGS; idx++)
        SendMessageA(win->flag[idx], BM_SETCHECK,
                     ((regs.eflags >> k_flags[idx].bit) & 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    win->updating = 0;
    video[0] = '\0';
    msdosdebug_video_info(win->dbg, video, sizeof video);
    {
        const char *card = strip(video);
        if (regs.is386)
            snprintf(buf, sizeof buf, "%s   %s   CR0 %08X   %s", regs.cpu,
                     modes[regs.mode < 0 ? 0 : regs.mode > 2 ? 2 : regs.mode], (unsigned)regs.cr0, card);
        else
            snprintf(buf, sizeof buf, "%s   %s", regs.cpu, card);
    }
    set_label(win->cpu_info, buf);
}

/* The engine's memory map and machine text mean something only for a stopped
 * PC: the window keeps what it read and asks again only for a new stop (the
 * machine cannot change while it is stopped, and a running one is shown
 * stale): the first fill always, then never while running, and never more
 * often than SLOW_DEBOUNCE_MS (a stop inside that gap marks the cache dirty
 * and the timer comes back for it). Returns 1 when the caller should read now. */
static int cache_due(debugger *win, int valid, uint64_t *serial, DWORD *stamp, int *dirty)
{
    const uint64_t now_serial = msdosdebug_stop_serial(win->dbg);
    const DWORD now = GetTickCount();
    if (!valid) {
        *serial = now_serial;
        *stamp = now;
        *dirty = 0;
        return 1;
    }
    if (!machine_stopped(win) || (now_serial == *serial && !*dirty)) return 0;
    if (now - *stamp < SLOW_DEBOUNCE_MS) {
        *dirty = 1;
        return 0;
    }
    *serial = now_serial;
    *stamp = now;
    *dirty = 0;
    return 1;
}

static void machine_fetch(debugger *win)
{
    int count;
    if (!cache_due(win, win->machine_valid, &win->machine_serial, &win->machine_stamp, &win->machine_dirty)) return;
    count = msdosdebug_memory_map(win->dbg, win->map, MAP_MAX);
    win->map_count = count > 0 ? count : 0;
    win->io_buf[0] = '\0';
    msdosdebug_io_text(win->dbg, win->io_buf, sizeof win->io_buf);
    win->machine_valid = 1;
}

/* Where a byte's two hex digits start in a Memory row: the address, two
 * blanks, then three characters a byte, one more after the eighth. */
static int mem_hex_col(const debugger *win, int col)
{
    return win->mem_digits + 2 + col * 3 + (col >= 8 ? 1 : 0);
}

static void refresh_mem(debugger *win)
{
    static uint8_t mem[MEM_BYTES];
    static char buf[(MEM_ROWS + 1) * 160];
    static int rowoff[MEM_ROWS + 1];    /* each row's first character in the control (CR LF counted) */
    char region[80], last_region[80];
    size_t len = 0;
    int row, col;

    win->mem_digits = win->mem_top > 0x1000000u - MEM_BYTES ? 8 : 6;
    memset(mem, 0, sizeof mem);
    msdosdebug_read(win->dbg, win->mem_top, mem, MEM_BYTES);
    machine_fetch(win);
    last_region[0] = '\0';
    len = addf(buf, len, sizeof buf, "%*s", win->mem_digits + 2, "");
    for (col = 0; col < 16; col++)
        len = addf(buf, len, sizeof buf, col == 8 ? " %X  " : "%X  ", col);
    len = addf(buf, len, sizeof buf, "\n");
    for (row = 0; row < MEM_ROWS; row++) {
        const uint8_t *bytes = mem + row * 16;
        const uint32_t addr = win->mem_top + (uint32_t)row * 16;
        rowoff[row] = (int)len + row + 1;
        len = addf(buf, len, sizeof buf, "%0*X  ", win->mem_digits, (unsigned)addr);
        for (col = 0; col < 16; col++)
            len = addf(buf, len, sizeof buf, col == 8 ? " %02X " : "%02X ", bytes[col]);
        len = addf(buf, len, sizeof buf, " ");
        for (col = 0; col < 16; col++)
            len = addf(buf, len, sizeof buf, "%c", bytes[col] >= 0x20 && bytes[col] < 0x7F ? bytes[col] : '.');
        /* the memory map's region, on the first row of each run */
        region[0] = '\0';
        msdosmem_region_name(win->map, win->map_count, addr, region, (int)sizeof region);
        if (region[0] && strcmp(region, last_region) != 0) len = addf(buf, len, sizeof buf, "  ; %s", region);
        snprintf(last_region, sizeof last_region, "%s", region);
        len = addf(buf, len, sizeof buf, "\n");
    }
    set_text_at(win->mem_view, buf, 0);

    /* the bytes Go to, a search hit or a watch found stay selected across refreshes */
    if (win->mem_hl && win->mem_hl_high >= win->mem_top && win->mem_hl_low < win->mem_top + MEM_BYTES) {
        const uint32_t first = win->mem_hl_low > win->mem_top ? win->mem_hl_low : win->mem_top;
        const uint32_t last = win->mem_hl_high < win->mem_top + MEM_BYTES ? win->mem_hl_high
                                                                         : win->mem_top + MEM_BYTES - 1;
        const int first_row = (int)((first - win->mem_top) / 16), last_row = (int)((last - win->mem_top) / 16);
        const int top_line = (int)SendMessageW(win->mem_view, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(win->mem_view, EM_SETSEL,
                     (WPARAM)(rowoff[first_row] + mem_hex_col(win, (int)((first - win->mem_top) % 16))),
                     (LPARAM)(rowoff[last_row] + mem_hex_col(win, (int)((last - win->mem_top) % 16)) + 2));
        edit_scroll_to(win->mem_view, top_line);
    }
}

/* Shows bytes low..high in the Memory tab, selected, the page starting just above them. */
static void show_in_memory(debugger *win, uint32_t low, uint32_t high)
{
    char shown[16];
    int top;
    if (high < low) high = low;
    win->mem_hl = 1;
    win->mem_hl_low = low;
    win->mem_hl_high = high;
    win->mem_top = (low > 0x100 ? low - 0x100 : 0) & ~0xFu;
    if (win->mem_top > 0xFFFFFFFFu - MEM_BYTES) win->mem_top = 0xFFFFFFFFu - MEM_BYTES + 1;
    snprintf(shown, sizeof shown, "%05X", (unsigned)low);
    SetWindowTextA(win->mem_goto, shown);
    refresh_mem(win);
    /* the row 30% of the way down */
    top = 1 + (int)((low - win->mem_top) / 16) - edit_rows(win, win->mem_view) * 3 / 10;
    edit_scroll_to(win->mem_view, top > 0 ? top : 0);
}

/* The disassembly lines that fit the view. */
static int visible_rows(debugger *win)
{
    RECT area;
    int rows;
    GetClientRect(win->disasm, &area);
    if (win->line_h <= 0 || area.bottom <= 0) return 32;
    rows = area.bottom / win->line_h;
    return rows < 4 ? 4 : rows > DISASM_WINDOW ? DISASM_WINDOW : rows;
}

/* x86 code cannot be read backwards: the offset rows instructions before
 * off is the nearest start whose instructions land on off. */
static uint32_t rows_back(debugger *win, uint16_t seg, uint32_t off, int rows)
{
    static msdos_dis_line lines[DISASM_WINDOW * 4];
    const int max = (int)(sizeof lines / sizeof lines[0]);
    const int count = rows * 7 + 1 < max ? rows * 7 + 1 : max;
    int back, idx, found;
    for (back = rows; back <= rows * 7 && (uint32_t)back <= off; back++) {
        found = msdosdebug_disassemble(win->dbg, seg, off - (uint32_t)back, count, lines);
        for (idx = 0; idx < found && lines[idx].off < off; idx++)
            ;
        if (idx < found && lines[idx].off == off && idx >= rows) return lines[idx - rows].off;
    }
    return off > (uint32_t)rows * 3 ? off - (uint32_t)rows * 3 : 0;
}

static uint32_t rows_on(debugger *win, uint16_t seg, uint32_t off, int rows)
{
    static msdos_dis_line lines[DISASM_WINDOW + 1];
    const int want = rows + 1 < DISASM_WINDOW + 1 ? rows + 1 : DISASM_WINDOW + 1;
    const int found = msdosdebug_disassemble(win->dbg, seg, off, want, lines);
    return found > rows ? lines[rows].off : off;
}

/* 1 when an enabled execute breakpoint sits at linear (the gutter's toggle
 * makes these), 2 when a disabled one does, else 0. */
static int exec_breakpoint_state(const msdos_breakpoint *list, int count, uint32_t linear)
{
    int idx, state = 0;
    for (idx = 0; idx < count; idx++)
        if (list[idx].type == MSDOS_BP_EXEC && list[idx].start == linear && list[idx].end == linear) {
            if (list[idx].enabled) return 1;
            state = 2;
        }
    return state;
}

static void refresh_disasm(debugger *win)
{
    static msdos_breakpoint marks[BP_MAX];
    msdos_cpu_regs regs;
    const int rows = visible_rows(win);
    const int have_regs = msdosdebug_regs(win->dbg, &regs) == 0;
    int idx, found, mark_count = msdosdebug_bp_list(win->dbg, marks, BP_MAX);
    if (mark_count < 0) mark_count = 0;

    /* Following, CS:IP stays put while it shows, else sits a third of the
     * way down. */
    if (have_regs && SendMessageA(win->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        const uint32_t pc_linear = msdosdebug_linear(win->dbg, regs.cs, regs.eip);
        int shows = 0;
        for (idx = 1; idx < win->line_count && idx < rows - 2; idx++)
            if (win->lines[idx].linear == pc_linear) shows = 1;
        if (!shows || win->top_seg != regs.cs) {
            win->top_seg = regs.cs;
            win->top_off = rows_back(win, regs.cs, regs.eip, rows / 3);
        }
    }
    found = msdosdebug_disassemble(win->dbg, win->top_seg, win->top_off, DISASM_WINDOW, win->lines);
    win->line_count = found > 0 ? found : 0;
    win->pc_line = -1;
    for (idx = 0; idx < win->line_count; idx++) {
        win->bp_state[idx] = exec_breakpoint_state(marks, mark_count, win->lines[idx].linear);
        if (have_regs && win->lines[idx].seg == regs.cs && win->lines[idx].off == regs.eip) win->pc_line = idx;
    }
    InvalidateRect(win->disasm, NULL, FALSE);
}

/* The list, or (empty) the hint in its place. */
static void show_bp_list(debugger *win)
{
    const int shown = page_visible(win, TAB_BREAKS);
    ShowWindow(win->bp_list, shown && win->nbps > 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(win->bp_empty, shown && win->nbps == 0 ? SW_SHOW : SW_HIDE);
}

static void lv_set(HWND list, int row, int col, const char *text)
{
    wchar_t wtext[256];
    LVITEMW item;
    msdos_wide(text, wtext, 256);
    memset(&item, 0, sizeof item);
    item.iSubItem = col;
    item.pszText = wtext;
    SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&item);
}

/* Rebuilt only when something in it changed (each step refreshes), keeping
 * the selected breakpoint selected. */
static void refresh_bps(debugger *win)
{
    static char sig[sizeof win->bp_sig];
    size_t len = 0;
    int idx, sel_id = -1;
    const int sel = (int)SendMessageA(win->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

    if (sel >= 0 && sel < win->nbps) sel_id = win->bps[sel].id;
    win->nbps = msdosdebug_bp_list(win->dbg, win->bps, BP_MAX);
    if (win->nbps < 0) win->nbps = 0;
    sig[0] = '\0';
    for (idx = 0; idx < win->nbps; idx++)
        len = addf(sig, len, sizeof sig, "%d %d %u %s|%s\n", win->bps[idx].id, win->bps[idx].enabled,
                   win->bps[idx].hits, win->bps[idx].text, win->bps[idx].condition);
    if (win->bp_sig_ok && strcmp(sig, win->bp_sig) == 0) {
        show_bp_list(win);
        return;
    }
    memcpy(win->bp_sig, sig, sizeof sig);
    win->bp_sig_ok = 1;

    win->updating = 1;
    SendMessageA(win->bp_list, LVM_DELETEALLITEMS, 0, 0);
    for (idx = 0; idx < win->nbps; idx++) {
        const msdos_breakpoint *bpt = &win->bps[idx];
        LVITEMA item;
        char ident[16], hits[24];
        snprintf(ident, sizeof ident, "#%d", bpt->id);
        snprintf(hits, sizeof hits, "%u", bpt->hits);
        memset(&item, 0, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = idx;
        item.pszText = ident;
        SendMessageA(win->bp_list, LVM_INSERTITEMA, 0, (LPARAM)&item);
        lv_set(win->bp_list, idx, 1, bpt->text);
        lv_set(win->bp_list, idx, 2, bpt->condition);
        lv_set(win->bp_list, idx, 3, hits);
        memset(&item, 0, sizeof item);
        item.stateMask = LVIS_STATEIMAGEMASK | LVIS_SELECTED | LVIS_FOCUSED;
        item.state = INDEXTOSTATEIMAGEMASK(bpt->enabled ? 2 : 1);
        if (bpt->id == sel_id) item.state |= LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageA(win->bp_list, LVM_SETITEMSTATE, (WPARAM)idx, (LPARAM)&item);
    }
    win->updating = 0;
    show_bp_list(win);
}

/* Why the machine stopped, in words, under the breakpoint list. */
static void refresh_bp_stop(debugger *win)
{
    msdos_stop_snapshot snap;
    char line[300];
    if (msdossession_is_running(win->session) && msdosdebug_stop_snapshot(win->dbg, &snap))
        snprintf(line, sizeof line, "Stopped: %s", snap.text);
    else
        snprintf(line, sizeof line, msdossession_is_running(win->session) ? "Running." : "The PC is not running.");
    set_label(win->bp_stop, line);
}

/* Rebuilt only when a line changed, keeping the selection. */
static void refresh_watches(debugger *win)
{
    static char last[MSDOSWATCH_MAX * 220];
    static char now[MSDOSWATCH_MAX * 220];
    const int count = msdoswatch_count();
    const int keep = (int)SendMessageA(win->wt_list, LB_GETCURSEL, 0, 0);
    size_t len = 0;
    int idx;

    now[0] = '\0';
    for (idx = 0; idx < count; idx++) {
        char line[200];
        line[0] = '\0';
        msdoswatch_format(win->dbg, idx, line, (int)sizeof line);
        len = addf(now, len, sizeof now, "%s\n", line);
    }
    if (strcmp(now, last) == 0 && (int)SendMessageA(win->wt_list, LB_GETCOUNT, 0, 0) == count) return;
    memcpy(last, now, sizeof last);
    SendMessageA(win->wt_list, WM_SETREDRAW, FALSE, 0);
    SendMessageA(win->wt_list, LB_RESETCONTENT, 0, 0);
    for (idx = 0; idx < count; idx++) {
        char line[200];
        wchar_t wline[200];
        line[0] = '\0';
        msdoswatch_format(win->dbg, idx, line, (int)sizeof line);
        msdos_wide(line, wline, 200);
        SendMessageW(win->wt_list, LB_ADDSTRING, 0, (LPARAM)wline);
    }
    if (keep >= 0 && keep < count) SendMessageA(win->wt_list, LB_SETCURSEL, (WPARAM)keep, 0);
    SendMessageA(win->wt_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(win->wt_list, NULL, TRUE);
}

/* The memory map as it is now (left) and the machine's text (right). */
static void refresh_machine(debugger *win)
{
    static char buf[4096];
    size_t len = 0;
    int idx;

    machine_fetch(win);
    if (!win->machine_valid || !msdossession_is_running(win->session)) {
        set_view(win->map_text, "(the PC is not running)");
        set_view(win->io_text, "(the PC is not running)");
        return;
    }
    len = addf(buf, len, sizeof buf, "Memory map\n");
    for (idx = 0; idx < win->map_count; idx++)
        len = addf(buf, len, sizeof buf, "  %06X-%06X  %c%c  %s\n", (unsigned)win->map[idx].low,
                   (unsigned)win->map[idx].high, win->map[idx].readable ? 'r' : '-', win->map[idx].writable ? 'w' : '-',
                   win->map[idx].name);
    if (win->map_count == 0) len = addf(buf, len, sizeof buf, "  (none)\n");
    set_view(win->map_text, buf);
    set_view(win->io_text, win->io_buf[0] ? win->io_buf : "(no machine)");
}

/* The PC speaker and the FM chip, as the engine words them (stopped only). */
static void refresh_sound(debugger *win)
{
    static char buf[16384];
    if (!msdossession_is_running(win->session)) {
        set_view(win->sound_text, "(the PC is not running)");
        return;
    }
    if (!msdosdebug_stopped(win->dbg)) {
        set_view(win->sound_text, "Stop the PC to read the sound state.");
        return;
    }
    buf[0] = '\0';
    msdosdebug_sound_text(win->dbg, buf, sizeof buf);
    set_view(win->sound_text, buf[0] ? buf : "(no sound state)");
}

/* Trace (0: live, the ring as it fills) and History (1: what ran before the
 * machine stopped, while it is stopped) read the same ring: the newest
 * HIST_SHOWN entries that pass the filter, oldest first. */
static void hist_refresh(debugger *win, int which, int force)
{
    static msdos_trace_entry ring[HIST_COPY];
    static char text[HIST_SHOWN * 320];
    const int live = which == 0;
    const int stopped = machine_stopped(win);
    const int recording = live ? msdosdebug_trace_enabled(win->dbg) : msdosdebug_history_enabled(win->dbg);
    const uint64_t count = live ? msdosdebug_trace_total(win->dbg) : msdosdebug_history_count(win->dbg);
    uint32_t low = 0, high = 0xFFFFFFFFu;
    int filter = MSDOS_TRACE_ALL, found, pos, shown = 0, hit[HIST_SHOWN];
    size_t len = 0;
    char note[160];

    SendMessageA(win->h_rec[which], BM_SETCHECK, recording ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(win->h_rec[which], NULL, FALSE);
    if (!force && count == win->h_seen[which] && (live || stopped)) return;
    win->h_seen[which] = count;
    if (!live && !stopped) {
        set_label_right(win->h_note[which], "Stop the PC to read its history.");
        return;
    }
    switch ((int)SendMessageA(win->h_filter[which], CB_GETCURSEL, 0, 0)) {
    case 1: filter = MSDOS_TRACE_BRANCHES; break;
    case 2: {
        char row[64];
        GetWindowTextA(win->h_range[which], row, sizeof row);
        if (msdossym_parse_range(win->dbg, row, &low, &high)) filter = MSDOS_TRACE_RANGE;
        break;
    }
    default: break;
    }
    text[0] = '\0';
    if (live) {
        found = msdosdebug_trace_read(win->dbg, ring, HIST_COPY);      /* newest first */
        for (pos = 0; pos < found && shown < HIST_SHOWN; pos++)
            if (msdosdebug_trace_match(&ring[pos], filter, low, high)) hit[shown++] = pos;
        if (!recording && found == 0) snprintf(text, sizeof text, "The trace is off. Press Record, then let the PC run (it slows the PC down).");
    } else {
        found = msdosdebug_history_copy(win->dbg, ring, HIST_COPY);    /* oldest first */
        for (pos = found - 1; pos >= 0 && shown < HIST_SHOWN; pos--)
            if (msdosdebug_trace_match(&ring[pos], filter, low, high)) hit[shown++] = pos;
        if (!recording && found == 0) snprintf(text, sizeof text, "History is off. Press Record, then let the PC run (it slows the PC down).");
    }
    for (pos = shown - 1; pos >= 0 && len + 340 < sizeof text; pos--) {
        char line[300];
        msdosdebug_trace_format(&ring[hit[pos]], line, (int)sizeof line);
        len = addf(text, len, sizeof text, "%s\n", line);
    }
    set_text_at(win->h_text[which], text, 1);
    snprintf(note, sizeof note, "%s%llu instructions recorded, last %d shown", recording ? "Recording. " : "",
             (unsigned long long)count, shown);
    set_label_right(win->h_note[which], note);
}

/* The symbols that pass the filter, one a row with its address kept for an
 * activate; rebuilt only when the filter or the symbols changed. */
static void refresh_symbols(debugger *win)
{
    static int picked[SYM_MAX];
    char filter[64], count_text[64];
    const unsigned generation = msdosdebug_symbols_generation(win->dbg);
    int found, idx, shown = 0;

    GetWindowTextA(win->sym_filter, filter, sizeof filter);
    if (win->sym_ok && generation == win->sym_generation && strcmp(filter, win->sym_filter_text) == 0) return;
    found = msdosdebug_symbols_filter(win->dbg, filter, picked, SYM_MAX);
    if (found < 0) found = 0;
    SendMessageA(win->sym_list, WM_SETREDRAW, FALSE, 0);
    SendMessageA(win->sym_list, LB_RESETCONTENT, 0, 0);
    for (idx = 0; idx < found; idx++) {
        uint32_t linear;
        char name[96], line[140];
        wchar_t wline[140];
        if (!msdosdebug_symbols_get(win->dbg, picked[idx], &linear, name, (int)sizeof name)) continue;
        snprintf(line, sizeof line, "%06X  %s", (unsigned)linear, name);
        msdos_wide(line, wline, 140);
        SendMessageW(win->sym_list, LB_ADDSTRING, 0, (LPARAM)wline);
        win->sym_addr[shown++] = linear;
    }
    SendMessageA(win->sym_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(win->sym_list, NULL, TRUE);
    win->sym_shown = shown;
    win->sym_generation = generation;
    win->sym_ok = 1;
    snprintf(win->sym_filter_text, sizeof win->sym_filter_text, "%s", filter);
    snprintf(count_text, sizeof count_text, "%d symbol%s%s", shown, shown == 1 ? "" : "s", filter[0] ? " (filtered)" : "");
    set_label(win->sym_count, count_text);
}

static void refresh_ints(debugger *win, int force)
{
    static msdos_int_event events[TRACE_MAX];
    static char text[TRACE_MAX * 140 + 256];
    size_t len = 0;
    int count = msdosdebug_int_trace(win->dbg, events, TRACE_MAX), idx, follow;

    win->updating = 1;
    SendMessageA(win->int_on, BM_SETCHECK, msdosdebug_int_trace_enabled(win->dbg) ? BST_CHECKED : BST_UNCHECKED, 0);
    win->updating = 0;
    if (count < 0) count = 0;

    /* the vector table: changes rarely, cheap to read */
    for (idx = 0; idx < 256; idx++) {
        uint16_t seg = 0, off = 0;
        msdosdebug_ivt(win->dbg, idx, &seg, &off);
        len = addf(text, len, sizeof text, "%02X %04X:%04X%s", idx, seg, off, (idx & 3) == 3 ? "\n" : "   ");
    }
    set_text_at(win->ivt_text, text, 0);

    /* a log: set again only when it moved */
    if (!force && count == win->int_count && (count == 0 || events[count - 1].serial == win->int_last)) return;
    follow = force || win->int_count < 0 || edit_at_end(win, win->int_text);
    win->int_count = count;
    win->int_last = count > 0 ? events[count - 1].serial : 0;
    len = 0;
    len = addf(text, len, sizeof text, "From       AX   BX   CX   DX   DS   ES    Call");
    for (idx = 0; idx < count; idx++)
        len = addf(text, len, sizeof text, "\n%04X:%04X  %04X %04X %04X %04X %04X %04X  %s", events[idx].cs,
                   (unsigned)(events[idx].ip & 0xFFFF), events[idx].ax, events[idx].bx, events[idx].cx,
                   events[idx].dx, events[idx].ds, events[idx].es, events[idx].text);
    if (count == 0)
        len = addf(text, len, sizeof text, msdosdebug_int_trace_enabled(win->dbg)
                       ? "\n(no INT calls since the trace was cleared)"
                       : "\n(turn the trace on to record INT calls; it costs speed)");
    set_text_at(win->int_text, text, follow);
}

static void refresh_fujinet(debugger *win, int force)
{
    static msdos_fuji_packet packets[TRACE_MAX];
    static char text[TRACE_MAX * 320 + 128];
    static char uart[2048];
    char note[200];
    size_t len = 0;
    int count = msdosdebug_fujinet_trace(win->dbg, packets, TRACE_MAX), idx, follow;

    if (msdossession_fujinet_external(win->session))
        snprintf(note, sizeof note, "External FujiNet at %s on COM%d%s.",
                 msdossession_boip_endpoint(win->session), msdossession_fujinet_com(win->session),
                 msdossession_fujinet_link(win->session) ? ", connected" : ", not connected");
    else if (!msdossession_fujinet_running(win->session))
        snprintf(note, sizeof note, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet).");
    else
        snprintf(note, sizeof note, "FujiNet is on COM%d%s.", msdossession_fujinet_com(win->session),
                 msdossession_fujinet_link(win->session) ? ", connected" : ", not connected");
    set_label(win->fuji_note, note);
    uart[0] = '\0';
    msdosdebug_uart(win->dbg, uart, sizeof uart);
    set_text_at(win->uart_text, uart, 0);
    if (count < 0) count = 0;

    if (!force && count == win->fuji_count && (count == 0 || packets[count - 1].serial == win->fuji_last)) return;
    follow = force || win->fuji_count < 0 || edit_at_end(win, win->fuji_text);
    win->fuji_count = count;
    win->fuji_last = count > 0 ? packets[count - 1].serial : 0;
    text[0] = '\0';
    for (idx = 0; idx < count; idx++) {
        int sub;
        len = addf(text, len, sizeof text, "%s%s\n     ", idx ? "\n" : "", packets[idx].text);
        for (sub = 0; sub < packets[idx].nbytes && sub < (int)sizeof packets[idx].data; sub++)
            len = addf(text, len, sizeof text, "%02X ", packets[idx].data[sub]);
        if (packets[idx].nbytes > (int)sizeof packets[idx].data) len = addf(text, len, sizeof text, "\xe2\x80\xa6");
    }
    if (count == 0) len = addf(text, len, sizeof text, "(no FujiBus packets since the trace was cleared)");
    set_text_at(win->fuji_text, text, follow);
}

/* One tab brought up to date (force: the live ones too, the Video tab redrawn). */
static void refresh_page(debugger *win, int page, int force)
{
    if (!msdossession_is_running(win->session) && page != TAB_SYMBOLS && page != TAB_WATCHES && page != TAB_BREAKS) {
        if (page == TAB_MACHINE) refresh_machine(win);
        if (page == TAB_SOUND) refresh_sound(win);
        return;
    }
    switch (page) {
    case TAB_MEMORY: refresh_mem(win); break;
    case TAB_DISASM: refresh_disasm(win); break;
    case TAB_BREAKS: refresh_bps(win); refresh_bp_stop(win); break;
    case TAB_WATCHES: refresh_watches(win); break;
    case TAB_MACHINE: refresh_machine(win); break;
    case TAB_TRACE: hist_refresh(win, 0, force); break;
    case TAB_HISTORY: hist_refresh(win, 1, force); break;
    case TAB_VIDEO: if (win->video) dbg_video_refresh(win->video, force); break;
    case TAB_SOUND: refresh_sound(win); break;
    case TAB_SYMBOLS: refresh_symbols(win); break;
    case TAB_INTS: refresh_ints(win, force); break;
    case TAB_FUJINET: refresh_fujinet(win, force); break;
    default: break;
    }
}

/* The tabs on screen: the one the tab control shows, and any in a window of its own. */
static void refresh_shown(debugger *win, int force)
{
    int page;
    for (page = 0; page < TAB_COUNT; page++)
        if (page_visible(win, page)) refresh_page(win, page, force);
}

/* The tabs whose contents move while the PC runs. */
static void refresh_live(debugger *win)
{
    static const int live[] = { TAB_BREAKS, TAB_WATCHES, TAB_TRACE, TAB_INTS, TAB_FUJINET };
    size_t idx;
    for (idx = 0; idx < sizeof live / sizeof live[0]; idx++)
        if (page_visible(win, live[idx])) refresh_page(win, live[idx], 0);
}

/* After anything that may have moved the engine: the status, the register
 * strip and the tabs showing follow it. */
static void resync(debugger *win)
{
    msdos_stop_snapshot snap;
    const int running = msdossession_is_running(win->session) != 0;
    const int stopped = running && msdosdebug_stop_snapshot(win->dbg, &snap) != 0;
    if (stopped) console_stop(win, &snap);
    win->was_stopped = stopped;
    win->seen_serial = stopped ? snap.serial : msdosdebug_stop_serial(win->dbg);
    win->seen_generation = msdosdebug_generation(win->dbg);
    win->running_ticks = 0;
    refresh_status(win);
    if (running) refresh_cpu(win);
    refresh_shown(win, 1);
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
static void settle_and_sync(debugger *win, int expect_stop)
{
    int idx;
    win->notice[0] = '\0';        /* the machine moved: its new state replaces a notice */
    for (idx = 0; expect_stop && idx < 40 && !msdosdebug_stopped(win->dbg); idx++)
        Sleep(1);
    resync(win);
}

static void attach_and_refresh(debugger *win)
{
    msdosdebug_attach(win->dbg);     /* stops the machine */
    win->attached_running = msdossession_is_running(win->session) != 0;
    win->was_stopped = 0;
    win->machine_valid = 0;
    settle_and_sync(win, 1);
}

static void tick(debugger *win)
{
    msdos_stop_snapshot snap;
    unsigned generation;
    int stopped;
    if (!IsWindowVisible(win->hwnd)) return;
    if (win->notice[0] && !notice_showing(win)) {
        win->notice[0] = '\0';
        refresh_status(win);
    }
    if (!msdossession_is_running(win->session)) {
        if (win->attached_running) {
            win->attached_running = 0;
            refresh_status(win);
        }
        return;
    }
    /* Opened before the PC started, or showing across a restart: the
     * engine attaches (stopping the machine) as it comes up. */
    if (!win->attached_running) {
        msdosdebug_detach(win->dbg);
        attach_and_refresh(win);
        return;
    }
    generation = msdosdebug_generation(win->dbg);
    stopped = msdosdebug_stop_snapshot(win->dbg, &snap) != 0;
    if (generation != win->seen_generation || stopped != win->was_stopped || (stopped && snap.serial != win->seen_serial)) {
        if (stopped != win->was_stopped) win->notice[0] = '\0';   /* a stop (or a go) says so at once */
        resync(win);
        return;
    }
    if (!stopped) {
        /* the Video tab follows the beam: every tick, not twice a second */
        if (page_visible(win, TAB_VIDEO) && win->video) dbg_video_refresh(win->video, 0);
        if (++win->running_ticks >= 5) {
            /* live values twice a second while the machine runs */
            win->running_ticks = 0;
            refresh_status(win);
            refresh_cpu(win);
            refresh_live(win);
        }
    } else {
        if (page_visible(win, TAB_FUJINET) && ++win->fuji_ticks >= 5) {
            win->fuji_ticks = 0;
            refresh_fujinet(win, 0);
        }
        /* a stop that came inside the debounce gap of the slow reads: catch up */
        if (win->machine_dirty) {
            if (page_visible(win, TAB_MACHINE)) refresh_page(win, TAB_MACHINE, 0);
            if (page_visible(win, TAB_MEMORY)) refresh_page(win, TAB_MEMORY, 0);
        }
    }
}

/* ---- actions -------------------------------------------------------------------- */

static void on_run(debugger *win)
{
    const int stopped = machine_stopped(win);
    if (stopped) msdosdebug_resume(win->dbg);
    else msdosdebug_stop(win->dbg);
    settle_and_sync(win, !stopped);
}

static void on_run_to(debugger *win)
{
    if (win->selected < 0) {
        toast(win, "Select a line in the Disassembly to run to");
        return;
    }
    msdosdebug_run_to(win->dbg, (uint32_t)win->selected);
    settle_and_sync(win, 1);
}

static void toggle_breakpoint(debugger *win, uint32_t linear)
{
    if (msdosdebug_bp_toggle(win->dbg, linear) < 0) toast(win, "No room for another breakpoint");
    resync(win);
}

/* F9: the breakpoint on the selected disassembly line. */
static void on_toggle_bp(debugger *win)
{
    if (win->selected < 0) {
        toast(win, "Select a line in the Disassembly first");
        return;
    }
    toggle_breakpoint(win, (uint32_t)win->selected);
}

static void on_console(debugger *win)
{
    static char out[65536];
    char buf[256], line[300], *cmd;
    const int was = machine_stopped(win);

    edit_text(win->con_in, buf, sizeof buf);
    cmd = strip(buf);
    if (!*cmd) return;
    /* history: the newest last, no repeats of the one before */
    if (win->nhistory == 0 || strcmp(win->history[win->nhistory - 1], cmd) != 0) {
        if (win->nhistory == CMD_HISTORY) {
            memmove(win->history[0], win->history[1], sizeof win->history[0] * (CMD_HISTORY - 1));
            win->nhistory--;
        }
        snprintf(win->history[win->nhistory++], sizeof win->history[0], "%s", cmd);
    }
    win->history_pos = win->nhistory;
    snprintf(line, sizeof line, "-%s\n", cmd);
    edit_to_end(win->con_out);     /* a command brings the reader back down */
    console_append(win, line);

    out[0] = '\0';
    msdosdebug_command(win->dbg, cmd, out, sizeof out);
    SetWindowTextA(win->con_in, "");
    console_output(win, out);
    settle_and_sync(win, was);
}

/* Up and Down walk the commands already run; past the newest is an empty
 * line again. */
static void history_step(debugger *win, int newer)
{
    int length;
    if (win->nhistory == 0) return;
    win->history_pos += newer ? 1 : -1;
    if (win->history_pos < 0) win->history_pos = 0;
    if (win->history_pos >= win->nhistory) {
        win->history_pos = win->nhistory;
        SetWindowTextA(win->con_in, "");
        return;
    }
    SetWindowTextA(win->con_in, win->history[win->history_pos]);
    length = GetWindowTextLengthA(win->con_in);
    SendMessageA(win->con_in, EM_SETSEL, (WPARAM)length, (LPARAM)length);
}

/* Tab: the engine completes the end of the line; one answer takes its
 * place, several are listed (and the part they share is typed). */
static void complete_console(debugger *win)
{
    char text[256], comps[4096], common[256] = "", line[256];
    char *space, *cur, *newline;
    int count, first = 1;
    edit_text(win->con_in, text, sizeof text);
    count = msdosdebug_completions(win->dbg, text, comps, sizeof comps);
    if (count <= 0) return;
    for (cur = comps; *cur; cur = newline ? newline + 1 : cur + strlen(cur)) {
        int same = 0;
        newline = strchr(cur, '\n');
        snprintf(line, sizeof line, "%.*s", newline ? (int)(newline - cur) : (int)strlen(cur), cur);
        if (!line[0]) continue;
        if (first) {
            snprintf(common, sizeof common, "%s", line);
            first = 0;
        } else {
            while (common[same] && line[same] && common[same] == line[same]) same++;
            common[same] = '\0';
        }
    }
    if (count > 1) console_append(win, comps);
    space = strrchr(text, ' ');
    if ((int)strlen(common) < (int)strlen(space ? space + 1 : text))
        snprintf(common, sizeof common, "%s", space ? space + 1 : text);   /* never shorter than typed */
    if (space) space[1] = '\0'; else text[0] = '\0';
    {
        char joined[600];
        snprintf(joined, sizeof joined, "%s%s%s", text, common, count == 1 ? " " : "");
        SetWindowTextA(win->con_in, joined);
        SendMessageA(win->con_in, EM_SETSEL, (WPARAM)strlen(joined), (LPARAM)strlen(joined));
    }
}

/* A value typed into a register box: hex ("1F", "$1F", "0x1F"), "#31" decimal,
 * or anything the engine's expressions take (a symbol, "ax + 2", "[ds:si]"). */
static int parse_value(debugger *win, const char *text, long long *out, char *err, int errsz)
{
    char buf[128], *trimmed;
    int64_t value;
    snprintf(buf, sizeof buf, "%s", text);
    trimmed = strip(buf);
    if (!*trimmed) {
        snprintf(err, (size_t)errsz, "Type a value");
        return 0;
    }
    if (parse_num(trimmed, out)) return 1;
    if (msdosdebug_eval(win->dbg, trimmed, &value, err, errsz) != 0) return 0;
    *out = value;
    return 1;
}

/* A register box (Enter): the 32-bit name when the 32-bit value shows. Refused,
 * with a notice, unless stopped. */
static void on_reg(debugger *win, int idx, int segment)
{
    char buf[64], err[160];
    const char *name = segment ? k_sregs[idx] : win->is386 ? k_regs32[idx] : k_regs16[idx];
    long long val;
    err[0] = '\0';
    edit_text(segment ? win->sreg[idx] : win->reg[idx], buf, sizeof buf);
    if (!machine_stopped(win)) {
        toast(win, "Stop the PC first to change a register");
    } else if (!parse_value(win, buf, &val, err, sizeof err)) {
        toast(win, err[0] ? err : "Not a number (hex, # decimal, or an expression)");
    } else if (!msdosdebug_register_set(win->dbg, name, val, err, sizeof err)) {
        toast(win, err[0] ? err : "That register cannot be set here");
    }
    SetFocus(win->hwnd);
    resync(win);
}

static void on_flag(debugger *win, int idx)
{
    char err[160];
    const int checked = SendMessageA(win->flag[idx], BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (win->updating) return;
    err[0] = '\0';
    if (!machine_stopped(win)) {
        toast(win, "Stop the PC first to change a flag");
        refresh_cpu(win);     /* the box goes back to the CPU's */
        return;
    }
    if (!msdosdebug_register_set(win->dbg, k_flags[idx].name, checked, err, sizeof err))
        toast(win, err[0] ? err : "That flag cannot be set here");
    resync(win);
}

/* Go to: the dump from just above an address (seg:off, a register pair, a
 * linear address, a symbol, an expression or a range), its bytes marked and
 * scrolled to. */
static void on_mem_goto(debugger *win)
{
    char buf[64];
    uint32_t low, high;
    edit_text(win->mem_goto, buf, sizeof buf);
    if (!msdossym_parse_range(win->dbg, buf, &low, &high)) {
        win->mem_hl = 0;
        refresh_mem(win);
        toast(win, "Not an address (seg:off, ds:si, a linear address, a symbol or a range)");
        return;
    }
    show_in_memory(win, low, high);
}

/* The dump a page of MEM_BYTES on or back. */
static void on_mem_page(debugger *win, int dir)
{
    if (dir < 0) win->mem_top = win->mem_top > MEM_BYTES ? win->mem_top - MEM_BYTES : 0;
    else if (win->mem_top < 0xFFFFFFFFu - 2 * MEM_BYTES) win->mem_top += MEM_BYTES;
    refresh_mem(win);
}

/* "address value [value...]": bytes into memory from the address on. */
static void on_mem_poke(debugger *win)
{
    char buf[256], *token, *save = NULL, msg[96];
    uint8_t bytes[64];
    uint16_t seg;
    uint32_t off, lin;
    int count = 0;
    edit_text(win->mem_poke, buf, sizeof buf);
    token = strtok_s(buf, " \t,", &save);
    if (!token) return;
    if (!parse_addr(win, token, &seg, &off, &lin)) {
        toast(win, "Not an address (seg:off, a linear address, a symbol)");
        return;
    }
    while ((token = strtok_s(NULL, " \t,", &save)) != NULL) {
        long long val;
        if (!parse_num(token, &val) || val < 0 || val > 0xFF) {
            toast(win, "A value is a byte (00-FF)");
            return;
        }
        if (count >= (int)sizeof bytes) {
            toast(win, "At most 64 bytes at a time");
            return;
        }
        bytes[count++] = (uint8_t)val;
    }
    if (!count) {
        toast(win, "Type: address value [value...]");
        return;
    }
    msdosdebug_write(win->dbg, lin, bytes, count);
    snprintf(msg, sizeof msg, "Wrote %d byte%s at %05X", count, count == 1 ? "" : "s", (unsigned)lin);
    toast(win, msg);
    resync(win);
}

static void do_search(debugger *win, int backward)
{
    char text[200];
    uint8_t pat[MSDOSSEARCH_MAX], mask;
    uint32_t found, from;
    int count, flags, result;

    edit_text(win->srch_entry, text, sizeof text);
    if (!msdossearch_parse((int)SendMessageA(win->srch_kind, CB_GETCURSEL, 0, 0), text, pat, &count, &mask, &flags)) {
        set_label(win->srch_note, "Type hex bytes (B8 34 12) or some text.");
        return;
    }
    if (!machine_stopped(win)) {
        set_label(win->srch_note, "Stop the PC to search its memory.");
        return;
    }
    from = win->mem_hl ? (backward ? win->mem_hl_low : win->mem_hl_low + 1) : win->mem_top;
    result = msdosdebug_search_next(win->dbg, pat, count, mask, flags, from, backward, &found);
    if (result > 0) {
        char note[40];
        snprintf(note, sizeof note, "Found at %05X", (unsigned)found);
        set_label(win->srch_note, note);
        show_in_memory(win, found, found + (uint32_t)count - 1);
    } else {
        set_label(win->srch_note, result < 0 ? "Stop the PC to search its memory." : "Not found.");
    }
}

/* The disassembly from seg:off, Follow CS:IP off, the line selected. */
static void show_in_disasm(debugger *win, uint16_t seg, uint32_t off, uint32_t linear)
{
    SendMessageA(win->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    win->top_seg = seg;
    win->top_off = off;
    win->selected = linear;
    refresh_disasm(win);
}

static void on_jump(debugger *win)
{
    char buf[64];
    uint16_t seg;
    uint32_t off, lin;
    edit_text(win->jump, buf, sizeof buf);
    if (!parse_addr(win, buf, &seg, &off, &lin)) {
        toast(win, "Not an address (seg:off, cs:ip, a linear address, a symbol)");
        return;
    }
    show_in_disasm(win, seg, off, lin);
}

/* The disassembly at a linear address (a symbol was activated). */
static void jump_to_linear(debugger *win, uint32_t linear)
{
    char text[16];
    uint16_t seg;
    uint32_t off, lin;
    snprintf(text, sizeof text, "%X", (unsigned)linear);
    if (!parse_addr(win, text, &seg, &off, &lin)) return;
    show_in_disasm(win, seg, off, lin);
}

/* An execute breakpoint at an address or symbol, unless it has one already. */
static int break_at_text(debugger *win, const char *text)
{
    char msg[64];
    uint16_t seg;
    uint32_t off, lin;
    if (!parse_addr(win, text, &seg, &off, &lin)) {
        toast(win, "Not an address (seg:off, a linear address, a symbol)");
        return 0;
    }
    if (!msdosdebug_bp_at(win->dbg, lin) && msdosdebug_bp_toggle(win->dbg, lin) < 0) {
        toast(win, "No room for another breakpoint");
        return 0;
    }
    snprintf(msg, sizeof msg, "Breakpoint at %05X", (unsigned)lin);
    toast(win, msg);
    resync(win);
    return 1;
}

static void on_break_at(debugger *win)
{
    char buf[64];
    edit_text(win->break_at, buf, sizeof buf);
    if (break_at_text(win, buf)) SetWindowTextA(win->break_at, "");
}

/* The selected breakpoint row's index, or -1. */
static int bp_selected(debugger *win)
{
    const int idx = (int)SendMessageA(win->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    return idx >= 0 && idx < win->nbps ? idx : -1;
}

static void on_bp_remove(debugger *win)
{
    const int idx = bp_selected(win);
    if (idx < 0) {
        set_label(win->bp_msg, "Select a breakpoint to remove");
        return;
    }
    set_label(win->bp_msg, "");
    msdosdebug_bp_remove(win->dbg, win->bps[idx].id);
    resync(win);
}

/* Execute and memory breakpoints take addresses; port and INT ones
 * numbers. The end is optional; AH only means something for an INT. */
static void on_bp_add(debugger *win)
{
    char start[64], end[64], ahs[32], cond[256], err[160], *start_text, *end_text, *ah_text;
    int type = (int)SendMessageA(win->bp_type, CB_GETCURSEL, 0, 0), ident;
    uint32_t low, high;
    long long ah_value = -1;
    if (type < 0 || type >= MSDOS_BP_TYPES) type = MSDOS_BP_EXEC;
    edit_text(win->bp_start, start, sizeof start);
    edit_text(win->bp_end, end, sizeof end);
    edit_text(win->bp_ah, ahs, sizeof ahs);
    edit_text(win->bp_cond, cond, sizeof cond);
    start_text = strip(start);
    end_text = strip(end);
    ah_text = strip(ahs);
    if (type <= MSDOS_BP_ACCESS) {
        uint16_t seg;
        uint32_t off;
        if (!parse_addr(win, start_text, &seg, &off, &low)) {
            set_label(win->bp_msg, "Not an address (seg:off, a linear address, a symbol)");
            return;
        }
        high = low;
        if (*end_text && !parse_addr(win, end_text, &seg, &off, &high)) {
            set_label(win->bp_msg, "The end is not an address");
            return;
        }
    } else {
        long long val, last;
        if (!parse_num(start_text, &val) || val < 0 || val > (type == MSDOS_BP_INT ? 0xFF : 0xFFFF)) {
            set_label(win->bp_msg, type == MSDOS_BP_INT ? "An interrupt is 00-FF" : "A port is 0000-FFFF");
            return;
        }
        last = val;
        if (*end_text && type != MSDOS_BP_INT && (!parse_num(end_text, &last) || last < val || last > 0xFFFF)) {
            set_label(win->bp_msg, "The last port is a port at or after the first");
            return;
        }
        low = (uint32_t)val;
        high = (uint32_t)last;
        if (type == MSDOS_BP_INT && *ah_text && (!parse_num(ah_text, &ah_value) || ah_value < 0 || ah_value > 0xFF)) {
            set_label(win->bp_msg, "AH is 00-FF, or empty for any");
            return;
        }
    }
    if (high < low) {
        set_label(win->bp_msg, "The end is before the start");
        return;
    }
    err[0] = '\0';
    ident = msdosdebug_bp_add_if(win->dbg, type, low, high, (int)ah_value, strip(cond), err, sizeof err);
    if (ident < 0) {
        set_label(win->bp_msg, err[0] ? err : "That breakpoint could not be set");
        return;
    }
    set_label(win->bp_msg, "");
    SetWindowTextA(win->bp_start, "");
    SetWindowTextA(win->bp_end, "");
    SetWindowTextA(win->bp_ah, "");
    SetWindowTextA(win->bp_cond, "");
    resync(win);
}

/* Replaces the selected breakpoint's condition with the edit box's text. */
static void on_bp_set(debugger *win)
{
    char cond[256], err[160];
    const int idx = bp_selected(win);
    if (idx < 0) {
        set_label(win->bp_msg, "Select a breakpoint to change its condition");
        return;
    }
    edit_text(win->bp_edit, cond, sizeof cond);
    err[0] = '\0';
    if (!msdosdebug_bp_set_condition(win->dbg, win->bps[idx].id, strip(cond), err, sizeof err)) {
        set_label(win->bp_msg, err[0] ? err : "That condition could not be set");
        return;
    }
    set_label(win->bp_msg, "");
    resync(win);
}

/* The type decides what the boxes take. */
static void on_bp_type(debugger *win)
{
    const int type = (int)SendMessageA(win->bp_type, CB_GETCURSEL, 0, 0);
    EnableWindow(win->bp_ah, type == MSDOS_BP_INT);
    EnableWindow(win->bp_end, type != MSDOS_BP_INT);
    SendMessageW(win->bp_start, EM_SETCUEBANNER, TRUE,
                 (LPARAM)(type <= MSDOS_BP_ACCESS ? L"1234:0010 / symbol" : type == MSDOS_BP_INT ? L"21" : L"2F8"));
    SendMessageW(win->bp_end, EM_SETCUEBANNER, TRUE, (LPARAM)(type <= MSDOS_BP_ACCESS ? L"(end)" : L"(last port)"));
}

/* ---- watches ---------------------------------------------------------------------- */

static void add_watch(debugger *win)
{
    char text[128];
    edit_text(win->wt_entry, text, sizeof text);
    if (msdoswatch_add(strip(text)) < 0) {
        toast(win, "Type an address, symbol, expression or a range (B8000..B8FFF)");
        return;
    }
    SetWindowTextA(win->wt_entry, "");
    refresh_watches(win);
}

static int watch_range(debugger *win, uint32_t *low, uint32_t *high)
{
    const int sel = (int)SendMessageA(win->wt_list, LB_GETCURSEL, 0, 0);
    return sel >= 0 && msdoswatch_range(win->dbg, sel, low, high);
}

/* Break on a write that changes the watched bytes. */
static void watch_break(debugger *win, uint32_t low, uint32_t high)
{
    char err[160];
    err[0] = '\0';
    if (msdosdebug_bp_add_if(win->dbg, MSDOS_BP_WRITE, low, high, -1, "value != old", err, sizeof err) >= 0)
        toast(win, "Breaking when it changes");
    else
        toast(win, err[0] ? err : "That breakpoint could not be set");
    resync(win);
}

/* ---- files: symbols and save ------------------------------------------------------- */

static int pick_file(debugger *win, int save, const char *title, const char *filter, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = win->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void on_symbols(debugger *win)
{
    char path[MAX_PATH], seg_text[32], msg[512];
    long long seg = 0;
    if (!pick_file(win, 0, "Load Symbols (MS LINK or Watcom WLINK .map)",
                   "Linker maps (*.map)\0*.map\0All files\0*.*\0\0", path, sizeof path))
        return;
    edit_text(win->sym_seg, seg_text, sizeof seg_text);
    if (!parse_num(seg_text, &seg) || seg < 0 || seg > 0xFFFF) seg = 0;
    msg[0] = '\0';
    if (msdosdebug_load_symbols(win->dbg, path, (uint16_t)seg, msg, sizeof msg) < 0 && !msg[0])
        snprintf(msg, sizeof msg, "No symbols found in %s", base_name(path));
    say(win, msg);
    resync(win);
}

/* The selected symbol's row, or -1. */
static int symbol_selected(debugger *win)
{
    const int idx = (int)SendMessageA(win->sym_list, LB_GETCURSEL, 0, 0);
    return idx >= 0 && idx < win->sym_shown ? idx : -1;
}

static void on_symbol_break(debugger *win)
{
    const int idx = symbol_selected(win);
    char text[16];
    if (idx < 0) {
        toast(win, "Select a symbol first");
        return;
    }
    snprintf(text, sizeof text, "%X", (unsigned)win->sym_addr[idx]);
    break_at_text(win, text);
}

static void on_save(debugger *win, int which)
{
    char path[MAX_PATH], msg[512], title[64];
    snprintf(title, sizeof title, "Save %s", k_save_titles[which]);
    if (!pick_file(win, 1, title, "All files\0*.*\0\0", path, sizeof path)) return;
    msg[0] = '\0';
    msdosdebug_save(win->dbg, k_save_kinds[which], path, msg, sizeof msg);
    if (!msg[0]) snprintf(msg, sizeof msg, "Saved %s", base_name(path));
    say(win, msg);
}

static void save_menu(debugger *win)
{
    HMENU menu = CreatePopupMenu();
    RECT area;
    int idx;
    for (idx = 0; idx < NSAVE; idx++) AppendMenuA(menu, MF_STRING, (UINT_PTR)(IDC_SAVE_KIND0 + idx), k_save_titles[idx]);
    GetWindowRect(win->save_btn, &area);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, area.left, area.bottom, 0, win->hwnd, NULL);
    DestroyMenu(menu);
}

static void on_accept(debugger *win, int ident)
{
    if (ident >= IDC_REG0 && ident <= IDC_REG_LAST) {
        on_reg(win, ident - IDC_REG0, 0);
        return;
    }
    if (ident >= IDC_SREG0 && ident <= IDC_SREG_LAST) {
        on_reg(win, ident - IDC_SREG0, 1);
        return;
    }
    switch (ident) {
    case IDC_CON_IN: on_console(win); break;
    case IDC_MEM_GOTO: on_mem_goto(win); break;
    case IDC_MEM_POKE: on_mem_poke(win); break;
    case IDC_SRCH_ENTRY: do_search(win, 0); break;
    case IDC_JUMP: on_jump(win); break;
    case IDC_BREAK_AT: on_break_at(win); break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_AH:
    case IDC_BP_COND: on_bp_add(win); break;
    case IDC_BP_EDIT: on_bp_set(win); break;
    case IDC_WT_ENTRY: add_watch(win); break;
    case IDC_SYM_FILTER: refresh_symbols(win); break;
    case IDC_SYM_SEG: on_symbols(win); break;
    case IDC_H_RANGE0: hist_refresh(win, 0, 1); break;
    case IDC_H_RANGE1: hist_refresh(win, 1, 1); break;
    default: break;
    }
}

/* ---- the disassembly -------------------------------------------------------------- */

/* Drawn by hand, a line an instruction as the other frontends show it: the
 * breakpoint gutter (a red dot when the breakpoint is enabled, grey when
 * disabled), seg:off, bytes, symbol and instruction. The CS:IP line is filled
 * with the accent (the icon's ink on it), the selected one with the selection
 * colour. */
static void paint_disasm(debugger *win, HWND hwnd, HDC hdc)
{
    RECT area;
    int idx;
    const int gutter = scaled(win, GUTTER_W);
    GetClientRect(hwnd, &area);
    FillRect(hdc, &area, GetSysColorBrush(COLOR_WINDOW));
    SelectObject(hdc, win->mono);
    SetBkMode(hdc, TRANSPARENT);
    if (win->line_count == 0) {
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        TextOutA(hdc, gutter, 0, "(no disassembly)", 16);
        return;
    }
    for (idx = 0; idx < win->line_count && idx * win->line_h < area.bottom; idx++) {
        const msdos_dis_line *line = &win->lines[idx];
        const int pos_y = idx * win->line_h, text_y = pos_y + (win->line_h - win->mono_h) / 2;
        const int sel = (int64_t)line->linear == win->selected, is_pc = idx == win->pc_line;
        COLORREF text_color = GetSysColor(COLOR_WINDOWTEXT);
        RECT row;
        char text[256];
        int len;
        SetRect(&row, 0, pos_y, area.right, pos_y + win->line_h);
        if (is_pc) {
            const int contrast = high_contrast();
            FillRect(hdc, &row, contrast ? GetSysColorBrush(COLOR_HIGHLIGHT) : win->accent);
            text_color = contrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : MSDOS_ACCENT_INK_COLORREF;
        } else if (sel) {
            FillRect(hdc, &row, GetSysColorBrush(COLOR_HIGHLIGHT));
            text_color = GetSysColor(COLOR_HIGHLIGHTTEXT);
        }
        if (win->bp_state[idx]) {
            const int diameter = win->line_h - scaled(win, 6) > 4 ? win->line_h - scaled(win, 6) : 4;
            const int top = pos_y + (win->line_h - diameter) / 2;
            HGDIOBJ old_brush = SelectObject(hdc, win->bp_state[idx] == 1 ? win->bp_on : win->bp_off);
            HGDIOBJ old_pen = SelectObject(hdc, is_pc ? GetStockObject(WHITE_PEN) : GetStockObject(NULL_PEN));
            Ellipse(hdc, scaled(win, 4), top, scaled(win, 4) + diameter + 1, top + diameter + 1);
            SelectObject(hdc, old_pen);
            SelectObject(hdc, old_brush);
        }
        len = snprintf(text, sizeof text, "%04X:%0*X  %-20s %s%s%s", line->seg, line->off > 0xFFFF ? 8 : 4,
                       (unsigned)line->off, line->bytes, line->symbol, line->symbol[0] ? ": " : "", line->text);
        if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
        if (len < 0) len = 0;
        SetTextColor(hdc, text_color);
        TextOutA(hdc, gutter, text_y, text, len);
    }
}

/* A click selects a line (Run to Cursor; it works while the machine runs,
 * too); a click on the gutter, or a double-click, toggles its breakpoint. The
 * wheel and the keys browse by instructions (and turn Follow CS:IP off). */
static LRESULT CALLBACK disasm_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    debugger *win = g_dbg;
    if (!win) return DefWindowProcA(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        RECT area;
        HDC mem;
        HBITMAP bitmap;
        HGDIOBJ old_bitmap, old_font;
        GetClientRect(hwnd, &area);
        mem = CreateCompatibleDC(hdc);
        bitmap = CreateCompatibleBitmap(hdc, area.right > 0 ? area.right : 1, area.bottom > 0 ? area.bottom : 1);
        old_bitmap = SelectObject(mem, bitmap);
        old_font = SelectObject(mem, win->mono);
        paint_disasm(win, hwnd, mem);
        BitBlt(hdc, 0, 0, area.right, area.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old_font);
        SelectObject(mem, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(mem);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint_disasm(win, hwnd, (HDC)wparam);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        /* more or fewer lines fit now */
        if (win->disasm == hwnd && page_visible(win, TAB_DISASM) && msdossession_is_running(win->session))
            refresh_disasm(win);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const int line = win->line_h > 0 ? GET_Y_LPARAM(lparam) / win->line_h : -1;
        const int gutter = GET_X_LPARAM(lparam) < scaled(win, GUTTER_W);
        SetFocus(hwnd);
        if (line < 0 || line >= win->line_count) return 0;
        win->selected = win->lines[line].linear;
        if (gutter ? msg == WM_LBUTTONDOWN : msg == WM_LBUTTONDBLCLK)
            toggle_breakpoint(win, win->lines[line].linear);
        else
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        int rows = -delta * 3 / WHEEL_DELTA;
        if (!rows) rows = delta > 0 ? -1 : 1;
        SendMessageA(win->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        win->top_off = rows > 0 ? rows_on(win, win->top_seg, win->top_off, rows)
                                : rows_back(win, win->top_seg, win->top_off, -rows);
        refresh_disasm(win);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_PRIOR || wparam == VK_NEXT || wparam == VK_UP || wparam == VK_DOWN) {
            const int half = visible_rows(win) / 2 > 0 ? visible_rows(win) / 2 : 1;
            const int rows = wparam == VK_PRIOR ? -half : wparam == VK_NEXT ? half : wparam == VK_UP ? -1 : 1;
            SendMessageA(win->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
            win->top_off = rows > 0 ? rows_on(win, win->top_seg, win->top_off, rows)
                                    : rows_back(win, win->top_seg, win->top_off, -rows);
            refresh_disasm(win);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;

/* Enter in a single-line field means "apply this value"; Tab in the console
 * completes and Up/Down recall. None may beep its way through the default
 * handler. The notifications go to the debugger window, whatever window the
 * field's tab is in. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int ident = (int)GetWindowLongPtrA(hwnd, GWLP_ID);
    HWND main_win = g_dbg ? g_dbg->hwnd : GetAncestor(hwnd, GA_ROOT);
    if (msg == WM_KEYDOWN && wparam == VK_RETURN) {
        PostMessageA(main_win, WM_DBG_ACCEPT, (WPARAM)ident, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wparam == '\r' || wparam == '\n')) return 0;
    if (ident == IDC_CON_IN) {
        if (msg == WM_KEYDOWN && wparam == VK_TAB) {
            PostMessageA(main_win, WM_DBG_TAB, 0, 0);
            return 0;
        }
        if (msg == WM_KEYDOWN && (wparam == VK_UP || wparam == VK_DOWN)) {
            PostMessageA(main_win, WM_DBG_HISTORY, wparam == VK_DOWN, 0);
            return 0;
        }
        if (msg == WM_CHAR && wparam == VK_TAB) return 0;
        if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    }
    return CallWindowProcA(g_edit_proc, hwnd, msg, wparam, lparam);
}

/* ---- construction ----------------------------------------------------------------- */

static HWND child_ex(debugger *win, DWORD ex_style, const char *cls, const char *text, DWORD style, int ident,
                     HFONT font)
{
    HWND hwnd = CreateWindowExA(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                                win->new_parent ? win->new_parent : win->hwnd, (HMENU)(INT_PTR)ident,
                                (HINSTANCE)GetWindowLongPtrA(win->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    return hwnd;
}

static HWND child(debugger *win, const char *cls, const char *text, DWORD style, int ident, HFONT font)
{
    return child_ex(win, 0, cls, text, style, ident, font);
}

/* A read-only monospace text view. */
static HWND view(debugger *win, int ident, DWORD extra)
{
    HWND hwnd = child(win, "EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | extra,
                      ident, win->mono);
    SendMessageA(hwnd, EM_SETLIMITTEXT, 0, 0);
    if (win->nviews < (int)(sizeof win->views / sizeof win->views[0])) win->views[win->nviews++] = hwnd;
    return hwnd;
}

static HWND field(debugger *win, int ident, const wchar_t *cue)
{
    HWND hwnd = child(win, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, ident, win->mono);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    if (cue) SendMessageW(hwnd, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    return hwnd;
}

static HWND label(debugger *win, const char *text, HFONT font, DWORD style)
{
    return child(win, "STATIC", text, style | SS_NOPREFIX, IDC_LABEL_FIRST + win->nlabels++, font);
}

/* A caption or hint, drawn grey. */
static HWND dim_label(debugger *win, const char *text, DWORD style)
{
    HWND hwnd = label(win, text, win->ui_font, style);
    if (win->ndim < (int)(sizeof win->dim / sizeof win->dim[0])) win->dim[win->ndim++] = hwnd;
    return hwnd;
}

static HWND button(debugger *win, const char *text, int ident)
{
    return child(win, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, ident, win->ui_font);
}

static HWND checkbox(debugger *win, const char *text, int ident)
{
    return child(win, "BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, ident, win->ui_font);
}

static HWND combo(debugger *win, int ident)
{
    return child(win, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ident, win->ui_font);
}

/* A list box of rows in the monospace font. */
static HWND list_box(debugger *win, int ident)
{
    return child_ex(win, WS_EX_CLIENTEDGE, "LISTBOX", "",
                    WS_VSCROLL | WS_HSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, ident, win->mono);
}

static void tip(debugger *win, HWND hwnd, const char *text)
{
    TTTOOLINFOA info;
    memset(&info, 0, sizeof info);
    info.cbSize = TTTOOLINFOA_V2_SIZE;
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = win->hwnd;
    info.uId = (UINT_PTR)hwnd;
    info.lpszText = (char *)text;
    SendMessageA(win->tip, TTM_ADDTOOLA, 0, (LPARAM)&info);
}

static void size_columns(debugger *win)
{
    static const int widths[4] = { 70, 520, 340, 80 };
    int idx;
    for (idx = 0; idx < 4; idx++)
        SendMessageA(win->bp_list, LVM_SETCOLUMNWIDTH, (WPARAM)idx, scaled(win, widths[idx]));
}

/* A tab control reads '&' as a mnemonic, so the label's own is doubled. */
static const char *tab_text(int page, char *buf, size_t cap)
{
    const char *src = k_tabs[page].label;
    size_t used = 0;
    for (; *src && used + 3 < cap; src++) {
        buf[used++] = *src;
        if (*src == '&') buf[used++] = '&';
    }
    buf[used] = '\0';
    return buf;
}

static void build_controls(debugger *win)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(win->hwnd, GWLP_HINSTANCE);
    TCITEMA item;
    int idx;

    win->tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, win->hwnd, NULL, inst, NULL);
    SendMessageA(win->tip, TTM_SETMAXTIPWIDTH, 0, scaled(win, 420));

    win->tabs = child(win, WC_TABCONTROLA, "", WS_CLIPSIBLINGS | WS_TABSTOP, IDC_TABS, win->ui_font);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (idx = 0; idx < TAB_COUNT; idx++) {
        char text[40];
        item.pszText = (char *)tab_text(idx, text, sizeof text);
        SendMessageA(win->tabs, TCM_INSERTITEMA, (WPARAM)idx, (LPARAM)&item);
        win->tab_page[idx] = idx;
    }
    win->ntabs = TAB_COUNT;
    for (idx = 0; idx < TAB_COUNT; idx++) {       /* a container per tab: its controls are its children */
        win->pages[idx] = CreateWindowExA(0, PAGE_CLASS, "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10,
                                          win->hwnd, NULL, inst, NULL);
        SetWindowLongPtrA(win->pages[idx], GWLP_USERDATA, (LONG_PTR)win->hwnd);
    }

    /* The toolbar. Run/Stop is owner-drawn so it can wear the accent colour
     * while the machine is stopped. */
    win->run_btn = child(win, "BUTTON", "Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, win->ui_font);
    win->step_btn = button(win, "Step (F7)", IDC_STEP);
    win->over_btn = button(win, "Step Over (F8)", IDC_OVER);
    win->out_btn = button(win, "Step Out (Shift+F8)", IDC_OUT);
    win->run_to_btn = button(win, "Run to Cursor", IDC_RUN_TO);
    win->save_btn = button(win, "Save...", IDC_SAVE);
    win->popout_btn = button(win, "Pop Out", IDC_POPOUT);
    win->help_btn = button(win, "Help", IDC_HELP_BTN);
    win->status = child(win, "STATIC", "", SS_RIGHT | SS_ENDELLIPSIS | SS_NOTIFY | SS_NOPREFIX, IDC_STATUS, win->ui_font);
    if (win->ndim < (int)(sizeof win->dim / sizeof win->dim[0])) win->dim[win->ndim++] = win->status;
    tip(win, win->step_btn, "One instruction");
    tip(win, win->over_btn, "Over a CALL, INT, LOOP or REP, to the instruction after it");
    tip(win, win->out_btn, "On until the routine returns (RET, RETF or IRET)");
    tip(win, win->run_to_btn, "On to the line selected in the Disassembly");
    tip(win, win->popout_btn, "Peel the tab showing off into a window of its own (Dock puts it back)");
    tip(win, win->status, "");

    /* The register strip, under every tab. */
    for (idx = 0; idx < NREGS; idx++) {
        char name[8];
        snprintf(name, sizeof name, "%s", k_regs16[idx]);
        CharUpperA(name);
        win->reg_label[idx] = dim_label(win, name, SS_RIGHT);
        win->reg[idx] = field(win, IDC_REG0 + idx, NULL);
        SendMessageA(win->reg[idx], EM_SETLIMITTEXT, 10, 0);
        tip(win, win->reg[idx], "Enter sets it (hex, # decimal or an expression); only while the PC is stopped");
    }
    for (idx = 0; idx < NSREGS; idx++) {
        char name[8];
        snprintf(name, sizeof name, "%s", k_sregs[idx]);
        CharUpperA(name);
        win->sreg_label[idx] = dim_label(win, name, SS_RIGHT);
        win->sreg[idx] = field(win, IDC_SREG0 + idx, NULL);
        SendMessageA(win->sreg[idx], EM_SETLIMITTEXT, 6, 0);
        tip(win, win->sreg[idx], "Enter sets it (hex); only while the PC is stopped");
        win->sbase[idx] = dim_label(win, "", SS_LEFT);
    }
    for (idx = 0; idx < NFLAGS; idx++) {
        char name[4];
        snprintf(name, sizeof name, "%s", k_flags[idx].name);
        CharUpperA(name);
        win->flag[idx] = checkbox(win, name, IDC_FLAG0 + idx);
    }
    win->cpu_info = dim_label(win, "", SS_LEFT | SS_ENDELLIPSIS);

    /* Console */
    win->new_parent = win->pages[TAB_CONSOLE];
    win->con_out = view(win, IDC_CON_OUT, 0);
    SetWindowTextA(win->con_out, "DEBUG.COM's commands: r d e u g t p, plus the family's: step over out run stop runto "
                                 "bp bpr bpw bpa bpin bpout bpint delete enable disable hits print set mem poke find "
                                 "trace watch symbols map sound video save. Type h for the list; Tab completes, "
                                 "Up/Down recall.\r\n");
    win->con_prompt = child(win, "STATIC", "-", SS_LEFT, IDC_CON_PROMPT, win->mono);
    win->con_in = field(win, IDC_CON_IN, L"u cs:ip   d ds:0   bpw b8000 value != old   bpint 21 9   g");

    /* Disassembly */
    win->new_parent = win->pages[TAB_DISASM];
    win->follow_pc = checkbox(win, "Follow CS:IP", IDC_FOLLOW_PC);
    SendMessageA(win->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    win->jump_label = dim_label(win, "Jump to", SS_LEFT);
    win->jump = field(win, IDC_JUMP, L"F000:FFF0 or _main");
    win->break_label = dim_label(win, "Break at", SS_LEFT);
    win->break_at = field(win, IDC_BREAK_AT, L"0060:0100 or _main");
    win->disasm_hint = dim_label(win, "Click a line to select it (Run to Cursor); click the gutter, double-click or "
                                      "press F9 for a breakpoint; scroll to browse",
                                 SS_LEFT | SS_ENDELLIPSIS);
    win->disasm = CreateWindowExA(WS_EX_CLIENTEDGE, DISASM_CLASS, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                                  win->pages[TAB_DISASM], (HMENU)(INT_PTR)IDC_DISASM, inst, NULL);

    /* Memory: Go to, Poke, the search, the dump */
    win->new_parent = win->pages[TAB_MEMORY];
    win->goto_label = dim_label(win, "Go to", SS_LEFT);
    win->mem_goto = field(win, IDC_MEM_GOTO, L"ds:si / B800:0 / symbol / a-b");
    win->mem_back = button(win, "Back 4K", IDC_MEM_BACK);
    win->mem_on = button(win, "On 4K", IDC_MEM_ON);
    win->poke_label = dim_label(win, "Poke", SS_LEFT);
    win->mem_poke = field(win, IDC_MEM_POKE, L"address value [value...]");
    win->srch_label = dim_label(win, "Search", SS_LEFT);
    win->srch_kind = combo(win, IDC_SRCH_KIND);
    SendMessageA(win->srch_kind, CB_ADDSTRING, 0, (LPARAM)"Hex bytes");
    SendMessageA(win->srch_kind, CB_ADDSTRING, 0, (LPARAM)"Text");
    SendMessageA(win->srch_kind, CB_SETCURSEL, 0, 0);
    win->srch_entry = field(win, IDC_SRCH_ENTRY, L"B8 34 12 / ABC");
    win->srch_next = button(win, "Find Next", IDC_SRCH_NEXT);
    win->srch_prev = button(win, "Find Previous", IDC_SRCH_PREV);
    win->srch_note = dim_label(win, "", SS_LEFT | SS_ENDELLIPSIS);
    win->mem_view = view(win, IDC_MEM_VIEW, WS_HSCROLL | ES_NOHIDESEL);
    win->mem_digits = 6;

    /* Breakpoints */
    win->new_parent = win->pages[TAB_BREAKS];
    win->bp_hint = dim_label(win, "Execute stops before the instruction runs; memory, ports and INT after the "
                                  "instruction that touched them.",
                             SS_LEFT | SS_ENDELLIPSIS);
    win->bp_remove = button(win, "Remove", IDC_BP_REMOVE);
    win->bp_hits = button(win, "Clear Hits", IDC_BP_HITS);
    win->bp_clear = button(win, "Clear All", IDC_BP_CLEAR);
    win->bp_list = child_ex(win, WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                            WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_NOSORTHEADER,
                            IDC_BP_LIST, win->mono);
    SendMessageA(win->bp_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    {
        static const char *const cols[4] = { "#", "Breakpoint", "Condition", "Hits" };
        LVCOLUMNA col;
        memset(&col, 0, sizeof col);
        col.mask = LVCF_TEXT;
        for (idx = 0; idx < 4; idx++) {
            col.pszText = (char *)cols[idx];
            SendMessageA(win->bp_list, LVM_INSERTCOLUMNA, (WPARAM)idx, (LPARAM)&col);
        }
        size_columns(win);
    }
    win->bp_empty = dim_label(win, "No breakpoints. Click the gutter of a disassembly line, or add one below.", SS_CENTER);
    win->edit_label = dim_label(win, "Condition of the selected breakpoint", SS_LEFT);
    win->bp_edit = field(win, IDC_BP_EDIT, L"empty for none, e.g. value != old   (value, old, addr, ax, [ds:si])");
    win->bp_set = button(win, "Set", IDC_BP_SET);
    win->bp_type = combo(win, IDC_BP_TYPE);
    for (idx = 0; idx < MSDOS_BP_TYPES; idx++) SendMessageA(win->bp_type, CB_ADDSTRING, 0, (LPARAM)k_bp_types[idx]);
    SendMessageA(win->bp_type, CB_SETCURSEL, 0, 0);
    win->bp_start = field(win, IDC_BP_START, NULL);
    win->bp_end = field(win, IDC_BP_END, NULL);
    win->bp_ah = field(win, IDC_BP_AH, L"AH (any)");
    win->bp_cond = field(win, IDC_BP_COND, L"condition, e.g. value != old");
    win->bp_add = button(win, "Add", IDC_BP_ADD);
    win->bp_msg = label(win, "", win->ui_font, SS_LEFT | SS_ENDELLIPSIS);
    win->bp_stop = dim_label(win, "", SS_LEFT | SS_ENDELLIPSIS);
    on_bp_type(win);

    /* Watches */
    win->new_parent = win->pages[TAB_WATCHES];
    win->wt_label = dim_label(win, "Watch", SS_LEFT);
    win->wt_entry = field(win, IDC_WT_ENTRY, L"an address, symbol, expression or range (ds:0, _count, B8000..B8FFF, [bx])");
    win->wt_add = button(win, "Add", IDC_WT_ADD);
    win->wt_list = list_box(win, IDC_WT_LIST);
    win->wt_mem = button(win, "Show in Memory", IDC_WT_MEM);
    win->wt_brk = button(win, "Break on Change", IDC_WT_BRK);
    win->wt_remove = button(win, "Remove", IDC_WT_REMOVE);

    /* Machine & I/O */
    win->new_parent = win->pages[TAB_MACHINE];
    win->map_text = view(win, IDC_MAP_TEXT, WS_HSCROLL);
    win->io_text = view(win, IDC_IO_TEXT, WS_HSCROLL);

    /* Trace (0, live) and History (1, when stopped) share one ring */
    for (idx = 0; idx < 2; idx++) {
        win->new_parent = win->pages[TAB_TRACE + idx];
        win->h_rec[idx] = child(win, "BUTTON", "Record", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, IDC_H_REC0 + idx,
                                win->ui_font);
        win->h_filter[idx] = combo(win, IDC_H_FILTER0 + idx);
        SendMessageA(win->h_filter[idx], CB_ADDSTRING, 0, (LPARAM)"All instructions");
        SendMessageA(win->h_filter[idx], CB_ADDSTRING, 0, (LPARAM)"Branches only");
        SendMessageA(win->h_filter[idx], CB_ADDSTRING, 0, (LPARAM)"In range");
        SendMessageA(win->h_filter[idx], CB_SETCURSEL, 0, 0);
        win->h_range[idx] = field(win, IDC_H_RANGE0 + idx, L"range, e.g. F000:E000..F000:FFFF");
        win->h_clear[idx] = button(win, "Clear", IDC_H_CLEAR0 + idx);
        win->h_note[idx] = dim_label(win, "", SS_RIGHT | SS_ENDELLIPSIS);
        win->h_text[idx] = view(win, IDC_H_TEXT0 + idx, WS_HSCROLL);
        win->h_seen[idx] = (uint64_t)-1;
    }

    /* Video: one child holding all its controls (dbg_video.c) */
    win->new_parent = NULL;
    win->video = dbg_video_create(win->pages[TAB_VIDEO], win->ui_font, win->mono, win->dpi, win->session);
    if (win->video) ShowWindow(win->video, SW_SHOW);

    /* Sound */
    win->new_parent = win->pages[TAB_SOUND];
    win->sound_text = view(win, IDC_SOUND_TEXT, WS_HSCROLL);

    /* Symbols */
    win->new_parent = win->pages[TAB_SYMBOLS];
    win->sym_filter_label = dim_label(win, "Filter", SS_LEFT);
    win->sym_filter = field(win, IDC_SYM_FILTER, L"part of a name");
    win->sym_count = dim_label(win, "", SS_LEFT);
    win->sym_break = button(win, "Break at Selected", IDC_SYM_BREAK);
    win->sym_clear = button(win, "Clear Symbols", IDC_SYM_CLEAR);
    win->sym_seg_label = dim_label(win, "Load segment", SS_LEFT);
    win->sym_seg = field(win, IDC_SYM_SEG, L"0000");
    SendMessageA(win->sym_seg, EM_SETLIMITTEXT, 6, 0);
    win->sym_load = button(win, "Load Symbols...", IDC_SYM_LOAD);
    win->sym_list = list_box(win, IDC_SYM_LIST);
    tip(win, win->sym_seg, "The segment the program was loaded at: a .COM's PSP, an .EXE's PSP + 10h. Enter loads.");
    tip(win, win->sym_load, "Labels from an MS LINK or Watcom WLINK .map file, at the load segment");
    tip(win, win->sym_list, "Double-click a symbol to show it in the Disassembly");

    /* Interrupts */
    win->new_parent = win->pages[TAB_INTS];
    win->int_title = label(win, "INT calls", win->ui_bold, SS_LEFT);
    win->int_on = checkbox(win, "Trace", IDC_INT_ON);
    tip(win, win->int_on, "Record every INT the PC runs (costs speed)");
    win->int_clear = button(win, "Clear", IDC_INT_CLEAR);
    win->int_text = view(win, IDC_INT_TEXT, WS_HSCROLL);
    win->ivt_title = label(win, "Vector table", win->ui_bold, SS_LEFT);
    win->ivt_text = view(win, IDC_IVT_TEXT, WS_HSCROLL);
    win->int_count = -1;

    /* FujiNet */
    win->new_parent = win->pages[TAB_FUJINET];
    win->fuji_title = label(win, "FujiBus packets (SLIP-decoded)", win->ui_bold, SS_LEFT);
    win->fuji_note = dim_label(win, "", SS_LEFT | SS_ENDELLIPSIS);
    win->fuji_clear = button(win, "Clear", IDC_FUJI_CLEAR);
    win->fuji_text = view(win, IDC_FUJI_TEXT, WS_HSCROLL);
    win->uart_title = label(win, "FujiNet's UART", win->ui_bold, SS_LEFT);
    win->uart_text = view(win, IDC_UART_TEXT, WS_HSCROLL);
    win->fuji_count = -1;

    /* The tab control was made first, so it sits below the pages' containers
     * already; with WS_CLIPSIBLINGS and the bottom of the z-order a repaint of
     * it never paints its body over a page. */
    win->new_parent = NULL;
    SetWindowPos(win->tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* On screen: the tab the tab control shows, or one living in a window of its own. */
static int page_visible(debugger *win, int page)
{
    return win->popped[page] != NULL || win->page == page;
}

static void show_page(debugger *win, int page)
{
    int idx;
    char ctx[48];
    win->page = page;
    /* Help, F1 and Ctrl+Shift+H open the tab's section */
    snprintf(ctx, sizeof ctx, "debugger/%s", k_tabs[page].name);
    msdos_help_set_context(win->hwnd, ctx, 1);
    for (idx = 0; idx < TAB_COUNT; idx++)     /* a tab's controls follow its container */
        ShowWindow(win->pages[idx], win->popped[idx] || idx == page ? SW_SHOW : SW_HIDE);
    show_bp_list(win);
}

/* The tab control's index for a tab, or -1 when it is in a window of its own. */
static int tab_index(debugger *win, int page)
{
    int idx;
    for (idx = 0; idx < win->ntabs; idx++)
        if (win->tab_page[idx] == page) return idx;
    return -1;
}

static void place(HWND hwnd, int pos_x, int pos_y, int width, int height);
static void layout(debugger *win);

/* Brings a tab to the front: its window if it has one, else the tab control's. */
static void goto_page(debugger *win, int page)
{
    const int idx = tab_index(win, page);
    if (win->popped[page]) {
        SetForegroundWindow(win->popped[page]);
        return;
    }
    if (idx >= 0) {
        SendMessageA(win->tabs, TCM_SETCURSEL, (WPARAM)idx, 0);
        show_page(win, page);
        layout(win);
        refresh_page(win, page, 1);
    }
}

/* ---- peeling a tab off into a window of its own, and putting it back ----------- */

static void dock_page(debugger *win, int page)
{
    TCITEMA item;
    char shown[40];
    HWND pop = win->popped[page];
    int pos = 0, idx;

    if (!pop) return;
    win->popped[page] = NULL;
    SetParent(win->pages[page], win->hwnd);
    for (idx = 0; idx < page; idx++)          /* back where it was in the order */
        if (!win->popped[idx]) pos++;
    for (idx = win->ntabs; idx > pos; idx--) win->tab_page[idx] = win->tab_page[idx - 1];
    win->tab_page[pos] = page;
    win->ntabs++;
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    item.pszText = (char *)tab_text(page, shown, sizeof shown);
    SendMessageA(win->tabs, TCM_INSERTITEMA, (WPARAM)pos, (LPARAM)&item);
    DestroyWindow(pop);
    SendMessageA(win->tabs, TCM_SETCURSEL, (WPARAM)pos, 0);
    show_page(win, page);
    layout(win);
    refresh_page(win, page, 1);
}

static void pop_out(debugger *win, int page)
{
    HWND pop;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(win->hwnd, GWLP_HINSTANCE);
    char title[96], ctx[48];
    int tab_pos, idx, next;

    if (page < 0 || page >= TAB_COUNT || win->popped[page] || win->ntabs <= 1) return;   /* one tab always stays */
    tab_pos = tab_index(win, page);
    if (tab_pos < 0) return;
    SendMessageA(win->tabs, TCM_DELETEITEM, (WPARAM)tab_pos, 0);
    for (idx = tab_pos; idx < win->ntabs - 1; idx++) win->tab_page[idx] = win->tab_page[idx + 1];
    win->ntabs--;

    snprintf(title, sizeof title, "MS-DOS Debugger - %s", k_tabs[page].label);
    pop = CreateWindowExA(0, POP_CLASS, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                          scaled(win, 900), scaled(win, 620), win->hwnd, NULL, inst, NULL);
    SetWindowLongPtrA(pop, GWLP_USERDATA, page);
    snprintf(ctx, sizeof ctx, "debugger/%s", k_tabs[page].name);
    msdos_help_set_context(pop, ctx, 1);       /* F1 in the window opens its tab's section */
    {
        HWND dock = CreateWindowExA(0, "BUTTON", "Dock", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                    scaled(win, 4), scaled(win, 4), scaled(win, 70), scaled(win, 26), pop,
                                    (HMENU)(INT_PTR)IDC_DOCK, inst, NULL);
        SendMessageA(dock, WM_SETFONT, (WPARAM)win->ui_font, TRUE);
    }
    SetParent(win->pages[page], pop);
    win->popped[page] = pop;
    if (page == win->page) {              /* the tab in front left: the one after it comes forward */
        next = tab_pos < win->ntabs ? tab_pos : win->ntabs - 1;
        SendMessageA(win->tabs, TCM_SETCURSEL, (WPARAM)next, 0);
        show_page(win, win->tab_page[next]);
    } else {                              /* another tab left: the one in front stays */
        SendMessageA(win->tabs, TCM_SETCURSEL, (WPARAM)tab_index(win, win->page), 0);
        show_page(win, win->page);
    }
    layout(win);
    ShowWindow(pop, SW_SHOW);
    refresh_page(win, page, 1);
    refresh_page(win, win->page, 1);
}

/* A tab's container: its controls' notifications go to the debugger window,
 * whichever window the container is in. */
static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    HWND main_win = (HWND)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (main_win) return SendMessageA(main_win, msg, wparam, lparam);
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

/* The window a peeled-off tab lives in. */
static LRESULT CALLBACK pop_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int page = (int)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_SIZE:
        if (g_dbg) layout(g_dbg);
        return 0;
    case WM_COMMAND:
        if (g_dbg && LOWORD(wparam) == IDC_DOCK) dock_page(g_dbg, page);
        return 0;
    case WM_CLOSE:
        if (g_dbg) dock_page(g_dbg, page);
        else DestroyWindow(hwnd);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lparam)->ptMinTrackSize.x = MulDiv(420, g_dbg && g_dbg->dpi ? g_dbg->dpi : 96, 96);
        ((MINMAXINFO *)lparam)->ptMinTrackSize.y = MulDiv(300, g_dbg && g_dbg->dpi ? g_dbg->dpi : 96, 96);
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

/* ---- layout ----------------------------------------------------------------------- */

static void place(HWND hwnd, int pos_x, int pos_y, int width, int height)
{
    MoveWindow(hwnd, pos_x, pos_y, width > 1 ? width : 1, height > 1 ? height : 1, TRUE);
}

/* The controls of each tab inside its container. */
static void layout_pages(debugger *win)
{
    const int button_h = scaled(win, 26), edit_h = scaled(win, 24), label_h = scaled(win, 18), gap = scaled(win, 6);
    RECT area;
    int page, width, height, pos_x, pos_y, idx, item_w;

    for (page = 0; page < TAB_COUNT; page++) {
        if (!win->pages[page]) continue;
        GetClientRect(win->pages[page], &area);
        width = area.right;
        height = area.bottom;
        if (width < 40 || height < 40) continue;
        switch (page) {
        case TAB_CONSOLE:
            item_w = win->mono_w + scaled(win, 6);
            place(win->con_out, 0, 0, width, height - edit_h - scaled(win, 8));
            place(win->con_prompt, 0, height - edit_h + (edit_h - win->mono_h) / 2, item_w, win->mono_h);
            place(win->con_in, item_w + scaled(win, 2), height - edit_h, width - item_w - scaled(win, 2), edit_h);
            break;
        case TAB_DISASM:
            item_w = text_w(win, win->ui_font, "Follow CS:IP") + scaled(win, 24);
            place(win->follow_pc, 0, scaled(win, 1), item_w, scaled(win, 22));
            pos_x = item_w + scaled(win, 12);
            item_w = text_w(win, win->ui_font, "Jump to") + scaled(win, 6);
            place(win->jump_label, pos_x, (edit_h - label_h) / 2, item_w, label_h);
            pos_x += item_w;
            place(win->jump, pos_x, 0, scaled(win, 150), edit_h);
            pos_x += scaled(win, 162);
            item_w = text_w(win, win->ui_font, "Break at") + scaled(win, 6);
            place(win->break_label, pos_x, (edit_h - label_h) / 2, item_w, label_h);
            pos_x += item_w;
            place(win->break_at, pos_x, 0, scaled(win, 150), edit_h);
            pos_x += scaled(win, 162);
            place(win->disasm_hint, pos_x, (edit_h - label_h) / 2, width - pos_x, label_h);
            place(win->disasm, 0, edit_h + scaled(win, 6), width, height - edit_h - scaled(win, 6));
            break;
        case TAB_MEMORY:
            pos_x = 0;
            item_w = text_w(win, win->ui_font, "Go to") + scaled(win, 6);
            place(win->goto_label, pos_x, (edit_h - label_h) / 2, item_w, label_h);
            pos_x += item_w;
            place(win->mem_goto, pos_x, 0, scaled(win, 190), edit_h);
            pos_x += scaled(win, 196);
            item_w = btn_w(win, "Back 4K");
            place(win->mem_back, pos_x, -scaled(win, 1), item_w, button_h);
            pos_x += item_w + scaled(win, 4);
            item_w = btn_w(win, "On 4K");
            place(win->mem_on, pos_x, -scaled(win, 1), item_w, button_h);
            pos_x += item_w + scaled(win, 24);
            item_w = text_w(win, win->ui_font, "Poke") + scaled(win, 6);
            place(win->poke_label, pos_x, (edit_h - label_h) / 2, item_w, label_h);
            pos_x += item_w;
            place(win->mem_poke, pos_x, 0, width - pos_x > scaled(win, 120) ? (width - pos_x < scaled(win, 300) ? width - pos_x : scaled(win, 300)) : scaled(win, 120), edit_h);
            pos_y = button_h + gap;
            pos_x = 0;
            item_w = text_w(win, win->ui_font, "Search") + scaled(win, 6);
            place(win->srch_label, pos_x, pos_y + (edit_h - label_h) / 2, item_w, label_h);
            pos_x += item_w;
            place(win->srch_kind, pos_x, pos_y, scaled(win, 110), scaled(win, 200));
            pos_x += scaled(win, 116);
            place(win->srch_entry, pos_x, pos_y, scaled(win, 230), edit_h);
            pos_x += scaled(win, 236);
            item_w = btn_w(win, "Find Next");
            place(win->srch_next, pos_x, pos_y - scaled(win, 1), item_w, button_h);
            pos_x += item_w + scaled(win, 4);
            item_w = btn_w(win, "Find Previous");
            place(win->srch_prev, pos_x, pos_y - scaled(win, 1), item_w, button_h);
            pos_x += item_w + scaled(win, 12);
            place(win->srch_note, pos_x, pos_y + (edit_h - label_h) / 2, width - pos_x, label_h);
            pos_y += button_h + gap;
            place(win->mem_view, 0, pos_y, width, height - pos_y);
            break;
        case TAB_BREAKS: {
            const int stop_y = height - label_h, msg_y = stop_y - label_h - scaled(win, 2);
            const int add_y = msg_y - scaled(win, 8) - edit_h, edit_y = add_y - scaled(win, 8) - edit_h;
            const int list_y = button_h + gap;
            const int clear_w = btn_w(win, "Clear All"), hits_w = btn_w(win, "Clear Hits"), remove_w = btn_w(win, "Remove");
            place(win->bp_clear, width - clear_w, 0, clear_w, button_h);
            place(win->bp_hits, width - clear_w - gap - hits_w, 0, hits_w, button_h);
            place(win->bp_remove, width - clear_w - hits_w - 2 * gap - remove_w, 0, remove_w, button_h);
            place(win->bp_hint, 0, (button_h - label_h) / 2, width - clear_w - hits_w - remove_w - 4 * gap, label_h);
            place(win->bp_list, 0, list_y, width, edit_y - gap - list_y);
            place(win->bp_empty, 0, list_y + scaled(win, 12), width, label_h);
            /* the selected breakpoint's condition */
            item_w = text_w(win, win->ui_font, "Condition of the selected breakpoint") + scaled(win, 8);
            place(win->edit_label, 0, edit_y + (edit_h - label_h) / 2, item_w, label_h);
            pos_x = btn_w(win, "Set");
            place(win->bp_edit, item_w, edit_y, width - item_w - pos_x - gap, edit_h);
            place(win->bp_set, width - pos_x, edit_y - scaled(win, 1), pos_x, button_h);
            /* a new breakpoint: type, start, end, AH, condition, Add */
            pos_x = 0;
            place(win->bp_type, pos_x, add_y, scaled(win, 150), scaled(win, 240));
            pos_x += scaled(win, 156);
            place(win->bp_start, pos_x, add_y, scaled(win, 170), edit_h);
            pos_x += scaled(win, 176);
            place(win->bp_end, pos_x, add_y, scaled(win, 110), edit_h);
            pos_x += scaled(win, 116);
            place(win->bp_ah, pos_x, add_y, scaled(win, 80), edit_h);
            pos_x += scaled(win, 86);
            item_w = btn_w(win, "Add");
            place(win->bp_cond, pos_x, add_y, width - pos_x - item_w - gap, edit_h);
            place(win->bp_add, width - item_w, add_y - scaled(win, 1), item_w, button_h);
            place(win->bp_msg, 0, msg_y, width, label_h);
            place(win->bp_stop, 0, stop_y, width, label_h);
            break;
        }
        case TAB_WATCHES: {
            const int bottom = height - button_h;
            item_w = text_w(win, win->ui_font, "Watch") + scaled(win, 6);
            place(win->wt_label, 0, (edit_h - label_h) / 2, item_w, label_h);
            idx = btn_w(win, "Add");
            place(win->wt_entry, item_w, 0, width - item_w - idx - gap, edit_h);
            place(win->wt_add, width - idx, -scaled(win, 1), idx, button_h);
            place(win->wt_list, 0, button_h + gap, width, bottom - button_h - 2 * gap);
            pos_x = 0;
            item_w = btn_w(win, "Show in Memory");
            place(win->wt_mem, pos_x, bottom, item_w, button_h);
            pos_x += item_w + gap;
            item_w = btn_w(win, "Break on Change");
            place(win->wt_brk, pos_x, bottom, item_w, button_h);
            pos_x += item_w + gap;
            item_w = btn_w(win, "Remove");
            place(win->wt_remove, pos_x, bottom, item_w, button_h);
            break;
        }
        case TAB_MACHINE: {
            const int left = (width - gap) * 9 / 20;
            place(win->map_text, 0, 0, left, height);
            place(win->io_text, left + gap, 0, width - left - gap, height);
            break;
        }
        case TAB_TRACE:
        case TAB_HISTORY:
            idx = page - TAB_TRACE;
            pos_x = 0;
            item_w = btn_w(win, "Record");
            place(win->h_rec[idx], pos_x, 0, item_w, button_h);
            pos_x += item_w + gap;
            place(win->h_filter[idx], pos_x, 1, scaled(win, 150), scaled(win, 200));
            pos_x += scaled(win, 156);
            place(win->h_range[idx], pos_x, 1, scaled(win, 230), edit_h);
            pos_x += scaled(win, 236);
            item_w = btn_w(win, "Clear");
            place(win->h_clear[idx], pos_x, 0, item_w, button_h);
            pos_x += item_w + gap;
            place(win->h_note[idx], pos_x, (button_h - label_h) / 2, width - pos_x - scaled(win, 6), label_h);
            place(win->h_text[idx], 0, button_h + gap, width, height - button_h - gap);
            break;
        case TAB_VIDEO:
            if (win->video) dbg_video_place(win->video, 0, 0, width, height);
            break;
        case TAB_SOUND:
            place(win->sound_text, 0, 0, width, height);
            break;
        case TAB_SYMBOLS: {
            const int clear_w = btn_w(win, "Clear Symbols"), break_w = btn_w(win, "Break at Selected");
            item_w = text_w(win, win->ui_font, "Filter") + scaled(win, 6);
            place(win->sym_filter_label, 0, (edit_h - label_h) / 2, item_w, label_h);
            place(win->sym_filter, item_w, 0, scaled(win, 240), edit_h);
            place(win->sym_count, item_w + scaled(win, 250), (edit_h - label_h) / 2, scaled(win, 240), label_h);
            place(win->sym_clear, width - clear_w, -scaled(win, 1), clear_w, button_h);
            place(win->sym_break, width - clear_w - gap - break_w, -scaled(win, 1), break_w, button_h);
            pos_y = button_h + gap;
            item_w = text_w(win, win->ui_font, "Load segment") + scaled(win, 6);
            place(win->sym_seg_label, 0, pos_y + (edit_h - label_h) / 2, item_w, label_h);
            place(win->sym_seg, item_w, pos_y, win->mono_w * 6 + scaled(win, 10), edit_h);
            pos_x = item_w + win->mono_w * 6 + scaled(win, 10) + gap;
            item_w = btn_w(win, "Load Symbols...");
            place(win->sym_load, pos_x, pos_y - scaled(win, 1), item_w, button_h);
            pos_y += button_h + gap;
            place(win->sym_list, 0, pos_y, width, height - pos_y);
            break;
        }
        case TAB_INTS: {
            /* the trace left, the vector table right */
            const int needed = win->mono_w * 58 + scaled(win, 30);      /* four vectors a row */
            const int right_w = width - needed >= scaled(win, 300) ? needed : width / 2;
            const int left_w = width - right_w - scaled(win, 12);
            const int clear_w = btn_w(win, "Clear"), trace_w = text_w(win, win->ui_font, "Trace") + scaled(win, 26);
            place(win->int_clear, left_w - clear_w, 0, clear_w, button_h);
            place(win->int_on, left_w - clear_w - gap - trace_w, (button_h - scaled(win, 22)) / 2, trace_w, scaled(win, 22));
            place(win->int_title, 0, (button_h - label_h) / 2, left_w - clear_w - trace_w - 2 * gap, label_h);
            place(win->int_text, 0, button_h + gap, left_w, height - button_h - gap);
            place(win->ivt_title, left_w + scaled(win, 12), (button_h - label_h) / 2, right_w, label_h);
            place(win->ivt_text, left_w + scaled(win, 12), button_h + gap, right_w, height - button_h - gap);
            break;
        }
        case TAB_FUJINET: {
            /* the packets left (a note and Clear over them), the UART right */
            const int right_w = width / 3 > scaled(win, 320) ? scaled(win, 320) : width / 3 > scaled(win, 220) ? width / 3 : scaled(win, 220);
            const int left_w = width - right_w - scaled(win, 12);
            const int clear_w = btn_w(win, "Clear");
            place(win->fuji_title, 0, 0, left_w, label_h);
            place(win->fuji_clear, left_w - clear_w, label_h + scaled(win, 4), clear_w, button_h);
            place(win->fuji_note, 0, label_h + scaled(win, 4) + (button_h - label_h) / 2, left_w - clear_w - gap, label_h);
            place(win->fuji_text, 0, label_h + button_h + scaled(win, 10), left_w, height - label_h - button_h - scaled(win, 10));
            place(win->uart_title, left_w + scaled(win, 12), 0, right_w, label_h);
            place(win->uart_text, left_w + scaled(win, 12), label_h + scaled(win, 4), right_w, height - label_h - scaled(win, 4));
            break;
        }
        default: break;
        }
    }
}

/* The register strip's height: three rows. */
static int strip_height(const debugger *win)
{
    return 3 * (scaled(win, 24) + scaled(win, 5)) + scaled(win, 6);
}

static void layout(debugger *win)
{
    RECT client, page_rect;
    int pos_x, pos_y, width, idx, page_x, page_y, page_w, page_h, strip_y, tab_y;
    const int button_h = scaled(win, 26), edit_h = scaled(win, 24), label_h = scaled(win, 18), gap = scaled(win, 6);

    if (!win->tabs) return;
    GetClientRect(win->hwnd, &client);

    /* The toolbar: stepping, then save and pop out, the status after them.
     * Too narrow for the status beside them, it goes on a line of its own. */
    {
        HWND bar[7] = { win->run_btn, win->step_btn, win->over_btn, win->out_btn, win->run_to_btn, win->save_btn,
                        win->popout_btn };
        const char *btext[7] = { "Stop (F5)", "Step (F7)", "Step Over (F8)", "Step Out (Shift+F8)", "Run to Cursor",
                                 "Save...", "Pop Out" };
        const int help_w = btn_w(win, "Help");
        const int status_right = client.right - help_w - 3 * gap;
        pos_x = scaled(win, 8);
        for (idx = 0; idx < 7; idx++) {
            width = btn_w(win, btext[idx]);
            place(bar[idx], pos_x, scaled(win, 8), width, button_h);
            pos_x += width + gap + (idx == 4 ? scaled(win, 14) : 0);
        }
        pos_y = scaled(win, 8) + button_h + scaled(win, 8);
        place(win->help_btn, client.right - scaled(win, 8) - help_w, scaled(win, 8), help_w, button_h);
        if (status_right - scaled(win, 8) - pos_x >= scaled(win, 220)) {
            place(win->status, pos_x, scaled(win, 8) + (button_h - label_h) / 2, status_right - scaled(win, 2) - pos_x, label_h);
        } else {
            place(win->status, scaled(win, 8), pos_y - scaled(win, 2), client.right - scaled(win, 16), label_h);
            pos_y += label_h + scaled(win, 4);
        }
        tab_y = pos_y;
    }

    /* The register strip, under every tab: the general registers / the
     * segment registers with their bases / the flags and the CPU. */
    strip_y = client.bottom - strip_height(win);
    {
        const int row_h = edit_h + scaled(win, 5);
        const int reg_w = win->mono_w * 8 + scaled(win, 10), sreg_w = win->mono_w * 4 + scaled(win, 10);
        const int reg_cell = (client.right - scaled(win, 16) + scaled(win, 8)) / NREGS;
        const int sreg_cell = (client.right - scaled(win, 16) + scaled(win, 8)) / NSREGS;
        const int name_w = text_w(win, win->ui_font, "WWW") + scaled(win, 2);
        page_x = scaled(win, 12);
        pos_y = strip_y + scaled(win, 3);
        for (idx = 0; idx < NREGS; idx++) {
            const int label_w = reg_cell - reg_w - scaled(win, 12) > name_w ? reg_cell - reg_w - scaled(win, 12) : name_w;
            place(win->reg_label[idx], page_x + idx * reg_cell, pos_y + (edit_h - label_h) / 2, label_w, label_h);
            place(win->reg[idx], page_x + idx * reg_cell + label_w + scaled(win, 4), pos_y, reg_w, edit_h);
        }
        pos_y += row_h;
        for (idx = 0; idx < NSREGS; idx++) {
            const int label_w = text_w(win, win->ui_font, "WW") + scaled(win, 6);
            place(win->sreg_label[idx], page_x + idx * sreg_cell, pos_y + (edit_h - label_h) / 2, label_w, label_h);
            place(win->sreg[idx], page_x + idx * sreg_cell + label_w + scaled(win, 4), pos_y, sreg_w, edit_h);
            place(win->sbase[idx], page_x + idx * sreg_cell + label_w + sreg_w + scaled(win, 10),
                  pos_y + (edit_h - label_h) / 2, sreg_cell - label_w - sreg_w - scaled(win, 18), label_h);
        }
        pos_y += row_h;
        pos_x = page_x;
        for (idx = 0; idx < NFLAGS; idx++) {
            place(win->flag[idx], pos_x, pos_y + scaled(win, 1), scaled(win, 44), scaled(win, 22));
            pos_x += scaled(win, 48);
        }
        place(win->cpu_info, pos_x + scaled(win, 16), pos_y + (edit_h - label_h) / 2,
              client.right - pos_x - scaled(win, 16) - page_x, label_h);
    }

    /* The tab control is the strip alone: the pages sit on the window's own
     * background, which their captions are drawn on. */
    SetRect(&page_rect, 0, 0, scaled(win, 400), scaled(win, 400));
    SendMessageA(win->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page_rect);
    place(win->tabs, scaled(win, 8), tab_y, client.right - scaled(win, 16), page_rect.top);
    page_x = scaled(win, 12);
    page_y = tab_y + page_rect.top + scaled(win, 6);
    page_w = client.right - 2 * page_x;
    page_h = strip_y - scaled(win, 6) - page_y;
    if (page_w < scaled(win, 300)) page_w = scaled(win, 300);
    if (page_h < scaled(win, 160)) page_h = scaled(win, 160);

    /* each tab's container: in the tab control's page, or filling the window it was peeled off into */
    for (idx = 0; idx < TAB_COUNT; idx++) {
        if (win->popped[idx]) {
            RECT pop_rect;
            GetClientRect(win->popped[idx], &pop_rect);
            MoveWindow(win->pages[idx], scaled(win, 6), scaled(win, 34),
                       pop_rect.right > scaled(win, 12) ? pop_rect.right - scaled(win, 12) : 1,
                       pop_rect.bottom > scaled(win, 40) ? pop_rect.bottom - scaled(win, 40) : 1, TRUE);
        } else {
            MoveWindow(win->pages[idx], page_x, page_y, page_w, page_h, TRUE);
        }
    }
    layout_pages(win);

    /* everything again: what the controls left behind where they were */
    RedrawWindow(win->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* ---- the window ------------------------------------------------------------------- */

static void hide_window(debugger *win)
{
    KillTimer(win->hwnd, TIMER_REFRESH);
    msdosdebug_trace_enable(win->dbg, 0);        /* the ring costs the interpreter: only while a window wants it */
    msdosdebug_history_enable(win->dbg, 0);
    msdosdebug_detach(win->dbg);                 /* lets it run on */
    win->attached_running = 0;
    ShowWindow(win->hwnd, SW_HIDE);
}

static void draw_run_button(debugger *win, const DRAWITEMSTRUCT *item)
{
    char text[32];
    const int stopped = machine_stopped(win);
    RECT row = item->rcItem;
    if (stopped) {
        const int contrast = high_contrast();
        FillRect(item->hDC, &row, contrast ? GetSysColorBrush(COLOR_HIGHLIGHT) : win->accent);
        FrameRect(item->hDC, &row, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(item->hDC, contrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : MSDOS_ACCENT_INK_COLORREF);
    } else {
        DrawFrameControl(item->hDC, &row, DFC_BUTTON,
                         DFCS_BUTTONPUSH | ((item->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(item->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(item->hDC, TRANSPARENT);
    GetWindowTextA(item->hwndItem, text, sizeof text);
    SelectObject(item->hDC, win->ui_font);
    DrawTextA(item->hDC, text, -1, &row, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (item->itemState & ODS_FOCUS) {
        InflateRect(&row, -3, -3);
        DrawFocusRect(item->hDC, &row);
    }
}

static LRESULT on_notify(debugger *win, NMHDR *header)
{
    if (header->hwndFrom == win->tabs && header->code == (UINT)TCN_SELCHANGE) {
        const int idx = (int)SendMessageA(win->tabs, TCM_GETCURSEL, 0, 0);
        if (idx >= 0 && idx < win->ntabs) {
            show_page(win, win->tab_page[idx]);
            refresh_page(win, win->tab_page[idx], 1);
        }
        return 0;
    }
    if (header->code == (UINT)NM_CUSTOMDRAW && (header->hwndFrom == win->h_rec[0] || header->hwndFrom == win->h_rec[1]))
        return msdos_accent_customdraw((const NMCUSTOMDRAW *)header,
                                       SendMessageA(header->hwndFrom, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (header->hwndFrom != win->bp_list) return 0;
    if (header->code == (UINT)LVN_ITEMCHANGED) {
        const NMLISTVIEW *notify = (const NMLISTVIEW *)header;
        if (win->updating || notify->iItem < 0 || notify->iItem >= win->nbps || !(notify->uChanged & LVIF_STATE)) return 0;
        /* a row's box: its breakpoint on or off (the list follows after) */
        if (((notify->uNewState ^ notify->uOldState) & LVIS_STATEIMAGEMASK) && (notify->uOldState & LVIS_STATEIMAGEMASK)) {
            const int enabled_now = ((notify->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
            if (enabled_now != (win->bps[notify->iItem].enabled != 0)) {
                msdosdebug_bp_enable(win->dbg, win->bps[notify->iItem].id, enabled_now);
                PostMessageA(win->hwnd, WM_DBG_SYNC, 0, 0);
            }
        }
        /* a row picked: its condition is the edit box's */
        if ((notify->uNewState & LVIS_SELECTED) && !(notify->uOldState & LVIS_SELECTED)) {
            win->edit_id = win->bps[notify->iItem].id;
            SetWindowTextA(win->bp_edit, win->bps[notify->iItem].condition);
        }
    } else if (header->code == (UINT)LVN_KEYDOWN) {
        /* after the list's own handling: removing rebuilds it */
        if (((const NMLVKEYDOWN *)header)->wVKey == VK_DELETE)
            PostMessageA(win->hwnd, WM_COMMAND, MAKEWPARAM(IDC_BP_REMOVE, BN_CLICKED), 0);
    } else if (header->code == (UINT)NM_DBLCLK) {
        /* an address breakpoint's row opens the disassembly there */
        const int idx = bp_selected(win);
        if (idx >= 0 && win->bps[idx].type <= MSDOS_BP_ACCESS) {
            goto_page(win, TAB_DISASM);
            jump_to_linear(win, win->bps[idx].start);
        }
    }
    return 0;
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    debugger *win = g_dbg;
    if (msg == WM_GETMINMAXINFO) {
        const int dpi = win && win->dpi ? win->dpi : 96;
        ((MINMAXINFO *)lparam)->ptMinTrackSize.x = MulDiv(1100, dpi, 96);
        ((MINMAXINFO *)lparam)->ptMinTrackSize.y = MulDiv(640, dpi, 96);
        return 0;
    }
    if (!win || win->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wparam, lparam);

    switch (msg) {
    case WM_SIZE:
        layout(win);
        return 0;
    case WM_DPICHANGED: {
        const RECT *rect = (const RECT *)lparam;
        win->dpi = HIWORD(wparam);
        make_fonts(win);
        size_columns(win);
        SendMessageA(win->tip, TTM_SETMAXTIPWIDTH, 0, scaled(win, 420));
        SetWindowPos(hwnd, NULL, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout(win);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wparam == TIMER_REFRESH) tick(win);
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(win, (int)wparam);
        return 0;
    case WM_DBG_TAB:
        complete_console(win);
        return 0;
    case WM_DBG_HISTORY:
        history_step(win, (int)wparam);
        return 0;
    case WM_DBG_SYNC:
        resync(win);
        return 0;
    case WM_NOTIFY:
        return on_notify(win, (NMHDR *)lparam);
    case WM_DRAWITEM:
        if (wparam == IDC_RUN) {
            draw_run_button(win, (const DRAWITEMSTRUCT *)lparam);
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HWND ctl = (HWND)lparam;
        HDC hdc = (HDC)wparam;
        if (in_list(ctl, win->views, win->nviews)) {
            /* read-only views on the window colour, as text views are */
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        if (ctl == win->bp_msg) {
            SetTextColor(hdc, RGB(0xc0, 0x1c, 0x28));
            SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (ctl == win->status && notice_showing(win)) {
            SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
            SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (in_list(ctl, win->dim, win->ndim)) {
            SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        /* the disassembly scrolls under the pointer, focused or not */
        POINT where = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        RECT area;
        GetWindowRect(win->disasm, &area);
        if (page_visible(win, TAB_DISASM) && PtInRect(&area, where)) return SendMessageA(win->disasm, msg, wparam, lparam);
        break;
    }
    case WM_COMMAND: {
        const int ident = LOWORD(wparam);
        if (ident >= IDC_FLAG0 && ident <= IDC_FLAG_LAST && HIWORD(wparam) == BN_CLICKED) {
            on_flag(win, ident - IDC_FLAG0);
            return 0;
        }
        if (ident >= IDC_SAVE_KIND0 && ident <= IDC_SAVE_KIND_LAST) {
            on_save(win, ident - IDC_SAVE_KIND0);
            return 0;
        }
        switch (ident) {
        case IDC_RUN: on_run(win); return 0;
        case IDC_STEP: msdosdebug_step(win->dbg); settle_and_sync(win, 1); return 0;
        case IDC_OVER: msdosdebug_step_over(win->dbg); settle_and_sync(win, 1); return 0;
        case IDC_OUT: msdosdebug_step_out(win->dbg); settle_and_sync(win, 1); return 0;
        case IDC_RUN_TO: on_run_to(win); return 0;
        case IDC_TOGGLE_BP: on_toggle_bp(win); return 0;
        case IDC_HELP_BTN: msdos_help_for(win->hwnd); return 0;
        case IDC_SAVE: save_menu(win); return 0;
        case IDC_POPOUT: pop_out(win, win->page); return 0;
        case IDC_SYM_LOAD: on_symbols(win); return 0;
        case IDC_SYM_BREAK: on_symbol_break(win); return 0;
        case IDC_SYM_CLEAR:
            msdosdebug_clear_symbols(win->dbg);
            say(win, "Symbols cleared");
            resync(win);
            return 0;
        case IDC_SYM_FILTER:
            if (HIWORD(wparam) == EN_CHANGE) refresh_symbols(win);       /* the list follows the typing */
            return 0;
        case IDC_SYM_LIST:
            if (HIWORD(wparam) == LBN_DBLCLK) {
                const int idx = symbol_selected(win);
                if (idx >= 0) {
                    goto_page(win, TAB_DISASM);
                    jump_to_linear(win, win->sym_addr[idx]);
                }
            }
            return 0;
        case IDC_MEM_BACK: on_mem_page(win, -1); return 0;
        case IDC_MEM_ON: on_mem_page(win, 1); return 0;
        case IDC_SRCH_NEXT: do_search(win, 0); return 0;
        case IDC_SRCH_PREV: do_search(win, 1); return 0;
        case IDC_FOLLOW_PC: refresh_disasm(win); return 0;
        case IDC_BP_TYPE:
            if (HIWORD(wparam) == CBN_SELCHANGE) on_bp_type(win);
            return 0;
        case IDC_BP_ADD: on_bp_add(win); return 0;
        case IDC_BP_SET: on_bp_set(win); return 0;
        case IDC_BP_REMOVE: on_bp_remove(win); return 0;
        case IDC_BP_HITS:
            msdosdebug_bp_clear_hits(win->dbg);
            resync(win);
            return 0;
        case IDC_BP_CLEAR:
            msdosdebug_bp_clear(win->dbg);
            set_label(win->bp_msg, "");
            resync(win);
            return 0;
        case IDC_WT_ADD: add_watch(win); return 0;
        case IDC_WT_MEM: {
            uint32_t low, high;
            if (watch_range(win, &low, &high)) {
                goto_page(win, TAB_MEMORY);
                show_in_memory(win, low, high);
            } else {
                toast(win, "Select a watch that resolves to an address");
            }
            return 0;
        }
        case IDC_WT_BRK: {
            uint32_t low, high;
            if (watch_range(win, &low, &high)) watch_break(win, low, high);
            else toast(win, "Select a watch that resolves to an address");
            return 0;
        }
        case IDC_WT_REMOVE: {
            const int sel = (int)SendMessageA(win->wt_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0) {
                msdoswatch_remove(sel);
                refresh_watches(win);
            }
            return 0;
        }
        case IDC_H_REC0:
            msdosdebug_trace_enable(win->dbg, SendMessageA((HWND)lparam, BM_GETCHECK, 0, 0) == BST_CHECKED);
            hist_refresh(win, 0, 1);
            return 0;
        case IDC_H_REC1:
            msdosdebug_history_enable(win->dbg, SendMessageA((HWND)lparam, BM_GETCHECK, 0, 0) == BST_CHECKED);
            hist_refresh(win, 1, 1);
            return 0;
        case IDC_H_CLEAR0:
            msdosdebug_trace_clear(win->dbg);
            hist_refresh(win, 0, 1);
            return 0;
        case IDC_H_CLEAR1:
            msdosdebug_history_clear(win->dbg);
            hist_refresh(win, 1, 1);
            return 0;
        case IDC_H_FILTER0:
        case IDC_H_FILTER1:
            if (HIWORD(wparam) == CBN_SELCHANGE) hist_refresh(win, ident == IDC_H_FILTER1, 1);
            return 0;
        case IDC_INT_ON:
            if (HIWORD(wparam) == BN_CLICKED && !win->updating) {
                msdosdebug_int_trace_enable(win->dbg, SendMessageA(win->int_on, BM_GETCHECK, 0, 0) == BST_CHECKED);
                refresh_ints(win, 1);
            }
            return 0;
        case IDC_INT_CLEAR:
            msdosdebug_int_trace_clear(win->dbg);
            refresh_ints(win, 1);
            return 0;
        case IDC_FUJI_CLEAR:
            msdosdebug_fujinet_trace_clear(win->dbg);
            refresh_fujinet(win, 1);
            return 0;
        case IDCANCEL: hide_window(win); return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide and let the machine run; Ctrl+Shift+D brings it back, stopped. */
        hide_window(win);
        return 0;
    case WM_DESTROY: {
        int idx;
        KillTimer(hwnd, TIMER_REFRESH);
        msdosdebug_trace_enable(win->dbg, 0);
        msdosdebug_history_enable(win->dbg, 0);
        msdosdebug_detach(win->dbg);
        for (idx = 0; idx < TAB_COUNT; idx++)     /* a peeled-off tab's window goes with the debugger */
            if (win->popped[idx]) {
                HWND pop = win->popped[idx];
                win->popped[idx] = NULL;
                DestroyWindow(pop);
            }
        DeleteObject(win->ui_font);
        DeleteObject(win->ui_bold);
        DeleteObject(win->mono);
        DeleteObject(win->accent);
        DeleteObject(win->bp_on);
        DeleteObject(win->bp_off);
        DestroyAcceleratorTable(win->accel);
        free(win);
        g_dbg = NULL;
        return 0;
    }
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

/* ---- entry points ----------------------------------------------------------------- */

static void register_class(HINSTANCE inst, const char *name, WNDPROC proc, HBRUSH background, UINT style)
{
    WNDCLASSEXA wnd_class;
    memset(&wnd_class, 0, sizeof wnd_class);
    wnd_class.cbSize = sizeof wnd_class;
    wnd_class.style = style;
    wnd_class.lpfnWndProc = proc;
    wnd_class.hInstance = inst;
    wnd_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    wnd_class.hbrBackground = background;
    wnd_class.lpszClassName = name;
    if (proc == dbg_proc || proc == pop_proc) wnd_class.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(101));
    RegisterClassExA(&wnd_class);
}

/* A tab named by an environment variable: a number, or a tab's name or label
 * (any case), or the start of one, or of a word of a label ("memory",
 * "i/o"...), or one of the old names. -1 if unset or unknown. */
static int tab_from_env_var(const char *var)
{
    const char *tab = getenv(var);
    char *end;
    long num;
    size_t len;
    int idx, pass;
    if (!tab || !*tab) return -1;
    for (idx = 0; idx < (int)(sizeof k_tab_aliases / sizeof k_tab_aliases[0]); idx++)
        if (_stricmp(tab, k_tab_aliases[idx].name) == 0) return k_tab_aliases[idx].tab;
    num = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (num >= 0 && num < TAB_COUNT) ? (int)num : -1;
    len = strlen(tab);
    for (pass = 0; pass < 3; pass++)
        for (idx = 0; idx < TAB_COUNT; idx++) {
            if (pass == 0 && _strnicmp(tab, k_tabs[idx].name, len) == 0) return idx;
            if (pass == 1 && _strnicmp(tab, k_tabs[idx].label, len) == 0) return idx;
            if (pass == 2) {
                const char *word = k_tabs[idx].label;
                while ((word = strchr(word, ' ')) != NULL)
                    if (_strnicmp(tab, ++word, len) == 0) return idx;
            }
        }
    return -1;
}

/* For captures and tests: MSDOS_DEBUGGER_HISTORY records from the start,
 * MSDOS_DEBUGGER_WATCH="x,y" adds watches, MSDOS_DEBUGGER_WATCH_BREAK="x" a
 * write-when-changed breakpoint on it, MSDOS_DEBUGGER_MEM_GOTO="x" shows it in
 * Memory, MSDOS_DEBUGGER_POPOUT=<tab> peels a tab off. */
static void apply_env_hooks(debugger *win)
{
    const char *hist = getenv("MSDOS_DEBUGGER_HISTORY"), *watch_text = getenv("MSDOS_DEBUGGER_WATCH"),
               *break_text = getenv("MSDOS_DEBUGGER_WATCH_BREAK"), *goto_text = getenv("MSDOS_DEBUGGER_MEM_GOTO");
    uint32_t low, high;
    int pop;

    if (hist && *hist && *hist != '0') msdosdebug_history_enable(win->dbg, 1);
    if (watch_text && *watch_text) {
        char list[512], *item, *save = NULL;
        snprintf(list, sizeof list, "%s", watch_text);
        for (item = strtok_s(list, ",", &save); item; item = strtok_s(NULL, ",", &save)) {
            item = strip(item);
            if (*item) msdoswatch_add(item);
        }
    }
    if (break_text && *break_text && msdossym_parse_range(win->dbg, break_text, &low, &high)) {
        char err[160];
        msdosdebug_bp_add_if(win->dbg, MSDOS_BP_WRITE, low, high, -1, "value != old", err, sizeof err);
    }
    if (goto_text && *goto_text && msdossym_parse_range(win->dbg, goto_text, &low, &high)) show_in_memory(win, low, high);
    pop = tab_from_env_var("MSDOS_DEBUGGER_POPOUT");
    if (pop >= 0) pop_out(win, pop);
    resync(win);
}

void msdos_debugger_show(HWND parent, msdossession *session)
{
    static int registered;
    INITCOMMONCONTROLSEX icc;
    HINSTANCE inst;
    debugger *win;
    ACCEL accels[6];
    int tab;

    if (g_dbg) {
        win = g_dbg;
        if (win->session != session) {
            win->session = session;
            win->dbg = msdossession_debugger(session);
        }
        ShowWindow(win->hwnd, IsIconic(win->hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(win->hwnd);
        SetTimer(win->hwnd, TIMER_REFRESH, 100, NULL);
        attach_and_refresh(win);
        return;
    }

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);
    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);
    if (!inst) inst = GetModuleHandleA(NULL);
    if (!registered) {
        register_class(inst, DBG_CLASS, dbg_proc, (HBRUSH)(COLOR_BTNFACE + 1), CS_HREDRAW | CS_VREDRAW);
        register_class(inst, DISASM_CLASS, disasm_proc, NULL, CS_DBLCLKS);
        register_class(inst, PAGE_CLASS, page_proc, (HBRUSH)(COLOR_BTNFACE + 1), 0);
        register_class(inst, POP_CLASS, pop_proc, (HBRUSH)(COLOR_BTNFACE + 1), 0);
        registered = 1;
    }

    win = calloc(1, sizeof *win);
    if (!win) return;
    win->session = session;
    win->dbg = msdossession_debugger(session);
    win->top_seg = 0xF000;
    win->top_off = 0xFFF0;
    win->selected = -1;
    win->pc_line = -1;
    win->int_count = -1;
    win->fuji_count = -1;
    win->h_seen[0] = win->h_seen[1] = (uint64_t)-1;
    win->sym_ok = 0;
    g_dbg = win;

    win->dpi = msdos_window_dpi(parent);
    /* owned by the main window: over it, and gone with it */
    win->hwnd = CreateWindowExA(0, DBG_CLASS, "Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, scaled(win, 1280), scaled(win, 840), parent, NULL, inst, NULL);
    if (!win->hwnd) {
        free(win);
        g_dbg = NULL;
        return;
    }
    if (msdos_window_dpi(win->hwnd) != win->dpi) {
        /* placed on a monitor of another DPI than the parent's */
        win->dpi = msdos_window_dpi(win->hwnd);
        SetWindowPos(win->hwnd, NULL, 0, 0, scaled(win, 1280), scaled(win, 840), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    make_fonts(win);
    win->accent = CreateSolidBrush(MSDOS_ACCENT_COLORREF);
    win->bp_on = CreateSolidBrush(RGB(0xe0, 0x1b, 0x24));
    win->bp_off = CreateSolidBrush(RGB(0x9a, 0x99, 0x96));

    build_controls(win);

    /* F5/F7/F8/Shift+F8/F9 and Ctrl+Shift+D work wherever the focus is in the
     * window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY;          accels[4].key = VK_F9;  accels[4].cmd = IDC_TOGGLE_BP;
    accels[5].fVirt = FVIRTKEY | FCONTROL | FSHIFT; accels[5].key = 'D'; accels[5].cmd = IDCANCEL;
    win->accel = CreateAcceleratorTableA(accels, 6);

    tab = tab_from_env_var("MSDOS_DEBUGGER_TAB");
    win->page = tab >= 0 ? tab : TAB_CONSOLE;
    SendMessageA(win->tabs, TCM_SETCURSEL, (WPARAM)win->page, 0);
    show_page(win, win->page);
    layout(win);

    ShowWindow(win->hwnd, SW_SHOW);
    if (win->page == TAB_CONSOLE) SetFocus(win->con_in);
    SetTimer(win->hwnd, TIMER_REFRESH, 100, NULL);
    attach_and_refresh(win);
    apply_env_hooks(win);
}

void msdos_debugger_toggle(HWND parent, msdossession *session)
{
    if (msdos_debugger_visible()) hide_window(g_dbg);
    else msdos_debugger_show(parent, session);
}

int msdos_debugger_visible(void)
{
    return g_dbg && IsWindowVisible(g_dbg->hwnd);
}

int msdos_debugger_pretranslate(MSG *msg)
{
    int idx, mine;
    if (!g_dbg || !g_dbg->accel) return 0;
    mine = msg->hwnd == g_dbg->hwnd || IsChild(g_dbg->hwnd, msg->hwnd);
    for (idx = 0; !mine && idx < TAB_COUNT; idx++)     /* a peeled-off tab's window too */
        if (g_dbg->popped[idx] && (msg->hwnd == g_dbg->popped[idx] || IsChild(g_dbg->popped[idx], msg->hwnd))) mine = 1;
    if (!mine) return 0;
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}
