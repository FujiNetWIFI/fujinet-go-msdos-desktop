/*
 * Debugger window (GTK4/libadwaita) over the session's PC debugger engine,
 * via core/include/msdosdebug.h. Thirteen tabs, in the family order: Console
 * (the prompt: DEBUG.COM's commands plus expressions, completion, history),
 * Disassembly (a breakpoint gutter and a selectable row), Memory (linear
 * addresses in 4K pages, with a Poke bar and a search), Breakpoints (Execute /
 * memory Read, Write, Access / port In, Out, In/Out / INT, each with an
 * expression condition), Watches, Machine & I/O (the CPU, A20, RAM, the
 * controllers, the ports, the drives and the memory map), Trace and History
 * (one ring of executed instructions), Video (dbg_video.c), Sound (the PC
 * speaker, the OPL chip, the Sound Blaster), Symbols (from linker maps),
 * Interrupts (a DOS/BIOS call trace and the vector table) and FujiNet (its
 * UART and the FujiBus packets on its COM port). An editable register strip
 * sits under all of them. A toolbar carries the stepping controls, file saves
 * and Pop Out (a tab in a window of its own; Dock puts it back); the family's
 * keys apply (F5 run / stop, F7 step, F8 step over, Shift+F8 step out, F9
 * breakpoint on the selected line).
 *
 * Showing this window attaches (which stops the machine, as on every
 * sibling); hiding it detaches and lets the machine run on.
 *
 * The window polls the engine's generation counter and stop snapshot on a
 * short timer and refreshes only what is on screen (the tab showing, and any
 * tab in a window of its own), and only when something changed, so a stopped
 * machine costs nothing and a running one shows live values twice a second.
 * It does not use the engine's stop callback: that fires on the emulation
 * thread, and the snapshot says the same thing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msdosdebug.h"
#include "msdosmemtools.h"
#include "msdossnd.h"

#include "dbg_video.h"
#include "../help/help_window.h"

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define NREGS 9
#define NSEGS 6
#define NFLAGS 9
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define MEM_PAGE (MEM_ROWS * 16)
#define POKE_MAX 64
#define BP_MAX MSDOS_MAX_BREAKPOINTS
#define TRACE_MAX 512
#define TICK_MS 50
#define LIVE_TICKS 10           /* live values twice a second while the machine runs */
#define HIST_SHOWN 600          /* lines on the Trace / History page */
#define HIST_COPY 4000
#define CONSOLE_KEEP (256 * 1024)

/* The tabs, in order; MSDOS_DEBUGGER_TAB and MSDOS_DEBUGGER_POPOUT take a number or the
 * start of a name. */
enum { TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_MACHINE, TAB_TRACE,
       TAB_HISTORY, TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_INTS, TAB_FUJINET, TAB_COUNT };

static const char *const k_tab_names[TAB_COUNT] = {
    "Console", "Disassembly", "Memory", "Breakpoints", "Watches", "Machine & I/O", "Trace",
    "History", "Video", "Sound", "Symbols", "Interrupts", "FujiNet",
};

/* Help's contexts (msdoshelp.h), one per tab */
static const char *const k_tab_contexts[TAB_COUNT] = {
    "debugger/console", "debugger/disassembly", "debugger/memory", "debugger/breakpoints",
    "debugger/watches", "debugger/machine", "debugger/trace", "debugger/history",
    "debugger/video", "debugger/sound", "debugger/symbols", "debugger/interrupts",
    "debugger/fujinet",
};

/* The tabs' older names, which the environment variables still take. */
static const struct { const char *name; int tab; } k_tab_aliases[] = {
    { "cpu", TAB_DISASM }, { "registers", TAB_DISASM }, { "io", TAB_MACHINE },
    { "i/o", TAB_MACHINE }, { "machine", TAB_MACHINE }, { "int", TAB_INTS },
    { "ivt", TAB_INTS }, { "speaker", TAB_SOUND }, { "opl", TAB_SOUND },
};

/* the general registers, as msdosdebug_register_set names them */
static const char *const k_regs16[NREGS] = { "ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "ip" };
static const char *const k_regs32[NREGS] = { "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip" };
static const char *const k_sregs[NSEGS] = { "cs", "ds", "es", "ss", "fs", "gs" };
/* the flags shown, with their EFLAGS bits */
static const struct { const char *name; int bit; } k_flags[NFLAGS] = {
    { "of", 11 }, { "df", 10 }, { "if", 9 }, { "tf", 8 }, { "sf", 7 }, { "zf", 6 },
    { "af", 4 }, { "pf", 2 }, { "cf", 0 },
};

typedef struct {
    GtkWindow *win;
    msdossession *session;
    msdosdebug *dbg;
    unsigned seen_generation;
    uint64_t seen_serial;
    gboolean was_stopped;
    gboolean attached_running;  /* attached while the machine was running */
    gboolean torn_down;
    int running_ticks;
    guint timer;
    gulong dark_id;

    AdwWindowTitle *title;      /* "Debugger", with the stop reason under it */
    GtkButton *run_btn;
    AdwToastOverlay *toasts;
    GtkNotebook *notebook;
    GtkWidget *pages[TAB_COUNT];
    GtkWindow *detached[TAB_COUNT];
    GtkTextTag *tag_pc, *tag_mem_mark;

    /* console */
    GtkTextView *console_out;
    GtkEntry *console_in;
    GPtrArray *history;
    guint history_pos;

    /* the register strip */
    GtkLabel *reg_label[NREGS];
    GtkEntry *reg[NREGS];
    GtkEntry *sreg[NSEGS];
    GtkWidget *sreg_cell[NSEGS];
    GtkLabel *sbase[NSEGS];
    GtkCheckButton *flag[NFLAGS];
    GtkLabel *state;
    gboolean updating_flags;
    gboolean force_entries;     /* a refresh may overwrite a focused entry */
    gboolean is386;

    /* memory */
    GtkTextView *mem_view;
    GtkEntry *mem_goto, *mem_write;
    uint32_t mem_base;
    msdos_region map[16];       /* the memory map, fetched once per stop */
    int map_count;
    uint64_t map_serial;
    gboolean map_valid;
    gboolean mem_hl;            /* the range Go to / a watch / a search found, marked */
    uint32_t mem_hl_lo, mem_hl_hi;
    GtkDropDown *srch_kind;
    GtkEntry *srch_entry;
    GtkLabel *srch_note;

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
    GtkDropDown *bp_type;
    GtkEntry *bp_addr, *bp_ah, *bp_cond;
    GtkLabel *bp_msg, *bp_stop;
    GString *bp_sig;            /* what the list shows, to skip a needless rebuild */

    /* watches */
    GtkEntry *wt_entry;
    GtkListBox *wt_list;
    GString *wt_sig;

    /* machine & i/o, sound */
    GtkTextView *machine_text, *map_text, *sound_text;

    /* trace (0, live) and history (1, when stopped) share one ring */
    GtkToggleButton *h_rec[2];
    GtkDropDown *h_filter[2];
    GtkEntry *h_range[2];
    GtkTextView *h_text[2];
    GtkLabel *h_note[2];
    uint64_t h_seen[2];
    gboolean h_sync;

    /* symbols */
    GtkListBox *symlist;
    GtkEntry *sym_filter, *sym_seg;
    GtkLabel *sym_count;
    int sym_built;              /* entries the list was built from, -1 = never */
    unsigned sym_gen;
    char sym_text[64];

    /* interrupts */
    GtkSwitch *int_on;
    GtkTextView *int_text, *ivt_text;
    uint64_t int_last;
    int int_count;
    gboolean updating_int;

    /* fujinet */
    GtkTextView *uart_text, *fuji_text;
    GtkLabel *fuji_note;
    uint64_t fuji_last;
    int fuji_count;
} DbgWin;

static DbgWin *g_win;

static void refresh_now(DbgWin *self);

/* ---- accent --------------------------------------------------------------- */

static gboolean dark_theme(void)
{
    return adw_style_manager_get_dark(adw_style_manager_get_default());
}

/* The accent as a fill (with its ink) and as text on the theme: the icon's
 * blue is unreadable on a dark theme, so there the light variant is used with
 * black ink. */
static unsigned accent_fill(void) { return dark_theme() ? MSDOSSESSION_ACCENT_LIGHT_RGB : MSDOSSESSION_ACCENT_RGB; }
static unsigned accent_ink(void) { return dark_theme() ? 0x000000u : MSDOSSESSION_ACCENT_INK_RGB; }

static void install_css(void)
{
    static GtkCssProvider *css;
    char buf[640];
    g_snprintf(buf, sizeof buf,
        ".msdos-dbg-accent { background: #%06x; color: #%06x; }\n"
        ".msdos-dbg-accent:hover { background: #%06x; }\n"
        ".msdos-dbg-accent-text { background: #%06x; color: #%06x; font-weight: bold; padding: 0 4px; border-radius: 3px; }\n"
        ".msdos-dbg-toggle:checked { background: #%06x; color: #%06x; }\n",
        accent_fill(), accent_ink(), accent_fill(), accent_fill(), accent_ink(), accent_fill(), accent_ink());
    if (!css) {
        css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
            GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(css, buf);
#else
    gtk_css_provider_load_from_data(css, buf, -1);
#endif
}

/* The enabled-breakpoint dot: a red that reads on both themes. */
static const char *dot_colour(void) { return dark_theme() ? "#ff7b72" : "#e01b24"; }

static void accent_tag_style(GtkTextTag *tag)
{
    char fill[16], ink[16];
    g_snprintf(fill, sizeof fill, "#%06x", accent_fill());
    g_snprintf(ink, sizeof ink, "#%06x", accent_ink());
    g_object_set(tag, "paragraph-background", fill, "foreground", ink, NULL);
}

static GtkTextTag *accent_tag(GtkTextView *view, const char *name)
{
    GtkTextTag *tag = gtk_text_buffer_create_tag(gtk_text_view_get_buffer(view), name, NULL);
    accent_tag_style(tag);
    return tag;
}

/* The theme changed: the CSS and the tags that wear the accent follow. */
static void on_dark_changed(GObject *obj, GParamSpec *spec, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)obj; (void)spec;
    install_css();
    if (self->disasm) {
        GtkTextTag *dot = gtk_text_tag_table_lookup(
            gtk_text_buffer_get_tag_table(gtk_text_view_get_buffer(self->disasm)), "bp_on");
        if (dot) g_object_set(dot, "foreground", dot_colour(), NULL);
    }
    if (self->tag_pc) accent_tag_style(self->tag_pc);
    if (self->tag_mem_mark) {
        char fill[16], ink[16];
        g_snprintf(fill, sizeof fill, "#%06x", accent_fill());
        g_snprintf(ink, sizeof ink, "#%06x", accent_ink());
        g_object_set(self->tag_mem_mark, "background", fill, "foreground", ink, NULL);
    }
}

/* ---- helpers -------------------------------------------------------------- */

/* Engine text for a text buffer: valid UTF-8, control bytes dropped. */
static char *clean_text(const char *text)
{
    char *valid = g_utf8_make_valid(text, -1), *src, *dst;
    for (src = dst = valid; *src; src++)
        if ((unsigned char)*src >= 0x20 || *src == '\n' || *src == '\t') *dst++ = *src;
    *dst = '\0';
    return valid;
}

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

/* A log (the console, the FujiNet packets, the trace) follows its end while
 * its reader is there. New text grows the view only once it is laid out (or
 * its tab first shows), so the end is pinned whenever the view's height
 * changes; whether to follow is decided as text arrives. */
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
    /* _object: the handler goes when the view does */
    g_signal_connect_object(vadj(view), "changed", G_CALLBACK(on_log_resized), view, 0);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 ||
        gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
            gtk_adjustment_get_upper(adj) - 2.0));
}

/* Replaces a view's text where the reader left it: a refresh (a step, or
 * the live values twice a second) must not throw a scrolled dump back to
 * its top. The top line is noted and scrolled back to once the new text is
 * laid out (scroll_to_mark waits for that). A log that is following goes
 * to its end instead. */
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *tbuf = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    char *clean = clean_text(text);
    GtkTextIter iter;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &iter, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&iter);
    }
    gtk_text_buffer_set_text(tbuf, clean, -1);
    g_free(clean);
    if (follow) {
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
        if (adj) on_log_resized(adj, view);
        return;
    }
    if (top > 0) {
        GtkTextMark *mark;
        gtk_text_buffer_get_iter_at_line(tbuf, &iter, top);
        mark = gtk_text_buffer_create_mark(tbuf, NULL, &iter, TRUE);
        gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark(tbuf, mark);
    }
}

static void set_text(GtkTextView *view, const char *text)
{
    set_text_at(view, text, FALSE);
}

static void append_text(GtkTextView *view, const char *text, gboolean force_follow)
{
    GtkTextBuffer *tbuf = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    char *clean = clean_text(text);
    log_note_reader(view);
    if (force_follow)
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    gtk_text_buffer_get_end_iter(tbuf, &end);
    gtk_text_buffer_insert(tbuf, &end, clean, -1);
    g_free(clean);
}

/* A number as typed: hex by default, $ or 0x or a trailing h for hex, #
 * for decimal. */
static int parse_num(const char *text, long *out)
{
    char buf[64], *end;
    size_t count;
    long val;
    while (*text == ' ') text++;
    g_strlcpy(buf, text, sizeof buf);
    count = strlen(buf);
    while (count && buf[count - 1] == ' ') buf[--count] = '\0';
    if (!count) return 0;
    if (buf[0] == '$') val = strtol(buf + 1, &end, 16);
    else if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) val = strtol(buf + 2, &end, 16);
    else if (buf[0] == '#') val = strtol(buf + 1, &end, 10);
    else if (buf[count - 1] == 'h' || buf[count - 1] == 'H') { buf[count - 1] = '\0'; val = strtol(buf, &end, 16); }
    else val = strtol(buf, &end, 16);
    if (end == buf || *end) return 0;
    *out = val;
    return 1;
}

/* A value typed in a register entry or a poke: a number, else an expression
 * ("ax+1", "[ss:sp]", a label). */
static int parse_value(DbgWin *self, const char *text, int64_t *out)
{
    char err[160];
    long num;
    if (parse_num(text, &num)) {
        *out = num;
        return 1;
    }
    return msdosdebug_eval(self->dbg, text, out, err, sizeof err) == 0;
}

/* seg:off, a register pair, a linear address or a symbol. */
static int parse_addr(DbgWin *self, const char *text, uint16_t *seg, uint32_t *off, uint32_t *lin)
{
    while (*text == ' ') text++;
    if (!*text) return 0;
    return msdosdebug_parse_address(self->dbg, text, seg, off, lin) == 0;
}

/* One address (a label, seg:off, a number or an expression) or a range (lo-hi, lo l len). */
static int parse_range(DbgWin *self, const char *text, uint32_t *range_lo, uint32_t *range_hi)
{
    while (*text == ' ') text++;
    if (!*text) return 0;
    return msdossym_parse_range(self->dbg, text, range_lo, range_hi) != 0;
}

/* A refresh does not overwrite a value being typed: a focused entry keeps
 * what is in it, until Enter (force_entries) says to take the engine's. */
static void set_entry(DbgWin *self, GtkEntry *ent, const char *text)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(ent));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    if (focus && !self->force_entries &&
        (focus == GTK_WIDGET(ent) || gtk_widget_is_ancestor(focus, GTK_WIDGET(ent))))
        return;
    gtk_editable_set_text(GTK_EDITABLE(ent), text);
}

static GtkWidget *mono_view(GtkTextView **out, gboolean wrap)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *lbl = gtk_label_new(text);
    gtk_widget_add_css_class(lbl, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

/* A heading over a block. */
static GtkWidget *headed(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *lbl = gtk_label_new(text);
    gtk_widget_add_css_class(lbl, "heading");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *padded(GtkWidget *self)
{
    gtk_widget_set_margin_top(self, 8);
    gtk_widget_set_margin_bottom(self, 8);
    gtk_widget_set_margin_start(self, 8);
    gtk_widget_set_margin_end(self, 8);
    return self;
}

static void toast(DbgWin *self, const char *msg)
{
    adw_toast_overlay_add_toast(self->toasts, adw_toast_new(msg));
}

/* Messages from Load Symbols and Save land in the console's transcript and,
 * since the console may not be the tab showing, in a toast. */
static void notice(DbgWin *self, const char *msg)
{
    char line[700];
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(self->console_out, line, FALSE);
    toast(self, msg);
}

/* The step controls wait a moment for the engine to come to rest, so a
 * step does not flash "Running" before the status catches up. */
static void settle(DbgWin *self)
{
    if (msdossession_is_running(self->session))
        msdosdebug_wait_stopped(self->dbg, 120);
}

/* Tabs can be moved into windows of their own, so a page's place in the
 * notebook is not its identity: each carries its original index. */
static int page_tab(GtkWidget *page)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "tab-index")) - 1;
}

static int current_tab(DbgWin *self)
{
    GtkWidget *page = gtk_notebook_get_nth_page(self->notebook, gtk_notebook_get_current_page(self->notebook));
    return page ? page_tab(page) : -1;
}

/* On screen: the notebook's page, or a window of its own. */
static gboolean tab_visible(DbgWin *self, int tab)
{
    return current_tab(self) == tab || self->detached[tab] != NULL;
}

static void show_tab(DbgWin *self, int tab)
{
    if (self->detached[tab])
        gtk_window_present(self->detached[tab]);
    else
        gtk_notebook_set_current_page(self->notebook, gtk_notebook_page_num(self->notebook, self->pages[tab]));
}

/* ---- the register strip --------------------------------------------------- */

static gboolean pc_stopped(DbgWin *self)
{
    return msdossession_is_running(self->session) && msdosdebug_stopped(self->dbg);
}

static void refresh_regs(DbgWin *self)
{
    static const char *const modes[] = { "real mode", "protected mode", "virtual 8086 mode" };
    msdos_cpu_regs regs;
    char buf[240], video[160] = "";
    int idx;
    const gboolean present = msdossession_is_running(self->session) != 0;
    const gboolean stopped = present && msdosdebug_stopped(self->dbg);

    for (idx = 0; idx < NREGS; idx++) gtk_widget_set_sensitive(GTK_WIDGET(self->reg[idx]), stopped);
    for (idx = 0; idx < NSEGS; idx++) gtk_widget_set_sensitive(GTK_WIDGET(self->sreg[idx]), stopped);
    for (idx = 0; idx < NFLAGS; idx++) gtk_widget_set_sensitive(GTK_WIDGET(self->flag[idx]), stopped);
    if (!stopped || msdosdebug_regs(self->dbg, &regs) != 0) {
        for (idx = 0; idx < NREGS; idx++) set_entry(self, self->reg[idx], "");
        for (idx = 0; idx < NSEGS; idx++) {
            set_entry(self, self->sreg[idx], "");
            gtk_label_set_text(self->sbase[idx], "");
            if (idx >= 4) gtk_widget_set_visible(self->sreg_cell[idx], self->is386);   /* FS, GS */
        }
        self->updating_flags = TRUE;
        for (idx = 0; idx < NFLAGS; idx++) gtk_check_button_set_active(self->flag[idx], FALSE);
        self->updating_flags = FALSE;
        gtk_label_set_text(self->state, !present ? "The PC is not running"
                                            : "running: stop the PC to read or edit the registers");
        return;
    }
    self->is386 = regs.is386 != 0;
    {
        const uint32_t vals[NREGS] = { regs.eax, regs.ebx, regs.ecx, regs.edx, regs.esi, regs.edi,
                                       regs.ebp, regs.esp, regs.eip };
        const uint16_t svals[NSEGS] = { regs.cs, regs.ds, regs.es, regs.ss, regs.fs, regs.gs };
        const uint32_t bases[NSEGS] = { regs.cs_base, regs.ds_base, regs.es_base, regs.ss_base,
                                        regs.fs_base, regs.gs_base };
        for (idx = 0; idx < NREGS; idx++) {
            char name[8];
            g_strlcpy(name, self->is386 ? k_regs32[idx] : k_regs16[idx], sizeof name);
            for (char *letter = name; *letter; letter++) *letter = g_ascii_toupper(*letter);
            gtk_label_set_text(self->reg_label[idx], name);
            gtk_editable_set_width_chars(GTK_EDITABLE(self->reg[idx]), self->is386 ? 9 : 5);
            gtk_editable_set_max_width_chars(GTK_EDITABLE(self->reg[idx]), self->is386 ? 9 : 5);
            g_snprintf(buf, sizeof buf, self->is386 ? "%08X" : "%04X",
                       self->is386 ? vals[idx] : vals[idx] & 0xFFFFu);
            set_entry(self, self->reg[idx], buf);
        }
        for (idx = 0; idx < NSEGS; idx++) {
            const gboolean shown = idx < 4 || self->is386;
            gtk_widget_set_visible(self->sreg_cell[idx], shown);
            gtk_widget_set_visible(GTK_WIDGET(self->sbase[idx]), shown);
            g_snprintf(buf, sizeof buf, "%04X", svals[idx]);
            set_entry(self, self->sreg[idx], buf);
            g_snprintf(buf, sizeof buf, "base %05X", bases[idx]);
            gtk_label_set_text(self->sbase[idx], buf);
        }
    }
    self->updating_flags = TRUE;
    for (idx = 0; idx < NFLAGS; idx++)
        gtk_check_button_set_active(self->flag[idx], (regs.eflags >> k_flags[idx].bit) & 1);
    self->updating_flags = FALSE;
    msdosdebug_video_info(self->dbg, video, sizeof video);
    g_strchomp(video);
    if (regs.is386)
        g_snprintf(buf, sizeof buf, "%s   %s   CR0 %08X   %s", regs.cpu,
                   modes[CLAMP(regs.mode, 0, 2)], regs.cr0, video);
    else
        g_snprintf(buf, sizeof buf, "%s   %s", regs.cpu, video);
    {
        char *clean = clean_text(buf);
        gtk_label_set_text(self->state, clean);
        g_free(clean);
    }
}

/* Enter in a register entry sets it while the PC is stopped. */
static void on_reg_activate(GtkEntry *entry, gpointer user_data)
{
    DbgWin *self = user_data;
    const int gpr = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "gpr"));
    const int seg = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "seg"));
    const char *name = gpr ? (self->is386 ? k_regs32[gpr - 1] : k_regs16[gpr - 1]) : k_sregs[seg - 1];
    char err[200] = "";
    int64_t val;
    if (!pc_stopped(self)) {
        toast(self, "Stop the PC to edit the registers");
    } else if (!parse_value(self, gtk_editable_get_text(GTK_EDITABLE(entry)), &val)) {
        toast(self, "A register takes a number (hex by default, # for decimal) or an expression");
    } else {
        if (!gpr || !self->is386) val &= 0xFFFF;
        else val &= 0xFFFFFFFFll;
        if (!msdosdebug_register_set(self->dbg, name, val, err, sizeof err))
            toast(self, err[0] ? err : "That register cannot be set here");
    }
    /* Enter ends the edit: show the engine's value, or put the old one back */
    self->force_entries = TRUE;
    refresh_now(self);
    self->force_entries = FALSE;
}

static void on_flag_toggled(GtkCheckButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    const char *name = g_object_get_data(G_OBJECT(btn), "flag");
    char err[200] = "";
    if (self->updating_flags) return;
    if (!pc_stopped(self)) {
        toast(self, "Stop the PC to edit the registers");
        refresh_regs(self);          /* put the box back */
        return;
    }
    if (!msdosdebug_register_set(self->dbg, name, gtk_check_button_get_active(btn), err, sizeof err))
        toast(self, err[0] ? err : "That flag cannot be set here");
    refresh_now(self);
}

/* ---- refreshers ----------------------------------------------------------- */

/* The address column is 5 digits up to the first megabyte and 8 beyond it. */
static int mem_digits(uint32_t base)
{
    return (uint64_t)base + MEM_PAGE > 0x100000ull ? 8 : 5;
}

static void refresh_mem(DbgWin *self)
{
    static uint8_t data[MEM_PAGE];
    GString *str;
    GtkTextBuffer *tbuf = gtk_text_view_get_buffer(self->mem_view);
    const int digits = mem_digits(self->mem_base);
    int row, col;
    char last[64] = "";

    if (!msdossession_is_running(self->session)) {
        set_text(self->mem_view, "The PC is not running.\n");
        return;
    }
    str = g_string_sized_new(MEM_ROWS * 90);
    memset(data, 0, sizeof data);
    msdosdebug_read(self->dbg, self->mem_base, data, MEM_PAGE);
    /* the map does not change while the PC is stopped: one fetch per stop */
    if (!self->map_valid || (self->map_serial != self->seen_serial && msdosdebug_stopped(self->dbg))) {
        self->map_count = msdosdebug_memory_map(self->dbg, self->map, (int)G_N_ELEMENTS(self->map));
        self->map_serial = self->seen_serial;
        self->map_valid = TRUE;
    }
    g_string_append_printf(str, "%*s", digits + 2, "");
    for (col = 0; col < 16; col++)
        g_string_append_printf(str, col == 8 ? " %X  " : "%X  ", col);
    g_string_append_c(str, '\n');
    for (row = 0; row < MEM_ROWS; row++) {
        char region[64];
        const uint32_t addr = self->mem_base + (uint32_t)row * 16;
        g_string_append_printf(str, "%0*X  ", digits, addr);
        for (col = 0; col < 16; col++)
            g_string_append_printf(str, col == 8 ? " %02X " : "%02X ", data[row * 16 + col]);
        g_string_append_c(str, ' ');
        for (col = 0; col < 16; col++) {
            const uint8_t chr = data[row * 16 + col];
            g_string_append_c(str, (chr >= 0x20 && chr <= 0x7E) ? (char)chr : '.');
        }
        /* the region is named on the first row of a run, not on every row */
        msdosmem_region_name(self->map, self->map_count, addr, region, sizeof region);
        if (region[0] && strcmp(region, last) != 0)
            g_string_append_printf(str, "  ; %s", region);
        g_strlcpy(last, region, sizeof last);
        g_string_append_c(str, '\n');
    }
    set_text(self->mem_view, str->str);
    g_string_free(str, TRUE);

    /* the range Go to found stays marked across refreshes, byte by byte */
    if (self->mem_hl) {
        uint64_t addr;
        for (addr = self->mem_hl_lo; addr <= self->mem_hl_hi && addr < (uint64_t)self->mem_base + MEM_PAGE; addr++) {
            GtkTextIter iter_start, iter_end;
            int line, off;
            if (addr < self->mem_base) continue;
            line = (int)((addr - self->mem_base) / 16) + 1;
            off = digits + 2 + (int)((addr - self->mem_base) % 16) * 3 + ((addr - self->mem_base) % 16 >= 8 ? 1 : 0);
            gtk_text_buffer_get_iter_at_line_offset(tbuf, &iter_start, line, off);
            gtk_text_buffer_get_iter_at_line_offset(tbuf, &iter_end, line, off + 2);
            gtk_text_buffer_apply_tag_by_name(tbuf, "mark", &iter_start, &iter_end);
        }
    }
}

/* The disassembly lines that fit the view. */
static int visible_rows(DbgWin *self)
{
    GtkTextIter iter;
    int top = 0, height = 0, avail = gtk_widget_get_height(self->disasm_scroll);
    gtk_text_buffer_get_start_iter(gtk_text_view_get_buffer(self->disasm), &iter);
    gtk_text_view_get_line_yrange(self->disasm, &iter, &top, &height);
    if (height <= 0 || avail <= 0) return 32;
    return CLAMP(avail / height, 4, DISASM_WINDOW);
}

/* x86 code cannot be read backwards: the offset rows instructions before
 * off is the nearest start whose instructions land on off. */
static uint32_t rows_back(DbgWin *self, uint16_t seg, uint32_t off, int rows)
{
    static msdos_dis_line lines[DISASM_WINDOW * 4];
    const int count = MIN(rows * 7 + 1, (int)G_N_ELEMENTS(lines));
    int back, idx, got;
    for (back = rows; back <= rows * 7 && (uint32_t)back <= off; back++) {
        got = msdosdebug_disassemble(self->dbg, seg, off - (uint32_t)back, count, lines);
        for (idx = 0; idx < got && lines[idx].off < off; idx++)
            ;
        if (idx < got && lines[idx].off == off && idx >= rows)
            return lines[idx - rows].off;
    }
    return off > (uint32_t)rows * 3 ? off - (uint32_t)rows * 3 : 0;
}

static uint32_t rows_on(DbgWin *self, uint16_t seg, uint32_t off, int rows)
{
    static msdos_dis_line lines[DISASM_WINDOW + 1];
    int got = msdosdebug_disassemble(self->dbg, seg, off, MIN(rows + 1, DISASM_WINDOW + 1), lines);
    return got > rows ? lines[rows].off : off;
}

static void refresh_disasm(DbgWin *self)
{
    static msdos_dis_line lines[DISASM_WINDOW];
    static msdos_breakpoint bps[BP_MAX];
    int gutter[DISASM_WINDOW];      /* 0 none, 1 enabled, 2 disabled */
    GtkTextBuffer *tbuf = gtk_text_view_get_buffer(self->disasm);
    GString *str = g_string_sized_new(DISASM_WINDOW * 100);
    GtkTextIter iter_start, iter_end;
    msdos_cpu_regs regs;
    int count, pc_line = -1, idx, sub, rows = visible_rows(self);
    int bp_count;
    uint32_t pc_linear = 0;
    gboolean have_regs;
    char *clean;

    if (!msdossession_is_running(self->session)) {
        set_text_at(self->disasm, "The PC is not running.\n", FALSE);
        self->line_count = 0;
        return;
    }
    have_regs = msdosdebug_regs(self->dbg, &regs) == 0;
    if (have_regs) pc_linear = msdosdebug_linear(self->dbg, regs.cs, regs.eip);

    /* Following, CS:IP stays put while it shows, else sits a third of the way down. */
    if (have_regs && gtk_check_button_get_active(self->follow_pc)) {
        gboolean shows = FALSE;
        for (idx = 1; idx < self->line_count && idx < rows - 2; idx++)
            if (self->line_lin[idx] == pc_linear) shows = TRUE;
        if (!shows || self->top_seg != regs.cs) {
            self->top_seg = regs.cs;
            self->top_off = rows_back(self, regs.cs, regs.eip, rows / 3);
        }
    }

    bp_count = msdosdebug_bp_list(self->dbg, bps, BP_MAX);
    count = msdosdebug_disassemble(self->dbg, self->top_seg, self->top_off, DISASM_WINDOW, lines);
    self->line_count = count;
    for (idx = 0; idx < count; idx++) {
        const msdos_dis_line *line = &lines[idx];
        const gboolean is_pc = have_regs && line->linear == pc_linear;
        gutter[idx] = 0;
        for (sub = 0; sub < bp_count; sub++)
            if (bps[sub].type == MSDOS_BP_EXEC && bps[sub].start == line->linear && bps[sub].end == line->linear &&
                gutter[idx] != 1)
                gutter[idx] = bps[sub].enabled ? 1 : 2;
        self->line_lin[idx] = line->linear;
        if (is_pc) pc_line = idx;
        g_string_append_printf(str, "%s%c %04X:%0*X  %-20s %s%s%s\n",
                               gutter[idx] ? "\xe2\x97\x8f" : " ", is_pc ? '>' : ' ',
                               line->seg, line->off > 0xFFFF ? 8 : 4, line->off, line->bytes,
                               line->symbol, line->symbol[0] ? ": " : "", line->text);
    }
    if (count == 0)
        g_string_append(str, "(no disassembly)\n");
    clean = clean_text(str->str);
    gtk_text_buffer_set_text(tbuf, clean, -1);
    g_free(clean);
    g_string_free(str, TRUE);

    for (idx = 0; idx < count; idx++) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (gutter[idx]) {
            gtk_text_buffer_get_iter_at_line(tbuf, &iter_start, idx);
            iter_end = iter_start;
            gtk_text_iter_forward_char(&iter_end);
            gtk_text_buffer_apply_tag_by_name(tbuf, gutter[idx] == 2 ? "bp_off" : "bp_on", &iter_start, &iter_end);
        }
        /* the selected line shaded */
        if ((int64_t)lines[idx].linear == self->selected) {
            gtk_text_buffer_get_iter_at_line(tbuf, &iter_start, idx);
            gtk_text_buffer_get_iter_at_line(tbuf, &iter_end, idx + 1);
            gtk_text_buffer_apply_tag_by_name(tbuf, "sel", &iter_start, &iter_end);
        }
    }
    /* the CS:IP line in the accent colour (over the selection) */
    if (pc_line >= 0 && pc_line < count) {
        gtk_text_buffer_get_iter_at_line(tbuf, &iter_start, pc_line);
        gtk_text_buffer_get_iter_at_line(tbuf, &iter_end, pc_line + 1);
        if (gutter[pc_line]) gtk_text_iter_forward_char(&iter_start);   /* the dot keeps its colour */
        gtk_text_buffer_apply_tag_by_name(tbuf, "pc", &iter_start, &iter_end);
    }
    /* the wheel scrolls by instructions: the view itself stays at its top */
    gtk_adjustment_set_value(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->disasm_scroll)), 0.0);
}

/* The packet log is rebuilt only when it changed: a new packet. */
static void refresh_fujinet(DbgWin *self, gboolean force)
{
    static msdos_fuji_packet pkt[TRACE_MAX];
    static char uart[2048];
    GString *txt;
    char note[200];
    int count = msdosdebug_fujinet_trace(self->dbg, pkt, TRACE_MAX), idx;
    const gboolean external = msdossession_fujinet_external(self->session) != 0;
    const gboolean fuji = msdossession_fujinet_running(self->session) != 0 || external;

    if (!fuji)
        g_strlcpy(note, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet).", sizeof note);
    else if (external)
        g_snprintf(note, sizeof note, "External FujiNet at %s on COM%d%s.",
                   msdossession_boip_endpoint(self->session), msdossession_fujinet_com(self->session),
                   msdossession_fujinet_link(self->session) ? ", connected" : ", not connected");
    else
        g_snprintf(note, sizeof note, "FujiNet is on COM%d%s.", msdossession_fujinet_com(self->session),
                   msdossession_fujinet_link(self->session) ? ", connected" : ", not connected");
    gtk_label_set_text(self->fuji_note, note);
    msdosdebug_uart(self->dbg, uart, sizeof uart);
    {   /* unchanged keeps a selection */
        GtkTextBuffer *sbuf = gtk_text_view_get_buffer(self->uart_text);
        GtkTextIter iter_start, iter_end;
        char *cur, *clean;
        gtk_text_buffer_get_bounds(sbuf, &iter_start, &iter_end);
        cur = gtk_text_buffer_get_text(sbuf, &iter_start, &iter_end, FALSE);
        clean = clean_text(uart);
        if (strcmp(cur, clean) != 0) set_text(self->uart_text, uart);
        g_free(cur);
        g_free(clean);
    }

    if (!force && count == self->fuji_count && (count == 0 || pkt[count - 1].serial == self->fuji_last))
        return;
    self->fuji_count = count;
    self->fuji_last = count > 0 ? pkt[count - 1].serial : 0;
    log_note_reader(self->fuji_text);
    txt = g_string_new(NULL);
    for (idx = 0; idx < count; idx++) {
        int sub;
        g_string_append_printf(txt, "%s%s\n     ", idx ? "\n" : "", pkt[idx].text);
        for (sub = 0; sub < pkt[idx].nbytes && sub < (int)sizeof pkt[idx].data; sub++)
            g_string_append_printf(txt, "%02X ", pkt[idx].data[sub]);
        if (pkt[idx].nbytes > (int)sizeof pkt[idx].data) g_string_append(txt, "\xe2\x80\xa6");
    }
    if (count == 0)
        g_string_append(txt, "(no FujiBus packets since the trace was cleared)");
    set_text_at(self->fuji_text, txt->str, force || following(self->fuji_text));
    g_string_free(txt, TRUE);
}

static void refresh_ints(DbgWin *self, gboolean force)
{
    static msdos_int_event int_events[TRACE_MAX];
    GString *txt;
    int count = msdosdebug_int_trace(self->dbg, int_events, TRACE_MAX), idx;

    self->updating_int = TRUE;
    gtk_switch_set_active(self->int_on, msdosdebug_int_trace_enabled(self->dbg) != 0);
    self->updating_int = FALSE;

    /* the vector table: changes rarely, cheap to read */
    txt = g_string_new(NULL);
    for (idx = 0; idx < 256; idx++) {
        uint16_t seg = 0, off = 0;
        msdosdebug_ivt(self->dbg, idx, &seg, &off);
        g_string_append_printf(txt, "%02X %04X:%04X%s", idx, seg, off, (idx & 3) == 3 ? "\n" : "   ");
    }
    set_text(self->ivt_text, txt->str);
    g_string_free(txt, TRUE);

    /* a log: set again only when it moved */
    if (!force && count == self->int_count && (count == 0 || int_events[count - 1].serial == self->int_last))
        return;
    self->int_count = count;
    self->int_last = count > 0 ? int_events[count - 1].serial : 0;
    log_note_reader(self->int_text);
    txt = g_string_new("From       AX   BX   CX   DX   DS   ES    Call");
    for (idx = 0; idx < count; idx++)
        g_string_append_printf(txt, "\n%04X:%04X  %04X %04X %04X %04X %04X %04X  %s", int_events[idx].cs,
                               int_events[idx].ip & 0xFFFF, int_events[idx].ax, int_events[idx].bx, int_events[idx].cx, int_events[idx].dx,
                               int_events[idx].ds, int_events[idx].es, int_events[idx].text);
    if (count == 0)
        g_string_append(txt, msdosdebug_int_trace_enabled(self->dbg)
                                 ? "\n(no INT calls since the trace was cleared)"
                                 : "\n(turn the trace on to record INT calls; it costs speed)");
    set_text_at(self->int_text, txt->str, force || following(self->int_text));
    g_string_free(txt, TRUE);
}

/* ---- Breakpoints and Watches ---------------------------------------------- */

static void bp_enable_toggled(GtkCheckButton *btn, gpointer user_data);
static void bp_remove_clicked(GtkButton *btn, gpointer user_data);
static void on_bp_cond_activate(GtkEntry *entry, gpointer user_data);

static void refresh_bps(DbgWin *self)
{
    static msdos_breakpoint bps[BP_MAX];
    msdos_stop_snapshot snap;
    GtkWidget *child;
    GString *sig = g_string_new(NULL);
    int count = msdosdebug_bp_list(self->dbg, bps, BP_MAX), idx;
    char text[240];

    /* why the machine stopped, in the core's words */
    msdosdebug_stop_snapshot(self->dbg, &snap);
    if (snap.paused) {
        g_snprintf(text, sizeof text, "%s", snap.text[0] ? snap.text : "paused");
        text[0] = (char)g_ascii_toupper(text[0]);
    } else {
        g_strlcpy(text, msdossession_is_running(self->session) ? "Running." : "The PC is not running.",
                  sizeof text);
    }
    {
        char *clean = clean_text(text);
        gtk_label_set_text(self->bp_stop, clean);
        g_free(clean);
    }

    /* the list is rebuilt only when what it shows changed (hit counts move
     * while the machine runs), so a click on a row is not pulled out from
     * under the pointer */
    for (idx = 0; idx < count; idx++)
        g_string_append_printf(sig, "%d/%d/%u-%u/%d/%u/%s/%d\n", bps[idx].id, bps[idx].type,
                               bps[idx].start, bps[idx].end, bps[idx].enabled, bps[idx].hits,
                               bps[idx].condition, snap.paused && snap.stop.bp_id == bps[idx].id);
    if (self->bp_sig && g_string_equal(sig, self->bp_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (self->bp_sig) g_string_free(self->bp_sig, TRUE);
    self->bp_sig = sig;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->bp_list))) != NULL)
        gtk_list_box_remove(self->bp_list, child);
    if (count == 0) {
        GtkWidget *lbl = gtk_label_new("No breakpoints. Select a disassembly line and press F9, "
                                       "or add one above.");
        gtk_widget_add_css_class(lbl, "dim-label");
        gtk_widget_set_margin_top(lbl, 12);
        gtk_widget_set_margin_bottom(lbl, 12);
        gtk_list_box_append(self->bp_list, lbl);
        return;
    }
    for (idx = 0; idx < count; idx++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *enable = gtk_check_button_new();
        GtkWidget *remove_btn = gtk_button_new_with_label("Remove");
        GtkWidget *cond = gtk_entry_new();
        GtkWidget *lbl;
        char desc[300], line[300];
        msdosbp_describe(self->dbg, &bps[idx], desc, sizeof desc);
        g_snprintf(line, sizeof line, "#%-3d %-34s hits %u", bps[idx].id, bps[idx].text, bps[idx].hits);
        {
            char *clean = clean_text(line);
            lbl = gtk_label_new(clean);
            g_free(clean);
            clean = clean_text(desc);
            gtk_widget_set_tooltip_text(lbl, clean);
            g_free(clean);
        }
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_widget_add_css_class(lbl, "monospace");
        if (snap.paused && snap.stop.bp_id == bps[idx].id)   /* the one that stopped the machine */
            gtk_widget_add_css_class(lbl, "msdos-dbg-accent-text");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(enable), bps[idx].enabled != 0);
        gtk_widget_set_tooltip_text(enable, "Enabled");
        gtk_editable_set_text(GTK_EDITABLE(cond), bps[idx].condition);
        gtk_editable_set_width_chars(GTK_EDITABLE(cond), 26);
        gtk_entry_set_placeholder_text(GTK_ENTRY(cond), "condition");
        gtk_widget_set_tooltip_text(cond, "The condition (an expression); Enter applies it, empty removes it");
        g_object_set_data(G_OBJECT(enable), "bp", GINT_TO_POINTER(bps[idx].id));
        g_object_set_data(G_OBJECT(remove_btn), "bp", GINT_TO_POINTER(bps[idx].id));
        g_object_set_data(G_OBJECT(cond), "bp", GINT_TO_POINTER(bps[idx].id));
        g_signal_connect(enable, "toggled", G_CALLBACK(bp_enable_toggled), self);
        g_signal_connect(remove_btn, "clicked", G_CALLBACK(bp_remove_clicked), self);
        g_signal_connect(cond, "activate", G_CALLBACK(on_bp_cond_activate), self);
        gtk_widget_add_css_class(remove_btn, "flat");
        gtk_widget_set_tooltip_text(remove_btn, "Remove this breakpoint");
        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_end(row, 6);
        gtk_box_append(GTK_BOX(row), enable);
        gtk_box_append(GTK_BOX(row), lbl);
        gtk_box_append(GTK_BOX(row), cond);
        gtk_box_append(GTK_BOX(row), remove_btn);
        gtk_list_box_append(self->bp_list, row);
    }
}

static void on_wt_remove(GtkButton *btn, gpointer user_data);
static void on_wt_memory(GtkButton *btn, gpointer user_data);
static void on_wt_break(GtkButton *btn, gpointer user_data);

/* Rebuilt only when a line changed: a live value moving must not pull a
 * button out from under the pointer. */
static void refresh_watches(DbgWin *self)
{
    const int count = msdoswatch_count();
    GString *sig = g_string_new(NULL);
    char lines[MSDOSWATCH_MAX][200];
    int idx;

    for (idx = 0; idx < count; idx++) {
        msdoswatch_format(self->dbg, idx, lines[idx], sizeof lines[idx]);
        g_string_append_printf(sig, "%s\n", lines[idx]);
    }
    if (self->wt_sig && g_string_equal(sig, self->wt_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (self->wt_sig) g_string_free(self->wt_sig, TRUE);
    self->wt_sig = sig;

    gtk_list_box_remove_all(self->wt_list);
    if (count == 0) {
        GtkWidget *lbl = gtk_label_new("No watches. Type an address, label, expression or lo-hi above.");
        gtk_widget_add_css_class(lbl, "dim-label");
        gtk_widget_set_margin_top(lbl, 12);
        gtk_widget_set_margin_bottom(lbl, 12);
        gtk_list_box_append(self->wt_list, lbl);
        return;
    }
    for (idx = 0; idx < count; idx++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label;
        static const struct { const char *text, *tip; GCallback handler; } btns[] = {
            { "Memory", "Show it in the Memory tab", G_CALLBACK(on_wt_memory) },
            { "Break on Change", "Break when it changes", G_CALLBACK(on_wt_break) },
            { "Remove", "Forget this watch", G_CALLBACK(on_wt_remove) },
        };
        size_t sub;
        char *clean = clean_text(lines[idx]);
        label = gtk_label_new(clean);
        g_free(clean);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "monospace");
        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_end(row, 6);
        gtk_box_append(GTK_BOX(row), label);
        for (sub = 0; sub < G_N_ELEMENTS(btns); sub++) {
            GtkWidget *btn = gtk_button_new_with_label(btns[sub].text);
            gtk_widget_add_css_class(btn, "flat");
            gtk_widget_set_tooltip_text(btn, btns[sub].tip);
            g_object_set_data(G_OBJECT(btn), "wt-idx", GINT_TO_POINTER(idx));
            g_signal_connect(btn, "clicked", btns[sub].handler, self);
            gtk_box_append(GTK_BOX(row), btn);
        }
        gtk_list_box_append(self->wt_list, row);
    }
}

/* ---- Machine & I/O ---------------------------------------------------------- */

static void refresh_machine(DbgWin *self)
{
    static char text[16384];
    static msdos_region map[16];
    GString *str = g_string_new(NULL);
    int count, idx, digits = 5;

    if (!msdossession_is_running(self->session)) {
        set_text_at(self->machine_text, "The PC is not running.\n", FALSE);
        set_text_at(self->map_text, "", FALSE);
        g_string_free(str, TRUE);
        return;
    }
    text[0] = '\0';
    msdosdebug_io_text(self->dbg, text, (int)sizeof text);
    set_text_at(self->machine_text, text[0] ? text : "No machine.\n", FALSE);

    count = msdosdebug_memory_map(self->dbg, map, (int)G_N_ELEMENTS(map));
    for (idx = 0; idx < count; idx++)
        if (map[idx].high > 0xFFFFF) digits = 8;
    if (!count) g_string_append(str, "Stop the PC to read its memory map.\n");
    for (idx = 0; idx < count; idx++)
        g_string_append_printf(str, "%0*X-%0*X  %c%c  %s\n", digits, map[idx].low, digits, map[idx].high,
                               map[idx].readable ? 'r' : '-', map[idx].writable ? 'w' : '-', map[idx].name);
    set_text_at(self->map_text, str->str, FALSE);
    g_string_free(str, TRUE);
}

/* ---- Sound ---------------------------------------------------------------- */

static void refresh_sound(DbgWin *self)
{
    static msdos_snd_state snd;
    GString *txt;
    char note[16];
    int chan, sub;

    if (!msdossession_is_running(self->session) || msdosdebug_sound_get(self->dbg, &snd) != 0) {
        set_text_at(self->sound_text, "The PC is not running.\n", FALSE);
        return;
    }
    txt = g_string_new(NULL);
    g_string_append_printf(txt, "PC speaker (%s)\n", snd.speaker_direct ? "read from the PIT and port 61h" :
                                                       "from the writes seen since the debugger attached");
    if (snd.speaker_on)
        g_string_append_printf(txt, "  sounding at %.1f Hz (%s)\n", snd.speaker_hz,
                               msdos_snd_note_name(msdos_snd_hz_to_note(snd.speaker_hz), note, sizeof note));
    else
        g_string_append(txt, snd.speaker_dac ? "  silent on the timer; the speaker data bit is being toggled by hand\n"
                                             : "  silent\n");
    g_string_append_printf(txt, "  port 61h = %02X (timer 2 gate %d, speaker data %d)   PIT counter 2: mode %d, reload %u\n",
                           snd.port61, snd.port61 & 1, (snd.port61 >> 1) & 1, snd.pit_mode, snd.pit_reload);
    g_string_append_printf(txt, "  writes seen: port 61h %u, PIT counter 2 %u\n\n", snd.port61_writes, snd.pit2_writes);

    g_string_append_printf(txt, "FM chip (AdLib / Sound Blaster OPL2 or OPL3, ports 388h-38Bh)\n");
    if (!snd.opl_seen) {
        g_string_append(txt, "  no writes seen since the debugger attached or the PC last reset\n\n");
    } else {
        g_string_append_printf(txt, "  %s, %u register writes; rhythm %s (BDh = %02X), tremolo depth %d, vibrato depth %d\n",
                               snd.opl3 ? "OPL3 mode, 18 channels" : "OPL2 mode, 9 channels", snd.opl_writes,
                               (snd.rhythm & 0x20) ? "on" : "off", snd.rhythm, (snd.tremolo_depth & 1), (snd.vibrato_depth & 1));
        {
            /* the headings share the rows' column widths */
            static const char *const op_head = "mul TL A D S R W";
            char row_head[160];
            const int prefix = g_snprintf(row_head, sizeof row_head, "  %2d  %-3s %8.1f  %-4s  %d  %4d  %d  %-4s %c%c%*s",
                                          1, "on", 0.0, "--", 0, 0, 0, "FM", 'L', 'R', 7, "");
            g_string_append_printf(txt, "  Ch  Key       Hz  Note  Blk FNum  FB Conn Pan%*s%-26s%s\n",
                                   prefix - 47, "", "Modulator", "Carrier");
            g_string_append_printf(txt, "%*s%-26s%s\n", prefix, "", op_head, op_head);
        }
        for (chan = 0; chan < snd.opl_channels && chan < MSDOS_SND_OPL_CHANNELS; chan++) {
            const msdos_snd_channel *chan_info = &snd.channel[chan];
            char mod_text[40], car_text[40];
            g_snprintf(mod_text, sizeof mod_text, "%-4X%2d %X %X %X %X %d", chan_info->mod.multiple,
                       chan_info->mod.total_level, chan_info->mod.attack, chan_info->mod.decay,
                       chan_info->mod.sustain_level, chan_info->mod.release, chan_info->mod.wave);
            g_snprintf(car_text, sizeof car_text, "%-4X%2d %X %X %X %X %d", chan_info->car.multiple,
                       chan_info->car.total_level, chan_info->car.attack, chan_info->car.decay,
                       chan_info->car.sustain_level, chan_info->car.release, chan_info->car.wave);
            g_string_append_printf(txt, "  %2d  %-3s %8.1f  %-4s  %d  %4d  %d  %-4s %c%c%s%*s%-26s%s\n",
                chan + 1, chan_info->keyon ? "on" : "off", chan_info->keyon ? chan_info->hz : 0.0,
                chan_info->keyon ? msdos_snd_note_name(chan_info->note, note, sizeof note) : "--", chan_info->block,
                chan_info->fnum, chan_info->feedback, chan_info->additive ? "add" : "FM",
                (chan_info->pan & 1) ? 'L' : '-', (chan_info->pan & 2) ? 'R' : '-', chan_info->four_op ? " 4op" : "",
                chan_info->four_op ? 3 : 7, "", mod_text, car_text);
        }
        g_string_append(txt, "  registers (00-FF):");
        for (sub = 0; sub < 256; sub++)
            g_string_append_printf(txt, "%s%02X", sub % 16 ? " " : "\n    ", snd.opl_reg[sub]);
        g_string_append_c(txt, '\n');
        if (snd.opl3) {
            g_string_append(txt, "  registers (100-1FF):");
            for (sub = 0; sub < 256; sub++)
                g_string_append_printf(txt, "%s%02X", sub % 16 ? " " : "\n    ", snd.opl_reg[256 + sub]);
            g_string_append_c(txt, '\n');
        }
        g_string_append_c(txt, '\n');
    }

    g_string_append(txt, "Sound Blaster DSP and mixer (only the default base 220h is watched)\n");
    if (!snd.sb_seen) {
        g_string_append(txt, "  no DSP or mixer writes seen\n\n");
    } else {
        char rate[32] = "unknown", constant[32] = "unknown";
        if (snd.sb_sample_rate) g_snprintf(rate, sizeof rate, "%d Hz", snd.sb_sample_rate);
        if (snd.sb_time_constant >= 0) g_snprintf(constant, sizeof constant, "%d", snd.sb_time_constant);
        g_string_append_printf(txt, "  last DSP command %02X, %u commands; sample rate %s; time constant %s; speaker %s\n",
                               snd.sb_last_command, snd.sb_commands, rate, constant,
                               snd.sb_speaker_on < 0 ? "unknown" : snd.sb_speaker_on ? "on" : "off");
        g_string_append_printf(txt, "  mixer index %02X; master %02X, voice %02X, FM %02X, CD %02X\n\n",
                               snd.sb_mixer_index, snd.sb_mixer[0x22], snd.sb_mixer[0x04], snd.sb_mixer[0x26],
                               snd.sb_mixer[0x28]);
    }

    g_string_append(txt, "Sound cards in this PC\n");
    if (snd.card_count == 0) g_string_append(txt, "  none configured (the PC speaker only)\n");
    for (sub = 0; sub < snd.card_count; sub++)
        g_string_append_printf(txt, "  %-40s %s\n", snd.card[sub].name,
                               snd.card[sub].decoded ? "decoded above" : "listed only: its registers are not decoded");
    set_text_at(self->sound_text, txt->str, FALSE);
    g_string_free(txt, TRUE);
}

/* ---- Trace and History ------------------------------------------------------ */

static void hist_refresh(DbgWin *self, int idx, gboolean force)
{
    static msdos_trace_entry buf[HIST_COPY];
    const gboolean live = idx == 0;
    const gboolean paused = pc_stopped(self);
    const gboolean active = msdosdebug_history_enabled(self->dbg) != 0 || msdosdebug_trace_enabled(self->dbg) != 0;
    const uint64_t count = msdosdebug_trace_total(self->dbg);
    uint32_t range_lo = 0, range_hi = 0xFFFFFFFFu;
    int filter = MSDOS_TRACE_ALL, total, pos, shown = 0;
    g_autofree char *note = NULL;
    GString *txt;
    int hit[HIST_SHOWN];

    self->h_sync = TRUE;   /* the two Record buttons follow the engine */
    gtk_toggle_button_set_active(self->h_rec[0], active);
    gtk_toggle_button_set_active(self->h_rec[1], active);
    self->h_sync = FALSE;

    if (!force && count == self->h_seen[idx] && (live || paused))
        return;
    self->h_seen[idx] = count;
    if (!live && !paused) {
        gtk_label_set_text(self->h_note[idx], msdossession_is_running(self->session)
                                                 ? "Stop the PC to read its history." : "The PC is not running.");
        return;
    }
    switch (gtk_drop_down_get_selected(self->h_filter[idx])) {
    case 1: filter = MSDOS_TRACE_BRANCHES; break;
    case 2:
        if (parse_range(self, gtk_editable_get_text(GTK_EDITABLE(self->h_range[idx])), &range_lo, &range_hi))
            filter = MSDOS_TRACE_RANGE;
        break;
    default: break;
    }
    total = msdosdebug_trace_read(self->dbg, buf, HIST_COPY);   /* newest first */
    for (pos = 0; pos < total && shown < HIST_SHOWN; pos++)
        if (msdosdebug_trace_match(&buf[pos], filter, range_lo, range_hi))
            hit[shown++] = pos;
    txt = g_string_new(NULL);
    if (!active && total == 0)
        g_string_append_printf(txt, "%s is off. Press Record, then let the PC run (it runs slower while recording).",
                               live ? "Trace" : "History");
    for (pos = shown - 1; pos >= 0; pos--) {   /* oldest first */
        char line[400];
        msdosdebug_trace_format(&buf[hit[pos]], line, sizeof line);
        g_string_append(txt, line);
        g_string_append_c(txt, '\n');
    }
    set_text_at(self->h_text[idx], txt->str, TRUE);
    g_string_free(txt, TRUE);
    note = g_strdup_printf("%s%" G_GUINT64_FORMAT " instructions recorded, last %d shown",
                           active ? "Recording. " : "", count, shown);
    gtk_label_set_text(self->h_note[idx], note);
}

/* ---- Symbols -------------------------------------------------------------- */

/* The list is rebuilt only when the table (its size, or a command since) or the
 * filter changed -- a real map can be a few thousand entries. */
static void refresh_symbols(DbgWin *self, gboolean force)
{
    static int idx[4096];
    const char *filter = gtk_editable_get_text(GTK_EDITABLE(self->sym_filter));
    const int total = msdosdebug_symbols_count(self->dbg);
    const unsigned gen = msdosdebug_symbols_generation(self->dbg);
    int count, pos;
    char text[128];

    if (!force && total == self->sym_built && gen == self->sym_gen && strcmp(filter, self->sym_text) == 0)
        return;
    self->sym_built = total;
    self->sym_gen = gen;
    g_strlcpy(self->sym_text, filter, sizeof self->sym_text);

    gtk_list_box_remove_all(self->symlist);
    count = msdosdebug_symbols_filter(self->dbg, filter, idx, (int)G_N_ELEMENTS(idx));
    for (pos = 0; pos < count; pos++) {
        uint32_t addr;
        char name[96], line[160], *clean;
        GtkWidget *row, *label;
        if (!msdosdebug_symbols_get(self->dbg, idx[pos], &addr, name, sizeof name)) continue;
        g_snprintf(line, sizeof line, "%0*X  %s", addr > 0xFFFFF ? 8 : 5, addr, name);
        clean = clean_text(line);
        row = gtk_list_box_row_new();
        label = gtk_label_new(clean);
        g_free(clean);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_add_css_class(label, "monospace");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data(G_OBJECT(row), "addr", GUINT_TO_POINTER(addr));
        gtk_list_box_append(self->symlist, row);
    }
    g_snprintf(text, sizeof text, "%d of %d symbol%s%s", count, total, total == 1 ? "" : "s",
               filter[0] ? " (filtered)" : "");
    gtk_label_set_text(self->sym_count, text);
}

/* ---- status and the tab showing ------------------------------------------- */

static void refresh_status(DbgWin *self)
{
    msdos_stop_snapshot snap;
    char text[256];
    gboolean stopped;
    const gboolean present = msdossession_is_running(self->session) != 0;
    msdosdebug_stop_snapshot(self->dbg, &snap);
    stopped = present && snap.paused != 0;
    if (!present)
        g_strlcpy(text, "The PC is not running", sizeof text);
    else if (stopped) {
        g_snprintf(text, sizeof text, "%s", snap.text[0] ? snap.text : "paused");
        text[0] = (char)g_ascii_toupper(text[0]);
    } else
        g_snprintf(text, sizeof text, "Running");
    {
        char *clean = clean_text(text);
        adw_window_title_set_subtitle(self->title, clean);
        g_free(clean);
    }
    gtk_button_set_label(self->run_btn, stopped || !present ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(self->run_btn), "msdos-dbg-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(self->run_btn), "msdos-dbg-accent");
}

/* At a new stop the console shows where, as DEBUG's R does. */
static void console_stop(DbgWin *self, const msdos_stop_snapshot *snap)
{
    static char out[4096];
    char line[300];
    if (snap->text[0]) {
        g_snprintf(line, sizeof line, "\xe2\x80\x94 %s\n", snap->text);
        append_text(self->console_out, line, FALSE);
    }
    out[0] = '\0';
    msdosdebug_command(self->dbg, "r", out, sizeof out);
    append_text(self->console_out, out, FALSE);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(self->console_out, "\n", FALSE);
}

/* The panes that wait for a stop say so while the machine runs, rather than
 * showing nothing. */
static void running_hint(DbgWin *self, int tab)
{
    if (tab == TAB_DISASM) {
        set_text_at(self->disasm, msdossession_is_running(self->session)
                                      ? "\n  Stop the PC (F5) to read its disassembly.\n"
                                      : "\n  The PC is not running.\n", FALSE);
        self->line_count = 0;
    } else {
        set_text_at(self->machine_text, msdossession_is_running(self->session)
                                            ? "Stop the PC (F5) to read the machine and its I/O.\n"
                                            : "The PC is not running.\n", FALSE);
        set_text_at(self->map_text, "", FALSE);
    }
}

/* One tab brought up to date; the others catch up when their tab is shown. */
static void refresh_tab(DbgWin *self, int tab)
{
    const gboolean present = msdossession_is_running(self->session) != 0;
    const gboolean stopped = present && msdosdebug_stopped(self->dbg);
    /* a PC that is off has nothing for these tabs */
    if (!present && (tab == TAB_VIDEO || tab == TAB_INTS || tab == TAB_TRACE || tab == TAB_HISTORY))
        return;
    /* a running machine is not asked for its disassembly or its machine state on every
     * tick: those wait for a stop, or for their tab to be opened (on_switch_page), so the
     * UI never blocks on them */
    if (present && !stopped && (tab == TAB_DISASM || tab == TAB_MACHINE)) {
        running_hint(self, tab);
        return;
    }
    switch (tab) {
    case TAB_DISASM: refresh_disasm(self); break;
    case TAB_MEMORY: refresh_mem(self); break;
    case TAB_BREAKS: refresh_bps(self); break;
    case TAB_WATCHES: refresh_watches(self); break;
    case TAB_MACHINE: refresh_machine(self); break;
    case TAB_TRACE: hist_refresh(self, 0, FALSE); break;
    case TAB_HISTORY: hist_refresh(self, 1, FALSE); break;
    case TAB_VIDEO: dbg_video_refresh(self->pages[TAB_VIDEO], FALSE); break;
    case TAB_SOUND: refresh_sound(self); break;
    case TAB_SYMBOLS: refresh_symbols(self, FALSE); break;
    case TAB_INTS: refresh_ints(self, FALSE); break;
    case TAB_FUJINET: refresh_fujinet(self, FALSE); break;
    default: break;
    }
}

/* The status, the register strip, and every tab on screen: the one showing and
 * any in a window of its own. Stops and actions refresh everything; ticks of a
 * running machine leave out the tabs that cost several engine round trips. */
static void refresh_now(DbgWin *self)
{
    msdos_stop_snapshot snap;
    gboolean stopped;
    int tab;
    msdosdebug_stop_snapshot(self->dbg, &snap);
    stopped = msdossession_is_running(self->session) && snap.paused != 0;
    if (stopped && (!self->was_stopped || snap.serial != self->seen_serial))
        console_stop(self, &snap);
    self->seen_generation = msdosdebug_generation(self->dbg);
    self->seen_serial = snap.serial;
    self->was_stopped = stopped;
    refresh_status(self);
    refresh_regs(self);
    for (tab = 0; tab < TAB_COUNT; tab++)
        if (tab_visible(self, tab))
            refresh_tab(self, tab);
}

/* ---- the timer ------------------------------------------------------------ */

/* The engine attaches, which stops the PC. */
static void attach_and_refresh(DbgWin *self)
{
    int tries;
    msdosdebug_attach(self->dbg);
    self->attached_running = msdossession_is_running(self->session) != 0;
    /* the machine stops within a moment of attaching: let the first picture show it */
    for (tries = 0; tries < 20 && self->attached_running && !msdosdebug_stopped(self->dbg); tries++)
        g_usleep(2000);
    self->was_stopped = FALSE;
    self->running_ticks = 0;
    self->map_valid = FALSE;
    self->sym_built = -1;
    if (self->bp_sig) { g_string_free(self->bp_sig, TRUE); self->bp_sig = NULL; }
    if (self->wt_sig) { g_string_free(self->wt_sig, TRUE); self->wt_sig = NULL; }
    refresh_now(self);
}

static gboolean tick_refresh(gpointer user_data)
{
    DbgWin *self = user_data;
    msdos_stop_snapshot snap;
    gboolean stopped;
    if (self->torn_down || !gtk_widget_get_visible(GTK_WIDGET(self->win))) return G_SOURCE_CONTINUE;
    if (!msdossession_is_running(self->session)) {
        if (self->attached_running) {
            self->attached_running = FALSE;
            refresh_now(self);
        }
        return G_SOURCE_CONTINUE;
    }
    /* Opened before the PC started, or showing across a restart: the
     * engine attaches (stopping the machine) as it comes up. */
    if (!self->attached_running) {
        msdosdebug_detach(self->dbg);
        attach_and_refresh(self);
        return G_SOURCE_CONTINUE;
    }
    msdosdebug_stop_snapshot(self->dbg, &snap);
    stopped = snap.paused != 0;
    if (msdosdebug_generation(self->dbg) != self->seen_generation || stopped != self->was_stopped ||
        snap.serial != self->seen_serial) {
        self->running_ticks = 0;
        refresh_now(self);
    } else if (!stopped) {
        /* live values twice a second while the machine runs; the Video tab
         * follows the beam, every tick */
        if (++self->running_ticks >= LIVE_TICKS) {
            self->running_ticks = 0;
            refresh_now(self);
        } else if (tab_visible(self, TAB_VIDEO)) {
            dbg_video_refresh(self->pages[TAB_VIDEO], FALSE);
        }
    }
    return G_SOURCE_CONTINUE;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    if (!msdossession_is_running(self->session)) return;
    if (msdosdebug_stopped(self->dbg)) {
        msdosdebug_resume(self->dbg);
    } else {
        msdosdebug_stop(self->dbg);
        settle(self);
    }
    refresh_now(self);
}

/* A step: the machine is stopped, runs a moment, stops again. */
#define STEP_HANDLER(name, call) \
    static void name(GtkButton *btn, gpointer user_data) \
    { \
        DbgWin *self = user_data; \
        (void)btn; \
        if (!pc_stopped(self)) return; \
        call; \
        settle(self); \
        refresh_now(self); \
    }
STEP_HANDLER(on_step, msdosdebug_step(self->dbg))
STEP_HANDLER(on_step_over, msdosdebug_step_over(self->dbg))
STEP_HANDLER(on_step_out, msdosdebug_step_out(self->dbg))
#undef STEP_HANDLER

static void on_run_to_cursor(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    if (self->selected < 0) {
        toast(self, "Select a disassembly line to run to");
        return;
    }
    if (!pc_stopped(self)) return;
    msdosdebug_run_to(self->dbg, (uint32_t)self->selected);
    settle(self);
    refresh_now(self);
}

/* F9 and the gutter: an execute breakpoint on the selected line. */
static void toggle_breakpoint(DbgWin *self, uint32_t linear)
{
    if (msdosdebug_bp_toggle(self->dbg, linear) < 0)
        toast(self, "No room for another breakpoint");
    refresh_now(self);
}

static void toggle_selected(DbgWin *self)
{
    if (self->selected < 0) {
        toast(self, "Select a disassembly line first");
        return;
    }
    toggle_breakpoint(self, (uint32_t)self->selected);
}

static void on_console(GtkEntry *entry, gpointer user_data)
{
    DbgWin *self = user_data;
    static char out[65536];
    char *cmd = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
    char line[700];
    if (!*g_strstrip(cmd)) {
        g_free(cmd);
        return;
    }
    /* the history keeps each command once in a row */
    if (self->history->len == 0 ||
        strcmp(g_ptr_array_index(self->history, self->history->len - 1), cmd) != 0)
        g_ptr_array_add(self->history, g_strdup(cmd));
    self->history_pos = self->history->len;
    g_snprintf(line, sizeof line, "-%s\n", cmd);
    append_text(self->console_out, line, TRUE);   /* a command brings the reader back down */
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    out[0] = '\0';
    msdosdebug_command(self->dbg, cmd, out, sizeof out);
    g_free(cmd);
    append_text(self->console_out, out, TRUE);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(self->console_out, "\n", TRUE);
    /* a t, p or g that stops again reports where: refresh_now sees the new stop */
    settle(self);
    refresh_now(self);
}

static void history_show(DbgWin *self)
{
    const char *text = self->history_pos < self->history->len
                           ? g_ptr_array_index(self->history, self->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(self->console_in), text);
    gtk_editable_set_position(GTK_EDITABLE(self->console_in), -1);
}

/* Tab completes the current word against the commands, the registers and the
 * labels; several matches are listed, with their
 * common start filled in. Up and Down walk the command history. */
static gboolean on_console_key(GtkEventControllerKey *ctl, guint keyval, guint code,
                               GdkModifierType mods, gpointer user_data)
{
    DbgWin *self = user_data;
    char comps[4096];
    const char *text, *word;
    int count;
    (void)ctl; (void)code; (void)mods;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        if (self->history_pos > 0) {
            self->history_pos--;
            history_show(self);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (self->history_pos < self->history->len) {
            self->history_pos++;
            history_show(self);
        }
        return TRUE;
    }
    if (keyval != GDK_KEY_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(self->console_in));
    comps[0] = '\0';
    /* the engine completes the end of the whole line (a command, then its
     * arguments), so the whole line goes and the last word is replaced */
    count = msdosdebug_completions(self->dbg, text, comps, sizeof comps);
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    if (count == 1) {
        char merged[600];
        char *eol = strchr(comps, '\n');
        if (eol) *eol = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(self->console_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(self->console_in), -1);
    } else if (count > 1) {
        char **cand = g_strsplit(comps, "\n", -1);
        size_t common = strlen(cand[0]), len = strlen(word);
        int idx;
        for (idx = 1; cand[idx] && *cand[idx]; idx++) {
            size_t sub = 0;
            while (sub < common && cand[idx][sub] == cand[0][sub]) sub++;
            common = sub;
        }
        if (common > len) {
            char merged[600];
            g_snprintf(merged, sizeof merged, "%.*s%.*s", (int)(word - text), text, (int)common, cand[0]);
            gtk_editable_set_text(GTK_EDITABLE(self->console_in), merged);
            gtk_editable_set_position(GTK_EDITABLE(self->console_in), -1);
        }
        g_strfreev(cand);
        append_text(self->console_out, comps, FALSE);
        if (comps[0] && comps[strlen(comps) - 1] != '\n')
            append_text(self->console_out, "\n", FALSE);
    }
    return TRUE;
}

/* ---- Memory ---------------------------------------------------------------- */

static void mem_set_base(DbgWin *self, uint64_t base)
{
    const uint64_t top = 0x100000000ull - MEM_PAGE;
    if (base > top) base = top;
    self->mem_base = (uint32_t)(base & ~0xFull);
    refresh_mem(self);
}

/* Shows a range in the Memory tab, marked, with a little context above it. */
static void show_in_memory(DbgWin *self, uint32_t range_lo, uint32_t range_hi)
{
    self->mem_hl = TRUE;
    self->mem_hl_lo = range_lo;
    self->mem_hl_hi = range_hi;
    mem_set_base(self, (range_lo & ~0xFu) >= 4 * 16 ? (range_lo & ~0xFu) - 4 * 16 : 0);
}

/* Go to: the page holding an address (seg:off, a label, an expression) or a range, marked. */
static void on_mem_goto(GtkEntry *entry, gpointer user_data)
{
    DbgWin *self = user_data;
    uint32_t range_lo, range_hi;
    if (!parse_range(self, gtk_editable_get_text(GTK_EDITABLE(entry)), &range_lo, &range_hi)) {
        toast(self, "Not an address (seg:off, ds:si, a linear address, a label, an expression or a range)");
        return;
    }
    show_in_memory(self, range_lo, range_hi);
}

static void on_mem_page(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    const int delta = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "delta"));
    if (delta < 0)
        mem_set_base(self, self->mem_base > MEM_PAGE ? self->mem_base - MEM_PAGE : 0);
    else
        mem_set_base(self, (uint64_t)self->mem_base + MEM_PAGE);
}

/* The wheel scrolls by rows. */
static gboolean on_mem_scroll(GtkEventControllerScroll *ctl, double delta_x, double delta_y, gpointer user_data)
{
    DbgWin *self = user_data;
    int rows = (int)(delta_y * 3);
    (void)ctl; (void)delta_x;
    if (!rows) rows = delta_y > 0 ? 1 : -1;
    if (rows < 0 && self->mem_base < (uint32_t)(-rows) * 16) mem_set_base(self, 0);
    else mem_set_base(self, (uint64_t)((int64_t)self->mem_base + (int64_t)rows * 16));
    return TRUE;
}

/* "address value...": bytes written from the address on, as the CPU would write them. */
static void on_mem_write(GtkEntry *entry, gpointer user_data)
{
    DbgWin *self = user_data;
    char **parts = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(entry)), " \t=,", -1);
    uint8_t bytes[POKE_MAX], back[POKE_MAX];
    uint32_t addr = 0, addr_hi;
    int count = 0, idx;
    gboolean have_addr = FALSE, valid = TRUE;

    for (idx = 0; parts[idx] && valid; idx++) {
        int64_t val;
        if (!*parts[idx]) continue;
        if (!have_addr) {
            valid = parse_range(self, parts[idx], &addr, &addr_hi);
            have_addr = TRUE;
        } else if (count < POKE_MAX && parse_value(self, parts[idx], &val) && val >= -128 && val <= 255) {
            bytes[count++] = (uint8_t)val;
        } else {
            valid = FALSE;
        }
    }
    g_strfreev(parts);
    if (!valid || !have_addr || count == 0) {
        toast(self, "Poke takes an address and byte values, e.g. 0040:0017 41 42");
        return;
    }
    if (!pc_stopped(self)) {
        toast(self, "Stop the PC to poke its memory");
        return;
    }
    msdosdebug_write(self->dbg, addr, bytes, count);
    memset(back, 0, sizeof back);
    msdosdebug_read(self->dbg, addr, back, count);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    show_in_memory(self, addr, addr + (uint32_t)count - 1);
    if (memcmp(back, bytes, (size_t)count) != 0)
        toast(self, "Some bytes did not change: that memory is ROM or empty");
}

static void on_poke_button(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    on_mem_write(self->mem_write, self);
}

static void on_search(GtkButton *button, gpointer user_data)
{
    DbgWin *self = user_data;
    const int backward = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "back"));
    uint8_t pat[MSDOSSEARCH_MAX], mask;
    uint32_t found, from;
    int flags, count, got;

    if (!msdossearch_parse((int)gtk_drop_down_get_selected(self->srch_kind),
                           gtk_editable_get_text(GTK_EDITABLE(self->srch_entry)), pat, &count, &mask, &flags)) {
        gtk_label_set_text(self->srch_note, "Type hex bytes (3E 01) or some text.");
        return;
    }
    if (!pc_stopped(self)) {
        gtk_label_set_text(self->srch_note, "Stop the PC to search its memory.");
        return;
    }
    from = self->mem_hl ? (backward ? self->mem_hl_lo : self->mem_hl_lo + 1) : self->mem_base;
    got = msdosdebug_search_next(self->dbg, pat, count, mask, flags, from, backward, &found);
    if (got > 0) {
        g_autofree char *msg = g_strdup_printf("Found at %05X", found);
        gtk_label_set_text(self->srch_note, msg);
        show_in_memory(self, found, found + (uint32_t)count - 1);
    } else {
        gtk_label_set_text(self->srch_note, got == 0 ? "Not found." : "Could not search the memory.");
    }
}

static void on_search_entry_activate(GtkEntry *entry, gpointer user_data)
{
    GtkWidget *fwd = g_object_get_data(G_OBJECT(entry), "next-button");
    on_search(GTK_BUTTON(fwd), user_data);
}

/* ---- Disassembly ----------------------------------------------------------- */

/* The selected line's shading moved, without disassembling again. */
static void mark_selected(DbgWin *self)
{
    GtkTextBuffer *tbuf = gtk_text_view_get_buffer(self->disasm);
    GtkTextIter iter_start, iter_end;
    int idx;
    gtk_text_buffer_get_bounds(tbuf, &iter_start, &iter_end);
    gtk_text_buffer_remove_tag_by_name(tbuf, "sel", &iter_start, &iter_end);
    for (idx = 0; idx < self->line_count; idx++)
        if ((int64_t)self->line_lin[idx] == self->selected) {
            gtk_text_buffer_get_iter_at_line(tbuf, &iter_start, idx);
            gtk_text_buffer_get_iter_at_line(tbuf, &iter_end, idx + 1);
            gtk_text_buffer_apply_tag_by_name(tbuf, "sel", &iter_start, &iter_end);
        }
}

/* A click selects a line (Run to Cursor); a click on its left edge, or a
 * double-click, toggles its breakpoint. */
static void on_disasm_click(GtkGestureClick *gesture, int clicks, double pos_x, double pos_y, gpointer user_data)
{
    DbgWin *self = user_data;
    GtkTextIter iter;
    int buf_x, buf_y, line;
    gboolean gutter;
    (void)gesture;
    gtk_text_view_window_to_buffer_coords(self->disasm, GTK_TEXT_WINDOW_WIDGET, (int)pos_x, (int)pos_y, &buf_x, &buf_y);
    if (!gtk_text_view_get_iter_at_location(self->disasm, &iter, buf_x, buf_y)) return;
    line = gtk_text_iter_get_line(&iter);
    if (line < 0 || line >= self->line_count) return;
    gutter = gtk_text_iter_get_line_offset(&iter) < 2;
    self->selected = self->line_lin[line];
    if (gutter ? clicks == 1 : clicks == 2)
        toggle_breakpoint(self, self->line_lin[line]);
    else
        mark_selected(self);
}

static void on_follow_toggled(GtkCheckButton *btn, gpointer user_data)
{
    (void)btn;
    refresh_disasm(user_data);
}

static void on_jump(GtkEntry *entry, gpointer user_data)
{
    DbgWin *self = user_data;
    uint16_t seg;
    uint32_t off, lin;
    if (!parse_addr(self, gtk_editable_get_text(GTK_EDITABLE(entry)), &seg, &off, &lin)) {
        toast(self, "Not an address (seg:off, cs:ip, a linear address, a label)");
        return;
    }
    gtk_check_button_set_active(self->follow_pc, FALSE);
    self->top_seg = seg;
    self->top_off = off;
    self->selected = lin;
    refresh_disasm(self);
}

/* The wheel scrolls by instruction. */
static gboolean on_disasm_scroll(GtkEventControllerScroll *ctl, double delta_x, double delta_y, gpointer user_data)
{
    DbgWin *self = user_data;
    int rows = (int)(delta_y * 3);
    (void)ctl; (void)delta_x;
    if (!rows) rows = delta_y > 0 ? 1 : -1;
    if (!pc_stopped(self)) return TRUE;
    gtk_check_button_set_active(self->follow_pc, FALSE);
    self->top_off = rows > 0 ? rows_on(self, self->top_seg, self->top_off, rows)
                             : rows_back(self, self->top_seg, self->top_off, -rows);
    refresh_disasm(self);
    return TRUE;
}

/* ---- Breakpoints ----------------------------------------------------------- */

static void bp_enable_toggled(GtkCheckButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    msdosdebug_bp_enable(self->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bp")),
                         gtk_check_button_get_active(btn));
    refresh_now(self);
}

static void bp_remove_clicked(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    msdosdebug_bp_remove(self->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bp")));
    refresh_now(self);
}

static void on_bp_cond_activate(GtkEntry *entry, gpointer user_data)
{
    DbgWin *self = user_data;
    char err[256] = "", cond[128];
    g_strlcpy(cond, gtk_editable_get_text(GTK_EDITABLE(entry)), sizeof cond);
    g_strstrip(cond);
    if (!msdosdebug_bp_set_condition(self->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "bp")),
                                     cond, err, sizeof err)) {
        char *clean = clean_text(err[0] ? err : "The condition was refused");
        toast(self, clean);
        g_free(clean);
    }
    refresh_now(self);
}

static void show_bp_msg(DbgWin *self, const char *msg)
{
    gtk_label_set_text(self->bp_msg, msg);
    gtk_widget_set_visible(GTK_WIDGET(self->bp_msg), TRUE);
}

/* Execute and memory breakpoints take addresses (or a range); port ones a port or a
 * range; an INT a number and, optionally, an AH. */
static void on_bp_add(GtkWidget *wgt, gpointer user_data)
{
    DbgWin *self = user_data;
    guint type = gtk_drop_down_get_selected(self->bp_type);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->bp_addr));
    const char *ahs = gtk_editable_get_text(GTK_EDITABLE(self->bp_ah));
    uint32_t range_lo, range_hi;
    long ah_value = -1;
    int bpid;
    char err[256] = "", cond[128];
    (void)wgt;
    if (type >= MSDOS_BP_TYPES) type = MSDOS_BP_EXEC;
    if (type == MSDOS_BP_INT) {
        long num;
        if (!parse_num(text, &num) || num < 0 || num > 0xFF) {
            show_bp_msg(self, "An interrupt is 00-FF");
            return;
        }
        range_lo = range_hi = (uint32_t)num;
        if (*ahs && (!parse_num(ahs, &ah_value) || ah_value < 0 || ah_value > 0xFF)) {
            show_bp_msg(self, "AH is 00-FF, or empty for any");
            return;
        }
    } else {
        if (!parse_range(self, text, &range_lo, &range_hi)) {
            show_bp_msg(self, type >= MSDOS_BP_IN
                                  ? "Not a port or a range of ports (2F8, 2F8-2FF)"
                                  : "Not an address (seg:off, a linear address, a label, an expression or a range)");
            return;
        }
        if (type == MSDOS_BP_EXEC) range_hi = range_lo;
        if (type >= MSDOS_BP_IN && range_hi > 0xFFFF) {
            show_bp_msg(self, "A port is 0000-FFFF");
            return;
        }
    }
    g_strlcpy(cond, gtk_editable_get_text(GTK_EDITABLE(self->bp_cond)), sizeof cond);
    g_strstrip(cond);
    bpid = msdosdebug_bp_add_if(self->dbg, (int)type, range_lo, range_hi, (int)ah_value, cond, err, sizeof err);
    if (bpid < 0) {
        char *clean = clean_text(err[0] ? err : "The breakpoint was refused");
        show_bp_msg(self, clean);
        g_free(clean);
        return;
    }
    gtk_widget_set_visible(GTK_WIDGET(self->bp_msg), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(self->bp_addr), "");
    gtk_editable_set_text(GTK_EDITABLE(self->bp_ah), "");
    gtk_editable_set_text(GTK_EDITABLE(self->bp_cond), "");
    refresh_now(self);
}

static void on_bp_type(GObject *dropdown, GParamSpec *spec, gpointer user_data)
{
    DbgWin *self = user_data;
    const guint type = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    (void)spec;
    gtk_widget_set_sensitive(GTK_WIDGET(self->bp_ah), type == MSDOS_BP_INT);
    gtk_entry_set_placeholder_text(self->bp_addr, type <= MSDOS_BP_ACCESS ? "address, seg:off, label, lo-hi or expression"
                                                  : type == MSDOS_BP_INT ? "interrupt number, e.g. 21"
                                                                         : "port or range, e.g. 2F8 or 2F8-2FF");
}

static void on_bp_clear_hits(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    msdosdebug_bp_clear_hits(self->dbg);
    refresh_now(self);
}

static void on_bp_clear(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    msdosdebug_bp_clear(self->dbg);
    refresh_now(self);
}

/* ---- Watches ---------------------------------------------------------------- */

static void on_wt_remove(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    msdoswatch_remove(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "wt-idx")));
    refresh_now(self);
}

static void on_wt_memory(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    uint32_t range_lo, range_hi;
    if (msdoswatch_range(self->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "wt-idx")), &range_lo, &range_hi)) {
        show_tab(self, TAB_MEMORY);
        show_in_memory(self, range_lo, range_hi);
    } else {
        toast(self, "That watch does not resolve to an address yet");
    }
}

static gboolean bp_exists(DbgWin *self, int type, uint32_t range_lo, uint32_t range_hi, const char *cond)
{
    static msdos_breakpoint have[BP_MAX];
    int count = msdosdebug_bp_list(self->dbg, have, BP_MAX), idx;
    for (idx = 0; idx < count; idx++)
        if (have[idx].type == type && have[idx].start == range_lo && have[idx].end == range_hi &&
            strcmp(have[idx].condition, cond) == 0)
            return TRUE;
    return FALSE;
}

static gboolean break_on_change(DbgWin *self, uint32_t range_lo, uint32_t range_hi)
{
    char err[160] = "";
    if (bp_exists(self, MSDOS_BP_WRITE, range_lo, range_hi, "value != old")) return TRUE;
    return msdosdebug_bp_add_if(self->dbg, MSDOS_BP_WRITE, range_lo, range_hi, -1, "value != old",
                                err, sizeof err) >= 0;
}

static void on_wt_break(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    uint32_t range_lo, range_hi;
    if (!msdoswatch_range(self->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "wt-idx")), &range_lo, &range_hi)) {
        toast(self, "That watch does not resolve to an address yet");
        return;
    }
    if (break_on_change(self, range_lo, range_hi)) toast(self, "Breaking when it changes");
    else toast(self, "The breakpoint was refused");
    refresh_now(self);
}

static void on_wt_add(GtkButton *button, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)button;
    if (msdoswatch_add(gtk_editable_get_text(GTK_EDITABLE(self->wt_entry))) < 0) {
        toast(self, "Type an address, label, expression or lo-hi");
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(self->wt_entry), "");
    refresh_now(self);
}

static void on_wt_entry_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    on_wt_add(NULL, user_data);
}

/* ---- Trace and History ------------------------------------------------------ */

static void on_hist_rec(GtkToggleButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    if (self->h_sync) return;
    /* Trace and History share one ring: the one button turns both flags */
    msdosdebug_history_enable(self->dbg, gtk_toggle_button_get_active(btn));
    msdosdebug_trace_enable(self->dbg, gtk_toggle_button_get_active(btn));
    refresh_now(self);
}

static void on_hist_clear(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    msdosdebug_history_clear(self->dbg);
    msdosdebug_trace_clear(self->dbg);
    self->h_seen[0] = self->h_seen[1] = (uint64_t)-1;
    refresh_now(self);
}

static void on_hist_changed(GObject *obj, GParamSpec *spec, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)obj; (void)spec;
    self->h_seen[0] = self->h_seen[1] = (uint64_t)-1;
    refresh_now(self);
}

/* ---- Symbols ---------------------------------------------------------------- */

/* A linear address as the segment:offset the Disassembly shows (real mode's: seg * 16 + off). */
static void split_linear(uint32_t linear, uint16_t *seg, uint32_t *off)
{
    if (linear < 0x100000u) {
        *seg = (uint16_t)(linear >> 4);
        *off = linear & 0xF;
    } else {
        *seg = 0xFFFF;
        *off = linear - 0xFFFF0u;
    }
}

static void on_sym_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    DbgWin *self = user_data;
    const uint32_t addr = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "addr"));
    (void)box;
    gtk_check_button_set_active(self->follow_pc, FALSE);
    split_linear(addr, &self->top_seg, &self->top_off);
    self->selected = addr;
    show_tab(self, TAB_DISASM);
    refresh_now(self);
}

/* Break at: an execute breakpoint at the selected symbol. */
static void on_sym_break(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(self->symlist);
    uint32_t addr;
    char msg[64];
    (void)btn;
    if (!row) {
        toast(self, "Select a symbol first");
        return;
    }
    addr = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "addr"));
    if (msdosdebug_bp_at(self->dbg, addr)) {
        g_snprintf(msg, sizeof msg, "Already a breakpoint at %05X", addr);
    } else if (msdosdebug_bp_add(self->dbg, MSDOS_BP_EXEC, addr, addr, -1) < 0) {
        g_snprintf(msg, sizeof msg, "No room for another breakpoint");
    } else {
        g_snprintf(msg, sizeof msg, "Breakpoint at %05X", addr);
    }
    toast(self, msg);
    refresh_now(self);
}

static void on_sym_filter_changed(GtkEditable *entry, gpointer user_data)
{
    (void)entry;
    refresh_symbols(user_data, FALSE);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GObject) wobj = g_weak_ref_get(ref);
    DbgWin *self = wobj ? g_object_get_data(wobj, "dbgwin") : NULL;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    char msg[512] = "";
    long seg = 0;
    g_weak_ref_clear(ref);
    g_free(ref);
    /* the debugger may have been closed while the picker was up */
    if (!self || self->torn_down || !file) return;
    path = g_file_get_path(file);
    if (!path) return;
    if (!parse_num(gtk_editable_get_text(GTK_EDITABLE(self->sym_seg)), &seg) || seg < 0 || seg > 0xFFFF)
        seg = 0;
    if (msdosdebug_load_symbols(self->dbg, path, (uint16_t)seg, msg, sizeof msg) < 0 && !msg[0])
        g_strlcpy(msg, "No symbols found", sizeof msg);
    notice(self, msg);
    refresh_symbols(self, TRUE);
}

static void on_symbols(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *maps = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    GWeakRef *ref = g_new0(GWeakRef, 1);
    (void)btn;
    gtk_file_filter_set_name(maps, "Linker maps (*.map)");
    gtk_file_filter_add_suffix(maps, "map");
    gtk_file_filter_add_suffix(maps, "MAP");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, maps);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Load Symbols (MS LINK or Watcom WLINK .map)");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_weak_ref_init(ref, self->win);
    gtk_file_dialog_open(dlg, self->win, NULL, on_symbols_chosen, ref);
    g_object_unref(maps);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

static void on_clear_symbols(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    msdosdebug_clear_symbols(self->dbg);
    notice(self, "Symbols cleared");
    refresh_symbols(self, TRUE);
    refresh_now(self);
}

static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GObject) wobj = g_weak_ref_get(ref);
    DbgWin *self = wobj ? g_object_get_data(wobj, "dbgwin") : NULL;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *kind = g_object_get_data(src, "kind");
    char msg[512] = "";
    g_weak_ref_clear(ref);
    g_free(ref);
    if (!self || self->torn_down || !file) return;
    path = g_file_get_path(file);
    if (!path) return;
    msdosdebug_save(self->dbg, kind, path, msg, sizeof msg);
    notice(self, msg);
}

static void popdown_of(GtkWidget *btn)
{
    GtkWidget *pop = gtk_widget_get_ancestor(btn, GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void on_save(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    const char *kind = g_object_get_data(G_OBJECT(btn), "kind");
    char title[96];
    GWeakRef *ref = g_new0(GWeakRef, 1);
    popdown_of(GTK_WIDGET(btn));
    g_snprintf(title, sizeof title, "Save %s", (const char *)g_object_get_data(G_OBJECT(btn), "title"));
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_initial_name(dlg, g_object_get_data(G_OBJECT(btn), "file"));
    g_object_set_data(G_OBJECT(dlg), "kind", (gpointer)kind);
    g_weak_ref_init(ref, self->win);
    gtk_file_dialog_save(dlg, self->win, NULL, on_save_chosen, ref);
    g_object_unref(dlg);
}

/* ---- Interrupts and FujiNet -------------------------------------------------- */

static void on_int_switch(GObject *toggle, GParamSpec *spec, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)spec;
    if (self->updating_int) return;
    msdosdebug_int_trace_enable(self->dbg, gtk_switch_get_active(GTK_SWITCH(toggle)));
    refresh_ints(self, TRUE);
}

static void on_int_clear(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    msdosdebug_int_trace_clear(self->dbg);
    refresh_ints(self, TRUE);
}

static void on_fuji_clear(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    msdosdebug_fujinet_trace_clear(self->dbg);
    refresh_fujinet(self, TRUE);
}

/* ---- tabs in windows of their own ---------------------------------------------- */

static gboolean on_key(GtkEventControllerKey *ctl, guint keyval, guint code,
                       GdkModifierType mods, gpointer user_data);

static void dock_tab(DbgWin *self, int tab, gboolean destroy)
{
    GtkWindow *win = self->detached[tab];
    GtkWidget *page = self->pages[tab], *vbox;
    int pos = 0, txt;

    if (!win) return;
    vbox = g_object_get_data(G_OBJECT(win), "vbox");
    g_object_ref(page);
    gtk_box_remove(GTK_BOX(vbox), page);
    self->detached[tab] = NULL;
    for (txt = 0; txt < tab; txt++)   /* back where it was in the order */
        if (!self->detached[txt]) pos++;
    gtk_notebook_insert_page(self->notebook, page, gtk_label_new(k_tab_names[tab]), pos);
    g_object_unref(page);
    gtk_notebook_set_current_page(self->notebook, pos);
    if (destroy) gtk_window_destroy(win);
    refresh_now(self);
}

static void on_dock_button(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    dock_tab(self, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "tab")), TRUE);
}

static gboolean on_detached_close(GtkWindow *win, gpointer user_data)
{
    DbgWin *self = user_data;
    if (g_win != self || self->torn_down) return FALSE;   /* the whole debugger is going */
    dock_tab(self, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "tab")), FALSE);
    return FALSE;   /* let it close */
}

static void pop_out_tab(DbgWin *self, int tab)
{
    GtkWidget *page, *vbox, *header, *dock;
    GtkWindow *win;
    GtkEventController *keys;
    g_autofree char *title = NULL;

    if (tab < 0 || tab >= TAB_COUNT || self->detached[tab] || gtk_notebook_get_n_pages(self->notebook) <= 1)
        return;   /* one tab always stays */
    page = self->pages[tab];
    g_object_ref(page);
    gtk_notebook_remove_page(self->notebook, gtk_notebook_page_num(self->notebook, page));

    win = GTK_WINDOW(adw_window_new());
    title = g_strdup_printf("Debugger \xe2\x80\x93 %s", k_tab_names[tab]);
    gtk_window_set_title(win, title);
    gtk_window_set_default_size(win, 760, 520);
    header = adw_header_bar_new();
    dock = gtk_button_new_with_label("Dock");
    gtk_widget_set_tooltip_text(dock, "Put this tab back in the debugger");
    g_object_set_data(G_OBJECT(dock), "tab", GINT_TO_POINTER(tab));
    g_signal_connect(dock, "clicked", G_CALLBACK(on_dock_button), self);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), dock);
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(vbox), header);
    gtk_box_append(GTK_BOX(vbox), page);
    gtk_widget_set_vexpand(page, TRUE);
    adw_window_set_content(ADW_WINDOW(win), vbox);
    g_object_unref(page);
    g_object_set_data(G_OBJECT(win), "vbox", vbox);
    g_object_set_data(G_OBJECT(win), "tab", GINT_TO_POINTER(tab));
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), self);
    gtk_widget_add_controller(GTK_WIDGET(win), keys);
    g_signal_connect(win, "close-request", G_CALLBACK(on_detached_close), self);
    self->detached[tab] = win;
    gtk_window_present(win);
    refresh_now(self);
}

static void on_pop_out(GtkButton *btn, gpointer user_data)
{
    DbgWin *self = user_data;
    (void)btn;
    pop_out_tab(self, current_tab(self));
}

static void on_switch_page(GtkNotebook *notebook, GtkWidget *page, guint num, gpointer user_data)
{
    DbgWin *self = user_data;
    int tab = page_tab(page);
    (void)notebook; (void)num;
    /* the notebook also switches pages as it is taken apart with its window (the main window
     * closing), before the window's "destroy": only a mapped, live window has a tab to refresh */
    if (self->torn_down || !gtk_widget_get_mapped(GTK_WIDGET(self->win))) return;
    if (tab >= 0 && tab < TAB_COUNT) {
        msdos_help_set_context(self->win, k_tab_contexts[tab]);
        refresh_tab(self, tab);
        if (tab == TAB_VIDEO && msdossession_is_running(self->session))
            dbg_video_refresh(self->pages[TAB_VIDEO], TRUE);
        if (tab == TAB_CONSOLE && self->console_in) gtk_widget_grab_focus(GTK_WIDGET(self->console_in));
    }
}

/* ---- closing ------------------------------------------------------------------ */

/* The windows are hidden rather than destroyed (see main.c): hiding docks every tab, then
 * detaches the engine, which lets the machine run on at full speed. */
static void hide_window(DbgWin *self)
{
    int tab;
    for (tab = 0; tab < TAB_COUNT; tab++)
        if (self->detached[tab]) dock_tab(self, tab, TRUE);
    msdosdebug_detach(self->dbg);
    self->attached_running = FALSE;
    gtk_widget_set_visible(GTK_WIDGET(self->win), FALSE);
}

static gboolean on_close(GtkWindow *win, gpointer user_data)
{
    (void)win;
    hide_window(user_data);
    return TRUE;
}

/* The application quits (or the parent goes): AdwWindow has dropped its children before
 * "destroy" emits, so only what is not a widget is undone here. */
static void on_destroy(GtkWidget *widget, gpointer user_data)
{
    DbgWin *self = user_data;
    int tab;
    (void)widget;
    if (!self->torn_down) {
        self->torn_down = TRUE;
        if (self->notebook) g_signal_handlers_disconnect_by_data(self->notebook, self);
        for (tab = 0; tab < TAB_COUNT; tab++)
            if (self->detached[tab]) {
                GtkWindow *detached_win = self->detached[tab];
                g_signal_handlers_disconnect_by_data(detached_win, self);
                self->detached[tab] = NULL;
                gtk_window_destroy(detached_win);
            }
        if (self->timer) { g_source_remove(self->timer); self->timer = 0; }
        if (self->dark_id) {
            g_signal_handler_disconnect(adw_style_manager_get_default(), self->dark_id);
            self->dark_id = 0;
        }
        msdosdebug_detach(self->dbg);
    }
    if (g_win == self) g_win = NULL;
}

/* The window's memory outlives its widgets: freed once the window is gone,
 * when no child can still signal into it. */
static void dbgwin_free(gpointer data, GObject *where)
{
    DbgWin *self = data;
    (void)where;
    if (self->history) g_ptr_array_unref(self->history);
    if (self->bp_sig) g_string_free(self->bp_sig, TRUE);
    if (self->wt_sig) g_string_free(self->wt_sig, TRUE);
    g_free(self);
}

static gboolean on_key(GtkEventControllerKey *ctl, guint keyval, guint code,
                       GdkModifierType mods, gpointer user_data)
{
    DbgWin *self = user_data;
    const GdkModifierType chord = mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK);
    (void)ctl; (void)code;
    if (g_win != self || self->torn_down) return FALSE;
    /* the main window's Ctrl+Shift+D closes it here too */
    if (chord == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && (keyval == GDK_KEY_D || keyval == GDK_KEY_d)) {
        hide_window(self);
        return TRUE;
    }
    switch (keyval) {
    case GDK_KEY_F1: msdos_help_for(self->win); return TRUE;
    case GDK_KEY_F5: on_run(NULL, self); return TRUE;
    case GDK_KEY_F7: on_step(NULL, self); return TRUE;
    case GDK_KEY_F8:
        if (mods & GDK_SHIFT_MASK) on_step_out(NULL, self); else on_step_over(NULL, self);
        return TRUE;
    case GDK_KEY_F9: toggle_selected(self); return TRUE;
    case GDK_KEY_Page_Down:
    case GDK_KEY_Page_Up:
        /* Ctrl+PageDown / PageUp walk the tabs even from inside an entry */
        if (mods & GDK_CONTROL_MASK) {
            int count = gtk_notebook_get_n_pages(self->notebook);
            int step = keyval == GDK_KEY_Page_Down ? 1 : count - 1;
            if (count > 0)
                gtk_notebook_set_current_page(self->notebook,
                                              (gtk_notebook_get_current_page(self->notebook) + step) % count);
            return TRUE;
        }
        return FALSE;
    default: return FALSE;
    }
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *tool_button(const char *label, const char *tip, GCallback handler, DbgWin *self)
{
    GtkWidget *btn = gtk_button_new_with_mnemonic(label);
    gtk_widget_set_tooltip_text(btn, tip);
    g_signal_connect(btn, "clicked", handler, self);
    return btn;
}

static GtkWidget *build_toolbar(DbgWin *self)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *save, *pop, *popbox;
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly (4096 lines from CS:IP)", "disassembly.asm" },
        { "mem", "Memory (the first megabyte)", "memory.bin" },
        { "regs", "Registers (text)", "registers.txt" } };
    unsigned idx;

    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    self->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(self->run_btn, "clicked", G_CALLBACK(on_run), self);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->run_btn));
    gtk_box_append(GTK_BOX(bar), tool_button("Step (F7)", "One instruction", G_CALLBACK(on_step), self));
    gtk_box_append(GTK_BOX(bar), tool_button("Step Over (F8)", "Over a CALL, INT, LOOP or REP, to the instruction after it",
                                             G_CALLBACK(on_step_over), self));
    gtk_box_append(GTK_BOX(bar), tool_button("Step Out (\xe2\x87\xa7""F8)", "On until the routine returns (RET, RETF or IRET)",
                                             G_CALLBACK(on_step_out), self));
    gtk_box_append(GTK_BOX(bar), tool_button("Run to Cursor", "On to the line selected in the Disassembly",
                                             G_CALLBACK(on_run_to_cursor), self));

    save = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(save), "Save\xe2\x80\xa6");
    pop = gtk_popover_new();
    popbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (idx = 0; idx < G_N_ELEMENTS(saves); idx++) {
        GtkWidget *save_btn = gtk_button_new_with_label(saves[idx].title);
        gtk_widget_add_css_class(save_btn, "flat");
        g_object_set_data(G_OBJECT(save_btn), "kind", (gpointer)saves[idx].kind);
        g_object_set_data(G_OBJECT(save_btn), "title", (gpointer)saves[idx].title);
        g_object_set_data(G_OBJECT(save_btn), "file", (gpointer)saves[idx].file);
        g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save), self);
        gtk_box_append(GTK_BOX(popbox), save_btn);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), popbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(save), pop);
    gtk_box_append(GTK_BOX(bar), save);
    gtk_box_append(GTK_BOX(bar), tool_button("Pop _Out",
        "Move the tab that shows into a window of its own; Dock puts it back", G_CALLBACK(on_pop_out), self));
    return bar;
}

static GtkWidget *build_console(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&self->console_out, TRUE);
    GtkEventController *keys;

    make_log(self->console_out);
    self->console_in = GTK_ENTRY(gtk_entry_new());
    gtk_widget_add_css_class(GTK_WIDGET(self->console_in), "monospace");
    gtk_entry_set_placeholder_text(self->console_in,
        "u cs:ip   d ds:0   bpo 2f8   bpint 21 9   bpw 46c value != old   g  \xe2\x80\x94 Tab completes, Up/Down recall");
    gtk_widget_set_hexpand(GTK_WIDGET(self->console_in), TRUE);
    g_signal_connect(self->console_in, "activate", G_CALLBACK(on_console), self);
    /* ahead of the entry's own text handling, so Tab and Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_console_key), self);
    gtk_widget_add_controller(GTK_WIDGET(self->console_in), keys);
    self->history = g_ptr_array_new_with_free_func(g_free);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->console_in));
    set_text_at(self->console_out,
                "PC debugger console. DEBUG.COM's commands (r d e u g t p, i o, bp bc bl) and the family's "
                "(step over out run stop runto, bpr bpw bpa bpin bpout bpio bpint, print, mem, poke, find, "
                "watch, trace, symbols, label, map, io, sound, video, save). Type 'h' for every command.\n"
                "Numbers are hex (B800, $B800, 0xB800, B800h). Registers ax eax cs ip flags cf; [addr] is the "
                "byte there, {addr} the word; seg:off works anywhere.\n", TRUE);
    return padded(box);
}

static GtkWidget *reg_cell(DbgWin *self, GtkLabel **label_out, GtkEntry **entry_out, const char *name,
                           int gpr, int seg, int chars)
{
    GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkEntry *entry = GTK_ENTRY(gtk_entry_new());
    GtkWidget *label = gtk_label_new(name);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_entry_set_max_length(entry, 12);
    gtk_editable_set_width_chars(GTK_EDITABLE(entry), chars);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(entry), chars);
    gtk_widget_add_css_class(GTK_WIDGET(entry), "monospace");
    gtk_widget_set_tooltip_text(GTK_WIDGET(entry), "Enter sets it (hex, or an expression) while the PC is stopped");
    if (gpr) g_object_set_data(G_OBJECT(entry), "gpr", GINT_TO_POINTER(gpr));
    if (seg) g_object_set_data(G_OBJECT(entry), "seg", GINT_TO_POINTER(seg));
    g_signal_connect(entry, "activate", G_CALLBACK(on_reg_activate), self);
    gtk_box_append(GTK_BOX(cell), label);
    gtk_box_append(GTK_BOX(cell), GTK_WIDGET(entry));
    *label_out = GTK_LABEL(label);
    *entry_out = entry;
    return cell;
}

/* The register strip under every tab: the general registers (32-bit on a 386 or
 * later), the segment registers with their bases, and the flags are entries and
 * boxes (Enter sets one while the PC is stopped); the CPU, its mode and the
 * video card are described beside them. */
static GtkWidget *build_regs(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *row3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    int idx;

    for (idx = 0; idx < NREGS; idx++) {
        char name[8];
        GtkLabel *unused_label;
        GtkWidget *cell;
        g_strlcpy(name, k_regs16[idx], sizeof name);
        for (char *letter = name; *letter; letter++) *letter = g_ascii_toupper(*letter);
        cell = reg_cell(self, &self->reg_label[idx], &self->reg[idx], name, idx + 1, 0, 5);
        (void)unused_label;
        gtk_box_append(GTK_BOX(row1), cell);
    }
    for (idx = 0; idx < NSEGS; idx++) {
        char name[4];
        GtkWidget *cell;
        GtkLabel *name_label;
        g_snprintf(name, sizeof name, "%c%c", g_ascii_toupper(k_sregs[idx][0]), g_ascii_toupper(k_sregs[idx][1]));
        cell = reg_cell(self, &name_label, &self->sreg[idx], name, 0, idx + 1, 5);
        self->sreg_cell[idx] = cell;
        self->sbase[idx] = GTK_LABEL(gtk_label_new(""));
        gtk_widget_add_css_class(GTK_WIDGET(self->sbase[idx]), "dim-label");
        gtk_widget_add_css_class(GTK_WIDGET(self->sbase[idx]), "caption");
        gtk_widget_add_css_class(GTK_WIDGET(self->sbase[idx]), "monospace");
        gtk_label_set_xalign(self->sbase[idx], 0);
        gtk_box_append(GTK_BOX(cell), GTK_WIDGET(self->sbase[idx]));
        gtk_box_append(GTK_BOX(row2), cell);
    }
    for (idx = 0; idx < NFLAGS; idx++) {
        char name[4];
        g_snprintf(name, sizeof name, "%c%c", g_ascii_toupper(k_flags[idx].name[0]), g_ascii_toupper(k_flags[idx].name[1]));
        self->flag[idx] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(name));
        g_object_set_data(G_OBJECT(self->flag[idx]), "flag", (gpointer)k_flags[idx].name);
        g_signal_connect(self->flag[idx], "toggled", G_CALLBACK(on_flag_toggled), self);
        gtk_box_append(GTK_BOX(row3), GTK_WIDGET(self->flag[idx]));
    }
    self->state = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->state), "dim-label");
    gtk_widget_add_css_class(GTK_WIDGET(self->state), "monospace");
    gtk_label_set_xalign(self->state, 0);
    gtk_label_set_ellipsize(self->state, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(self->state), TRUE);
    gtk_widget_set_margin_start(GTK_WIDGET(self->state), 12);
    gtk_box_append(GTK_BOX(row3), GTK_WIDGET(self->state));
    gtk_box_append(GTK_BOX(box), row1);
    gtk_box_append(GTK_BOX(box), row2);
    gtk_box_append(GTK_BOX(box), row3);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    return box;
}

static GtkWidget *build_disasm(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&self->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkTextBuffer *tbuf = gtk_text_view_get_buffer(self->disasm);
    GtkWidget *hint;

    /* later tags win: the selection under the CS:IP line */
    gtk_text_buffer_create_tag(tbuf, "bp_on", "foreground", dot_colour(), NULL);
    gtk_text_buffer_create_tag(tbuf, "bp_off", "foreground", "#8a8a8a", NULL);
    gtk_text_buffer_create_tag(tbuf, "sel", "paragraph-background", "rgba(128,128,128,0.35)", NULL);
    self->tag_pc = accent_tag(self->disasm, "pc");

    self->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow CS:IP"));
    gtk_check_button_set_active(self->follow_pc, TRUE);
    g_signal_connect(self->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), self);
    self->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->jump, "F000:FFF0 or _main");
    gtk_editable_set_width_chars(GTK_EDITABLE(self->jump), 18);
    g_signal_connect(self->jump, "activate", G_CALLBACK(on_jump), self);
    hint = gtk_label_new("Click selects a line (Run to Cursor); double-click, F9 or the gutter toggles its "
                         "breakpoint; scroll to browse");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(hint), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(self->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(self->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), self);
    gtk_widget_add_controller(GTK_WIDGET(self->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self->disasm), scrollc);
    /* the wheel is ours; EXTERNAL (unlike NEVER) does not ask for a window
     * tall enough to show every one of the DISASM_WINDOW lines */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_EXTERNAL);
    self->disasm_scroll = scroll;

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    self->top_seg = 0xF000;
    self->top_off = 0xFFF0;
    self->selected = -1;
    return padded(box);
}

static GtkWidget *build_search_bar(DbgWin *self)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *next = tool_button("Find _Next", "Next match after the highlight", G_CALLBACK(on_search), self);
    GtkWidget *prev = tool_button("Find _Previous", "Previous match before the highlight", G_CALLBACK(on_search), self);
    static const char *const kinds[] = { "Hex bytes", "Text", NULL };

    g_object_set_data(G_OBJECT(prev), "back", GINT_TO_POINTER(1));
    self->srch_kind = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(kinds));
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->srch_kind), "Hex bytes (B8 34 12) or text, one character per byte");
    self->srch_entry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(self->srch_entry), TRUE);
    g_object_set_data(G_OBJECT(self->srch_entry), "next-button", next);
    g_signal_connect(self->srch_entry, "activate", G_CALLBACK(on_search_entry_activate), self);
    self->srch_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->srch_note), "dim-label");
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Search:"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->srch_kind));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->srch_entry));
    gtk_box_append(GTK_BOX(bar), next);
    gtk_box_append(GTK_BOX(bar), prev);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->srch_note));
    return bar;
}

static GtkWidget *build_memory(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = mono_view(&self->mem_view, FALSE);
    GtkWidget *page_up, *down, *poke;
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);

    self->mem_goto = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(self->mem_goto), 20);
    gtk_entry_set_placeholder_text(self->mem_goto, "B800:0, ds:si, label, lo-hi");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->mem_goto),
        "A linear address, seg:off, a register pair, a label, an expression or a range; the bytes are marked");
    g_signal_connect(self->mem_goto, "activate", G_CALLBACK(on_mem_goto), self);
    self->mem_write = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->mem_write, "address byte...  e.g. 0040:0017 41 42");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->mem_write),
        "Writes the bytes from the address on, as the CPU would (the PC stopped). ROM ignores them.");
    gtk_widget_set_hexpand(GTK_WIDGET(self->mem_write), TRUE);
    g_signal_connect(self->mem_write, "activate", G_CALLBACK(on_mem_write), self);
    poke = gtk_button_new_with_mnemonic("Po_ke");
    gtk_widget_set_tooltip_text(poke, "Write the bytes");
    g_signal_connect(poke, "clicked", G_CALLBACK(on_poke_button), self);
    page_up = gtk_button_new_with_label("Back 4K");
    down = gtk_button_new_with_label("On 4K");
    g_object_set_data(G_OBJECT(page_up), "delta", GINT_TO_POINTER(-1));
    g_object_set_data(G_OBJECT(down), "delta", GINT_TO_POINTER(1));
    g_signal_connect(page_up, "clicked", G_CALLBACK(on_mem_page), self);
    g_signal_connect(down, "clicked", G_CALLBACK(on_mem_page), self);
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Go to:"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->mem_goto));
    gtk_box_append(GTK_BOX(bar), page_up);
    gtk_box_append(GTK_BOX(bar), down);
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Poke:"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->mem_write));
    gtk_box_append(GTK_BOX(bar), poke);

    {
        char fill[16], ink[16];
        g_snprintf(fill, sizeof fill, "#%06x", accent_fill());
        g_snprintf(ink, sizeof ink, "#%06x", accent_ink());
        self->tag_mem_mark = gtk_text_buffer_create_tag(gtk_text_view_get_buffer(self->mem_view), "mark",
                                                        "background", fill, "foreground", ink, NULL);
    }
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_mem_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self->mem_view), scrollc);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_EXTERNAL);

    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), build_search_bar(self));
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

static GtkWidget *build_breaks(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    static const char *const types[] = { "Execute", "Memory read", "Memory write", "Memory read/write",
                                         "Port IN", "Port OUT", "Port IN/OUT", "INT", NULL };

    self->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->bp_type),
        "Execute stops before an instruction; memory, ports and INT stop after the instruction that "
        "touched them");
    self->bp_addr = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->bp_addr, "address, seg:off, label, lo-hi or expression");
    gtk_widget_set_hexpand(GTK_WIDGET(self->bp_addr), TRUE);
    g_signal_connect(self->bp_addr, "activate", G_CALLBACK(on_bp_add), self);
    self->bp_ah = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->bp_ah, "AH (any)");
    gtk_editable_set_width_chars(GTK_EDITABLE(self->bp_ah), 9);
    gtk_widget_set_sensitive(GTK_WIDGET(self->bp_ah), FALSE);
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->bp_ah), "An INT breakpoint can match one AH function only");
    g_signal_connect(self->bp_ah, "activate", G_CALLBACK(on_bp_add), self);
    self->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->bp_cond, "condition (optional): value != old, ax == 4C00, [ds:si] > 5");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->bp_cond),
        "An expression: registers (al ax eax cs ip ...), flags (cf zf ...), [addr] the byte and {addr} the word "
        "there, seg:off, labels, frame, cycles; on an access `value`, `addr` and `old` (the data and the byte "
        "before a write). For a port breakpoint the port is addr; for an INT, addr is the number.");
    gtk_widget_set_hexpand(GTK_WIDGET(self->bp_cond), TRUE);
    g_signal_connect(self->bp_cond, "activate", G_CALLBACK(on_bp_add), self);
    g_signal_connect(self->bp_type, "notify::selected", G_CALLBACK(on_bp_type), self);
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(self->bp_type));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(self->bp_addr));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(self->bp_ah));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(self->bp_cond));
    gtk_box_append(GTK_BOX(form), tool_button("_Add", "Add the breakpoint", G_CALLBACK(on_bp_add), self));

    self->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->bp_list, GTK_SELECTION_NONE);
    gtk_widget_set_valign(GTK_WIDGET(self->bp_list), GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(self->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    self->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->bp_msg), "msdos-dbg-accent-text");
    gtk_label_set_xalign(self->bp_msg, 0);
    gtk_label_set_wrap(self->bp_msg, TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->bp_msg), FALSE);
    self->bp_stop = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(self->bp_stop, 0);
    gtk_widget_add_css_class(GTK_WIDGET(self->bp_stop), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(self->bp_stop), TRUE);
    gtk_box_append(GTK_BOX(foot), GTK_WIDGET(self->bp_stop));
    gtk_box_append(GTK_BOX(foot), tool_button("Clear _Hits", "Zero the hit counts", G_CALLBACK(on_bp_clear_hits), self));
    gtk_box_append(GTK_BOX(foot), tool_button("_Remove All", "Remove every breakpoint", G_CALLBACK(on_bp_clear), self));

    gtk_box_append(GTK_BOX(box), form);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->bp_msg));
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), foot);
    return padded(box);
}

static GtkWidget *build_watches(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();

    self->wt_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->wt_entry, "address, seg:off, label, expression, or lo-hi");
    gtk_widget_set_hexpand(GTK_WIDGET(self->wt_entry), TRUE);
    g_signal_connect(self->wt_entry, "activate", G_CALLBACK(on_wt_entry_activate), self);
    gtk_box_append(GTK_BOX(form), gtk_label_new("Watch:"));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(self->wt_entry));
    gtk_box_append(GTK_BOX(form), tool_button("_Add", "Watch it", G_CALLBACK(on_wt_add), self));

    self->wt_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->wt_list, GTK_SELECTION_NONE);
    gtk_widget_set_valign(GTK_WIDGET(self->wt_list), GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(self->wt_list));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), form);
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

static GtkWidget *build_machine(DbgWin *self)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = headed("CPU, memory, controllers, ports, drives and FujiNet", mono_view(&self->machine_text, FALSE));
    GtkWidget *right = headed("Memory map", mono_view(&self->map_text, FALSE));
    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(left, 560, -1);
    gtk_widget_set_size_request(right, 560, -1);
    return padded(paned);
}

static GtkWidget *build_hist(DbgWin *self, int idx)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = mono_view(&self->h_text[idx], FALSE);
    GtkWidget *clear = gtk_button_new_with_mnemonic("_Clear");
    static const char *const filters[] = { "All instructions", "Branches only", "In range", NULL };

    make_log(self->h_text[idx]);
    self->h_rec[idx] = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Record"));
    gtk_widget_add_css_class(GTK_WIDGET(self->h_rec[idx]), "msdos-dbg-toggle");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->h_rec[idx]),
                                "Record every instruction the CPU runs (the PC runs slower while recording)");
    g_signal_connect(self->h_rec[idx], "toggled", G_CALLBACK(on_hist_rec), self);
    self->h_filter[idx] = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(filters));
    g_signal_connect(self->h_filter[idx], "notify::selected", G_CALLBACK(on_hist_changed), self);
    self->h_range[idx] = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->h_range[idx], "range: lo-hi or a label");
    g_signal_connect(self->h_range[idx], "activate", G_CALLBACK(on_hist_changed), self);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_hist_clear), self);
    self->h_note[idx] = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->h_note[idx]), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(self->h_note[idx]), TRUE);
    gtk_label_set_xalign(self->h_note[idx], 1.0f);
    self->h_seen[idx] = (uint64_t)-1;

    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->h_rec[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->h_filter[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->h_range[idx]));
    gtk_box_append(GTK_BOX(bar), clear);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->h_note[idx]));
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

static GtkWidget *build_symbols(DbgWin *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *load_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *hint;

    self->sym_filter = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->sym_filter, "filter by name");
    gtk_widget_set_hexpand(GTK_WIDGET(self->sym_filter), TRUE);
    g_signal_connect(self->sym_filter, "changed", G_CALLBACK(on_sym_filter_changed), self);
    self->sym_count = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->sym_count), "dim-label");
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Filter:"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->sym_filter));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->sym_count));
    gtk_box_append(GTK_BOX(bar), tool_button("_Break at", "Execute breakpoint at the selected symbol",
                                             G_CALLBACK(on_sym_break), self));

    self->sym_seg = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->sym_seg, "0000");
    gtk_editable_set_width_chars(GTK_EDITABLE(self->sym_seg), 8);
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->sym_seg),
        "The segment the program was loaded at: a .COM's PSP, an .EXE's PSP + 10h (hex)");
    hint = gtk_label_new("MS LINK or Watcom WLINK .map; the load segment is a .COM's PSP or an .EXE's PSP + 10h");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_ellipsize(GTK_LABEL(hint), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_box_append(GTK_BOX(load_bar), gtk_label_new("Load segment:"));
    gtk_box_append(GTK_BOX(load_bar), GTK_WIDGET(self->sym_seg));
    gtk_box_append(GTK_BOX(load_bar), tool_button("_Load\xe2\x80\xa6", "Load labels from a linker map at that segment",
                                                  G_CALLBACK(on_symbols), self));
    gtk_box_append(GTK_BOX(load_bar), tool_button("Cl_ear", "Forget every symbol and label", G_CALLBACK(on_clear_symbols), self));
    gtk_box_append(GTK_BOX(load_bar), hint);

    self->symlist = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->symlist, GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(self->symlist, FALSE);   /* a click selects, a double-click goes there */
    g_signal_connect(self->symlist, "row-activated", G_CALLBACK(on_sym_row_activated), self);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(self->symlist));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), load_bar);
    gtk_box_append(GTK_BOX(box), scroll);
    self->sym_built = -1;     /* the first refresh always fills the list */
    return padded(box);
}

static GtkWidget *build_ints(DbgWin *self)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *title = gtk_label_new("INT calls");

    gtk_widget_add_css_class(title, "heading");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    self->int_on = GTK_SWITCH(gtk_switch_new());
    gtk_widget_set_valign(GTK_WIDGET(self->int_on), GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->int_on), "Record every INT the PC runs (costs speed)");
    g_signal_connect(self->int_on, "notify::active", G_CALLBACK(on_int_switch), self);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_int_clear), self);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_append(GTK_BOX(row), title);
    gtk_box_append(GTK_BOX(row), labeled("Trace", GTK_WIDGET(self->int_on)));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), mono_view(&self->int_text, FALSE));
    make_log(self->int_text);

    gtk_box_append(GTK_BOX(right), headed("Vector table", mono_view(&self->ivt_text, FALSE)));

    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(left, 560, -1);
    gtk_widget_set_size_request(right, 600, -1);
    self->int_count = -1;
    return padded(paned);
}

static GtkWidget *build_fujinet(DbgWin *self)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *start, *end;

    self->fuji_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(self->fuji_note), "dim-label");
    gtk_label_set_xalign(self->fuji_note, 0);
    gtk_label_set_wrap(self->fuji_note, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self->fuji_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), self);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(self->fuji_note));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), mono_view(&self->fuji_text, FALSE));
    make_log(self->fuji_text);

    start = headed("FujiBus packets (SLIP-decoded)", left);
    end = headed("FujiNet's UART", mono_view(&self->uart_text, FALSE));
    gtk_paned_set_start_child(GTK_PANED(paned), start);
    gtk_paned_set_end_child(GTK_PANED(paned), end);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(start, 620, -1);
    gtk_widget_set_size_request(end, 320, -1);
    self->fuji_count = -1;
    return padded(paned);
}

/* A tab from an environment variable (MSDOS_DEBUGGER_TAB, MSDOS_DEBUGGER_POPOUT): a
 * number, a tab's name or the start of one (any case), or an older name
 * (cpu, registers, io, speaker, opl, int). For tests and captures. */
static int tab_from_env(const char *var)
{
    const char *tab = g_getenv(var);
    char *end;
    long count;
    size_t idx;
    if (!tab || !*tab) return -1;
    count = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (count >= 0 && count < TAB_COUNT) ? (int)count : -1;
    for (idx = 0; idx < G_N_ELEMENTS(k_tab_aliases); idx++)
        if (g_ascii_strcasecmp(tab, k_tab_aliases[idx].name) == 0)
            return k_tab_aliases[idx].tab;
    for (idx = 0; idx < TAB_COUNT; idx++)
        if (g_ascii_strncasecmp(tab, k_tab_names[idx], strlen(tab)) == 0)
            return (int)idx;
    return -1;
}

/* The capture hooks: MSDOS_DEBUGGER_WATCH adds a watch, MSDOS_DEBUGGER_WATCH_BREAK a break-on-change
 * on it, MSDOS_DEBUGGER_MEM_GOTO shows it in Memory, MSDOS_DEBUGGER_HISTORY records from the start.
 * Opening the window again must not stack a second copy. */
static void apply_env_hooks(DbgWin *self)
{
    const char *watch = g_getenv("MSDOS_DEBUGGER_WATCH"), *brk = g_getenv("MSDOS_DEBUGGER_WATCH_BREAK"),
               *goto_text = g_getenv("MSDOS_DEBUGGER_MEM_GOTO");
    uint32_t range_lo, range_hi;
    if (watch && *watch) {
        char have_text[200];
        int count = msdoswatch_count(), idx;
        gboolean have = FALSE;
        for (idx = 0; idx < count; idx++)
            if (msdoswatch_text(idx, have_text, sizeof have_text) && strcmp(have_text, watch) == 0) have = TRUE;
        if (!have) msdoswatch_add(watch);
    }
    if (brk && *brk && parse_range(self, brk, &range_lo, &range_hi))
        break_on_change(self, range_lo, range_hi);
    if (goto_text && *goto_text && parse_range(self, goto_text, &range_lo, &range_hi))
        show_in_memory(self, range_lo, range_hi);
    if (g_getenv("MSDOS_DEBUGGER_HISTORY")) {
        msdosdebug_history_enable(self->dbg, 1);
        msdosdebug_trace_enable(self->dbg, 1);
    }
}

void msdos_debugger_show(GtkWindow *parent, msdossession *session)
{
    DbgWin *self;
    GtkWidget *toolbar, *header, *root, *notebook, *overlay, *scroller;
    GtkEventController *keys;
    int tab;

    if (g_win) {
        gtk_window_present(g_win->win);
        attach_and_refresh(g_win);
        return;
    }
    install_css();
    self = g_new0(DbgWin, 1);
    self->session = session;
    self->dbg = msdossession_debugger(session);
    self->mem_base = 0;
    g_win = self;

    self->win = GTK_WINDOW(adw_window_new());
    g_object_set_data(G_OBJECT(self->win), "dbgwin", self);
    g_object_weak_ref(G_OBJECT(self->win), dbgwin_free, self);
    gtk_window_set_title(self->win, "Debugger");
    gtk_window_set_default_size(self->win, 1310, 800);
    if (parent) {
        gtk_window_set_transient_for(self->win, parent);
        gtk_window_set_application(self->win, gtk_window_get_application(parent));
        gtk_window_set_destroy_with_parent(self->win, TRUE);
    }
    g_signal_connect(self->win, "close-request", G_CALLBACK(on_close), self);
    g_signal_connect(self->win, "destroy", G_CALLBACK(on_destroy), self);

    self->pages[TAB_CONSOLE] = build_console(self);
    self->pages[TAB_DISASM] = build_disasm(self);
    self->pages[TAB_MEMORY] = build_memory(self);
    self->pages[TAB_BREAKS] = build_breaks(self);
    self->pages[TAB_WATCHES] = build_watches(self);
    self->pages[TAB_MACHINE] = build_machine(self);
    self->pages[TAB_TRACE] = build_hist(self, 0);
    self->pages[TAB_HISTORY] = build_hist(self, 1);
    self->pages[TAB_VIDEO] = dbg_video_new(session);
    self->pages[TAB_SOUND] = padded(mono_view(&self->sound_text, FALSE));
    self->pages[TAB_SYMBOLS] = build_symbols(self);
    self->pages[TAB_INTS] = build_ints(self);
    self->pages[TAB_FUJINET] = build_fujinet(self);

    notebook = gtk_notebook_new();
    self->notebook = GTK_NOTEBOOK(notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_notebook_set_scrollable(self->notebook, TRUE);
    for (tab = 0; tab < TAB_COUNT; tab++) {
        /* each page keeps its identity when it is moved around */
        g_object_set_data(G_OBJECT(self->pages[tab]), "tab-index", GINT_TO_POINTER(tab + 1));
        gtk_notebook_append_page(self->notebook, self->pages[tab], gtk_label_new(k_tab_names[tab]));
    }
    tab = tab_from_env("MSDOS_DEBUGGER_TAB");
    if (tab >= 0) gtk_notebook_set_current_page(self->notebook, tab);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), self);
    {
        int cur = gtk_notebook_get_current_page(self->notebook);
        msdos_help_set_context(self->win, k_tab_contexts[cur >= 0 && cur < TAB_COUNT ? cur : 0]);
    }

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    toolbar = build_toolbar(self);
    gtk_box_append(GTK_BOX(root), toolbar);
    gtk_box_append(GTK_BOX(root), notebook);
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(root), build_regs(self));

    /* A narrow window scrolls the whole debugger instead of clipping it: the
     * pages' widest content would otherwise set the window's minimum width. */
    scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), root);
    overlay = adw_toast_overlay_new();
    self->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(self->toasts, scroller);

    /* the status line -- why the machine stopped -- is the title's subtitle */
    header = adw_header_bar_new();
    self->title = ADW_WINDOW_TITLE(adw_window_title_new("Debugger", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(self->title));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), msdos_help_button(NULL));
    {
        GtkWidget *view = adw_toolbar_view_new();
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), overlay);
        adw_window_set_content(ADW_WINDOW(self->win), view);
    }

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), self);
    gtk_widget_add_controller(GTK_WIDGET(self->win), keys);

    self->dark_id = g_signal_connect(adw_style_manager_get_default(), "notify::dark",
                                     G_CALLBACK(on_dark_changed), self);

    gtk_window_present(self->win);
    apply_env_hooks(self);
    attach_and_refresh(self);     /* stops the machine */
    self->timer = g_timeout_add(TICK_MS, tick_refresh, self);
    if (current_tab(self) == TAB_CONSOLE) gtk_widget_grab_focus(GTK_WIDGET(self->console_in));
    tab = tab_from_env("MSDOS_DEBUGGER_POPOUT");   /* a tab in a window of its own, for captures */
    if (tab >= 0) pop_out_tab(self, tab);
}

/* Ctrl+Shift+D and the menu item: show it, or hide it if it is showing. */
void msdos_debugger_toggle(GtkWindow *parent, msdossession *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        msdos_debugger_show(parent, session);
}
