/*
 * The debugger's Video tab (GTK4/cairo).
 *
 * Everything that decides what to show lives in the core (msdosvidtab.h): this file is
 * widgets. The picture is drawn from the controller's cached buffer, so a mouse move redraws
 * only the marks. The picture is 0xFFRRGGBB and drawn as CAIRO_FORMAT_RGB24 with a nearest
 * filter. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <string.h>

#include "msdosvidtab.h"

#define PAL_CELL 18
#define PAL_COLS 16

enum { SEC_PIXEL, SEC_REGS, SEC_PALETTE, SEC_MEMORY, SEC_TIMING };

static const int k_zoom_values[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };
static const char *const k_zoom_labels[] = { "Fit", "1×", "2×", "3×", "4×", "6×", "8×", "12×", "16×", NULL };
#define NZOOM 9

typedef struct {
    msdosvid_tab *tab;
    GtkWidget *page;
    GtkDropDown *view, *zoom;
    GtkCheckButton *ov[MSDOSVID_O_COUNT];
    GtkLabel *status, *note;
    GtkDrawingArea *pic;
    GtkWidget *pic_scroll;
    GtkNotebook *sections;
    GtkLabel *pixel_text, *pixel_note;
    GtkTextView *regs_text;
    char *regs_sig;
    GtkDrawingArea *pal;
    GtkLabel *pal_detail;
    int pal_hover;
    GtkLabel *mem_text, *timing_text;
    GtkProgressBar *beam;
    gboolean updating;
} Vid;

static GtkWidget *mono_label(void)
{
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "monospace");
    return l;
}

/* ---- the picture ---------------------------------------------------------- */

static void draw_message(cairo_t *cr, int w, int h, const char *msg)
{
    cairo_text_extents_t ext;
    cairo_set_source_rgb(cr, 0.55, 0.55, 0.58);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    cairo_text_extents(cr, msg, &ext);
    cairo_move_to(cr, (w - ext.width) / 2 - ext.x_bearing, (h - ext.height) / 2 - ext.y_bearing);
    cairo_show_text(cr, msg);
}

static void draw_pic(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Vid *v = ud;
    int pw, ph, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *px;
    (void)area;
    cairo_set_source_rgb(cr, 0.07, 0.07, 0.09);
    cairo_paint(cr);
    px = msdosvid_tab_picture(v->tab, &pw, &ph);
    if (!px) {
        const char *why = msdosvid_tab_why(v->tab);
        draw_message(cr, w, h, why ? why : "Nothing to show");
        return;
    }
    {
        cairo_surface_t *s = cairo_image_surface_create_for_data((unsigned char *)px, CAIRO_FORMAT_RGB24,
                                                                 pw, ph, pw * 4);
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_surface_destroy(s);
    }
    marks = msdosvid_tab_marks(v->tab, hover, pin);
    cairo_set_line_width(cr, 1);
    if (marks & 1) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
        cairo_rectangle(cr, hover[0] + 0.5, hover[1] + 0.5, hover[2] - 1, hover[3] - 1);
        cairo_stroke(cr);
    }
    if (marks & 2) {
        cairo_set_source_rgba(cr, 1.0, 0.85, 0.1, 1);
        cairo_set_line_width(cr, 2);
        cairo_rectangle(cr, pin[0] + 1, pin[1] + 1, pin[2] - 2, pin[3] - 2);
        cairo_stroke(cr);
    }
}

static void update_pixel_section(Vid *v)
{
    const msdosvid_pixel *p = msdosvid_tab_pixel(v->tab);
    if (!p) {
        gtk_label_set_text(v->pixel_text, "Move over the picture to inspect a pixel.\nClick to pin it; double-click to break on writes to it.");
        gtk_label_set_text(v->pixel_note, "");
    } else {
        gtk_label_set_text(v->pixel_text, p->text);
        gtk_label_set_text(v->pixel_note, "");
    }
    if (v->pal) gtk_widget_queue_draw(GTK_WIDGET(v->pal));
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    Vid *v = ud;
    (void)c;
    if (msdosvid_tab_hover(v->tab, (int)x, (int)y)) {
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
        update_pixel_section(v);
    }
}

static void on_leave(GtkEventControllerMotion *c, gpointer ud)
{
    Vid *v = ud;
    (void)c;
    if (msdosvid_tab_hover(v->tab, -1, -1)) {
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
        update_pixel_section(v);
    }
}

static void on_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    Vid *v = ud;
    (void)g;
    if (n == 1) {
        msdosvid_tab_pin(v->tab, (int)x, (int)y);
    } else if (n >= 2) {
        int id;
        msdosvid_tab_hover(v->tab, (int)x, (int)y);
        id = msdosvid_tab_break_on_pixel(v->tab);
        gtk_label_set_text(v->note, id > 0 ? "Breakpoint added: break on writes to that address (see Breakpoints)"
                                           : "That pixel has no CPU address to break on");
    }
    gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    update_pixel_section(v);
}

/* ---- sections -------------------------------------------------------------- */

static void fill_regs(Vid *v)
{
    const msdosvid_reg *r;
    int n = msdosvid_tab_regs(v->tab, &r), i;
    GString *s = g_string_new(NULL), *sig = g_string_new(NULL);
    const char *group = NULL;
    for (i = 0; i < n; i++) {
        if (!group || strcmp(group, r[i].group)) {
            g_string_append_printf(s, "%s── %s ──\n", i ? "\n" : "", r[i].group);
            group = r[i].group;
        }
        if (r[i].port && r[i].index < 0)
            g_string_append_printf(s, "%03X       %-22s %0*X  %s\n", r[i].port, r[i].name, r[i].digits, r[i].value, r[i].meaning);
        else
            g_string_append_printf(s, "%02X  %-3s  %-22s %0*X  %s\n", r[i].index, "", r[i].name, r[i].digits, r[i].value, r[i].meaning);
        g_string_append_printf(sig, "%d", r[i].age > 0 && r[i].age < MSDOSVID_REG_FADE);
    }
    g_string_append_len(sig, s->str, (gssize)s->len);
    if (!v->regs_sig || strcmp(v->regs_sig, sig->str)) {
        GtkTextBuffer *b = gtk_text_view_get_buffer(v->regs_text);
        GtkTextIter a, e;
        char *clean = g_utf8_make_valid(s->str, -1);
        double vadj = 0;
        GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(v->regs_text));
        if (adj) vadj = gtk_adjustment_get_value(adj);
        gtk_text_buffer_set_text(b, clean, -1);
        g_free(clean);
        {
            /* highlight lines whose register changed lately */
            int line = 0, k;
            group = NULL;
            for (k = 0; k < n; k++) {
                if (!group || strcmp(group, r[k].group)) {
                    group = r[k].group;
                    line += (k ? 2 : 1);      /* blank line + header (the first has no blank) */
                }
                if (r[k].age > 0 && r[k].age < MSDOSVID_REG_FADE) {
                    gtk_text_buffer_get_iter_at_line(b, &a, line);
                    e = a;
                    gtk_text_iter_forward_to_line_end(&e);
                    gtk_text_buffer_apply_tag_by_name(b, "changed", &a, &e);
                }
                line++;
            }
        }
        if (adj) gtk_adjustment_set_value(adj, vadj);
        g_free(v->regs_sig);
        v->regs_sig = g_strdup(sig->str);
    }
    g_string_free(s, TRUE);
    g_string_free(sig, TRUE);
}

static void draw_pal(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Vid *v = ud;
    const msdosvid_pal_entry *e;
    int n = msdosvid_tab_palette(v->tab, &e), i, sel = msdosvid_tab_pixel_palette_index(v->tab);
    int cols = n > 16 ? PAL_COLS : 8;
    (void)area; (void)w; (void)h;
    for (i = 0; i < n; i++) {
        int x = (i % cols) * (PAL_CELL + 2), y = (i / cols) * (PAL_CELL + 2);
        cairo_set_source_rgb(cr, ((e[i].rgb >> 16) & 255) / 255.0, ((e[i].rgb >> 8) & 255) / 255.0, (e[i].rgb & 255) / 255.0);
        cairo_rectangle(cr, x, y, PAL_CELL, PAL_CELL);
        cairo_fill(cr);
        if (!e[i].active) {
            cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
            cairo_rectangle(cr, x, y, PAL_CELL, PAL_CELL);
            cairo_fill(cr);
        }
        if (e[i].age > 0 && e[i].age < MSDOSVID_REG_FADE) {
            cairo_set_source_rgb(cr, 1, 0.3, 0.9);
            cairo_set_line_width(cr, 2);
            cairo_rectangle(cr, x + 1, y + 1, PAL_CELL - 2, PAL_CELL - 2);
            cairo_stroke(cr);
        }
        if (i == sel || i == v->pal_hover) {
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_set_line_width(cr, 2);
            cairo_rectangle(cr, x - 1, y - 1, PAL_CELL + 2, PAL_CELL + 2);
            cairo_stroke(cr);
        }
    }
}

static void pal_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    Vid *v = ud;
    const msdosvid_pal_entry *e;
    int n = msdosvid_tab_palette(v->tab, &e), cols = n > 16 ? PAL_COLS : 8, i;
    char buf[200];
    (void)c;
    i = ((int)y / (PAL_CELL + 2)) * cols + (int)x / (PAL_CELL + 2);
    if (i < 0 || i >= n) i = -1;
    if (i != v->pal_hover) {
        v->pal_hover = i;
        gtk_widget_queue_draw(GTK_WIDGET(v->pal));
    }
    if (i < 0) {
        gtk_label_set_text(v->pal_detail, "");
        return;
    }
    g_snprintf(buf, sizeof buf, "#%06X  %s\nused by %s%s%s", e[i].rgb, e[i].detail,
               e[i].used >= 0 ? g_strdup_printf("%d", e[i].used) : "?", e[i].used >= 0 ? " pixels" : "",
               e[i].active ? "" : "  (this mode cannot reach it)");
    gtk_label_set_text(v->pal_detail, buf);
}

static void fill_palette(Vid *v)
{
    const msdosvid_pal_entry *e;
    int n = msdosvid_tab_palette(v->tab, &e), cols = n > 16 ? PAL_COLS : 8;
    int rows = n ? (n + cols - 1) / cols : 1;
    gtk_drawing_area_set_content_width(v->pal, cols * (PAL_CELL + 2));
    gtk_drawing_area_set_content_height(v->pal, rows * (PAL_CELL + 2));
    gtk_widget_queue_draw(GTK_WIDGET(v->pal));
}

static void fill_memory(Vid *v)
{
    const msdosvid_region *r;
    int n = msdosvid_tab_regions(v->tab, &r), i;
    GString *s = g_string_new(NULL);
    const msdos_vid_snap *sn = msdosvid_tab_snap(v->tab);
    if (!n) g_string_append(s, "No video memory");
    for (i = 0; i < n; i++) {
        char *esc = g_markup_escape_text(r[i].name, -1);
        g_string_append_printf(s, "<span foreground=\"#%06X\">■■</span> %s\n     VRAM %05X + %05X", r[i].color, esc,
                               r[i].vram_off, r[i].size);
        g_free(esc);
        if (r[i].cpu_addr != MSDOS_VID_NONE) g_string_append_printf(s, "   CPU %05X", r[i].cpu_addr);
        g_string_append_c(s, '\n');
    }
    if (n)
        g_string_append_printf(s, "\n%u KB of VRAM%s; the CPU sees it at %05X-%05X.\nSelect the Memory map view to see it all.",
                               sn->vram_size / 1024, sn->planar ? ", four planes" : "", sn->cpu_base,
                               sn->cpu_base + (sn->cpu_size ? sn->cpu_size - 1 : 0));
    gtk_label_set_markup(v->mem_text, s->str);
    g_string_free(s, TRUE);
}

static void fill_timing(Vid *v)
{
    const char *const *lines;
    int n = msdosvid_tab_timing(v->tab, &lines), i;
    GString *s = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab) g_string_append_printf(s, "%-13.*s %s\n", (int)(tab - lines[i]), lines[i], tab + 1);
        else g_string_append_printf(s, "%s\n", lines[i]);
    }
    gtk_label_set_text(v->timing_text, s->len ? s->str : "The PC is off");
    g_string_free(s, TRUE);
    gtk_progress_bar_set_fraction(v->beam, msdosvid_tab_beam_permille(v->tab) / 1000.0);
}

/* ---- refresh ----------------------------------------------------------------- */

static void refresh_view(Vid *v, unsigned what)
{
    if (what & MSDOSVID_TAB_PICTURE) {
        int pw = 0, ph = 0;
        int vpw = gtk_widget_get_width(v->pic_scroll), vph = gtk_widget_get_height(v->pic_scroll);
        if (vpw > 4 && vph > 4) msdosvid_tab_set_viewport(v->tab, vpw - 2, vph - 2);
        if (msdosvid_tab_picture(v->tab, &pw, &ph)) {
            gtk_drawing_area_set_content_width(v->pic, pw);
            gtk_drawing_area_set_content_height(v->pic, ph);
        }
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    }
    if (what & MSDOSVID_TAB_TABLES) {
        int sec = gtk_notebook_get_current_page(v->sections);
        gtk_label_set_text(v->status, msdosvid_tab_status(v->tab));
        switch (sec) {
        case SEC_REGS: fill_regs(v); break;
        case SEC_PALETTE: fill_palette(v); break;
        case SEC_MEMORY: fill_memory(v); break;
        case SEC_TIMING: fill_timing(v); break;
        default: break;
        }
        update_pixel_section(v);
    }
}

void dbg_video_refresh(GtkWidget *page, gboolean force)
{
    Vid *v = g_object_get_data(G_OBJECT(page), "vid");
    unsigned what;
    if (!v) return;
    what = msdosvid_tab_refresh(v->tab, force);
    if (force) what |= MSDOSVID_TAB_PICTURE | MSDOSVID_TAB_TABLES;
    refresh_view(v, what);
}

/* ---- controls --------------------------------------------------------------- */

static void on_view(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    (void)o; (void)p;
    if (v->updating) return;
    msdosvid_tab_set_view(v->tab, (int)gtk_drop_down_get_selected(v->view));
    refresh_view(v, MSDOSVID_TAB_PICTURE | MSDOSVID_TAB_TABLES);
}

static void on_zoom(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    guint i = gtk_drop_down_get_selected(v->zoom);
    (void)o; (void)p;
    if (v->updating || i >= NZOOM) return;
    msdosvid_tab_set_zoom(v->tab, k_zoom_values[i]);
    refresh_view(v, MSDOSVID_TAB_PICTURE);
}

static void on_overlay(GtkCheckButton *b, gpointer ud)
{
    Vid *v = ud;
    unsigned m = 0;
    int n, i;
    const msdosvid_overlay_def *d = msdosvid_overlays(&n);
    (void)b;
    if (v->updating) return;
    for (i = 0; i < n; i++)
        if (gtk_check_button_get_active(v->ov[i])) m |= d[i].bit;
    msdosvid_tab_set_overlays(v->tab, m);
    refresh_view(v, MSDOSVID_TAB_PICTURE);
}

static void on_unpin(GtkButton *b, gpointer ud)
{
    Vid *v = ud;
    (void)b;
    msdosvid_tab_unpin(v->tab);
    gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    update_pixel_section(v);
}

static void on_break(GtkButton *b, gpointer ud)
{
    Vid *v = ud;
    int id = msdosvid_tab_break_on_pixel(v->tab);
    (void)b;
    gtk_label_set_text(v->note, id > 0 ? "Breakpoint added: break on writes to that address (see Breakpoints)"
                                       : "Pin a pixel that has a CPU address first");
}

static void on_section(GtkNotebook *nb, GtkWidget *page, guint n, gpointer ud)
{
    Vid *v = ud;
    (void)nb; (void)page; (void)n;
    refresh_view(v, MSDOSVID_TAB_TABLES);
}

static void on_destroy(GtkWidget *w, gpointer ud)
{
    Vid *v = ud;
    (void)w;
    msdosvid_tab_free(v->tab);
    g_free(v->regs_sig);
    g_free(v);
}

GtkWidget *dbg_video_new(msdossession *session)
{
    Vid *v = g_new0(Vid, 1);
    GtkWidget *root, *left, *bar, *flow, *right, *box, *btns, *b1, *b2, *scroll;
    GtkEventController *motion;
    GtkGesture *click;
    GtkTextBuffer *tb;
    const msdosvid_overlay_def *od;
    int n, i;
    const char *views[MSDOSVID_V_COUNT + 1];

    v->tab = msdosvid_tab_new(session);
    v->pal_hover = -1;

    root = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_set_data(G_OBJECT(root), "vid", v);
    v->page = root;
    g_signal_connect(root, "destroy", G_CALLBACK(on_destroy), v);

    /* ---- left: controls, status, picture */
    left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(left, 8);
    gtk_widget_set_margin_end(left, 4);
    gtk_widget_set_margin_top(left, 8);
    gtk_widget_set_margin_bottom(left, 8);

    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (i = 0; i < MSDOSVID_V_COUNT; i++) views[i] = msdosvid_view_name(i);
    views[MSDOSVID_V_COUNT] = NULL;
    v->view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(views));
    gtk_widget_set_tooltip_text(GTK_WIDGET(v->view),
        "Decoded: built from VRAM and the registers now.\nDisplay: the last finished frame (what the monitor shows).\n"
        "Beam: the live frame, dimmed below the beam.\nValues: false colour by the value in VRAM.\n"
        "Bit planes: the EGA/VGA planes, 0 1 over 2 3.\nCharacter set, Memory map: the font and VRAM.");
    v->zoom = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(k_zoom_labels));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("View"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(v->view));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Zoom"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(v->zoom));
    gtk_box_append(GTK_BOX(left), bar);

    flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 9);
    od = msdosvid_overlays(&n);
    for (i = 0; i < n; i++) {
        v->ov[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(od[i].label));
        gtk_widget_set_tooltip_text(GTK_WIDGET(v->ov[i]), od[i].tip);
        g_signal_connect(v->ov[i], "toggled", G_CALLBACK(on_overlay), v);
        gtk_flow_box_append(GTK_FLOW_BOX(flow), GTK_WIDGET(v->ov[i]));
    }
    gtk_box_append(GTK_BOX(left), flow);

    v->status = GTK_LABEL(mono_label());
    gtk_label_set_selectable(v->status, FALSE);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(v->status));

    v->pic = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_draw_func(v->pic, draw_pic, v, NULL);
    gtk_drawing_area_set_content_width(v->pic, 640);
    gtk_drawing_area_set_content_height(v->pic, 400);
    gtk_widget_set_halign(GTK_WIDGET(v->pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(v->pic), GTK_ALIGN_START);
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), v);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), v);
    gtk_widget_add_controller(GTK_WIDGET(v->pic), motion);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), v);
    gtk_widget_add_controller(GTK_WIDGET(v->pic), GTK_EVENT_CONTROLLER(click));
    scroll = gtk_scrolled_window_new();
    v->pic_scroll = scroll;
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(v->pic));
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(left), scroll);

    v->note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(v->note, 0);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(v->note));

    /* ---- right: sections */
    right = gtk_notebook_new();
    v->sections = GTK_NOTEBOOK(right);
    gtk_widget_set_size_request(right, 380, -1);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8); gtk_widget_set_margin_top(box, 8);
    v->pixel_text = GTK_LABEL(mono_label());
    gtk_label_set_wrap(v->pixel_text, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->pixel_text));
    v->pixel_note = GTK_LABEL(gtk_label_new(""));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->pixel_note));
    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b1 = gtk_button_new_with_label("Break on its writes");
    b2 = gtk_button_new_with_label("Unpin");
    g_signal_connect(b1, "clicked", G_CALLBACK(on_break), v);
    g_signal_connect(b2, "clicked", G_CALLBACK(on_unpin), v);
    gtk_box_append(GTK_BOX(btns), b1);
    gtk_box_append(GTK_BOX(btns), b2);
    gtk_box_append(GTK_BOX(box), btns);
    gtk_notebook_append_page(v->sections, box, gtk_label_new("Pixel"));

    v->regs_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(v->regs_text, FALSE);
    gtk_text_view_set_cursor_visible(v->regs_text, FALSE);
    gtk_text_view_set_monospace(v->regs_text, TRUE);
    tb = gtk_text_view_get_buffer(v->regs_text);
    gtk_text_buffer_create_tag(tb, "changed", "background", "#7a2a6a", "foreground", "#ffffff", NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(v->regs_text));
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new("Registers"));

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8); gtk_widget_set_margin_top(box, 8);
    v->pal = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_draw_func(v->pal, draw_pal, v, NULL);
    gtk_widget_set_halign(GTK_WIDGET(v->pal), GTK_ALIGN_START);
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(pal_motion), v);
    gtk_widget_add_controller(GTK_WIDGET(v->pal), motion);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->pal));
    v->pal_detail = GTK_LABEL(mono_label());
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->pal_detail));
    gtk_notebook_append_page(v->sections, box, gtk_label_new("Palette"));

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8); gtk_widget_set_margin_top(box, 8);
    v->mem_text = GTK_LABEL(mono_label());
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->mem_text));
    gtk_notebook_append_page(v->sections, box, gtk_label_new("Memory"));

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8); gtk_widget_set_margin_top(box, 8);
    v->beam = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_text(v->beam, "beam");
    gtk_progress_bar_set_show_text(v->beam, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->beam));
    v->timing_text = GTK_LABEL(mono_label());
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->timing_text));
    gtk_notebook_append_page(v->sections, box, gtk_label_new("Timing"));

    gtk_paned_set_start_child(GTK_PANED(root), left);
    gtk_paned_set_end_child(GTK_PANED(root), right);
    gtk_paned_set_resize_start_child(GTK_PANED(root), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(root), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(root), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(root), FALSE);

    /* start from the controller's state (the MSDOS_DEBUGGER_VIDEO_* variables) */
    v->updating = TRUE;
    gtk_drop_down_set_selected(v->view, (guint)msdosvid_tab_view(v->tab));
    for (i = 0; i < NZOOM; i++)
        if (k_zoom_values[i] == msdosvid_tab_zoom(v->tab)) gtk_drop_down_set_selected(v->zoom, (guint)i);
    for (i = 0; i < n; i++)
        gtk_check_button_set_active(v->ov[i], (msdosvid_tab_overlays(v->tab) & od[i].bit) != 0);
    gtk_notebook_set_current_page(v->sections, msdosvid_tab_env_section(v->tab));
    v->updating = FALSE;
    g_signal_connect(v->view, "notify::selected", G_CALLBACK(on_view), v);
    g_signal_connect(v->zoom, "notify::selected", G_CALLBACK(on_zoom), v);
    g_signal_connect(v->sections, "switch-page", G_CALLBACK(on_section), v);
    return root;
}
