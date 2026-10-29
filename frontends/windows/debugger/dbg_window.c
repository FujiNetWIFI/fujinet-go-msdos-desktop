/*
 * Debugger window (Win32) over the session's debugger engine, via
 * core/include/msdosdebug.h. Mirrors the GNOME debugger tab for tab, built
 * from plain common controls with the disassembly drawn by hand.
 *
 * Tabs: the Console (DEBUG.COM's commands a line at a time, with a command
 * history), CPU & Memory (registers, flags, a hex dump), the Disassembly
 * (click a line to select it for Run to Cursor; click its left edge, or
 * double-click it, for its breakpoint), Breakpoints (code, memory, I/O
 * ports, software interrupts), Interrupts (the vector table and a DOS/BIOS
 * call trace) and FujiNet (its UART and the FujiBus packets on its COM
 * port). The toolbar carries the stepping controls and symbol loading (a
 * linker .map at a load segment); the family's keys apply (F5 run/stop, F7
 * step, F8 step over, Shift+F8 step out, Ctrl+Shift+D close).
 *
 * Showing this window attaches (which stops the machine, as on every
 * sibling); hiding it detaches and lets the machine run on. A timer polls
 * the engine's generation counter every 100 ms and refreshes the tab showing
 * only when something changed; while the machine runs it refreshes the
 * status, the registers and the traces twice a second.
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

#include "../help/help_window.h"

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define CMD_HISTORY 100         /* console lines Up/Down recall */
#define TRACE_MAX 512
#define BP_MAX 64
#define CONSOLE_KEEP (256 * 1024)
#define NOTICE_MS 4000          /* how long a notice holds the status line */

/* Sizes at 96 dpi; S() scales them to the window's. */
#define DIS_MARGIN 6            /* the disassembly's left margin */
#define MONO_PX 13              /* the monospace font */

#define WM_DBG_ACCEPT  (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_HISTORY (WM_APP + 4)   /* wp: 0 Up (older), 1 Down (newer) */
#define WM_DBG_SYNC    (WM_APP + 5)   /* resync once a list's notification is over */

#define TIMER_REFRESH 1

#define DBG_CLASS "FujiNetGoMSDOSDebugger"
#define DISASM_CLASS "FujiNetGoMSDOSDisasm"

enum { TAB_CONSOLE, TAB_CPU, TAB_DISASM, TAB_BREAKS, TAB_INTS, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names MSDOS_DEBUGGER_TAB takes, and their labels
 * (which it takes too). */
static const struct { const char *name, *label; } k_tabs[TAB_COUNT] = {
    { "console", "Console" }, { "cpu", "CPU & Memory" }, { "disassembly", "Disassembly" },
    { "breakpoints", "Breakpoints" }, { "interrupts", "Interrupts" }, { "fujinet", "FujiNet" },
};

/* the general registers, as msdosdebug_set_reg names them */
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

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_RUN_TO, IDC_STATUS, IDC_SYM_SEG, IDC_SYMBOLS,
    IDC_SYM_CLEAR, IDC_HELP_BTN, IDC_TABS,
    IDC_CON_OUT, IDC_CON_PROMPT, IDC_CON_IN,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + NREGS - 1,
    IDC_SREG0, IDC_SREG_LAST = IDC_SREG0 + NSREGS - 1,
    IDC_FLAG0, IDC_FLAG_LAST = IDC_FLAG0 + NFLAGS - 1,
    IDC_MEM_VIEW, IDC_MEM_GOTO, IDC_MEM_BACK, IDC_MEM_ON, IDC_MEM_ADDR, IDC_MEM_VAL,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM,
    IDC_BP_LIST, IDC_BP_REMOVE, IDC_BP_CLEAR, IDC_BP_TYPE, IDC_BP_START, IDC_BP_END, IDC_BP_AH,
    IDC_BP_ADD,
    IDC_INT_ON, IDC_INT_CLEAR, IDC_INT_TEXT, IDC_IVT_TEXT,
    IDC_FUJI_CLEAR, IDC_FUJI_TEXT, IDC_UART_TEXT,
    IDC_LABEL_FIRST
};

typedef struct {
    HWND hwnd, tabs, tip;
    HWND run_btn, step_btn, over_btn, out_btn, run_to_btn, status;
    HWND seg_label, sym_seg, sym_btn, sym_clear, help_btn;

    /* console */
    HWND con_out, con_prompt, con_in;
    char history[CMD_HISTORY][256];
    int nhistory, history_pos;

    /* cpu + memory */
    HWND reg_label[NREGS], reg[NREGS];
    HWND sreg_label[NSREGS], sreg[NSREGS], sbase[NSREGS];
    HWND flag[NFLAGS], cpu_info;
    HWND mem_title, mem_view, goto_label, mem_goto, mem_back, mem_on, write_label, mem_addr, value_label,
         mem_val;
    uint32_t mem_top;           /* linear */
    int64_t mem_mark;           /* the address Go to found; -1 none */
    int is386;

    /* disassembly */
    HWND follow_pc, jump_label, jump, disasm_hint, disasm;
    uint16_t top_seg;           /* the first line */
    uint32_t top_off;
    int64_t selected;           /* the selected line's linear address, or -1 */
    msdos_dis_line lines[DISASM_WINDOW];
    int bp[DISASM_WINDOW];      /* each line's breakpoint: 1 on, 2 off */
    int line_count, pc_line;

    /* breakpoints */
    HWND bp_hint, bp_remove, bp_clear, bp_list, bp_empty;
    HWND bp_type, bp_start, bp_end, bp_ah, bp_add, bp_msg;
    msdos_breakpoint bps[BP_MAX];
    int nbps;
    char bp_sig[BP_MAX * 120];  /* what the list shows, to rebuild on change */
    int bp_sig_ok;

    /* interrupts */
    HWND int_title, int_on, int_clear, int_text, ivt_title, ivt_text;
    uint64_t int_last;
    int int_count;

    /* fujinet */
    HWND fuji_title, fuji_note, fuji_clear, fuji_text, uart_title, uart_text;
    uint64_t fuji_last;
    int fuji_count;

    HWND views[10];             /* the read-only text views, drawn on the window colour */
    int nviews;
    HWND dim[48];               /* captions and hints, drawn grey */
    int ndim, nlabels;

    HFONT ui, ui_bold, mono, mono_bold, mono_sel;
    int dpi;
    int mono_w, mono_h, line_h;
    HACCEL accel;
    HBRUSH accent;

    msdossession *session;
    msdosdebug *dbg;

    int page;
    unsigned seen_generation;
    int was_stopped;
    int attached_running;       /* attached while the machine was running */
    int running_ticks;
    int updating;               /* a refresh is setting controls */

    char notice[200];           /* a message holding the status line a moment */
    DWORD notice_until;
} debugger;

static debugger *g_dbg;

/* ---- helpers ---------------------------------------------------------------- */

static void edit_text(HWND h, char *out, int outsz) { GetWindowTextA(h, out, outsz); }

/* A label set only when it changed (no flicker on the live refresh). */
static void set_label(HWND h, const char *utf8)
{
    wchar_t cur[512], w[512];
    msdos_wide(utf8, w, 512);
    GetWindowTextW(h, cur, 512);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
}

/* EDIT controls want CR LF and draw control characters as boxes: engine
 * text (UTF-8) as the wide text a view shows. */
static wchar_t *wide_view_text(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    wchar_t *w = NULL;
    int wn;
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    if (wn > 0 && (w = malloc((size_t)wn * sizeof *w)) != NULL)
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, wn);
    free(buf);
    return w;
}

static size_t addf(char *s, size_t len, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (len >= cap) return len;
    va_start(ap, fmt);
    n = vsnprintf(s + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return len;
    return len + (size_t)n < cap ? len + (size_t)n : cap - 1;
}

/* Blanks off both ends, in place. */
static char *strip(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

/* Hex, as DEBUG.COM takes it ("2F8", "0x2F8", "2F8h"); # for decimal. */
static int parse_num(const char *text, long *out)
{
    char *end;
    long v;
    while (*text == ' ') text++;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) v = strtol(text + 2, &end, 16);
    else if (*text == '#') v = strtol(text + 1, &end, 10);
    else v = strtol(text, &end, 16);
    if (*end == 'h' || *end == 'H') end++;
    while (*end == ' ') end++;
    if (end == text || *end) return 0;
    *out = v;
    return 1;
}

/* seg:off, a register pair, a linear address or a symbol. */
static int parse_addr(debugger *d, const char *text, uint16_t *seg, uint32_t *off, uint32_t *lin)
{
    while (*text == ' ') text++;
    if (!*text) return 0;
    return msdosdebug_parse_address(d->dbg, text, seg, off, lin) == 0;
}

static const char *base_name(const char *path)
{
    const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    if (b && (!a || b > a)) a = b;
    return a ? a + 1 : path;
}

/* ---- scale and fonts --------------------------------------------------------- */

static int S(const debugger *d, int v)
{
    return msdos_scale(v, d->dpi);
}

static BOOL CALLBACK refont_child(HWND h, LPARAM lp)
{
    const HFONT *map = (const HFONT *)lp;     /* old ui, bold, mono; new ui, bold, mono */
    const HFONT f = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0);
    int i;
    for (i = 0; i < 3; i++)
        if (f && f == map[i]) SendMessageA(h, WM_SETFONT, (WPARAM)map[i + 3], TRUE);
    return TRUE;
}

/* The message font and Consolas at the window's DPI; the controls already
 * made take the new ones. */
static void make_fonts(debugger *d)
{
    HFONT old[5] = { d->ui, d->ui_bold, d->mono, d->mono_bold, d->mono_sel };
    HDC dc;
    TEXTMETRICA tm;
    HGDIOBJ prev;
    int i;

    d->ui = msdos_make_font(d->dpi, 0, FW_NORMAL, 0, NULL);
    d->ui_bold = msdos_make_font(d->dpi, 0, FW_BOLD, 0, NULL);
    d->mono = msdos_make_font(d->dpi, MONO_PX, FW_NORMAL, 0, "Consolas");
    d->mono_bold = msdos_make_font(d->dpi, MONO_PX, FW_BOLD, 0, "Consolas");
    d->mono_sel = msdos_make_font(d->dpi, MONO_PX, FW_BOLD, 1, "Consolas");

    /* the monospace cell, for the hand-drawn disassembly */
    dc = GetDC(d->hwnd);
    prev = SelectObject(dc, d->mono);
    d->mono_w = S(d, 7);
    d->mono_h = S(d, 15);
    if (GetTextMetricsA(dc, &tm)) {
        d->mono_w = tm.tmAveCharWidth;
        d->mono_h = tm.tmHeight;
    }
    d->line_h = d->mono_h + S(d, 2);
    SelectObject(dc, prev);
    ReleaseDC(d->hwnd, dc);

    if (old[0]) {
        HFONT map[6] = { old[0], old[1], old[2], d->ui, d->ui_bold, d->mono };
        EnumChildWindows(d->hwnd, refont_child, (LPARAM)map);
    }
    for (i = 0; i < 5; i++)
        if (old[i]) DeleteObject(old[i]);
}

static int text_w(debugger *d, HFONT f, const char *text)
{
    HDC dc = GetDC(d->hwnd);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(d->hwnd, dc);
    return sz.cx;
}

static int btn_w(debugger *d, const char *text)
{
    const int w = text_w(d, d->ui, text) + S(d, 24);
    return w < S(d, 64) ? S(d, 64) : w;
}

static int in_list(HWND h, const HWND *list, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (list[i] == h) return 1;
    return 0;
}

/* ---- the text views ------------------------------------------------------------ */

/* The rows an EDIT view shows. */
static int edit_rows(debugger *d, HWND h)
{
    RECT c;
    GetClientRect(h, &c);
    return d->mono_h > 0 && c.bottom > d->mono_h ? c.bottom / d->mono_h : 1;
}

/* Logs (the console, the traces) follow their end while their reader is
 * there. */
static int edit_at_end(debugger *d, HWND h)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    return first + edit_rows(d, h) >= count;
}

static void edit_to_end(HWND h)
{
    SendMessageW(h, EM_LINESCROLL, 0, SendMessageW(h, EM_GETLINECOUNT, 0, 0));
}

static void edit_scroll_to(HWND h, int first)
{
    SendMessageW(h, EM_LINESCROLL, 0, first - (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0));
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(HWND h, const char *text, int follow)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_view_text(text);
    if (!w) return;
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(h, w);
    if (follow) edit_to_end(h);
    else edit_scroll_to(h, first);
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);
    free(w);
}

/* The live refresh does not overwrite a value being typed: while the
 * machine runs, a focused field keeps what is in it. */
static void set_entry(debugger *d, HWND h, const char *text)
{
    if (GetFocus() == h && !msdosdebug_stopped(d->dbg)) return;
    set_label(h, text);
}

static void refresh_status(debugger *d);

static int notice_showing(debugger *d)
{
    return d->notice[0] && (LONG)(GetTickCount() - d->notice_until) < 0;
}

/* The other frontends' toasts: the message holds the status line a moment. */
static void toast(debugger *d, const char *msg)
{
    snprintf(d->notice, sizeof d->notice, "%s", msg);
    d->notice_until = GetTickCount() + NOTICE_MS;
    refresh_status(d);
}

/* ---- the console ---------------------------------------------------------------- */

/* Long sessions keep the last CONSOLE_KEEP characters, whole lines. */
static void console_trim(debugger *d)
{
    const int len = GetWindowTextLengthW(d->con_out);
    wchar_t *all, *nl;
    int cut;
    if (len <= CONSOLE_KEEP) return;
    all = malloc(((size_t)len + 1) * sizeof *all);
    if (!all) return;
    GetWindowTextW(d->con_out, all, len + 1);
    cut = len - CONSOLE_KEEP * 3 / 4;
    nl = wcschr(all + cut, L'\n');
    if (nl) cut = (int)(nl - all) + 1;
    free(all);
    SendMessageW(d->con_out, EM_SETSEL, 0, (LPARAM)cut);
    SendMessageW(d->con_out, EM_REPLACESEL, FALSE, (LPARAM)L"");
}

/* Onto the console's transcript, which follows its end while the reader
 * is there. */
static void console_append(debugger *d, const char *text)
{
    const int follow = edit_at_end(d, d->con_out);
    const int first = (int)SendMessageW(d->con_out, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_view_text(text);
    int len;
    if (!w) return;
    len = GetWindowTextLengthW(d->con_out);
    SendMessageW(d->con_out, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(d->con_out, EM_REPLACESEL, FALSE, (LPARAM)w);
    free(w);
    console_trim(d);
    if (follow) edit_to_end(d->con_out);
    else edit_scroll_to(d->con_out, first);
}

/* Command output, ended with a newline. */
static void console_output(debugger *d, const char *text)
{
    const size_t n = strlen(text);
    if (!n) return;
    console_append(d, text);
    if (text[n - 1] != '\n') console_append(d, "\n");
}

/* Messages from Load Symbols land in the console and, since it may not be
 * the tab showing, on the status line. */
static void say(debugger *d, const char *msg)
{
    console_output(d, msg);
    toast(d, msg);
}

/* At a stop the console shows where, as DEBUG's R does. */
static void console_stop(debugger *d)
{
    static char out[4096];
    char reason[160];
    if (!msdossession_is_running(d->session)) return;
    reason[0] = '\0';
    msdosdebug_stop_reason(d->dbg, reason, sizeof reason);
    if (reason[0]) {
        char line[200];
        snprintf(line, sizeof line, "\xe2\x80\x94 %s", reason);
        console_output(d, line);
    }
    out[0] = '\0';
    msdosdebug_command(d->dbg, "r", out, sizeof out);
    console_output(d, out);
}

/* ---- refresh ---------------------------------------------------------------------- */

static void refresh_status(debugger *d)
{
    char reason[160], text[220];
    const int running = msdossession_is_running(d->session) != 0;
    const int stopped = running && msdosdebug_stopped(d->dbg);

    if (notice_showing(d)) {
        snprintf(text, sizeof text, "%s", d->notice);
    } else if (!running) {
        snprintf(text, sizeof text, "The PC is not running");
    } else if (stopped) {
        reason[0] = '\0';
        msdosdebug_stop_reason(d->dbg, reason, sizeof reason);
        snprintf(text, sizeof text, "Stopped%s%s", reason[0] ? ": " : "", reason);
    } else {
        snprintf(text, sizeof text, "Running");
    }
    set_label(d->status, text);
    if (d->tip) {
        TTTOOLINFOA ti;
        memset(&ti, 0, sizeof ti);
        ti.cbSize = TTTOOLINFOA_V2_SIZE;
        ti.hwnd = d->hwnd;
        ti.uId = (UINT_PTR)d->status;
        ti.lpszText = text;
        SendMessageA(d->tip, TTM_UPDATETIPTEXTA, 0, (LPARAM)&ti);
    }
    set_label(d->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    InvalidateRect(d->run_btn, NULL, FALSE);
    InvalidateRect(d->status, NULL, TRUE);
}

static void refresh_cpu(debugger *d)
{
    static const char *const modes[] = { "real mode", "protected mode", "virtual 8086 mode" };
    msdos_cpu_regs r;
    char buf[200], video[160];
    int i;
    memset(&r, 0, sizeof r);
    if (msdosdebug_regs(d->dbg, &r) != 0) return;
    {
        const uint32_t vals[NREGS] = { r.eax, r.ebx, r.ecx, r.edx, r.esi, r.edi, r.ebp, r.esp, r.eip };
        const uint16_t svals[NSREGS] = { r.cs, r.ds, r.es, r.ss, r.fs, r.gs };
        const uint32_t bases[NSREGS] = { r.cs_base, r.ds_base, r.es_base, r.ss_base, r.fs_base, r.gs_base };
        const int was386 = d->is386;
        d->is386 = r.is386 != 0;
        for (i = 0; i < NREGS; i++) {
            char name[8];
            snprintf(name, sizeof name, "%s", r.is386 ? k_regs32[i] : k_regs16[i]);
            CharUpperA(name);
            set_label(d->reg_label[i], name);
            snprintf(buf, sizeof buf, r.is386 ? "%08X" : "%04X", r.is386 ? vals[i] : vals[i] & 0xFFFFu);
            set_entry(d, d->reg[i], buf);
        }
        for (i = 0; i < NSREGS; i++) {
            snprintf(buf, sizeof buf, "%04X", svals[i]);
            set_entry(d, d->sreg[i], buf);
            snprintf(buf, sizeof buf, "base %05X", bases[i]);
            set_label(d->sbase[i], buf);
        }
        if (was386 != d->is386 && d->page == TAB_CPU) {
            /* FS and GS come and go with the 386 */
            for (i = 4; i < NSREGS; i++) {
                ShowWindow(d->sreg_label[i], d->is386 ? SW_SHOW : SW_HIDE);
                ShowWindow(d->sreg[i], d->is386 ? SW_SHOW : SW_HIDE);
                ShowWindow(d->sbase[i], d->is386 ? SW_SHOW : SW_HIDE);
            }
        }
    }
    d->updating = 1;
    for (i = 0; i < NFLAGS; i++)
        SendMessageA(d->flag[i], BM_SETCHECK, ((r.eflags >> k_flags[i].bit) & 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    d->updating = 0;
    video[0] = '\0';
    msdosdebug_video_info(d->dbg, video, sizeof video);
    strip(video);
    {
        char *v = strip(video);
        if (r.is386)
            snprintf(buf, sizeof buf, "%s   %s   CR0 %08X   %s", r.cpu, modes[r.mode < 0 ? 0 : r.mode > 2 ? 2 : r.mode],
                     (unsigned)r.cr0, v);
        else
            snprintf(buf, sizeof buf, "%s   %s", r.cpu, v);
    }
    set_label(d->cpu_info, buf);
}

static void refresh_mem(debugger *d)
{
    static uint8_t mem[MEM_ROWS * 16];
    static char t[(MEM_ROWS + 1) * 90];
    size_t len = 0;
    int row, col;

    memset(mem, 0, sizeof mem);
    msdosdebug_read(d->dbg, d->mem_top, mem, (int)sizeof mem);
    len = addf(t, len, sizeof t, "          ");
    for (col = 0; col < 16; col++)
        len = addf(t, len, sizeof t, col == 8 ? " %X  " : "%X  ", col);
    len = addf(t, len, sizeof t, "\n");
    for (row = 0; row < MEM_ROWS; row++) {
        const uint8_t *r = mem + row * 16;
        const uint32_t a = d->mem_top + (uint32_t)row * 16;
        /* real-mode style, segment the address's own 64K */
        len = addf(t, len, sizeof t, "%04X:%04X ", (unsigned)((a >> 4) & 0xF000), (unsigned)(a & 0xFFFF));
        for (col = 0; col < 16; col++)
            len = addf(t, len, sizeof t, col == 8 ? " %02X " : "%02X ", r[col]);
        len = addf(t, len, sizeof t, " ");
        for (col = 0; col < 16; col++)
            len = addf(t, len, sizeof t, "%c", r[col] >= 0x20 && r[col] < 0x7F ? r[col] : '.');
        len = addf(t, len, sizeof t, "\n");
    }
    set_text_at(d->mem_view, t, 0);

    /* the row Go to found stays marked (selected) across refreshes */
    if (d->mem_mark >= 0 && (uint32_t)d->mem_mark >= d->mem_top) {
        const uint32_t off = (uint32_t)d->mem_mark - d->mem_top;
        if (off < MEM_ROWS * 16) {
            const int line = 1 + (int)off / 16;
            const int s = (int)SendMessageW(d->mem_view, EM_LINEINDEX, (WPARAM)line, 0);
            const int e = s + (int)SendMessageW(d->mem_view, EM_LINELENGTH, (WPARAM)s, 0);
            const int first = (int)SendMessageW(d->mem_view, EM_GETFIRSTVISIBLELINE, 0, 0);
            SendMessageW(d->mem_view, EM_SETSEL, (WPARAM)s, (LPARAM)e);
            edit_scroll_to(d->mem_view, first);
        }
    }
}

/* The disassembly lines that fit the view. */
static int visible_rows(debugger *d)
{
    RECT c;
    int rows;
    GetClientRect(d->disasm, &c);
    if (d->line_h <= 0 || c.bottom <= 0) return 32;
    rows = c.bottom / d->line_h;
    return rows < 4 ? 4 : rows > DISASM_WINDOW ? DISASM_WINDOW : rows;
}

/* x86 code cannot be read backwards: the offset rows instructions before
 * off is the nearest start whose instructions land on off. */
static uint32_t rows_back(debugger *d, uint16_t seg, uint32_t off, int rows)
{
    static msdos_dis_line lines[DISASM_WINDOW * 4];
    const int max = (int)(sizeof lines / sizeof lines[0]);
    const int count = rows * 7 + 1 < max ? rows * 7 + 1 : max;
    int k, i, n;
    for (k = rows; k <= rows * 7 && (uint32_t)k <= off; k++) {
        n = msdosdebug_disassemble(d->dbg, seg, off - (uint32_t)k, count, lines);
        for (i = 0; i < n && lines[i].off < off; i++)
            ;
        if (i < n && lines[i].off == off && i >= rows) return lines[i - rows].off;
    }
    return off > (uint32_t)rows * 3 ? off - (uint32_t)rows * 3 : 0;
}

static uint32_t rows_on(debugger *d, uint16_t seg, uint32_t off, int rows)
{
    static msdos_dis_line lines[DISASM_WINDOW + 1];
    const int want = rows + 1 < DISASM_WINDOW + 1 ? rows + 1 : DISASM_WINDOW + 1;
    const int n = msdosdebug_disassemble(d->dbg, seg, off, want, lines);
    return n > rows ? lines[rows].off : off;
}

static void refresh_disasm(debugger *d)
{
    msdos_cpu_regs r;
    const int rows = visible_rows(d);
    const int have_regs = msdosdebug_regs(d->dbg, &r) == 0;
    int i, n;

    /* Following, CS:IP stays put while it shows, else sits a third of the
     * way down. */
    if (have_regs && SendMessageA(d->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        const uint32_t pc = msdosdebug_linear(d->dbg, r.cs, r.eip);
        int shows = 0;
        for (i = 1; i < d->line_count && i < rows - 2; i++)
            if (d->lines[i].linear == pc) shows = 1;
        if (!shows || d->top_seg != r.cs) {
            d->top_seg = r.cs;
            d->top_off = rows_back(d, r.cs, r.eip, rows / 3);
        }
    }
    n = msdosdebug_disassemble(d->dbg, d->top_seg, d->top_off, DISASM_WINDOW, d->lines);
    d->line_count = n > 0 ? n : 0;
    d->pc_line = -1;
    for (i = 0; i < d->line_count; i++) {
        d->bp[i] = msdosdebug_bp_at(d->dbg, d->lines[i].linear);   /* 1 on, 2 off */
        if (have_regs && d->lines[i].seg == r.cs && d->lines[i].off == r.eip) d->pc_line = i;
    }
    InvalidateRect(d->disasm, NULL, FALSE);
}

/* The list, or (empty) the hint in its place. */
static void show_bp_list(debugger *d)
{
    const int on = d->page == TAB_BREAKS;
    ShowWindow(d->bp_list, on && d->nbps > 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(d->bp_empty, on && d->nbps == 0 ? SW_SHOW : SW_HIDE);
}

static void lv_set(HWND lv, int row, int col, const char *text)
{
    LVITEMA it;
    memset(&it, 0, sizeof it);
    it.iSubItem = col;
    it.pszText = (char *)text;
    SendMessageA(lv, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&it);
}

static void refresh_bps(debugger *d)
{
    char sig[sizeof d->bp_sig];
    size_t len = 0;
    int i;

    /* rebuilt only when it changed (each step refreshes) */
    d->nbps = msdosdebug_bp_list(d->dbg, d->bps, BP_MAX);
    if (d->nbps < 0) d->nbps = 0;
    sig[0] = '\0';
    for (i = 0; i < d->nbps; i++)
        len = addf(sig, len, sizeof sig, "%d %d %u %s\n", d->bps[i].id, d->bps[i].enabled, d->bps[i].hits,
                   d->bps[i].text);
    if (d->bp_sig_ok && strcmp(sig, d->bp_sig) == 0) return;
    memcpy(d->bp_sig, sig, sizeof sig);
    d->bp_sig_ok = 1;

    d->updating = 1;
    SendMessageA(d->bp_list, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < d->nbps; i++) {
        const msdos_breakpoint *b = &d->bps[i];
        LVITEMA it;
        char id[16], hits[24];
        snprintf(id, sizeof id, "#%d", b->id);
        snprintf(hits, sizeof hits, "%u", b->hits);
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = id;
        SendMessageA(d->bp_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        lv_set(d->bp_list, i, 1, b->text);
        lv_set(d->bp_list, i, 2, hits);
        memset(&it, 0, sizeof it);
        it.stateMask = LVIS_STATEIMAGEMASK;
        it.state = INDEXTOSTATEIMAGEMASK(b->enabled ? 2 : 1);
        SendMessageA(d->bp_list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&it);
    }
    d->updating = 0;
    show_bp_list(d);
}

static void refresh_ints(debugger *d, int force)
{
    static msdos_int_event e[TRACE_MAX];
    static char t[TRACE_MAX * 140 + 256];
    size_t len = 0;
    int n = msdosdebug_int_trace(d->dbg, e, TRACE_MAX), i, follow;

    d->updating = 1;
    SendMessageA(d->int_on, BM_SETCHECK, msdosdebug_int_trace_enabled(d->dbg) ? BST_CHECKED : BST_UNCHECKED, 0);
    d->updating = 0;
    if (n < 0) n = 0;

    /* the vector table: changes rarely, cheap to read */
    for (i = 0; i < 256; i++) {
        uint16_t seg = 0, off = 0;
        msdosdebug_ivt(d->dbg, i, &seg, &off);
        len = addf(t, len, sizeof t, "%02X %04X:%04X%s", i, seg, off, (i & 3) == 3 ? "\n" : "   ");
    }
    set_text_at(d->ivt_text, t, 0);

    /* a log: set again only when it moved */
    if (!force && n == d->int_count && (n == 0 || e[n - 1].serial == d->int_last)) return;
    follow = force || d->int_count < 0 || edit_at_end(d, d->int_text);
    d->int_count = n;
    d->int_last = n > 0 ? e[n - 1].serial : 0;
    len = 0;
    len = addf(t, len, sizeof t, "From       AX   BX   CX   DX   DS   ES    Call");
    for (i = 0; i < n; i++)
        len = addf(t, len, sizeof t, "\n%04X:%04X  %04X %04X %04X %04X %04X %04X  %s", e[i].cs,
                   (unsigned)(e[i].ip & 0xFFFF), e[i].ax, e[i].bx, e[i].cx, e[i].dx, e[i].ds, e[i].es, e[i].text);
    if (n == 0)
        len = addf(t, len, sizeof t, msdosdebug_int_trace_enabled(d->dbg)
                       ? "\n(no INT calls since the trace was cleared)"
                       : "\n(turn the trace on to record INT calls; it costs speed)");
    set_text_at(d->int_text, t, follow);
}

static void refresh_fujinet(debugger *d, int force)
{
    static msdos_fuji_packet p[TRACE_MAX];
    static char t[TRACE_MAX * 320 + 128];
    static char uart[2048];
    char note[200];
    size_t len = 0;
    int n = msdosdebug_fujinet_trace(d->dbg, p, TRACE_MAX), i, follow;

    if (msdossession_fujinet_external(d->session))
        snprintf(note, sizeof note, "External FujiNet at %s on COM%d%s.",
                 msdossession_boip_endpoint(d->session), msdossession_fujinet_com(d->session),
                 msdossession_fujinet_link(d->session) ? ", connected" : ", not connected");
    else if (!msdossession_fujinet_running(d->session))
        snprintf(note, sizeof note, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet).");
    else
        snprintf(note, sizeof note, "FujiNet is on COM%d%s.", msdossession_fujinet_com(d->session),
                 msdossession_fujinet_link(d->session) ? ", connected" : ", not connected");
    set_label(d->fuji_note, note);
    uart[0] = '\0';
    msdosdebug_uart(d->dbg, uart, sizeof uart);
    set_text_at(d->uart_text, uart, 0);
    if (n < 0) n = 0;

    if (!force && n == d->fuji_count && (n == 0 || p[n - 1].serial == d->fuji_last)) return;
    follow = force || d->fuji_count < 0 || edit_at_end(d, d->fuji_text);
    d->fuji_count = n;
    d->fuji_last = n > 0 ? p[n - 1].serial : 0;
    for (i = 0; i < n; i++) {
        int k;
        len = addf(t, len, sizeof t, "%s%s\n     ", i ? "\n" : "", p[i].text);
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            len = addf(t, len, sizeof t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) len = addf(t, len, sizeof t, "\xe2\x80\xa6");
    }
    if (n == 0) len = addf(t, len, sizeof t, "(no FujiBus packets since the trace was cleared)");
    set_text_at(d->fuji_text, t, follow);
}

static void refresh_page(debugger *d, int page)
{
    if (!msdossession_is_running(d->session)) return;
    switch (page) {
    case TAB_CPU: refresh_cpu(d); refresh_mem(d); break;
    case TAB_DISASM: refresh_disasm(d); break;
    case TAB_BREAKS: refresh_bps(d); break;
    case TAB_INTS: refresh_ints(d, 0); break;
    case TAB_FUJINET: refresh_fujinet(d, 0); break;
    default: break;
    }
}

static void refresh_all(debugger *d)
{
    refresh_status(d);
    refresh_page(d, d->page);
}

/* After anything that may have moved the engine: the console takes the
 * registers at a new stop, then the views follow. */
static void resync(debugger *d)
{
    const int stopped = msdosdebug_stopped(d->dbg) != 0;
    const unsigned gen = msdosdebug_generation(d->dbg);
    if (stopped && (!d->was_stopped || gen != d->seen_generation)) console_stop(d);
    d->was_stopped = stopped;
    d->seen_generation = gen;
    d->running_ticks = 0;
    refresh_all(d);
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
static void settle_and_sync(debugger *d)
{
    int i;
    d->notice[0] = '\0';        /* the machine moved: its new state replaces a notice */
    for (i = 0; i < 40 && !msdosdebug_stopped(d->dbg); i++)
        Sleep(1);
    resync(d);
}

static void attach_and_refresh(debugger *d)
{
    msdosdebug_attach(d->dbg);     /* stops the machine */
    d->attached_running = msdossession_is_running(d->session) != 0;
    d->was_stopped = 0;
    settle_and_sync(d);
}

static void tick(debugger *d)
{
    unsigned gen;
    int stopped;
    if (!IsWindowVisible(d->hwnd)) return;
    if (d->notice[0] && !notice_showing(d)) {
        d->notice[0] = '\0';
        refresh_status(d);
    }
    if (!msdossession_is_running(d->session)) {
        if (d->attached_running) {
            d->attached_running = 0;
            refresh_status(d);
        }
        return;
    }
    /* Opened before the PC started, or showing across a restart: the
     * engine attaches (stopping the machine) as it comes up. */
    if (!d->attached_running) {
        msdosdebug_detach(d->dbg);
        attach_and_refresh(d);
        return;
    }
    gen = msdosdebug_generation(d->dbg);
    stopped = msdosdebug_stopped(d->dbg) != 0;
    if (gen != d->seen_generation || stopped != d->was_stopped) {
        if (stopped != d->was_stopped) d->notice[0] = '\0';   /* a stop (or a go) says so at once */
        resync(d);
    } else if (!stopped && ++d->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        d->running_ticks = 0;
        refresh_status(d);
        if (d->page == TAB_CPU) refresh_cpu(d);
        if (d->page == TAB_INTS) refresh_ints(d, 0);
        if (d->page == TAB_FUJINET) refresh_fujinet(d, 0);
    }
}

/* ---- actions -------------------------------------------------------------------- */

static void on_run(debugger *d)
{
    if (msdosdebug_stopped(d->dbg)) msdosdebug_resume(d->dbg);
    else msdosdebug_stop(d->dbg);
    settle_and_sync(d);
}

static void on_run_to(debugger *d)
{
    if (d->selected < 0) {
        toast(d, "Select a line in the Disassembly to run to");
        return;
    }
    msdosdebug_run_to(d->dbg, (uint32_t)d->selected);
    settle_and_sync(d);
}

static void toggle_breakpoint(debugger *d, uint32_t linear)
{
    if (msdosdebug_bp_toggle(d->dbg, linear) < 0) toast(d, "No room for another breakpoint");
    resync(d);
}

static void on_console(debugger *d)
{
    static char out[65536];
    char buf[256], line[300], *cmd;

    edit_text(d->con_in, buf, sizeof buf);
    cmd = strip(buf);
    if (!*cmd) return;
    if (d->nhistory == 0 || strcmp(d->history[d->nhistory - 1], cmd) != 0) {
        if (d->nhistory == CMD_HISTORY) {
            memmove(d->history[0], d->history[1], sizeof d->history[0] * (CMD_HISTORY - 1));
            d->nhistory--;
        }
        snprintf(d->history[d->nhistory++], sizeof d->history[0], "%s", cmd);
    }
    d->history_pos = d->nhistory;
    snprintf(line, sizeof line, "-%s\n", cmd);
    edit_to_end(d->con_out);     /* a command brings the reader back down */
    console_append(d, line);

    out[0] = '\0';
    msdosdebug_command(d->dbg, cmd, out, sizeof out);
    SetWindowTextA(d->con_in, "");
    console_output(d, out);
    /* a t, p or g that stops again reports where, as resync sees the stop */
    d->was_stopped = 0;
    settle_and_sync(d);
}

/* Up and Down walk the history; past the newest is an empty line again. */
static void history_step(debugger *d, int newer)
{
    int len;
    if (newer) {
        if (d->history_pos >= d->nhistory) return;
        d->history_pos++;
    } else {
        if (d->history_pos <= 0) return;
        d->history_pos--;
    }
    SetWindowTextA(d->con_in, d->history_pos < d->nhistory ? d->history[d->history_pos] : "");
    len = GetWindowTextLengthA(d->con_in);
    SendMessageA(d->con_in, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

/* A register: the 32-bit name when the 32-bit value shows. */
static void on_reg(debugger *d, int i, int segment)
{
    char buf[32];
    const char *name = segment ? k_sregs[i] : d->is386 ? k_regs32[i] : k_regs16[i];
    long v;
    edit_text(segment ? d->sreg[i] : d->reg[i], buf, sizeof buf);
    if (!parse_num(buf, &v))
        toast(d, "Not a number (hex, or # decimal)");
    else if (!msdosdebug_stopped(d->dbg))
        toast(d, "Stop the PC first");
    else if (msdosdebug_set_reg(d->dbg, name, (uint32_t)v) != 0)
        toast(d, "That register cannot be set here");
    resync(d);
}

static void on_flag(debugger *d, int i)
{
    if (d->updating) return;
    if (!msdosdebug_stopped(d->dbg)) {
        refresh_cpu(d);     /* running: the box goes back to the CPU's */
        return;
    }
    msdosdebug_set_reg(d->dbg, k_flags[i].name, SendMessageA(d->flag[i], BM_GETCHECK, 0, 0) == BST_CHECKED);
    resync(d);
}

/* Go to: the dump from just above an address (seg:off, a register pair, a
 * linear address or a symbol), its row marked and scrolled to. */
static void on_mem_goto(debugger *d)
{
    char buf[64];
    uint16_t seg;
    uint32_t off, lin;
    int top;
    edit_text(d->mem_goto, buf, sizeof buf);
    if (!parse_addr(d, buf, &seg, &off, &lin)) {
        d->mem_mark = -1;
        refresh_mem(d);
        toast(d, "Not an address (seg:off, ds:si, a linear address, a symbol)");
        return;
    }
    d->mem_mark = lin;
    d->mem_top = (lin > 0x100 ? lin - 0x100 : 0) & ~0xFu;
    refresh_mem(d);
    /* the row 30% of the way down */
    top = 1 + (int)((lin - d->mem_top) / 16) - edit_rows(d, d->mem_view) * 3 / 10;
    edit_scroll_to(d->mem_view, top > 0 ? top : 0);
}

/* The dump a page of MEM_ROWS on or back. */
static void on_mem_page(debugger *d, int dir)
{
    if (dir < 0) d->mem_top = d->mem_top > MEM_ROWS * 16 ? d->mem_top - MEM_ROWS * 16 : 0;
    else d->mem_top += MEM_ROWS * 16;
    refresh_mem(d);
}

static void on_mem_write(debugger *d)
{
    char abuf[64], vbuf[32];
    uint16_t seg;
    uint32_t off, lin;
    long v;
    uint8_t byte;
    edit_text(d->mem_addr, abuf, sizeof abuf);
    edit_text(d->mem_val, vbuf, sizeof vbuf);
    if (!parse_addr(d, abuf, &seg, &off, &lin)) {
        toast(d, "Not an address");
        return;
    }
    if (!parse_num(vbuf, &v) || v < 0 || v > 0xFF) {
        toast(d, "The value is a byte (00-FF)");
        return;
    }
    byte = (uint8_t)v;
    msdosdebug_write(d->dbg, lin, &byte, 1);
    resync(d);
}

static void on_jump(debugger *d)
{
    char buf[64];
    uint16_t seg;
    uint32_t off, lin;
    edit_text(d->jump, buf, sizeof buf);
    if (!parse_addr(d, buf, &seg, &off, &lin)) {
        toast(d, "Not an address (seg:off, cs:ip, a linear address, a symbol)");
        return;
    }
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    d->top_seg = seg;
    d->top_off = off;
    d->selected = lin;
    refresh_disasm(d);
}

/* The selected breakpoint row's index, or -1. */
static int bp_selected(debugger *d)
{
    const int i = (int)SendMessageA(d->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    return i >= 0 && i < d->nbps ? i : -1;
}

static void on_bp_remove(debugger *d)
{
    const int i = bp_selected(d);
    if (i < 0) {
        set_label(d->bp_msg, "Select a breakpoint to remove");
        return;
    }
    msdosdebug_bp_remove(d->dbg, d->bps[i].id);
    resync(d);
}

/* Execute and memory breakpoints take addresses; port and INT ones
 * numbers. The end is optional; AH only means something for an INT. */
static void on_bp_add(debugger *d)
{
    char start[64], end[64], ahs[32], *s, *e, *a;
    int type = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    uint32_t lo, hi;
    long ah = -1;
    if (type < 0 || type >= MSDOS_BP_TYPES) type = MSDOS_BP_EXEC;
    edit_text(d->bp_start, start, sizeof start);
    edit_text(d->bp_end, end, sizeof end);
    edit_text(d->bp_ah, ahs, sizeof ahs);
    s = strip(start);
    e = strip(end);
    a = strip(ahs);
    if (type <= MSDOS_BP_ACCESS) {
        uint16_t seg;
        uint32_t off;
        if (!parse_addr(d, s, &seg, &off, &lo)) {
            set_label(d->bp_msg, "Not an address (seg:off, a linear address, a symbol)");
            return;
        }
        hi = lo;
        if (*e && !parse_addr(d, e, &seg, &off, &hi)) {
            set_label(d->bp_msg, "The end is not an address");
            return;
        }
    } else {
        long v, last;
        if (!parse_num(s, &v) || v < 0 || v > (type == MSDOS_BP_INT ? 0xFF : 0xFFFF)) {
            set_label(d->bp_msg, type == MSDOS_BP_INT ? "An interrupt is 00-FF" : "A port is 0000-FFFF");
            return;
        }
        last = v;
        if (*e && type != MSDOS_BP_INT && (!parse_num(e, &last) || last < v || last > 0xFFFF)) {
            set_label(d->bp_msg, "The last port is a port at or after the first");
            return;
        }
        lo = (uint32_t)v;
        hi = (uint32_t)last;
        if (type == MSDOS_BP_INT && *a && (!parse_num(a, &ah) || ah < 0 || ah > 0xFF)) {
            set_label(d->bp_msg, "AH is 00-FF, or empty for any");
            return;
        }
    }
    if (hi < lo) {
        set_label(d->bp_msg, "The end is before the start");
        return;
    }
    if (msdosdebug_bp_add(d->dbg, type, lo, hi, (int)ah) < 0) {
        set_label(d->bp_msg, "No room for another breakpoint");
        return;
    }
    set_label(d->bp_msg, "");
    SetWindowTextA(d->bp_start, "");
    SetWindowTextA(d->bp_end, "");
    SetWindowTextA(d->bp_ah, "");
    resync(d);
}

/* The type decides what the boxes take. */
static void on_bp_type(debugger *d)
{
    const int type = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    EnableWindow(d->bp_ah, type == MSDOS_BP_INT);
    EnableWindow(d->bp_end, type != MSDOS_BP_INT);
    SendMessageW(d->bp_start, EM_SETCUEBANNER, TRUE,
                 (LPARAM)(type <= MSDOS_BP_ACCESS ? L"1234:0010 / symbol" : type == MSDOS_BP_INT ? L"21" : L"2F8"));
    SendMessageW(d->bp_end, EM_SETCUEBANNER, TRUE,
                 (LPARAM)(type <= MSDOS_BP_ACCESS ? L"(end)" : L"(last port)"));
}

static int pick_map(debugger *d, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = "Load Symbols (MS LINK or Watcom WLINK .map)";
    ofn.lpstrFilter = "Linker maps (*.map)\0*.map\0All files\0*.*\0\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void on_symbols(debugger *d)
{
    char path[MAX_PATH], seg_text[32], msg[512], line[700];
    long seg = 0;
    if (!pick_map(d, path, sizeof path)) return;
    edit_text(d->sym_seg, seg_text, sizeof seg_text);
    if (!parse_num(seg_text, &seg) || seg < 0 || seg > 0xFFFF) seg = 0;
    msg[0] = '\0';
    if (msdosdebug_load_symbols(d->dbg, path, (uint16_t)seg, msg, sizeof msg) < 0 && !msg[0])
        snprintf(msg, sizeof msg, "No symbols found in %s", base_name(path));
    snprintf(line, sizeof line, "%s", msg);
    say(d, line);
    resync(d);
}

static void on_accept(debugger *d, int id)
{
    if (id >= IDC_REG0 && id <= IDC_REG_LAST) {
        on_reg(d, id - IDC_REG0, 0);
        return;
    }
    if (id >= IDC_SREG0 && id <= IDC_SREG_LAST) {
        on_reg(d, id - IDC_SREG0, 1);
        return;
    }
    switch (id) {
    case IDC_CON_IN: on_console(d); break;
    case IDC_MEM_GOTO: on_mem_goto(d); break;
    case IDC_MEM_ADDR:
    case IDC_MEM_VAL: on_mem_write(d); break;
    case IDC_JUMP: on_jump(d); break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_AH: on_bp_add(d); break;
    case IDC_SYM_SEG: on_symbols(d); break;
    default: break;
    }
}

/* ---- the disassembly -------------------------------------------------------------- */

/* Drawn by hand, a line an instruction as the other frontends show it: the
 * breakpoint mark (* on, red; o off, grey), > at CS:IP, seg:off, bytes,
 * symbol and instruction. The CS:IP line is filled with the accent (the
 * badge's ink on it), the selected one bold and underlined. */
static void paint_disasm(debugger *d, HWND hwnd, HDC dc)
{
    RECT c;
    int i;
    GetClientRect(hwnd, &c);
    FillRect(dc, &c, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    if (d->line_count == 0) {
        SelectObject(dc, d->mono);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        TextOutA(dc, S(d, DIS_MARGIN), 0, "(no disassembly)", 16);
        return;
    }
    for (i = 0; i < d->line_count && i * d->line_h < c.bottom; i++) {
        const msdos_dis_line *l = &d->lines[i];
        const int y = i * d->line_h, ty = y + (d->line_h - d->mono_h) / 2;
        const int sel = (int64_t)l->linear == d->selected, is_pc = i == d->pc_line;
        COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
        RECT row;
        char text[256];
        int len;
        SetRect(&row, 0, y, c.right, y + d->line_h);
        if (is_pc) {
            FillRect(dc, &row, d->accent);
            fg = MSDOS_INK_RGB;
        }
        len = snprintf(text, sizeof text, "%c %04X:%0*X  %-20s %s%s%s", is_pc ? '>' : ' ', l->seg,
                       l->off > 0xFFFF ? 8 : 4, (unsigned)l->off, l->bytes, l->symbol,
                       l->symbol[0] ? ": " : "", l->text);
        if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
        if (len < 0) len = 0;
        SelectObject(dc, sel ? d->mono_sel : d->mono);
        SetTextColor(dc, fg);
        TextOutA(dc, S(d, DIS_MARGIN) + d->mono_w, ty, text, len);
        if (d->bp[i]) {
            const int off = d->bp[i] == 2;
            SelectObject(dc, sel ? d->mono_sel : off ? d->mono : d->mono_bold);
            SetTextColor(dc, off ? RGB(0x9a, 0x99, 0x96) : RGB(0xe0, 0x1b, 0x24));
            TextOutA(dc, S(d, DIS_MARGIN), ty, off ? "o" : "*", 1);
        }
    }
}

/* A click selects a line (Run to Cursor); a click on its left edge, or a
 * double-click, toggles its breakpoint. The wheel and the keys browse by
 * instructions (and turn Follow CS:IP off). */
static LRESULT CALLBACK disasm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, d->mono);
        paint_disasm(d, hwnd, mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        /* more or fewer lines fit now */
        if (d->disasm == hwnd && d->page == TAB_DISASM) refresh_page(d, TAB_DISASM);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const int line = d->line_h > 0 ? GET_Y_LPARAM(lp) / d->line_h : -1;
        const int gutter = GET_X_LPARAM(lp) < S(d, DIS_MARGIN) + 3 * d->mono_w;
        SetFocus(hwnd);
        if (line < 0 || line >= d->line_count) return 0;
        d->selected = d->lines[line].linear;
        if (gutter ? msg == WM_LBUTTONDOWN : msg == WM_LBUTTONDBLCLK)
            toggle_breakpoint(d, d->lines[line].linear);
        else
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int rows = -delta * 3 / WHEEL_DELTA;
        if (!rows) rows = delta > 0 ? -1 : 1;
        SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        d->top_off = rows > 0 ? rows_on(d, d->top_seg, d->top_off, rows) : rows_back(d, d->top_seg, d->top_off, -rows);
        refresh_disasm(d);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN) {
            const int half = visible_rows(d) / 2 > 0 ? visible_rows(d) / 2 : 1;
            const int rows = wp == VK_PRIOR ? -half : wp == VK_NEXT ? half : wp == VK_UP ? -1 : 1;
            SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
            d->top_off = rows > 0 ? rows_on(d, d->top_seg, d->top_off, rows)
                                  : rows_back(d, d->top_seg, d->top_off, -rows);
            refresh_disasm(d);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;

/* Enter in a single-line field means "apply this value"; Up/Down in the
 * console recall. None may beep its way through the default handler. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int id = (int)GetWindowLongPtrA(hwnd, GWLP_ID);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_ACCEPT, (WPARAM)id, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    if (id == IDC_CON_IN) {
        if (msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
            PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_HISTORY, wp == VK_DOWN, 0);
            return 0;
        }
        if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    }
    return CallWindowProcA(g_edit_proc, hwnd, msg, wp, lp);
}

/* ---- construction ----------------------------------------------------------------- */

static HWND child_ex(debugger *d, DWORD ex, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExA(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, d->hwnd, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static HWND child(debugger *d, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    return child_ex(d, 0, cls, text, style, id, font);
}

/* A read-only monospace text view. */
static HWND view(debugger *d, int id, DWORD extra)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | extra,
                   id, d->mono);
    SendMessageA(h, EM_SETLIMITTEXT, 0, 0);
    if (d->nviews < (int)(sizeof d->views / sizeof d->views[0])) d->views[d->nviews++] = h;
    return h;
}

static HWND field(debugger *d, int id, const wchar_t *cue)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, id, d->mono);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    if (cue) SendMessageW(h, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    return h;
}

static HWND label(debugger *d, const char *text, HFONT font, DWORD style)
{
    return child(d, "STATIC", text, style | SS_NOPREFIX, IDC_LABEL_FIRST + d->nlabels++, font);
}

/* A caption or hint, drawn grey. */
static HWND dim_label(debugger *d, const char *text, DWORD style)
{
    HWND h = label(d, text, d->ui, style);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = h;
    return h;
}

static HWND button(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, d->ui);
}

static void tip(debugger *d, HWND h, const char *text)
{
    TTTOOLINFOA ti;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = TTTOOLINFOA_V2_SIZE;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = d->hwnd;
    ti.uId = (UINT_PTR)h;
    ti.lpszText = (char *)text;
    SendMessageA(d->tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

static void size_columns(debugger *d)
{
    static const int widths[3] = { 70, 420, 90 };
    int i;
    for (i = 0; i < 3; i++)
        SendMessageA(d->bp_list, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(d, widths[i]));
}

static void build_controls(debugger *d)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    TCITEMA item;
    int i;

    d->tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, d->hwnd, NULL, inst, NULL);
    SendMessageA(d->tip, TTM_SETMAXTIPWIDTH, 0, S(d, 420));

    d->tabs = child(d, WC_TABCONTROLA, "", WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, IDC_TABS, d->ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < TAB_COUNT; i++) {
        /* "&" would underline the next letter */
        char text[32];
        const char *s;
        size_t o = 0;
        for (s = k_tabs[i].label; *s && o < sizeof text - 2; s++) {
            if (*s == '&') text[o++] = '&';
            text[o++] = *s;
        }
        text[o] = '\0';
        item.pszText = text;
        SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&item);
    }

    /* The toolbar. Run/Stop is owner-drawn so it can wear the accent colour
     * while the machine is stopped. */
    d->run_btn = child(d, "BUTTON", "Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, d->ui);
    d->step_btn = button(d, "Step (F7)", IDC_STEP);
    d->over_btn = button(d, "Step Over (F8)", IDC_OVER);
    d->out_btn = button(d, "Step Out (Shift+F8)", IDC_OUT);
    d->run_to_btn = button(d, "Run to Cursor", IDC_RUN_TO);
    d->status = child(d, "STATIC", "", SS_RIGHT | SS_ENDELLIPSIS | SS_NOTIFY | SS_NOPREFIX, IDC_STATUS, d->ui);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = d->status;
    d->seg_label = dim_label(d, "Load segment", SS_LEFT);
    d->sym_seg = field(d, IDC_SYM_SEG, L"0000");
    SendMessageA(d->sym_seg, EM_SETLIMITTEXT, 6, 0);
    d->sym_btn = button(d, "Load Symbols...", IDC_SYMBOLS);
    d->sym_clear = button(d, "Clear Symbols", IDC_SYM_CLEAR);
    d->help_btn = button(d, "Help", IDC_HELP_BTN);
    {
        HWND bar[11] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->run_to_btn, d->status,
                         d->seg_label, d->sym_seg, d->sym_btn, d->sym_clear, d->help_btn };
        for (i = 0; i < 11; i++) ShowWindow(bar[i], SW_SHOW);
    }
    tip(d, d->step_btn, "One instruction");
    tip(d, d->over_btn, "Over a CALL, INT, LOOP or REP, to the instruction after it");
    tip(d, d->out_btn, "On until the routine returns (RET, RETF or IRET)");
    tip(d, d->run_to_btn, "On to the line selected in the Disassembly");
    tip(d, d->status, "");
    tip(d, d->sym_seg, "The segment the program was loaded at: a .COM's PSP, an .EXE's PSP + 10h.");
    tip(d, d->sym_btn, "Labels from an MS LINK or Watcom WLINK .map file, at the load segment");

    /* Console */
    d->con_out = view(d, IDC_CON_OUT, 0);
    SetWindowTextA(d->con_out, "DEBUG.COM's commands: r d e u g t p, plus bp/bpr/bpw/bpi/bpo/bpint, bl, bc, i, o. "
                               "Type h for the list; Up/Down recall.\r\n");
    d->con_prompt = child(d, "STATIC", "-", SS_LEFT, IDC_CON_PROMPT, d->mono);
    d->con_in = field(d, IDC_CON_IN, L"u cs:ip   d ds:0   bpo 2f8   bpint 21 9   g");

    /* CPU & Memory */
    for (i = 0; i < NREGS; i++) {
        char name[8];
        snprintf(name, sizeof name, "%s", k_regs16[i]);
        CharUpperA(name);
        d->reg_label[i] = dim_label(d, name, SS_RIGHT);
        d->reg[i] = field(d, IDC_REG0 + i, NULL);
        SendMessageA(d->reg[i], EM_SETLIMITTEXT, 10, 0);
        tip(d, d->reg[i], "Enter sets it (hex)");
    }
    for (i = 0; i < NSREGS; i++) {
        char name[8];
        snprintf(name, sizeof name, "%s", k_sregs[i]);
        CharUpperA(name);
        d->sreg_label[i] = dim_label(d, name, SS_RIGHT);
        d->sreg[i] = field(d, IDC_SREG0 + i, NULL);
        SendMessageA(d->sreg[i], EM_SETLIMITTEXT, 6, 0);
        tip(d, d->sreg[i], "Enter sets it (hex)");
        d->sbase[i] = dim_label(d, "", SS_LEFT);
    }
    for (i = 0; i < NFLAGS; i++) {
        char name[4];
        snprintf(name, sizeof name, "%s", k_flags[i].name);
        CharUpperA(name);
        d->flag[i] = child(d, "BUTTON", name, BS_AUTOCHECKBOX | WS_TABSTOP, IDC_FLAG0 + i, d->ui);
    }
    d->cpu_info = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    d->mem_title = label(d, "Memory (linear; video RAM at A0000-BFFFF reads as the CPU would)", d->ui_bold, SS_LEFT);
    d->mem_view = view(d, IDC_MEM_VIEW, WS_HSCROLL | ES_NOHIDESEL);
    d->goto_label = dim_label(d, "Go to", SS_LEFT);
    d->mem_goto = field(d, IDC_MEM_GOTO, L"ds:si / B800:0 / symbol");
    d->mem_back = button(d, "Back 4K", IDC_MEM_BACK);
    d->mem_on = button(d, "On 4K", IDC_MEM_ON);
    d->write_label = dim_label(d, "Write", SS_LEFT);
    d->mem_addr = field(d, IDC_MEM_ADDR, L"0040:0017");
    d->value_label = dim_label(d, "value", SS_LEFT);
    d->mem_val = field(d, IDC_MEM_VAL, L"00");

    /* Disassembly */
    d->follow_pc = child(d, "BUTTON", "Follow CS:IP", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_FOLLOW_PC, d->ui);
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    d->jump_label = dim_label(d, "Jump to", SS_LEFT);
    d->jump = field(d, IDC_JUMP, L"F000:FFF0 or _main");
    d->disasm_hint = dim_label(d, "Click a line to select it (Run to Cursor); click its left edge or "
                                  "double-click it for a breakpoint (* on, o off); scroll to browse",
                               SS_LEFT | SS_ENDELLIPSIS);
    d->disasm = CreateWindowExA(WS_EX_CLIENTEDGE, DISASM_CLASS, "", WS_CHILD | WS_TABSTOP, 0, 0, 10, 10,
                                d->hwnd, (HMENU)(INT_PTR)IDC_DISASM, inst, NULL);

    /* Breakpoints */
    d->bp_hint = dim_label(d, "Execute stops before the instruction runs; memory, ports and INT after the "
                              "instruction that touched them.", SS_LEFT | SS_ENDELLIPSIS);
    d->bp_remove = button(d, "Remove", IDC_BP_REMOVE);
    d->bp_clear = button(d, "Clear All", IDC_BP_CLEAR);
    d->bp_list = child_ex(d, WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                          WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_NOSORTHEADER,
                          IDC_BP_LIST, d->mono);
    SendMessageA(d->bp_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    {
        static const char *const cols[3] = { "#", "Breakpoint", "Hits" };
        LVCOLUMNA col;
        memset(&col, 0, sizeof col);
        col.mask = LVCF_TEXT;
        for (i = 0; i < 3; i++) {
            col.pszText = (char *)cols[i];
            SendMessageA(d->bp_list, LVM_INSERTCOLUMNA, (WPARAM)i, (LPARAM)&col);
        }
        size_columns(d);
    }
    d->bp_empty = dim_label(d, "No breakpoints. Click the left edge of a disassembly line, or add one below.",
                            SS_CENTER);
    d->bp_type = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_BP_TYPE, d->ui);
    for (i = 0; i < MSDOS_BP_TYPES; i++) SendMessageA(d->bp_type, CB_ADDSTRING, 0, (LPARAM)k_bp_types[i]);
    SendMessageA(d->bp_type, CB_SETCURSEL, 0, 0);
    d->bp_start = field(d, IDC_BP_START, L"1234:0010 / symbol");
    d->bp_end = field(d, IDC_BP_END, L"(end)");
    d->bp_ah = field(d, IDC_BP_AH, L"AH (any)");
    EnableWindow(d->bp_ah, FALSE);
    d->bp_add = button(d, "Add", IDC_BP_ADD);
    d->bp_msg = label(d, "", d->ui, SS_LEFT | SS_ENDELLIPSIS);

    /* Interrupts */
    d->int_title = label(d, "INT calls", d->ui_bold, SS_LEFT);
    d->int_on = child(d, "BUTTON", "Trace", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_INT_ON, d->ui);
    tip(d, d->int_on, "Record every INT the PC runs (costs speed)");
    d->int_clear = button(d, "Clear", IDC_INT_CLEAR);
    d->int_text = view(d, IDC_INT_TEXT, WS_HSCROLL);
    d->ivt_title = label(d, "Vector table", d->ui_bold, SS_LEFT);
    d->ivt_text = view(d, IDC_IVT_TEXT, WS_HSCROLL);

    /* FujiNet */
    d->fuji_title = label(d, "FujiBus packets (SLIP-decoded)", d->ui_bold, SS_LEFT);
    d->fuji_note = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    d->fuji_clear = button(d, "Clear", IDC_FUJI_CLEAR);
    d->fuji_text = view(d, IDC_FUJI_TEXT, WS_HSCROLL);
    d->uart_title = label(d, "FujiNet's UART", d->ui_bold, SS_LEFT);
    d->uart_text = view(d, IDC_UART_TEXT, WS_HSCROLL);
}

static void show_page(debugger *d, int page)
{
    HWND console[] = { d->con_out, d->con_prompt, d->con_in };
    HWND cpu[] = { d->cpu_info, d->mem_title, d->mem_view, d->goto_label, d->mem_goto, d->mem_back, d->mem_on,
                   d->write_label, d->mem_addr, d->value_label, d->mem_val };
    HWND dis[] = { d->follow_pc, d->jump_label, d->jump, d->disasm_hint, d->disasm };
    HWND brk[] = { d->bp_hint, d->bp_remove, d->bp_clear, d->bp_type, d->bp_start, d->bp_end, d->bp_ah,
                   d->bp_add, d->bp_msg };
    HWND ints[] = { d->int_title, d->int_on, d->int_clear, d->int_text, d->ivt_title, d->ivt_text };
    HWND fuji[] = { d->fuji_title, d->fuji_note, d->fuji_clear, d->fuji_text, d->uart_title, d->uart_text };
    size_t i;
    d->page = page;
#define SHOW(arr, p) for (i = 0; i < sizeof(arr) / sizeof((arr)[0]); i++) \
                         ShowWindow((arr)[i], page == (p) ? SW_SHOW : SW_HIDE)
    SHOW(console, TAB_CONSOLE);
    SHOW(cpu, TAB_CPU);
    SHOW(d->reg_label, TAB_CPU);
    SHOW(d->reg, TAB_CPU);
    SHOW(d->flag, TAB_CPU);
    for (i = 0; i < NSREGS; i++) {
        const int on = page == TAB_CPU && (i < 4 || d->is386);
        ShowWindow(d->sreg_label[i], on ? SW_SHOW : SW_HIDE);
        ShowWindow(d->sreg[i], on ? SW_SHOW : SW_HIDE);
        ShowWindow(d->sbase[i], on ? SW_SHOW : SW_HIDE);
    }
    SHOW(dis, TAB_DISASM);
    SHOW(brk, TAB_BREAKS);
    SHOW(ints, TAB_INTS);
    SHOW(fuji, TAB_FUJINET);
#undef SHOW
    show_bp_list(d);
    {
        /* Help, F1 and Ctrl+Shift+H: the tab's section */
        char ctx[48];
        snprintf(ctx, sizeof ctx, "debugger/%s", k_tabs[page].name);
        msdos_help_set_context(d->hwnd, ctx, 1);
    }
}

static void place(HWND h, int x, int y, int w, int hgt)
{
    MoveWindow(h, x, y, w > 1 ? w : 1, hgt > 1 ? hgt : 1, TRUE);
}

static void layout(debugger *d)
{
    RECT client, page;
    int x, y, w, i, px, py, pw, ph;
    const int bh = S(d, 26), eh = S(d, 24), lh = S(d, 18), gap = S(d, 6);

    if (!d->tabs) return;
    GetClientRect(d->hwnd, &client);

    /* The toolbar: stepping on the left, symbols on the right, the status
     * between. Narrower (a tiling compositor ignores the minimum size), the
     * status goes on a line of its own, then the symbols with it. */
    {
        HWND left[5] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->run_to_btn };
        const char *ltext[5] = { "Stop (F5)", "Step (F7)", "Step Over (F8)", "Step Out (Shift+F8)",
                                 "Run to Cursor" };
        const int seg_lw = text_w(d, d->ui, "Load segment") + S(d, 6), seg_w = d->mono_w * 6 + S(d, 10);
        const int load_w = btn_w(d, "Load Symbols..."), clear_w = btn_w(d, "Clear Symbols");
        const int help_w = btn_w(d, "Help");
        const int sym_w = seg_lw + seg_w + gap + load_w + gap + clear_w + gap + help_w;
        const int row2 = S(d, 8) + bh + S(d, 8);
        int right = client.right - S(d, 8), sym_x, sym_y = S(d, 8);
        x = S(d, 8);
        for (i = 0; i < 5; i++) {
            w = btn_w(d, ltext[i]);
            place(left[i], x, S(d, 8), w, bh);
            x += w + gap;
        }
        y = row2;
        if (right - sym_w >= x + gap) {
            sym_x = right - sym_w;
            right = sym_x - gap;
            if (right - x - 2 * gap >= S(d, 200)) {
                place(d->status, x + gap, S(d, 8) + (bh - lh) / 2, right - x - 2 * gap, lh);
            } else {
                place(d->status, S(d, 8), y - S(d, 2), client.right - S(d, 16), lh);
                y += lh + S(d, 4);
            }
        } else {
            /* the symbols under the stepping, the status beside them */
            sym_x = S(d, 8);
            sym_y = row2;
            place(d->status, sym_x + sym_w + gap, sym_y + (bh - lh) / 2,
                  client.right - S(d, 8) - (sym_x + sym_w + gap), lh);
            y = row2 + bh + S(d, 8);
        }
        place(d->seg_label, sym_x, sym_y + (bh - lh) / 2, seg_lw, lh);
        place(d->sym_seg, sym_x + seg_lw, sym_y + (bh - eh) / 2, seg_w, eh);
        place(d->sym_btn, sym_x + seg_lw + seg_w + gap, sym_y, load_w, bh);
        place(d->sym_clear, sym_x + seg_lw + seg_w + gap + load_w + gap, sym_y, clear_w, bh);
        place(d->help_btn, sym_x + seg_lw + seg_w + gap + load_w + gap + clear_w + gap, sym_y, help_w, bh);
    }

    /* The tab control is the strip alone: the pages sit on the window's own
     * background, which their captions are drawn on. */
    SetRect(&page, 0, 0, S(d, 400), S(d, 400));
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    place(d->tabs, S(d, 8), y, client.right - S(d, 16), page.top);
    px = S(d, 12);
    py = y + page.top + S(d, 6);
    pw = client.right - 2 * px;
    ph = client.bottom - S(d, 10) - py;
    if (pw < S(d, 300)) pw = S(d, 300);
    if (ph < S(d, 240)) ph = S(d, 240);

    /* Console: the transcript, then the prompt and its line */
    w = d->mono_w + S(d, 6);
    place(d->con_out, px, py, pw, ph - eh - S(d, 8));
    place(d->con_prompt, px, py + ph - eh + (eh - d->mono_h) / 2, w, d->mono_h);
    place(d->con_in, px + w + S(d, 2), py + ph - eh, pw - w - S(d, 2), eh);

    /* CPU & Memory: the general registers across, the segment registers
     * (with their bases) under them, the flags and the CPU; the dump; Go to
     * and Write under it */
    {
        const int rw = d->mono_w * 8 + S(d, 10), cell = (pw + S(d, 8)) / NREGS;
        x = px;
        for (i = 0; i < NREGS; i++) {
            const int lw = cell - rw - S(d, 12) > S(d, 24) ? cell - rw - S(d, 12) : S(d, 24);
            place(d->reg_label[i], x, py + (eh - lh) / 2, lw, lh);
            place(d->reg[i], x + lw + S(d, 4), py, rw, eh);
            x += cell;
        }
    }
    y = py + eh + S(d, 8);
    {
        const int sw = d->mono_w * 4 + S(d, 10), cell = (pw + S(d, 8)) / NSREGS;
        const int lw = text_w(d, d->ui, "WW") + S(d, 6);
        x = px;
        for (i = 0; i < NSREGS; i++) {
            place(d->sreg_label[i], x, y + (eh - lh) / 2, lw, lh);
            place(d->sreg[i], x + lw + S(d, 4), y, sw, eh);
            place(d->sbase[i], x + lw + S(d, 4) + sw + S(d, 6), y + (eh - lh) / 2,
                  cell - lw - sw - S(d, 18), lh);
            x += cell;
        }
    }
    y += eh + S(d, 8);
    x = px;
    for (i = 0; i < NFLAGS; i++) {
        place(d->flag[i], x, y, S(d, 44), S(d, 22));
        x += S(d, 48);
    }
    place(d->cpu_info, x + S(d, 16), y + (S(d, 22) - lh) / 2, pw - (x - px) - S(d, 16), lh);
    y += S(d, 30);
    {
        const int bottom = py + ph - eh;
        place(d->mem_title, px, y, pw, lh);
        y += lh + S(d, 4);
        place(d->mem_view, px, y, pw, bottom - S(d, 6) - y);
        y = bottom + (eh - lh) / 2;
        x = px;
        w = text_w(d, d->ui, "Go to") + S(d, 6);
        place(d->goto_label, x, y, w, lh);
        x += w;
        place(d->mem_goto, x, bottom, S(d, 170), eh);
        x += S(d, 176);
        w = btn_w(d, "Back 4K");
        place(d->mem_back, x, bottom - S(d, 1), w, bh);
        x += w + S(d, 4);
        w = btn_w(d, "On 4K");
        place(d->mem_on, x, bottom - S(d, 1), w, bh);
        x += w + S(d, 24);
        w = text_w(d, d->ui, "Write") + S(d, 6);
        place(d->write_label, x, y, w, lh);
        x += w;
        place(d->mem_addr, x, bottom, S(d, 110), eh);
        x += S(d, 116);
        w = text_w(d, d->ui, "value") + S(d, 6);
        place(d->value_label, x, y, w, lh);
        x += w;
        place(d->mem_val, x, bottom, S(d, 44), eh);
    }

    /* Disassembly: Follow CS:IP and Jump to over the lines */
    w = text_w(d, d->ui, "Follow CS:IP") + S(d, 24);
    place(d->follow_pc, px, py + S(d, 1), w, S(d, 22));
    x = px + w + S(d, 12);
    w = text_w(d, d->ui, "Jump to") + S(d, 6);
    place(d->jump_label, x, py + (eh - lh) / 2, w, lh);
    x += w;
    place(d->jump, x, py, S(d, 170), eh);
    x += S(d, 182);
    place(d->disasm_hint, x, py + (eh - lh) / 2, px + pw - x, lh);
    place(d->disasm, px, py + eh + S(d, 6), pw, ph - eh - S(d, 6));

    /* Breakpoints: the hint, Remove and Clear All over the list; Add and a
     * message under it */
    {
        const int msg_y = py + ph - lh, add_y = msg_y - S(d, 8) - eh;
        const int list_y = py + bh + S(d, 6);
        int bw = btn_w(d, "Clear All"), rbw = btn_w(d, "Remove");
        place(d->bp_clear, px + pw - bw, py, bw, bh);
        place(d->bp_remove, px + pw - bw - gap - rbw, py, rbw, bh);
        place(d->bp_hint, px, py + (bh - lh) / 2, pw - bw - rbw - 3 * gap, lh);
        place(d->bp_list, px, list_y, pw, add_y - gap - list_y);
        place(d->bp_empty, px, list_y + S(d, 12), pw, lh);
        x = px;
        place(d->bp_type, x, add_y, S(d, 150), S(d, 240));
        x += S(d, 156);
        place(d->bp_start, x, add_y, S(d, 170), eh);
        x += S(d, 176);
        place(d->bp_end, x, add_y, S(d, 110), eh);
        x += S(d, 116);
        place(d->bp_ah, x, add_y, S(d, 80), eh);
        x += S(d, 86);
        bw = btn_w(d, "Add");
        place(d->bp_add, x, add_y - S(d, 1), bw, bh);
        place(d->bp_msg, px, msg_y, pw, lh);
    }

    /* Interrupts: the trace left, the vector table right */
    {
        const int rw = pw / 3 > S(d, 360) ? S(d, 360) : pw / 3 > S(d, 240) ? pw / 3 : S(d, 240);
        const int lw = pw - rw - S(d, 12);
        int cw = btn_w(d, "Clear"), tw = text_w(d, d->ui, "Trace") + S(d, 26);
        place(d->int_clear, px + lw - cw, py, cw, bh);
        place(d->int_on, px + lw - cw - gap - tw, py + (bh - S(d, 22)) / 2, tw, S(d, 22));
        place(d->int_title, px, py + (bh - lh) / 2, lw - cw - tw - 2 * gap, lh);
        place(d->int_text, px, py + bh + S(d, 6), lw, ph - bh - S(d, 6));
        place(d->ivt_title, px + lw + S(d, 12), py + (bh - lh) / 2, rw, lh);
        place(d->ivt_text, px + lw + S(d, 12), py + bh + S(d, 6), rw, ph - bh - S(d, 6));
    }

    /* FujiNet: the packets left (a note and Clear over them), the UART right */
    {
        const int rw = pw / 3 > S(d, 320) ? S(d, 320) : pw / 3 > S(d, 220) ? pw / 3 : S(d, 220);
        const int lw = pw - rw - S(d, 12);
        int cw = btn_w(d, "Clear");
        place(d->fuji_title, px, py, lw, lh);
        place(d->fuji_clear, px + lw - cw, py + lh + S(d, 4), cw, bh);
        place(d->fuji_note, px, py + lh + S(d, 4) + (bh - lh) / 2, lw - cw - gap, lh);
        place(d->fuji_text, px, py + lh + bh + S(d, 10), lw, ph - lh - bh - S(d, 10));
        place(d->uart_title, px + lw + S(d, 12), py, rw, lh);
        place(d->uart_text, px + lw + S(d, 12), py + lh + S(d, 4), rw, ph - lh - S(d, 4));
    }

    /* everything again: what the controls left behind where they were */
    RedrawWindow(d->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* ---- the window ------------------------------------------------------------------- */

static void hide_window(debugger *d)
{
    KillTimer(d->hwnd, TIMER_REFRESH);
    msdosdebug_detach(d->dbg);     /* lets it run */
    d->attached_running = 0;
    ShowWindow(d->hwnd, SW_HIDE);
}

static void draw_run_button(debugger *d, const DRAWITEMSTRUCT *di)
{
    char text[32];
    const int stopped = msdossession_is_running(d->session) && msdosdebug_stopped(d->dbg);
    RECT r = di->rcItem;
    if (stopped) {
        FillRect(di->hDC, &r, d->accent);
        FrameRect(di->hDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(di->hDC, MSDOS_INK_RGB);
    } else {
        DrawFrameControl(di->hDC, &r, DFC_BUTTON,
                         DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(di->hDC, TRANSPARENT);
    GetWindowTextA(di->hwndItem, text, sizeof text);
    SelectObject(di->hDC, d->ui);
    DrawTextA(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(di->hDC, &r);
    }
}

static LRESULT on_notify(debugger *d, NMHDR *nh)
{
    if (nh->hwndFrom == d->tabs && nh->code == (UINT)TCN_SELCHANGE) {
        const int page = (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0);
        show_page(d, page);
        refresh_page(d, page);
        return 0;
    }
    if (nh->hwndFrom != d->bp_list) return 0;
    if (nh->code == (UINT)LVN_ITEMCHANGED) {
        const NMLISTVIEW *nm = (const NMLISTVIEW *)nh;
        /* a row's box: its breakpoint on or off (the list follows after) */
        if (!d->updating && nm->iItem >= 0 && nm->iItem < d->nbps && (nm->uChanged & LVIF_STATE) &&
            ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) && (nm->uOldState & LVIS_STATEIMAGEMASK)) {
            const int on = ((nm->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
            if (on != (d->bps[nm->iItem].enabled != 0)) {
                msdosdebug_bp_enable(d->dbg, d->bps[nm->iItem].id, on);
                PostMessageA(d->hwnd, WM_DBG_SYNC, 0, 0);
            }
        }
    } else if (nh->code == (UINT)LVN_KEYDOWN) {
        /* after the list's own handling: removing rebuilds it */
        if (((const NMLVKEYDOWN *)nh)->wVKey == VK_DELETE)
            PostMessageA(d->hwnd, WM_COMMAND, MAKEWPARAM(IDC_BP_REMOVE, BN_CLICKED), 0);
    }
    return 0;
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_GETMINMAXINFO) {
        const int dpi = d && d->dpi ? d->dpi : 96;
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(860, dpi, 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(560, dpi, 96);
        return 0;
    }
    if (!d || d->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        d->dpi = HIWORD(wp);
        make_fonts(d);
        size_columns(d);
        SendMessageA(d->tip, TTM_SETMAXTIPWIDTH, 0, S(d, 420));
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout(d);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_REFRESH) tick(d);
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_DBG_HISTORY:
        history_step(d, (int)wp);
        return 0;
    case WM_DBG_SYNC:
        resync(d);
        return 0;
    case WM_NOTIFY:
        return on_notify(d, (NMHDR *)lp);
    case WM_DRAWITEM:
        if (wp == IDC_RUN) {
            draw_run_button(d, (const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HWND h = (HWND)lp;
        HDC dc = (HDC)wp;
        if (in_list(h, d->views, d->nviews)) {
            /* read-only views on the window colour, as text views are */
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        if (h == d->bp_msg) {
            SetTextColor(dc, RGB(0xc0, 0x1c, 0x28));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (h == d->status && notice_showing(d)) {
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (in_list(h, d->dim, d->ndim)) {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        /* the disassembly scrolls under the pointer, focused or not */
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r;
        GetWindowRect(d->disasm, &r);
        if (d->page == TAB_DISASM && PtInRect(&r, p)) return SendMessageA(d->disasm, msg, wp, lp);
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= IDC_FLAG0 && id <= IDC_FLAG_LAST && HIWORD(wp) == BN_CLICKED) {
            on_flag(d, id - IDC_FLAG0);
            return 0;
        }
        switch (id) {
        case IDC_RUN: on_run(d); return 0;
        case IDC_STEP: msdosdebug_step(d->dbg); settle_and_sync(d); return 0;
        case IDC_OVER: msdosdebug_step_over(d->dbg); settle_and_sync(d); return 0;
        case IDC_OUT: msdosdebug_step_out(d->dbg); settle_and_sync(d); return 0;
        case IDC_RUN_TO: on_run_to(d); return 0;
        case IDC_SYMBOLS: on_symbols(d); return 0;
        case IDC_HELP_BTN: msdos_help_for(d->hwnd); return 0;
        case IDC_SYM_CLEAR:
            msdosdebug_clear_symbols(d->dbg);
            say(d, "Symbols cleared");
            resync(d);
            return 0;
        case IDC_MEM_BACK: on_mem_page(d, -1); return 0;
        case IDC_MEM_ON: on_mem_page(d, 1); return 0;
        case IDC_FOLLOW_PC: refresh_disasm(d); return 0;
        case IDC_BP_TYPE:
            if (HIWORD(wp) == CBN_SELCHANGE) on_bp_type(d);
            return 0;
        case IDC_BP_ADD: on_bp_add(d); return 0;
        case IDC_BP_REMOVE: on_bp_remove(d); return 0;
        case IDC_BP_CLEAR:
            msdosdebug_bp_clear(d->dbg);
            resync(d);
            return 0;
        case IDC_INT_ON:
            if (HIWORD(wp) == BN_CLICKED && !d->updating) {
                msdosdebug_int_trace_enable(d->dbg, SendMessageA(d->int_on, BM_GETCHECK, 0, 0) == BST_CHECKED);
                refresh_ints(d, 1);
            }
            return 0;
        case IDC_INT_CLEAR:
            msdosdebug_int_trace_clear(d->dbg);
            refresh_ints(d, 1);
            return 0;
        case IDC_FUJI_CLEAR:
            msdosdebug_fujinet_trace_clear(d->dbg);
            refresh_fujinet(d, 1);
            return 0;
        case IDCANCEL: hide_window(d); return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide and let the machine run; Ctrl+Shift+D brings it back, stopped. */
        hide_window(d);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        msdosdebug_detach(d->dbg);
        DeleteObject(d->ui);
        DeleteObject(d->ui_bold);
        DeleteObject(d->mono);
        DeleteObject(d->mono_bold);
        DeleteObject(d->mono_sel);
        DeleteObject(d->accent);
        DestroyAcceleratorTable(d->accel);
        free(d);
        g_dbg = NULL;
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points ----------------------------------------------------------------- */

static void register_class(HINSTANCE inst, const char *name, WNDPROC proc, HBRUSH background, UINT style)
{
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(101));
    RegisterClassExA(&wc);
}

/* MSDOS_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("memory"...). */
static int tab_from_env(void)
{
    const char *tab = getenv("MSDOS_DEBUGGER_TAB");
    char *end;
    long n;
    size_t len;
    int i, pass;
    if (!tab || !*tab) return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < TAB_COUNT) ? (int)n : -1;
    len = strlen(tab);
    for (pass = 0; pass < 3; pass++)
        for (i = 0; i < TAB_COUNT; i++) {
            if (pass == 0 && _strnicmp(tab, k_tabs[i].name, len) == 0) return i;
            if (pass == 1 && _strnicmp(tab, k_tabs[i].label, len) == 0) return i;
            if (pass == 2) {
                const char *p = k_tabs[i].label;
                while ((p = strchr(p, ' ')) != NULL)
                    if (_strnicmp(tab, ++p, len) == 0) return i;
            }
        }
    return -1;
}

void msdos_debugger_show(HWND parent, msdossession *session)
{
    static int registered;
    INITCOMMONCONTROLSEX icc;
    HINSTANCE inst;
    debugger *d;
    ACCEL accels[5];
    int tab;

    if (g_dbg) {
        d = g_dbg;
        if (d->session != session) {
            d->session = session;
            d->dbg = msdossession_debugger(session);
        }
        ShowWindow(d->hwnd, IsIconic(d->hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(d->hwnd);
        SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
        attach_and_refresh(d);
        return;
    }

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);
    if (!inst) inst = GetModuleHandleA(NULL);
    if (!registered) {
        register_class(inst, DBG_CLASS, dbg_proc, (HBRUSH)(COLOR_BTNFACE + 1), CS_HREDRAW | CS_VREDRAW);
        register_class(inst, DISASM_CLASS, disasm_proc, NULL, CS_DBLCLKS);
        registered = 1;
    }

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->session = session;
    d->dbg = msdossession_debugger(session);
    d->top_seg = 0xF000;
    d->top_off = 0xFFF0;
    d->selected = -1;
    d->mem_mark = -1;
    d->int_count = -1;
    d->fuji_count = -1;
    d->pc_line = -1;
    g_dbg = d;

    d->dpi = msdos_window_dpi(parent);
    /* owned by the main window: over it, and gone with it */
    d->hwnd = CreateWindowExA(0, DBG_CLASS, "Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                              CW_USEDEFAULT, S(d, 1150), S(d, 800), parent, NULL, inst, NULL);
    if (!d->hwnd) {
        free(d);
        g_dbg = NULL;
        return;
    }
    if (msdos_window_dpi(d->hwnd) != d->dpi) {
        /* placed on a monitor of another DPI than the parent's */
        d->dpi = msdos_window_dpi(d->hwnd);
        SetWindowPos(d->hwnd, NULL, 0, 0, S(d, 1150), S(d, 800), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    make_fonts(d);
    d->accent = CreateSolidBrush(msdos_accent_rgb());

    build_controls(d);

    /* F5/F7/F8/Shift+F8 and Ctrl+Shift+D work wherever the focus is in the
     * window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY | FCONTROL | FSHIFT; accels[4].key = 'D'; accels[4].cmd = IDCANCEL;
    d->accel = CreateAcceleratorTableA(accels, 5);

    tab = tab_from_env();
    d->page = tab >= 0 ? tab : TAB_CONSOLE;
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    show_page(d, d->page);
    layout(d);

    ShowWindow(d->hwnd, SW_SHOW);
    if (d->page == TAB_CONSOLE) SetFocus(d->con_in);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
    attach_and_refresh(d);
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
    if (!g_dbg || !g_dbg->accel) return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd)) return 0;
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}
