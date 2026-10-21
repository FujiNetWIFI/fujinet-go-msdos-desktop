/*
 * Debugger window (GTK4/libadwaita) over the session's debugger engine,
 * via core/include/msdosdebug.h.
 *
 * Tabs: the Console (DEBUG.COM's commands a line at a time, with a
 * command history), CPU & Memory (registers, flags, a hex dump), the
 * disassembly (click a line to select it for Run to Cursor; click its left
 * edge, or double-click it, for its breakpoint), breakpoints (code, memory,
 * I/O ports, software interrupts), Interrupts (the vector table and a
 * DOS/BIOS call trace) and FujiNet (its UART and the FujiBus packets on its
 * COM port). A toolbar carries the stepping controls, the header bar symbol
 * loading; the family's keys apply (F5 run/stop, F7 step, F8 step over,
 * Shift+F8 step out).
 *
 * Showing this window attaches (which stops the machine, as on every
 * sibling); hiding it detaches and lets the machine run on.
 *
 * The window polls the engine's generation counter on a short timer and
 * refreshes the tab showing only when something changed; while the machine
 * runs it refreshes the status, the registers and the traces twice a
 * second.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "msdosdebug.h"

#include "../help/help_window.h"

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define BP_MAX 64
#define TRACE_MAX 512
#define CONSOLE_KEEP (256 * 1024)

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

typedef struct {
    GtkWindow *win;
    msdossession *session;
    msdosdebug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    gboolean attached_running;  /* attached while the machine was running */
    gboolean updating;          /* a refresh is setting widgets */
    int running_ticks;
    guint timer;

    GtkLabel *status;
    GtkButton *run_btn;
    AdwToastOverlay *toasts;
    GtkNotebook *notebook;

    /* console */
    GtkTextView *con_out;
    GtkEntry *con_in;
    GPtrArray *history;
    guint history_pos;

    /* cpu + memory */
    GtkLabel *reg_label[NREGS];
    GtkEntry *reg[NREGS];
    GtkEntry *sreg[NSREGS];
    GtkLabel *sbase[NSREGS];
    GtkCheckButton *flag[NFLAGS];
    GtkLabel *cpu_info;
    GtkTextView *mem_view;
    GtkEntry *mem_goto, *mem_addr, *mem_val;
    uint32_t mem_top;           /* linear */
    int64_t mem_mark;           /* the address Go to found; -1 none */
    gboolean is386;

    /* disassembly */
    GtkTextView *disasm;
    GtkWidget *disasm_scroll;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    uint16_t top_seg;           /* the first line */
    uint32_t top_off;
    int64_t selected;           /* the selected line's linear address, or -1 */
    uint32_t line_lin[DISASM_WINDOW];
    int line_count;

    /* breakpoints */
    GtkListBox *bp_list;
    char *bp_sig;               /* what the list shows, to rebuild on change */
    GtkDropDown *bp_type;
    GtkEntry *bp_start, *bp_end, *bp_ah;
    GtkLabel *bp_msg;

    /* interrupts */
    GtkSwitch *int_on;
    GtkTextView *int_text, *ivt_text;
    uint64_t int_last;
    int int_count;

    /* fujinet */
    GtkTextView *uart_text, *fuji_text;
    GtkLabel *fuji_note;
    uint64_t fuji_last;
    int fuji_count;

    /* symbols */
    GtkEntry *sym_seg;
} DbgWin;

static DbgWin *g_win;

/* ---- helpers -------------------------------------------------------------- */

/* Engine text for a text buffer: valid UTF-8, control bytes dropped. */
static char *clean_text(const char *text)
{
    char *v = g_utf8_make_valid(text, -1), *s, *d;
    for (s = d = v; *s; s++)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') *d++ = *s;
    *d = '\0';
    return v;
}

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

static gboolean at_end(GtkAdjustment *adj)
{
    return gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
           gtk_adjustment_get_upper(adj) - 2.0;
}

/* Logs (the console, the traces) follow their end while their reader is
 * there. New text grows the view only once it is laid out (or its tab
 * first shows), so the end is pinned whenever the view's height changes;
 * whether to follow is decided as text arrives. */
static gboolean following(GtkTextView *view)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "follow")) != 0;
}

static void on_log_resized(GtkAdjustment *adj, gpointer view)
{
    if (following(view))
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
}

static void make_log(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    g_signal_connect(vadj(view), "changed", G_CALLBACK(on_log_resized), view);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 || at_end(adj)));
}

static void log_to_end(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    on_log_resized(vadj(view), view);
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    char *clean = clean_text(text);
    GtkTextMark *mark;
    GtkTextIter it;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &it, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&it);
    }
    gtk_text_buffer_set_text(b, clean, -1);
    g_free(clean);
    if (follow) {
        log_to_end(view);
        return;
    }
    if (top <= 0) return;
    /* scroll_to_mark waits for the new text's layout */
    gtk_text_buffer_get_iter_at_line(b, &it, top);
    mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
    gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
    gtk_text_buffer_delete_mark(b, mark);
}

static void set_text(GtkTextView *view, const char *text)
{
    set_text_at(view, text, FALSE);
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
static int parse_addr(DbgWin *w, const char *text, uint16_t *seg, uint32_t *off, uint32_t *lin)
{
    while (*text == ' ') text++;
    if (!*text) return 0;
    return msdosdebug_parse_address(w->dbg, text, seg, off, lin) == 0;
}

/* The live refresh does not overwrite a value being typed: while the
 * machine runs, a focused entry keeps what is in it. */
static void set_entry(DbgWin *w, GtkEntry *e, const char *text)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(e));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    if (focus && !msdosdebug_stopped(w->dbg) &&
        (focus == GTK_WIDGET(e) || gtk_widget_is_ancestor(focus, GTK_WIDGET(e))))
        return;
    gtk_editable_set_text(GTK_EDITABLE(e), text);
}

static GtkWidget *mono_view(GtkTextView **out, gboolean editable)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *padded(GtkWidget *w)
{
    gtk_widget_set_margin_top(w, 8);
    gtk_widget_set_margin_bottom(w, 8);
    gtk_widget_set_margin_start(w, 8);
    gtk_widget_set_margin_end(w, 8);
    return w;
}

static GtkWidget *hint_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

static GtkWidget *title_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "heading");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

static GtkEntry *small_entry(const char *placeholder, int chars)
{
    GtkEntry *e = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(e, placeholder);
    gtk_editable_set_width_chars(GTK_EDITABLE(e), chars);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(e), chars);
    return e;
}

/* The accent fill (the IBM badge's silver) with the badge's ink on it:
 * the CS:IP line, a marked row. */
static void accent_tag(GtkTextView *view, const char *name)
{
    char rgb[16];
    g_snprintf(rgb, sizeof rgb, "#%06x", MSDOSSESSION_ACCENT_RGB);
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(view), name,
                               "paragraph-background", rgb, "foreground", "#1e1e1e", NULL);
}

static void toast(DbgWin *w, const char *msg)
{
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(msg));
}

/* ---- the console ---------------------------------------------------------- */

/* Onto the console's transcript, which follows its end while the reader
 * is there. */
static void console_append(DbgWin *w, const char *text)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->con_out);
    char *clean = clean_text(text);
    GtkTextIter it;
    int count;

    log_note_reader(w->con_out);
    gtk_text_buffer_get_end_iter(b, &it);
    gtk_text_buffer_insert(b, &it, clean, -1);
    g_free(clean);
    /* long sessions keep the last CONSOLE_KEEP characters, whole lines */
    count = gtk_text_buffer_get_char_count(b);
    if (count > CONSOLE_KEEP) {
        GtkTextIter s, e;
        gtk_text_buffer_get_start_iter(b, &s);
        gtk_text_buffer_get_iter_at_offset(b, &e, count - CONSOLE_KEEP * 3 / 4);
        gtk_text_iter_forward_line(&e);
        gtk_text_buffer_delete(b, &s, &e);
    }
}

/* Command output, ended with a newline. */
static void console_output(DbgWin *w, const char *text)
{
    size_t n = strlen(text);
    if (!n) return;
    console_append(w, text);
    if (text[n - 1] != '\n') console_append(w, "\n");
}

/* Messages from Load Symbols land in the console and, since it may not be
 * the tab showing, in a toast. */
static void say(DbgWin *w, const char *msg)
{
    console_output(w, msg);
    toast(w, msg);
}

/* At a stop the console shows where, as DEBUG's R does. */
static void console_stop(DbgWin *w)
{
    static char out[4096];
    char reason[160];
    if (!msdossession_is_running(w->session)) return;
    msdosdebug_stop_reason(w->dbg, reason, sizeof reason);
    if (reason[0]) {
        char line[200];
        g_snprintf(line, sizeof line, "\xe2\x80\x94 %s", reason);
        console_output(w, line);
    }
    msdosdebug_command(w->dbg, "r", out, sizeof out);
    console_output(w, out);
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_status(DbgWin *w)
{
    char reason[160], text[220];
    gboolean running = msdossession_is_running(w->session) != 0;
    gboolean stopped = running && msdosdebug_stopped(w->dbg);

    if (!running)
        g_strlcpy(text, "The PC is not running", sizeof text);
    else if (stopped) {
        msdosdebug_stop_reason(w->dbg, reason, sizeof reason);
        if (!strcmp(reason, "Stopped")) reason[0] = '\0';
        g_snprintf(text, sizeof text, "Stopped%s%s", reason[0] ? ": " : "", reason);
    } else
        g_strlcpy(text, "Running", sizeof text);
    gtk_label_set_text(w->status, text);
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->status), text);
    gtk_button_set_label(w->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(w->run_btn), "msdos-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(w->run_btn), "msdos-accent");
}

static void refresh_cpu(DbgWin *w)
{
    msdos_cpu_regs r;
    char buf[160];
    int i;
    if (msdosdebug_regs(w->dbg, &r) != 0) return;
    {
        const uint32_t vals[NREGS] = { r.eax, r.ebx, r.ecx, r.edx, r.esi, r.edi, r.ebp, r.esp, r.eip };
        const uint16_t svals[NSREGS] = { r.cs, r.ds, r.es, r.ss, r.fs, r.gs };
        const uint32_t bases[NSREGS] = { r.cs_base, r.ds_base, r.es_base, r.ss_base, r.fs_base,
                                         r.gs_base };
        w->is386 = r.is386 != 0;
        for (i = 0; i < NREGS; i++) {
            char name[8];
            g_strlcpy(name, r.is386 ? k_regs32[i] : k_regs16[i], sizeof name);
            for (char *p = name; *p; p++) *p = g_ascii_toupper(*p);
            gtk_label_set_text(w->reg_label[i], name);
            g_snprintf(buf, sizeof buf, r.is386 ? "%08X" : "%04X",
                       r.is386 ? vals[i] : vals[i] & 0xFFFFu);
            set_entry(w, w->reg[i], buf);
        }
        for (i = 0; i < NSREGS; i++) {
            gboolean shown = i < 4 || r.is386;
            gtk_widget_set_visible(gtk_widget_get_parent(GTK_WIDGET(w->sreg[i])), shown);
            gtk_widget_set_visible(GTK_WIDGET(w->sbase[i]), shown);
            g_snprintf(buf, sizeof buf, "%04X", svals[i]);
            set_entry(w, w->sreg[i], buf);
            g_snprintf(buf, sizeof buf, "base %05X", bases[i]);
            gtk_label_set_text(w->sbase[i], buf);
        }
    }
    w->updating = TRUE;
    for (i = 0; i < NFLAGS; i++)
        gtk_check_button_set_active(w->flag[i], (r.eflags >> k_flags[i].bit) & 1);
    w->updating = FALSE;
    {
        static const char *const modes[] = { "real mode", "protected mode", "virtual 8086 mode" };
        char video[160] = "";
        msdosdebug_video_info(w->dbg, video, sizeof video);
        g_strchomp(video);
        if (r.is386)
            g_snprintf(buf, sizeof buf, "%s   %s   CR0 %08X   %s", r.cpu,
                       modes[CLAMP(r.mode, 0, 2)], r.cr0, video);
        else
            g_snprintf(buf, sizeof buf, "%s   %s", r.cpu, video);
        gtk_label_set_text(w->cpu_info, buf);
    }
}

static void refresh_mem(DbgWin *w)
{
    static uint8_t mem[MEM_ROWS * 16];
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->mem_view);
    GString *t = g_string_sized_new(MEM_ROWS * 80);
    int row, col;

    msdosdebug_read(w->dbg, w->mem_top, mem, (int)sizeof mem);
    g_string_append(t, "          ");
    for (col = 0; col < 16; col++)
        g_string_append_printf(t, col == 8 ? " %X  " : "%X  ", col);
    g_string_append_c(t, '\n');
    for (row = 0; row < MEM_ROWS; row++) {
        const uint8_t *r = mem + row * 16;
        const uint32_t a = w->mem_top + (uint32_t)row * 16;
        /* real-mode style, segment the address's own 64K */
        g_string_append_printf(t, "%04X:%04X ", (a >> 4) & 0xF000, a & 0xFFFF);
        for (col = 0; col < 16; col++)
            g_string_append_printf(t, col == 8 ? " %02X " : "%02X ", r[col]);
        g_string_append_c(t, ' ');
        for (col = 0; col < 16; col++)
            g_string_append_c(t, (char)(r[col] >= 0x20 && r[col] < 0x7F ? r[col] : '.'));
        g_string_append_c(t, '\n');
    }
    set_text(w->mem_view, t->str);
    g_string_free(t, TRUE);

    /* the row Go to found stays marked across refreshes */
    if (w->mem_mark >= 0 && (uint32_t)w->mem_mark >= w->mem_top) {
        uint32_t off = (uint32_t)w->mem_mark - w->mem_top;
        if (off < MEM_ROWS * 16) {
            GtkTextIter s, e;
            gtk_text_buffer_get_iter_at_line(b, &s, 1 + (int)off / 16);
            gtk_text_buffer_get_iter_at_line(b, &e, 2 + (int)off / 16);
            gtk_text_buffer_apply_tag_by_name(b, "mark", &s, &e);
        }
    }
}

/* The disassembly lines that fit the view. */
static int visible_rows(DbgWin *w)
{
    GtkTextIter it;
    int y = 0, h = 0, height = gtk_widget_get_height(w->disasm_scroll);
    gtk_text_buffer_get_start_iter(gtk_text_view_get_buffer(w->disasm), &it);
    gtk_text_view_get_line_yrange(w->disasm, &it, &y, &h);
    if (h <= 0 || height <= 0) return 32;
    return CLAMP(height / h, 4, DISASM_WINDOW);
}

/* x86 code cannot be read backwards: the offset rows instructions before
 * off is the nearest start whose instructions land on off. */
static uint32_t rows_back(DbgWin *w, uint16_t seg, uint32_t off, int rows)
{
    static msdos_dis_line lines[DISASM_WINDOW * 4];
    const int count = MIN(rows * 7 + 1, (int)G_N_ELEMENTS(lines));
    int k, i, n;
    for (k = rows; k <= rows * 7 && (uint32_t)k <= off; k++) {
        n = msdosdebug_disassemble(w->dbg, seg, off - (uint32_t)k, count, lines);
        for (i = 0; i < n && lines[i].off < off; i++)
            ;
        if (i < n && lines[i].off == off && i >= rows)
            return lines[i - rows].off;
    }
    return off > (uint32_t)rows * 3 ? off - (uint32_t)rows * 3 : 0;
}

static uint32_t rows_on(DbgWin *w, uint16_t seg, uint32_t off, int rows)
{
    static msdos_dis_line lines[DISASM_WINDOW + 1];
    int n = msdosdebug_disassemble(w->dbg, seg, off, MIN(rows + 1, DISASM_WINDOW + 1), lines);
    return n > rows ? lines[rows].off : off;
}

static void refresh_disasm(DbgWin *w)
{
    static msdos_dis_line lines[DISASM_WINDOW];
    int bp[DISASM_WINDOW];
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GString *t = g_string_sized_new(DISASM_WINDOW * 100);
    GtkTextIter s, e;
    msdos_cpu_regs r;
    int n, pc_line = -1, i, rows = visible_rows(w);
    gboolean have_regs = msdosdebug_regs(w->dbg, &r) == 0;
    char *clean;

    /* Following, CS:IP stays put while it shows, else sits a third of the
     * way down. */
    if (have_regs && gtk_check_button_get_active(w->follow_pc)) {
        uint32_t pc = msdosdebug_linear(w->dbg, r.cs, r.eip);
        gboolean shows = FALSE;
        for (i = 1; i < w->line_count && i < rows - 2; i++)
            if (w->line_lin[i] == pc) shows = TRUE;
        if (!shows || w->top_seg != r.cs) {
            w->top_seg = r.cs;
            w->top_off = rows_back(w, r.cs, r.eip, rows / 3);
        }
    }

    n = msdosdebug_disassemble(w->dbg, w->top_seg, w->top_off, DISASM_WINDOW, lines);
    w->line_count = n;
    for (i = 0; i < n; i++) {
        const msdos_dis_line *l = &lines[i];
        gboolean is_pc = have_regs && l->seg == r.cs && l->off == r.eip;
        bp[i] = msdosdebug_bp_at(w->dbg, l->linear);   /* 1 on, 2 off */
        w->line_lin[i] = l->linear;
        if (is_pc) pc_line = i;
        g_string_append_printf(t, "%c%c %04X:%0*X  %-20s %s%s%s\n",
                               bp[i] == 2 ? 'o' : bp[i] ? '*' : ' ', is_pc ? '>' : ' ',
                               l->seg, l->off > 0xFFFF ? 8 : 4, l->off, l->bytes,
                               l->symbol, l->symbol[0] ? ": " : "", l->text);
    }
    if (n == 0)
        g_string_append(t, "(no disassembly)\n");
    clean = clean_text(t->str);
    gtk_text_buffer_set_text(b, clean, -1);
    g_free(clean);
    g_string_free(t, TRUE);

    for (i = 0; i < n; i++) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (bp[i]) {
            gtk_text_buffer_get_iter_at_line(b, &s, i);
            e = s;
            gtk_text_iter_forward_char(&e);
            gtk_text_buffer_apply_tag_by_name(b, bp[i] == 2 ? "bpoff" : "bp", &s, &e);
        }
        /* the selected line underlined */
        if ((int64_t)lines[i].linear == w->selected) {
            gtk_text_buffer_get_iter_at_line(b, &s, i);
            gtk_text_buffer_get_iter_at_line(b, &e, i + 1);
            gtk_text_buffer_apply_tag_by_name(b, "sel", &s, &e);
        }
    }
    /* the CS:IP line in the accent colour */
    if (pc_line >= 0 && pc_line < n) {
        gtk_text_buffer_get_iter_at_line(b, &s, pc_line);
        gtk_text_buffer_get_iter_at_line(b, &e, pc_line + 1);
        gtk_text_buffer_apply_tag_by_name(b, "pc", &s, &e);
    }
    /* the wheel scrolls by instructions: the view itself stays at its top */
    gtk_adjustment_set_value(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(w->disasm_scroll)), 0.0);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud);
static void bp_remove_clicked(GtkButton *b, gpointer ud);

static void refresh_bps(DbgWin *w)
{
    static msdos_breakpoint bps[BP_MAX];
    GString *sig = g_string_new(NULL);
    GtkWidget *child;
    int n = msdosdebug_bp_list(w->dbg, bps, BP_MAX), i;

    /* rebuilt only when it changed (each step refreshes) */
    for (i = 0; i < n; i++)
        g_string_append_printf(sig, "%d %d %u %s\n", bps[i].id, bps[i].enabled, bps[i].hits,
                               bps[i].text);
    if (w->bp_sig && strcmp(w->bp_sig, sig->str) == 0) {
        g_string_free(sig, TRUE);
        return;
    }
    g_free(w->bp_sig);
    w->bp_sig = g_string_free(sig, FALSE);

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->bp_list))) != NULL)
        gtk_list_box_remove(w->bp_list, child);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No breakpoints. Click the left edge of a disassembly "
                                     "line, or add one below.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_widget_set_margin_top(l, 12);
        gtk_widget_set_margin_bottom(l, 12);
        gtk_list_box_append(w->bp_list, l);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *en = gtk_check_button_new();
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *l;
        char line[200];
        g_snprintf(line, sizeof line, "#%-4d %-40s hits %u", bps[i].id, bps[i].text, bps[i].hits);
        l = gtk_label_new(line);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_add_css_class(l, "monospace");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(en), bps[i].enabled != 0);
        gtk_widget_set_tooltip_text(en, "Enabled");
        g_object_set_data(G_OBJECT(en), "bp", GINT_TO_POINTER(bps[i].id));
        g_object_set_data(G_OBJECT(rm), "bp", GINT_TO_POINTER(bps[i].id));
        g_signal_connect(en, "toggled", G_CALLBACK(bp_enable_toggled), w);
        g_signal_connect(rm, "clicked", G_CALLBACK(bp_remove_clicked), w);
        gtk_widget_add_css_class(rm, "flat");
        gtk_widget_set_tooltip_text(rm, "Remove");
        gtk_box_append(GTK_BOX(row), en);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(row), rm);
        gtk_list_box_append(w->bp_list, row);
    }
}

static void refresh_ints(DbgWin *w, gboolean force)
{
    static msdos_int_event e[TRACE_MAX];
    GString *t;
    int n = msdosdebug_int_trace(w->dbg, e, TRACE_MAX), i;

    w->updating = TRUE;
    gtk_switch_set_active(w->int_on, msdosdebug_int_trace_enabled(w->dbg) != 0);
    w->updating = FALSE;

    /* the vector table: changes rarely, cheap to read */
    t = g_string_new(NULL);
    for (i = 0; i < 256; i++) {
        uint16_t seg = 0, off = 0;
        msdosdebug_ivt(w->dbg, i, &seg, &off);
        g_string_append_printf(t, "%02X %04X:%04X%s", i, seg, off, (i & 3) == 3 ? "\n" : "   ");
    }
    set_text(w->ivt_text, t->str);
    g_string_free(t, TRUE);

    /* a log: set again only when it moved */
    if (!force && n == w->int_count && (n == 0 || e[n - 1].serial == w->int_last))
        return;
    w->int_count = n;
    w->int_last = n > 0 ? e[n - 1].serial : 0;
    log_note_reader(w->int_text);
    t = g_string_new("From       AX   BX   CX   DX   DS   ES    Call");
    for (i = 0; i < n; i++)
        g_string_append_printf(t, "\n%04X:%04X  %04X %04X %04X %04X %04X %04X  %s", e[i].cs,
                               e[i].ip & 0xFFFF, e[i].ax, e[i].bx, e[i].cx, e[i].dx, e[i].ds,
                               e[i].es, e[i].text);
    if (n == 0)
        g_string_append(t, msdosdebug_int_trace_enabled(w->dbg)
                               ? "\n(no INT calls since the trace was cleared)"
                               : "\n(turn the trace on to record INT calls; it costs speed)");
    set_text_at(w->int_text, t->str, force || following(w->int_text));
    g_string_free(t, TRUE);
}

static void refresh_fujinet(DbgWin *w, gboolean force)
{
    static msdos_fuji_packet p[TRACE_MAX];
    static char uart[2048];
    GString *t;
    char note[200];
    int n = msdosdebug_fujinet_trace(w->dbg, p, TRACE_MAX), i;
    const gboolean fuji = msdossession_fujinet_running(w->session) != 0;

    if (!fuji)
        g_strlcpy(note, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet).", sizeof note);
    else
        g_snprintf(note, sizeof note, "FujiNet is on COM%d%s.", msdossession_fujinet_com(w->session),
                   msdossession_fujinet_link(w->session) ? ", connected" : ", not connected");
    gtk_label_set_text(w->fuji_note, note);
    msdosdebug_uart(w->dbg, uart, sizeof uart);
    set_text(w->uart_text, uart);

    if (!force && n == w->fuji_count && (n == 0 || p[n - 1].serial == w->fuji_last))
        return;
    w->fuji_count = n;
    w->fuji_last = n > 0 ? p[n - 1].serial : 0;
    log_note_reader(w->fuji_text);
    t = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        int k;
        g_string_append_printf(t, "%s%s\n     ", i ? "\n" : "", p[i].text);
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            g_string_append_printf(t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) g_string_append(t, "\xe2\x80\xa6");
    }
    if (n == 0)
        g_string_append(t, "(no FujiBus packets since the trace was cleared)");
    set_text_at(w->fuji_text, t->str, force || following(w->fuji_text));
    g_string_free(t, TRUE);
}

static void refresh_page(DbgWin *w, int page)
{
    if (!msdossession_is_running(w->session)) return;
    switch (page) {
    case TAB_CPU: refresh_cpu(w); refresh_mem(w); break;
    case TAB_DISASM: refresh_disasm(w); break;
    case TAB_BREAKS: refresh_bps(w); break;
    case TAB_INTS: refresh_ints(w, FALSE); break;
    case TAB_FUJINET: refresh_fujinet(w, FALSE); break;
    default: break;
    }
}

static void refresh_all(DbgWin *w)
{
    refresh_status(w);
    refresh_page(w, gtk_notebook_get_current_page(w->notebook));
}

/* After anything that may have moved the engine: the console takes the
 * registers at a new stop, then the views follow. */
static void resync(DbgWin *w)
{
    gboolean stopped = msdosdebug_stopped(w->dbg) != 0;
    unsigned gen = msdosdebug_generation(w->dbg);
    if (stopped && (!w->was_stopped || gen != w->seen_generation))
        console_stop(w);
    w->was_stopped = stopped;
    w->seen_generation = gen;
    w->running_ticks = 0;
    refresh_all(w);
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
static void settle_and_sync(DbgWin *w)
{
    int i;
    for (i = 0; i < 40 && !msdosdebug_stopped(w->dbg); i++)
        g_usleep(1000);
    resync(w);
}

static void attach_and_refresh(DbgWin *w)
{
    msdosdebug_attach(w->dbg);     /* stops the machine */
    w->attached_running = msdossession_is_running(w->session) != 0;
    w->was_stopped = FALSE;
    settle_and_sync(w);
}

static gboolean tick_refresh(gpointer ud)
{
    DbgWin *w = ud;
    unsigned gen;
    gboolean stopped;
    if (!gtk_widget_get_visible(GTK_WIDGET(w->win))) return G_SOURCE_CONTINUE;
    if (!msdossession_is_running(w->session)) {
        if (w->attached_running) {
            w->attached_running = FALSE;
            refresh_status(w);
        }
        return G_SOURCE_CONTINUE;
    }
    /* Opened before the PC started, or showing across a restart: the
     * engine attaches (stopping the machine) as it comes up. */
    if (!w->attached_running) {
        msdosdebug_detach(w->dbg);
        attach_and_refresh(w);
        return G_SOURCE_CONTINUE;
    }
    gen = msdosdebug_generation(w->dbg);
    stopped = msdosdebug_stopped(w->dbg) != 0;
    if (gen != w->seen_generation || stopped != w->was_stopped) {
        resync(w);
    } else if (!stopped && ++w->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        int page = gtk_notebook_get_current_page(w->notebook);
        w->running_ticks = 0;
        refresh_status(w);
        if (page == TAB_CPU) refresh_cpu(w);
        if (page == TAB_INTS) refresh_ints(w, FALSE);
        if (page == TAB_FUJINET) refresh_fujinet(w, FALSE);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (msdosdebug_stopped(w->dbg)) msdosdebug_resume(w->dbg);
    else msdosdebug_stop(w->dbg);
    settle_and_sync(w);
}

static void on_step(GtkButton *b, gpointer ud) { (void)b; msdosdebug_step(((DbgWin *)ud)->dbg); settle_and_sync(ud); }
static void on_step_over(GtkButton *b, gpointer ud) { (void)b; msdosdebug_step_over(((DbgWin *)ud)->dbg); settle_and_sync(ud); }
static void on_step_out(GtkButton *b, gpointer ud) { (void)b; msdosdebug_step_out(((DbgWin *)ud)->dbg); settle_and_sync(ud); }

static void on_run_to(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (w->selected < 0) {
        toast(w, "Select a line in the Disassembly to run to");
        return;
    }
    msdosdebug_run_to(w->dbg, (uint32_t)w->selected);
    settle_and_sync(w);
}

static void toggle_breakpoint(DbgWin *w, uint32_t linear)
{
    if (msdosdebug_bp_toggle(w->dbg, linear) < 0)
        toast(w, "No room for another breakpoint");
    resync(w);
}

static void on_console(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    static char out[65536];
    char *cmd = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
    char line[600];

    if (!*g_strstrip(cmd)) {
        g_free(cmd);
        return;
    }
    if (w->history->len == 0 ||
        strcmp(g_ptr_array_index(w->history, w->history->len - 1), cmd) != 0)
        g_ptr_array_add(w->history, g_strdup(cmd));
    w->history_pos = w->history->len;
    g_snprintf(line, sizeof line, "-%s\n", cmd);
    log_to_end(w->con_out);     /* a command brings the reader back down */
    console_append(w, line);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");

    msdosdebug_command(w->dbg, cmd, out, sizeof out);
    g_free(cmd);
    console_output(w, out);
    /* a t, p or g that stops again reports where, as resync sees the stop */
    w->was_stopped = FALSE;
    settle_and_sync(w);
}

static void history_show(DbgWin *w)
{
    const char *text = w->history_pos < w->history->len
                           ? g_ptr_array_index(w->history, w->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(w->con_in), text);
    gtk_editable_set_position(GTK_EDITABLE(w->con_in), -1);
}

/* Up and Down walk the history. */
static gboolean on_console_key(GtkEventControllerKey *c, guint keyval, guint code,
                               GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    (void)c; (void)code; (void)st;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        if (w->history_pos > 0) {
            w->history_pos--;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (w->history_pos < w->history->len) {
            w->history_pos++;
            history_show(w);
        }
        return TRUE;
    }
    return FALSE;
}

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    const char *name = g_object_get_data(G_OBJECT(entry), "reg");
    char full[8];
    long v;
    /* the 32-bit name when the 32-bit value shows */
    if (w->is386 && g_object_get_data(G_OBJECT(entry), "gpr")) {
        g_snprintf(full, sizeof full, "e%s", name);
        name = full;
    }
    if (!parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &v))
        toast(w, "Not a number (hex, or # decimal)");
    else if (!msdosdebug_stopped(w->dbg))
        toast(w, "Stop the PC first");
    else if (msdosdebug_set_reg(w->dbg, name, (uint32_t)v) != 0)
        toast(w, "That register cannot be set here");
    resync(w);
}

static void on_flag_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    const char *name = g_object_get_data(G_OBJECT(b), "flag");
    if (w->updating || !msdosdebug_stopped(w->dbg)) return;
    msdosdebug_set_reg(w->dbg, name, gtk_check_button_get_active(b));
    resync(w);
}

/* Go to: the dump from just above an address (seg:off, a register pair, a
 * linear address or a symbol), its row marked and scrolled to. */
static void on_mem_goto(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->mem_view);
    GtkTextMark *mark;
    GtkTextIter it;
    uint16_t seg;
    uint32_t off, lin;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &seg, &off, &lin)) {
        w->mem_mark = -1;
        refresh_mem(w);
        toast(w, "Not an address (seg:off, ds:si, a linear address, a symbol)");
        return;
    }
    w->mem_mark = lin;
    w->mem_top = (lin > 0x100 ? lin - 0x100 : 0) & ~0xFu;
    refresh_mem(w);
    gtk_text_buffer_get_iter_at_line(b, &it, 1 + (int)((lin - w->mem_top) / 16));
    mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
    gtk_text_view_scroll_to_mark(w->mem_view, mark, 0.0, TRUE, 0.0, 0.3);
    gtk_text_buffer_delete_mark(b, mark);
}

/* The dump a page of MEM_ROWS on or back. */
static void on_mem_page(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    int dir = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "dir"));
    if (dir < 0)
        w->mem_top = w->mem_top > MEM_ROWS * 16 ? w->mem_top - MEM_ROWS * 16 : 0;
    else
        w->mem_top += MEM_ROWS * 16;
    refresh_mem(w);
}

static void on_mem_write(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    uint16_t seg;
    uint32_t off, lin;
    long v;
    uint8_t byte;
    (void)entry;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->mem_addr)), &seg, &off, &lin)) {
        toast(w, "Not an address");
        return;
    }
    if (!parse_num(gtk_editable_get_text(GTK_EDITABLE(w->mem_val)), &v) || v < 0 || v > 0xFF) {
        toast(w, "The value is a byte (00-FF)");
        return;
    }
    byte = (uint8_t)v;
    msdosdebug_write(w->dbg, lin, &byte, 1);
    resync(w);
}

/* The selected line's underline moved, without disassembling again. */
static void mark_selected(DbgWin *w)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkTextIter s, e;
    int i;
    gtk_text_buffer_get_bounds(b, &s, &e);
    gtk_text_buffer_remove_tag_by_name(b, "sel", &s, &e);
    for (i = 0; i < w->line_count; i++)
        if ((int64_t)w->line_lin[i] == w->selected) {
            gtk_text_buffer_get_iter_at_line(b, &s, i);
            gtk_text_buffer_get_iter_at_line(b, &e, i + 1);
            gtk_text_buffer_apply_tag_by_name(b, "sel", &s, &e);
        }
}

/* A click selects a line (Run to Cursor); a click on its left edge, or a
 * double-click, toggles its breakpoint. */
static void on_disasm_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextIter it;
    int bx, by, line;
    gboolean gutter;
    (void)g;
    gtk_text_view_window_to_buffer_coords(w->disasm, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    gtk_text_view_get_iter_at_location(w->disasm, &it, bx, by);
    line = gtk_text_iter_get_line(&it);
    if (line < 0 || line >= w->line_count) return;
    gutter = gtk_text_iter_get_line_offset(&it) <= 2;
    w->selected = w->line_lin[line];
    if (gutter ? n == 1 : n == 2)
        toggle_breakpoint(w, w->line_lin[line]);
    else
        mark_selected(w);
}

static void on_follow_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    refresh_disasm(ud);
}

static void on_jump(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    uint16_t seg;
    uint32_t off, lin;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &seg, &off, &lin)) {
        toast(w, "Not an address (seg:off, cs:ip, a linear address, a symbol)");
        return;
    }
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->top_seg = seg;
    w->top_off = off;
    w->selected = lin;
    refresh_disasm(w);
}

static gboolean on_disasm_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud)
{
    DbgWin *w = ud;
    int rows = (int)(dy * 3);
    (void)c; (void)dx;
    if (!rows) rows = dy > 0 ? 1 : -1;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->top_off = rows > 0 ? rows_on(w, w->top_seg, w->top_off, rows)
                          : rows_back(w, w->top_seg, w->top_off, -rows);
    refresh_disasm(w);
    return TRUE;
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    /* the list itself follows on the next tick (it rebuilds) */
    msdosdebug_bp_enable(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")),
                         gtk_check_button_get_active(b));
}

static void bp_remove_clicked(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    msdosdebug_bp_remove(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")));
}

/* Execute and memory breakpoints take addresses; port and INT ones
 * numbers. The end is optional; AH only means something for an INT. */
static void on_bp_add(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    guint type = gtk_drop_down_get_selected(w->bp_type);
    const char *start = gtk_editable_get_text(GTK_EDITABLE(w->bp_start));
    const char *end = gtk_editable_get_text(GTK_EDITABLE(w->bp_end));
    const char *ahs = gtk_editable_get_text(GTK_EDITABLE(w->bp_ah));
    uint32_t a, b;
    long ah = -1;
    (void)wgt;
    if (type >= MSDOS_BP_TYPES) type = MSDOS_BP_EXEC;
    if (type <= MSDOS_BP_ACCESS) {
        uint16_t seg;
        uint32_t off;
        if (!parse_addr(w, start, &seg, &off, &a)) {
            gtk_label_set_text(w->bp_msg, "Not an address (seg:off, a linear address, a symbol)");
            return;
        }
        b = a;
        if (*end && !parse_addr(w, end, &seg, &off, &b)) {
            gtk_label_set_text(w->bp_msg, "The end is not an address");
            return;
        }
    } else {
        long v, e;
        if (!parse_num(start, &v) || v < 0 || v > (type == MSDOS_BP_INT ? 0xFF : 0xFFFF)) {
            gtk_label_set_text(w->bp_msg, type == MSDOS_BP_INT ? "An interrupt is 00-FF"
                                                               : "A port is 0000-FFFF");
            return;
        }
        e = v;
        if (*end && type != MSDOS_BP_INT && (!parse_num(end, &e) || e < v || e > 0xFFFF)) {
            gtk_label_set_text(w->bp_msg, "The last port is a port at or after the first");
            return;
        }
        a = (uint32_t)v;
        b = (uint32_t)e;
        if (type == MSDOS_BP_INT && *ahs && (!parse_num(ahs, &ah) || ah < 0 || ah > 0xFF)) {
            gtk_label_set_text(w->bp_msg, "AH is 00-FF, or empty for any");
            return;
        }
    }
    if (b < a) {
        gtk_label_set_text(w->bp_msg, "The end is before the start");
        return;
    }
    if (msdosdebug_bp_add(w->dbg, (int)type, a, b, (int)ah) < 0) {
        gtk_label_set_text(w->bp_msg, "No room for another breakpoint");
        return;
    }
    gtk_label_set_text(w->bp_msg, "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_start), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_end), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_ah), "");
    resync(w);
}

static void on_bp_type(GObject *dd, GParamSpec *ps, gpointer ud)
{
    DbgWin *w = ud;
    guint type = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    (void)ps;
    gtk_widget_set_sensitive(GTK_WIDGET(w->bp_ah), type == MSDOS_BP_INT);
    gtk_widget_set_sensitive(GTK_WIDGET(w->bp_end), type != MSDOS_BP_INT);
    gtk_entry_set_placeholder_text(w->bp_start, type <= MSDOS_BP_ACCESS ? "1234:0010 / symbol"
                                                : type == MSDOS_BP_INT ? "21" : "2F8");
    gtk_entry_set_placeholder_text(w->bp_end, type <= MSDOS_BP_ACCESS ? "(end)" : "(last port)");
}

static void on_bp_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    msdosdebug_bp_clear(w->dbg);
    resync(w);
}

static void on_int_switch(GObject *sw, GParamSpec *ps, gpointer ud)
{
    DbgWin *w = ud;
    (void)ps;
    if (w->updating) return;
    msdosdebug_int_trace_enable(w->dbg, gtk_switch_get_active(GTK_SWITCH(sw)));
    refresh_ints(w, TRUE);
}

static void on_int_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    msdosdebug_int_trace_clear(w->dbg);
    refresh_ints(w, TRUE);
}

static void on_fuji_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    msdosdebug_fujinet_trace_clear(w->dbg);
    refresh_fujinet(w, TRUE);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    char msg[512];
    long seg = 0;
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    if (!parse_num(gtk_editable_get_text(GTK_EDITABLE(w->sym_seg)), &seg) || seg < 0 || seg > 0xFFFF)
        seg = 0;
    if (msdosdebug_load_symbols(w->dbg, path, (uint16_t)seg, msg, sizeof msg) < 0 && !msg[0])
        g_strlcpy(msg, "No symbols found", sizeof msg);
    say(w, msg);
    resync(w);
}

static void on_symbols(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *maps = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    gtk_file_filter_set_name(maps, "Linker maps (*.map)");
    gtk_file_filter_add_suffix(maps, "map");
    gtk_file_filter_add_suffix(maps, "MAP");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, maps);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Load Symbols (MS LINK or Watcom WLINK .map)");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, w->win, NULL, on_symbols_chosen, w);
    g_object_unref(maps);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

static void on_symbols_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    msdosdebug_clear_symbols(w->dbg);
    say(w, "Symbols cleared");
    resync(w);
}

/* what Ctrl+Shift+H, F1 and the Help button open: the tab's section */
static void set_help_context(DbgWin *w, int tab)
{
    char ctx[48];
    g_snprintf(ctx, sizeof ctx, "debugger/%s", k_tabs[tab].name);
    msdos_help_set_context(w->win, ctx);
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud)
{
    (void)nb; (void)page;
    set_help_context(ud, (int)num);
    refresh_page(ud, (int)num);
}

static void hide_window(DbgWin *w)
{
    msdosdebug_detach(w->dbg);     /* lets it run */
    w->attached_running = FALSE;
    gtk_widget_set_visible(GTK_WIDGET(w->win), FALSE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    const GdkModifierType mods = st & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK);
    (void)c; (void)code;
    /* the main window's Ctrl+Shift+D closes it here too */
    if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) &&
        (keyval == GDK_KEY_D || keyval == GDK_KEY_d)) {
        hide_window(w);
        return TRUE;
    }
    switch (keyval) {
    case GDK_KEY_F1: msdos_help_for(w->win); return TRUE;
    case GDK_KEY_F5: on_run(NULL, w); return TRUE;
    case GDK_KEY_F7: on_step(NULL, w); return TRUE;
    case GDK_KEY_F8:
        if (st & GDK_SHIFT_MASK) on_step_out(NULL, w); else on_step_over(NULL, w);
        return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *win, gpointer ud)
{
    (void)win;
    hide_window(ud);
    return TRUE;
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *build_toolbar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b;
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
#define TB(label, tip, cb) do { b = gtk_button_new_with_label(label); \
    gtk_widget_set_tooltip_text(b, tip); \
    g_signal_connect(b, "clicked", G_CALLBACK(cb), w); gtk_box_append(GTK_BOX(bar), b); } while (0)
    w->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(w->run_btn, "clicked", G_CALLBACK(on_run), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->run_btn));
    TB("Step (F7)", "One instruction", on_step);
    TB("Step Over (F8)", "Over a CALL, INT, LOOP or REP, to the instruction after it", on_step_over);
    TB("Step Out (\xe2\x87\xa7""F8)", "On until the routine returns (RET, RETF or IRET)", on_step_out);
    TB("Run to Cursor", "On to the line selected in the Disassembly", on_run_to);
#undef TB

    w->status = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->status), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(w->status), TRUE);
    gtk_label_set_xalign(w->status, 1.0);
    gtk_label_set_ellipsize(w->status, PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->status));
    return bar;
}

/* The header bar's end: symbols, from a linker map at a load segment. */
static void build_header(DbgWin *w, AdwHeaderBar *header)
{
    GtkWidget *sym = gtk_menu_button_new(), *pop = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *load = gtk_button_new_with_label("Choose a .map\xe2\x80\xa6");
    GtkWidget *clear = gtk_button_new_with_label("Clear Symbols");

    gtk_menu_button_set_label(GTK_MENU_BUTTON(sym), "Symbols");
    gtk_widget_set_tooltip_text(sym, "Labels from an MS LINK or Watcom WLINK .map file");
    w->sym_seg = small_entry("0000", 6);
    gtk_box_append(GTK_BOX(row), labeled("Load segment", GTK_WIDGET(w->sym_seg)));
    g_signal_connect(load, "clicked", G_CALLBACK(on_symbols), w);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_symbols_clear), w);
    gtk_widget_add_css_class(clear, "flat");
    gtk_box_append(GTK_BOX(box), title_label("Symbols"));
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), hint_label("The segment the program was loaded at: a .COM's "
                                            "PSP, an .EXE's PSP + 10h."));
    gtk_box_append(GTK_BOX(box), load);
    gtk_box_append(GTK_BOX(box), clear);
    gtk_widget_set_size_request(box, 280, -1);
    padded(box);
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(sym), pop);
    adw_header_bar_pack_end(header, msdos_help_button(NULL));
    adw_header_bar_pack_end(header, sym);
}

static GtkWidget *build_console(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&w->con_out, FALSE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *prompt = gtk_label_new("-");
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->con_out);
    GtkEventController *keys;

    gtk_text_view_set_wrap_mode(w->con_out, GTK_WRAP_WORD_CHAR);
    gtk_text_buffer_set_text(b, "DEBUG.COM's commands: r d e u g t p, plus bp/bpr/bpw/bpi/bpo/"
                                "bpint, bl, bc, i, o. Type h for the list; Up/Down recall.\n", -1);
    make_log(w->con_out);

    gtk_widget_add_css_class(prompt, "monospace");
    w->con_in = GTK_ENTRY(gtk_entry_new());
    gtk_widget_add_css_class(GTK_WIDGET(w->con_in), "monospace");
    gtk_widget_set_hexpand(GTK_WIDGET(w->con_in), TRUE);
    gtk_entry_set_placeholder_text(w->con_in, "u cs:ip   d ds:0   bpo 2f8   bpint 21 9   g");
    g_signal_connect(w->con_in, "activate", G_CALLBACK(on_console), w);
    /* ahead of the entry's own handling, so Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_console_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->con_in), keys);
    w->history = g_ptr_array_new_with_free_func(g_free);

    gtk_box_append(GTK_BOX(row), prompt);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->con_in));
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    return padded(box);
}

static GtkWidget *reg_entry(DbgWin *w, const char *name, gboolean gpr, int chars)
{
    GtkEntry *e = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_max_length(e, 10);
    gtk_editable_set_width_chars(GTK_EDITABLE(e), chars);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(e), chars);
    gtk_widget_add_css_class(GTK_WIDGET(e), "monospace");
    gtk_widget_set_tooltip_text(GTK_WIDGET(e), "Enter sets it (hex)");
    g_object_set_data(G_OBJECT(e), "reg", (gpointer)name);
    if (gpr) g_object_set_data(G_OBJECT(e), "gpr", GINT_TO_POINTER(1));
    g_signal_connect(e, "activate", G_CALLBACK(on_reg_activate), w);
    return GTK_WIDGET(e);
}

static GtkWidget *build_cpu(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *regs = gtk_grid_new();
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mem = mono_view(&w->mem_view, FALSE);
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b;
    int i;

    gtk_grid_set_row_spacing(GTK_GRID(regs), 6);
    gtk_grid_set_column_spacing(GTK_GRID(regs), 8);
    /* the general registers across, the segment registers (with their
     * bases) below */
    for (i = 0; i < NREGS; i++) {
        GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        w->reg_label[i] = GTK_LABEL(gtk_label_new(k_regs16[i]));
        gtk_widget_add_css_class(GTK_WIDGET(w->reg_label[i]), "dim-label");
        gtk_label_set_width_chars(w->reg_label[i], 3);
        gtk_label_set_xalign(w->reg_label[i], 1);
        w->reg[i] = GTK_ENTRY(reg_entry(w, k_regs16[i], TRUE, 8));
        gtk_box_append(GTK_BOX(cell), GTK_WIDGET(w->reg_label[i]));
        gtk_box_append(GTK_BOX(cell), GTK_WIDGET(w->reg[i]));
        gtk_grid_attach(GTK_GRID(regs), cell, i, 0, 1, 1);
    }
    for (i = 0; i < NSREGS; i++) {
        char name[4];
        GtkWidget *cell;
        g_snprintf(name, sizeof name, "%c%c", g_ascii_toupper(k_sregs[i][0]),
                   g_ascii_toupper(k_sregs[i][1]));
        w->sreg[i] = GTK_ENTRY(reg_entry(w, k_sregs[i], FALSE, 4));
        cell = labeled(name, GTK_WIDGET(w->sreg[i]));
        gtk_grid_attach(GTK_GRID(regs), cell, i, 1, 1, 1);
        w->sbase[i] = GTK_LABEL(gtk_label_new(""));
        gtk_widget_add_css_class(GTK_WIDGET(w->sbase[i]), "dim-label");
        gtk_widget_add_css_class(GTK_WIDGET(w->sbase[i]), "caption");
        gtk_grid_attach(GTK_GRID(regs), GTK_WIDGET(w->sbase[i]), i, 2, 1, 1);
    }
    for (i = 0; i < NFLAGS; i++) {
        char name[4];
        g_snprintf(name, sizeof name, "%c%c", g_ascii_toupper(k_flags[i].name[0]),
                   g_ascii_toupper(k_flags[i].name[1]));
        w->flag[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(name));
        g_object_set_data(G_OBJECT(w->flag[i]), "flag", (gpointer)k_flags[i].name);
        g_signal_connect(w->flag[i], "toggled", G_CALLBACK(on_flag_toggled), w);
        gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->flag[i]));
    }
    w->cpu_info = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->cpu_info), "dim-label");
    gtk_widget_set_margin_start(GTK_WIDGET(w->cpu_info), 16);
    gtk_label_set_ellipsize(w->cpu_info, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(w->cpu_info), TRUE);
    gtk_label_set_xalign(w->cpu_info, 0);
    gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->cpu_info));

    /* the dump: Go to, a page back or on, and a write */
    w->mem_goto = small_entry("ds:si / B800:0 / symbol", 18);
    g_signal_connect(w->mem_goto, "activate", G_CALLBACK(on_mem_goto), w);
    gtk_box_append(GTK_BOX(nav), labeled("Go to", GTK_WIDGET(w->mem_goto)));
    b = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(b, "Back 4K");
    g_object_set_data(G_OBJECT(b), "dir", GINT_TO_POINTER(-1));
    g_signal_connect(b, "clicked", G_CALLBACK(on_mem_page), w);
    gtk_box_append(GTK_BOX(nav), b);
    b = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(b, "On 4K");
    g_object_set_data(G_OBJECT(b), "dir", GINT_TO_POINTER(1));
    g_signal_connect(b, "clicked", G_CALLBACK(on_mem_page), w);
    gtk_box_append(GTK_BOX(nav), b);
    w->mem_addr = small_entry("0040:0017", 12);
    w->mem_val = small_entry("00", 4);
    gtk_widget_set_margin_start(GTK_WIDGET(w->mem_addr), 12);
    g_signal_connect(w->mem_addr, "activate", G_CALLBACK(on_mem_write), w);
    g_signal_connect(w->mem_val, "activate", G_CALLBACK(on_mem_write), w);
    gtk_box_append(GTK_BOX(nav), labeled("Write", GTK_WIDGET(w->mem_addr)));
    gtk_box_append(GTK_BOX(nav), labeled("value", GTK_WIDGET(w->mem_val)));

    gtk_box_append(GTK_BOX(box), regs);
    gtk_box_append(GTK_BOX(box), flags);
    gtk_box_append(GTK_BOX(box), title_label("Memory (linear; video RAM at A0000-BFFFF reads "
                                             "as the CPU would)"));
    gtk_box_append(GTK_BOX(box), mem);
    gtk_box_append(GTK_BOX(box), nav);
    accent_tag(w->mem_view, "mark");
    w->mem_top = 0;
    w->mem_mark = -1;
    return padded(box);
}

static GtkWidget *build_disasm(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&w->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkWidget *hint;

    /* created in priority order: the CS:IP line's colours win */
    gtk_text_buffer_create_tag(b, "bp", "foreground", "#e01b24", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(b, "bpoff", "foreground", "#9a9996", NULL);
    gtk_text_buffer_create_tag(b, "sel", "underline", PANGO_UNDERLINE_SINGLE,
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    accent_tag(w->disasm, "pc");

    w->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow CS:IP"));
    gtk_check_button_set_active(w->follow_pc, TRUE);
    g_signal_connect(w->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), w);
    w->jump = small_entry("F000:FFF0 or _main", 18);
    g_signal_connect(w->jump, "activate", G_CALLBACK(on_jump), w);
    hint = hint_label("Click a line to select it (Run to Cursor); click its left edge or "
                      "double-click it for a breakpoint (* on, o off); scroll to browse");
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(w->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), scrollc);
    /* no scrollbar and no height of its own: as many lines show as fit */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_EXTERNAL);
    w->disasm_scroll = scroll;

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    w->top_seg = 0xF000;
    w->top_off = 0xFFF0;
    w->selected = -1;
    return padded(box);
}

static GtkWidget *build_breaks(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add, *clear, *hint;
    static const char *const types[] = { "Execute", "Memory read", "Memory write",
                                         "Memory read/write", "Port IN", "Port OUT",
                                         "Port IN/OUT", "INT", NULL };

    hint = hint_label("Execute stops before the instruction runs; memory, ports and INT after "
                      "the instruction that touched them.");
    gtk_widget_set_hexpand(hint, TRUE);
    clear = gtk_button_new_with_label("Clear All");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_bp_clear), w);
    gtk_box_append(GTK_BOX(top), hint);
    gtk_box_append(GTK_BOX(top), clear);

    w->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->bp_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_list), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(w->bp_list), GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    w->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    w->bp_start = small_entry("1234:0010 / symbol", 18);
    w->bp_end = small_entry("(end)", 12);
    w->bp_ah = small_entry("AH (any)", 8);
    gtk_widget_set_sensitive(GTK_WIDGET(w->bp_ah), FALSE);
    g_signal_connect(w->bp_type, "notify::selected", G_CALLBACK(on_bp_type), w);
    g_signal_connect(w->bp_start, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_end, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_ah, "activate", G_CALLBACK(on_bp_add), w);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_bp_add), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_type));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_start));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_end));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_ah));
    gtk_box_append(GTK_BOX(row), add);

    w->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(w->bp_msg, 0);
    gtk_label_set_wrap(w->bp_msg, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_msg), "error");

    gtk_box_append(GTK_BOX(box), top);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->bp_msg));
    return padded(box);
}

static GtkWidget *build_ints(DbgWin *w)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *title = title_label("INT calls");

    w->int_on = GTK_SWITCH(gtk_switch_new());
    gtk_widget_set_valign(GTK_WIDGET(w->int_on), GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->int_on), "Record every INT the PC runs (costs speed)");
    g_signal_connect(w->int_on, "notify::active", G_CALLBACK(on_int_switch), w);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_int_clear), w);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_append(GTK_BOX(row), title);
    gtk_box_append(GTK_BOX(row), labeled("Trace", GTK_WIDGET(w->int_on)));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), mono_view(&w->int_text, FALSE));
    make_log(w->int_text);

    gtk_box_append(GTK_BOX(right), title_label("Vector table"));
    gtk_box_append(GTK_BOX(right), mono_view(&w->ivt_text, FALSE));

    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(left, 620, -1);
    gtk_widget_set_size_request(right, 360, -1);
    w->int_count = -1;
    return padded(paned);
}

static GtkWidget *build_fujinet(DbgWin *w)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");

    w->fuji_note = GTK_LABEL(hint_label(""));
    gtk_widget_set_hexpand(GTK_WIDGET(w->fuji_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->fuji_note));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), title_label("FujiBus packets (SLIP-decoded)"));
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), mono_view(&w->fuji_text, FALSE));
    make_log(w->fuji_text);

    gtk_box_append(GTK_BOX(right), title_label("FujiNet's UART"));
    gtk_box_append(GTK_BOX(right), mono_view(&w->uart_text, FALSE));

    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(left, 620, -1);
    gtk_widget_set_size_request(right, 320, -1);
    w->fuji_count = -1;
    return padded(paned);
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (w->timer) g_source_remove(w->timer);
    msdosdebug_detach(w->dbg);
    g_free(w->bp_sig);
    if (w->history) g_ptr_array_unref(w->history);
    if (g_win == w) g_win = NULL;
    g_free(w);
}

/* MSDOS_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("memory"...). */
static int tab_from_env(void)
{
    const char *tab = g_getenv("MSDOS_DEBUGGER_TAB");
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
            if (pass == 0 && g_ascii_strncasecmp(tab, k_tabs[i].name, len) == 0) return i;
            if (pass == 1 && g_ascii_strncasecmp(tab, k_tabs[i].label, len) == 0) return i;
            if (pass == 2) {
                const char *p = k_tabs[i].label;
                while ((p = strchr(p, ' ')) != NULL)
                    if (g_ascii_strncasecmp(tab, ++p, len) == 0) return i;
            }
        }
    return -1;
}

void msdos_debugger_show(GtkWindow *parent, msdossession *session)
{
    DbgWin *w;
    GtkWidget *toolbar, *header, *root, *notebook, *overlay;
    GtkEventController *keys;
    int tab;

    if (g_win) {
        gtk_window_present(g_win->win);
        attach_and_refresh(g_win);
        return;
    }
    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = msdossession_debugger(session);
    g_win = w;

    w->win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(w->win, "Debugger");
    gtk_window_set_default_size(w->win, 1150, 800);
    if (parent) {
        gtk_window_set_transient_for(w->win, parent);
        gtk_window_set_application(w->win, gtk_window_get_application(parent));
        gtk_window_set_destroy_with_parent(w->win, TRUE);
    }
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_close), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(w));
    notebook = gtk_notebook_new();
    w->notebook = GTK_NOTEBOOK(notebook);
    gtk_notebook_set_scrollable(w->notebook, TRUE);
    gtk_widget_set_vexpand(notebook, TRUE);
    {
        GtkWidget *pages[TAB_COUNT];
        int i;
        pages[TAB_CONSOLE] = build_console(w);
        pages[TAB_CPU] = build_cpu(w);
        pages[TAB_DISASM] = build_disasm(w);
        pages[TAB_BREAKS] = build_breaks(w);
        pages[TAB_INTS] = build_ints(w);
        pages[TAB_FUJINET] = build_fujinet(w);
        for (i = 0; i < TAB_COUNT; i++)
            gtk_notebook_append_page(w->notebook, pages[i], gtk_label_new(k_tabs[i].label));
    }
    gtk_box_append(GTK_BOX(root), notebook);
    tab = tab_from_env();
    if (tab >= 0) gtk_notebook_set_current_page(w->notebook, tab);
    set_help_context(w, tab >= 0 ? tab : 0);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), w);

    overlay = adw_toast_overlay_new();
    w->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(w->toasts, root);

    header = adw_header_bar_new();
    build_header(w, ADW_HEADER_BAR(header));
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), overlay);
    adw_window_set_content(ADW_WINDOW(w->win), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);

    gtk_window_present(w->win);
    attach_and_refresh(w);
    w->timer = g_timeout_add(100, tick_refresh, w);
}

void msdos_debugger_toggle(GtkWindow *parent, msdossession *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        msdos_debugger_show(parent, session);
}
