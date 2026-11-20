/*
 * The Controllers panel: the current machine's own keyboard on screen (IBM
 * Model F XT, PCjr Freeboard or Model M, drawn from the core's skins), the
 * game port's two Kraft joysticks, and the gamepads that drive them.
 *
 * Two cairo pictures (the keyboard, the sticks) in a fixed-size floating
 * window, hidden at start and hidden (not destroyed) when closed. Map,
 * Clear, Defaults and Show bindings remap the game port's 16 targets.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../help/help_window.h"
#include "../window.h"

#include <math.h>
#include <string.h>

#define KEY_SCALE 28.0          /* pixels per key unit */
#define KEY_MARGIN 10.0
#define CTL_SCALE 26.0          /* pixels per joystick layout unit */
#define CTL_MARGIN 10.0
#define MAX_PADS 8

#define HELD_R (((MSDOS_HELD_RGB >> 16) & 255) / 255.0)
#define HELD_G (((MSDOS_HELD_RGB >> 8) & 255) / 255.0)
#define HELD_B ((MSDOS_HELD_RGB & 255) / 255.0)

/* ---- state ----------------------------------------------------------------- */

static msdossession *g_session;
static GtkWindow *g_parent;
static GtkWidget *g_window;
static GtkWidget *g_kbd_area, *g_ctl_area;
static GtkWidget *g_map_button, *g_clear_button, *g_defaults_button, *g_show_check;
static GtkWidget *g_hint, *g_pad_box;
static GtkWidget *g_pad_drives[MAX_PADS];
static int g_pad_rows;
static guint g_capture_timer, g_info_timer;
static unsigned g_pad_generation = (unsigned)-1;
static gboolean g_syncing;
static gboolean g_show_bindings;

/* the keyboard picture */
static const msdos_skin_item *g_skin;
static int g_skin_count;
static int g_skin_machine = -1;
static float g_skin_w, g_skin_h;
static double g_kbd_offx, g_kbd_offy;
static int g_kbd_mouse = -1;            /* the layout key the mouse holds, or -1 */
static guint64 g_kbd_sig;

/* the sticks picture */
static const msdos_control *g_layout;
static int g_layout_count;
static int g_layout_mode = -1;
static float g_layout_w, g_layout_h;
static double g_ctl_offx;
static int g_drag_idx = -1;             /* the part the drag started on, or -1 */
static unsigned g_pointer_buttons[MSDOS_JOYSTICKS];
static guint64 g_ctl_sig;

/* Map: -2 idle, -1 armed and waiting for a target, >= 0 the picked target
 * waiting for a key or a pad button. */
static int g_map_state = -2;
static guint g_swallow_code;            /* the key Map took: its repeats and release too */

/* ---- drawing primitives --------------------------------------------------------- */

static void rgb(cairo_t *cr, unsigned hex)
{
    cairo_set_source_rgb(cr, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0,
                         (hex & 255) / 255.0);
}

static void rgba(cairo_t *cr, unsigned hex, double alpha)
{
    cairo_set_source_rgba(cr, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0,
                          (hex & 255) / 255.0, alpha);
}

static unsigned shade(unsigned hex, double factor)
{
    unsigned red = (hex >> 16) & 255, green = (hex >> 8) & 255, blue = hex & 255;
    red = (unsigned)fmin(255, red * factor);
    green = (unsigned)fmin(255, green * factor);
    blue = (unsigned)fmin(255, blue * factor);
    return (red << 16) | (green << 8) | blue;
}

/* A rounded rectangle with a radius per corner (top left, top right, bottom
 * right, bottom left). */
static void rrect4(cairo_t *cr, double x, double y, double w, double h,
                   double rtl, double rtr, double rbr, double rbl)
{
    cairo_new_sub_path(cr);
    cairo_move_to(cr, x + rtl, y);
    cairo_line_to(cr, x + w - rtr, y);
    if (rtr > 0) cairo_arc(cr, x + w - rtr, y + rtr, rtr, -G_PI / 2, 0);
    cairo_line_to(cr, x + w, y + h - rbr);
    if (rbr > 0) cairo_arc(cr, x + w - rbr, y + h - rbr, rbr, 0, G_PI / 2);
    cairo_line_to(cr, x + rbl, y + h);
    if (rbl > 0) cairo_arc(cr, x + rbl, y + h - rbl, rbl, G_PI / 2, G_PI);
    cairo_line_to(cr, x, y + rtl);
    if (rtl > 0) cairo_arc(cr, x + rtl, y + rtl, rtl, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double radius)
{
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    rrect4(cr, x, y, w, h, radius, radius, radius, radius);
}

static void disc(cairo_t *cr, double cx, double cy, double radius)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
}

/* Text through Pango: the ink box is anchored at (ax, ay) by fractions fx, fy
 * (0 left/top .. 1 right/bottom) and shrunk to maxw. */
static void text_put(cairo_t *cr, const char *text, double ax, double ay, double fx, double fy,
                     double size, double maxw, gboolean bold, gboolean italic)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new();
    PangoRectangle ink;
    double scale = 1;
    pango_font_description_set_family(font, "Sans");
    pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_style(font, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    if (maxw > 0 && ink.width > maxw)
        scale = maxw / ink.width;
    cairo_save(cr);
    cairo_translate(cr, ax, ay);
    cairo_scale(cr, scale, scale);
    cairo_move_to(cr, -ink.x - ink.width * fx, -ink.y - ink.height * fy);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    pango_font_description_free(font);
    g_object_unref(layout);
}

/* The held light: a 20% wash and a 2 px outline of MSDOS_HELD_RGB. */
static void held_light(cairo_t *cr, gboolean round, double x, double y, double w, double h,
                       double radius, double tl, double tr, double br, double bl)
{
    cairo_set_source_rgba(cr, HELD_R, HELD_G, HELD_B, 0.20);
    if (round)
        disc(cr, x + w / 2, y + h / 2, w / 2);
    else
        rrect4(cr, x, y, w, h, tl, tr, br, bl);
    cairo_fill(cr);
    rgb(cr, MSDOS_HELD_RGB);
    cairo_set_line_width(cr, 2);
    if (round)
        disc(cr, x + w / 2, y + h / 2, w / 2 - 1);
    else
        rrect4(cr, x + 1, y + 1, w - 2, h - 2, fmax(0, tl - 1), fmax(0, tr - 1),
               fmax(0, br - 1), fmax(0, bl - 1));
    cairo_stroke(cr);
    (void)radius;
}

static void redraw(void)
{
    if (g_kbd_area) gtk_widget_queue_draw(g_kbd_area);
    if (g_ctl_area) gtk_widget_queue_draw(g_ctl_area);
}

static void set_hint(const char *text)
{
    if (g_hint)
        gtk_label_set_text(GTK_LABEL(g_hint), text);
}

/* ---- the keyboard skin ------------------------------------------------------------ */

static void draw_cap(cairo_t *cr, const msdos_skin_item *item, double x, double y,
                     double w, double h, gboolean lit)
{
    const double unit = KEY_SCALE;
    const double inset = 0.04 * unit;
    const double kx = x + inset, kw = w - 2 * inset;
    double ky = y + inset, kh = h - 2 * inset;
    const gboolean chiclet = item->cap_class == MSDOS_CAP_CHICLET;
    const gboolean below = item->shape == MSDOS_SHAPE_JOIN_BELOW;
    const gboolean above = item->shape == MSDOS_SHAPE_JOIN_ABOVE;
    const double radius = (chiclet ? 0.22 : 0.12) * unit;
    const double top = above ? 0 : radius, bottom = below ? 0 : radius;
    const double seam = below ? inset : 0;
    const double kh_draw = kh + seam;
    double legend_y = ky + kh / 2;

    /* a joined Enter's two halves meet with no seam: the upper one reaches
     * down over the gap, the lower one up over it */
    if (above) {
        ky -= inset;
        kh += inset;
        legend_y = ky + kh / 2;
    }
    if (chiclet) {
        rgba(cr, 0x000000, 0.20);
        rrect4(cr, kx, ky + 1.5, kw, below ? kh_draw : kh, top, top, bottom, bottom);
        cairo_fill(cr);
        rgb(cr, shade(item->fill_rgb, 0.80));
        rrect4(cr, kx, ky, kw, below ? kh_draw : kh, top, top, bottom, bottom);
        cairo_fill(cr);
        rgb(cr, item->fill_rgb);
        rrect4(cr, kx, ky, kw, (below ? kh_draw : kh) - (below || above ? 0.0 : 0.10 * unit),
               top, top, below ? 0 : bottom, below ? 0 : bottom);
        cairo_fill(cr);
    } else {
        const double face_x = kx + 0.07 * unit, face_w = kw - 0.14 * unit, face_h = kh - 0.16 * unit;
        cairo_pattern_t *pat;
        rgba(cr, 0x000000, 0.30);
        rrect(cr, kx, ky + 1.5, kw, kh, radius);
        cairo_fill(cr);
        rgb(cr, shade(item->fill_rgb, 0.74));
        rrect(cr, kx, ky, kw, kh, radius);
        cairo_fill(cr);
        pat = cairo_pattern_create_linear(0, ky, 0, ky + face_h);
        cairo_pattern_add_color_stop_rgb(pat, 0, ((shade(item->fill_rgb, 1.08) >> 16) & 255) / 255.0,
                                         ((shade(item->fill_rgb, 1.08) >> 8) & 255) / 255.0,
                                         (shade(item->fill_rgb, 1.08) & 255) / 255.0);
        cairo_pattern_add_color_stop_rgb(pat, 1, ((item->fill_rgb >> 16) & 255) / 255.0,
                                         ((item->fill_rgb >> 8) & 255) / 255.0,
                                         (item->fill_rgb & 255) / 255.0);
        cairo_set_source(cr, pat);
        rrect(cr, face_x, ky, face_w, face_h, 0.10 * unit);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);
    }

    /* the legends (the lower half of a joined Enter carries none) */
    if (!above && item->label && *item->label) {
        const gboolean word = g_utf8_strlen(item->label, -1) > 1;
        rgb(cr, item->ink_rgb);
        if (chiclet && item->fn_legend) {
            if (item->shifted)
                text_put(cr, item->shifted, kx + 0.10 * unit, ky + kh * 0.28, 0, 0.5, 0.26 * unit,
                         kw * 0.45, FALSE, FALSE);
            rgb(cr, item->alt_rgb);
            text_put(cr, item->fn_legend, kx + kw - 0.08 * unit, ky + kh * 0.28, 1, 0.5, 0.27 * unit,
                     kw * 0.55, TRUE, FALSE);
            rgb(cr, item->ink_rgb);
            text_put(cr, item->label, kx + kw / 2, ky + kh * 0.72, 0.5, 0.5,
                     (word ? 0.30 : 0.38) * unit, kw - 4, FALSE, FALSE);
        } else if (item->shifted && !word) {
            text_put(cr, item->shifted, kx + kw / 2, ky + kh * 0.30, 0.5, 0.5, 0.30 * unit,
                     kw - 4, FALSE, FALSE);
            text_put(cr, item->label, kx + kw / 2, ky + kh * 0.67, 0.5, 0.5, 0.38 * unit,
                     kw - 4, FALSE, FALSE);
        } else if (item->fn_legend) {
            rgb(cr, item->alt_rgb);
            text_put(cr, item->fn_legend, kx + kw / 2, ky + kh * 0.28, 0.5, 0.5, 0.27 * unit,
                     kw - 4, TRUE, FALSE);
            rgb(cr, item->ink_rgb);
            text_put(cr, item->label, kx + kw / 2, ky + kh * 0.70, 0.5, 0.5, 0.32 * unit,
                     kw - 4, FALSE, FALSE);
        } else {
            text_put(cr, item->label, kx + kw / 2, legend_y - (chiclet ? 0 : 0.04 * unit), 0.5, 0.5,
                     (word ? 0.30 : 0.42) * unit, kw - 5, FALSE, FALSE);
        }
    }
    if (lit)
        held_light(cr, FALSE, kx, ky, kw, below ? kh_draw : kh, radius, top, top, bottom, bottom);
}

static void draw_skin_body(cairo_t *cr, const msdos_skin_item *item, double x, double y,
                           double w, double h)
{
    const char *name = item->label ? item->label : "";
    if (!strcmp(name, "Case")) {
        rgba(cr, 0x000000, 0.25);
        rrect(cr, x, y + 3, w, h, 0.4 * KEY_SCALE);
        cairo_fill(cr);
        rgb(cr, item->fill_rgb);
        rrect(cr, x, y, w, h, 0.4 * KEY_SCALE);
        cairo_fill(cr);
        rgba(cr, 0x000000, 0.18);
        cairo_set_line_width(cr, 1);
        rrect(cr, x + 0.5, y + 0.5, w - 1, h - 1, 0.4 * KEY_SCALE);
        cairo_stroke(cr);
    } else if (!strcmp(name, "FnBracket") || !strcmp(name, "AltBracket")) {
        rgb(cr, item->alt_rgb);
        cairo_set_line_width(cr, 2);
        rrect(cr, x, y, w, h, 0.2 * KEY_SCALE);
        cairo_stroke(cr);
    } else if (!strcmp(name, "Logo")) {
        rgb(cr, item->ink_rgb);
        if (item->shifted)
            text_put(cr, item->shifted, x + w / 2, y + h / 2, 0.5, 0.5, 0.62 * KEY_SCALE, w,
                     TRUE, TRUE);
    } else {                                    /* Slab, Badge, Plate, Well */
        rgb(cr, item->fill_rgb);
        rrect(cr, x, y, w, h, 0.2 * KEY_SCALE);
        cairo_fill_preserve(cr);
        if (!strcmp(name, "Well")) {
            rgba(cr, 0x000000, 0.20);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        } else {
            cairo_new_path(cr);
        }
        if (item->shifted) {
            rgb(cr, item->ink_rgb);
            text_put(cr, item->shifted, x + w / 2, y + h / 2, 0.5, 0.5, 0.42 * KEY_SCALE, w - 8,
                     TRUE, FALSE);
        }
    }
}

static void draw_led(cairo_t *cr, const msdos_skin_item *item, double x, double y, double w,
                     double h, gboolean lit)
{
    if (lit) {
        rgba(cr, item->alt_rgb, 0.35);
        rrect(cr, x - 3, y - 3, w + 6, h + 6, 4);
        cairo_fill(cr);
    }
    rgb(cr, lit ? item->alt_rgb : item->fill_rgb);
    rrect(cr, x, y, w, h, 2);
    cairo_fill(cr);
    rgb(cr, item->ink_rgb);
    text_put(cr, item->label, x + w / 2, y + h + 3, 0.5, 0, 0.26 * KEY_SCALE, 0, FALSE, FALSE);
}

static void kbd_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    const unsigned lights = msdossession_lock_lights(g_session);
    (void)area; (void)data;
    if (!g_skin)
        return;
    g_kbd_offx = (width - g_skin_w * KEY_SCALE) / 2;
    g_kbd_offy = (height - g_skin_h * KEY_SCALE) / 2;
    for (int i = 0; i < g_skin_count; i++) {
        const msdos_skin_item *item = &g_skin[i];
        const double x = g_kbd_offx + item->x * KEY_SCALE, y = g_kbd_offy + item->y * KEY_SCALE;
        const double w = item->w * KEY_SCALE, h = item->h * KEY_SCALE;
        if (item->kind == MSDOS_SKIN_BODY)
            draw_skin_body(cr, item, x, y, w, h);
        else if (item->kind == MSDOS_SKIN_LED)
            draw_led(cr, item, x, y, w, h, (lights >> item->key) & 1u);
        else
            draw_cap(cr, item, x, y, w, h, msdossession_keyboard_held(g_session, item->key) != 0);
    }
}

static void kbd_release_mouse(void)
{
    if (g_kbd_mouse >= 0 && g_session) {
        msdossession_keyboard_press(g_session, g_kbd_mouse, 0);
        g_kbd_mouse = -1;
        if (g_kbd_area) gtk_widget_queue_draw(g_kbd_area);
    }
}

static void on_kbd_pressed(GtkGestureClick *gesture, int presses, double x, double y, gpointer data)
{
    int idx;
    (void)gesture; (void)presses; (void)data;
    kbd_release_mouse();
    if (!g_skin || g_map_state != -2)       /* Map is for the sticks; the keyboard stays the PC's */
        return;
    idx = msdossession_keyboard_skin_hit(g_skin_machine, (float)((x - g_kbd_offx) / KEY_SCALE),
                                         (float)((y - g_kbd_offy) / KEY_SCALE));
    if (idx < 0)
        return;
    g_kbd_mouse = g_skin[idx].key;
    msdossession_keyboard_press(g_session, g_kbd_mouse, 1);
    gtk_widget_queue_draw(g_kbd_area);
}

static void on_kbd_released(GtkGestureClick *gesture, int presses, double x, double y, gpointer data)
{
    (void)gesture; (void)presses; (void)x; (void)y; (void)data;
    kbd_release_mouse();
}

static void on_kbd_cancelled(GtkGesture *gesture, GdkEventSequence *seq, gpointer data)
{
    (void)gesture; (void)seq; (void)data;
    kbd_release_mouse();
}

/* The extent of the biggest skin: the window is sized for it. */
static void largest_skin(float *width, float *height)
{
    *width = *height = 0;
    for (int machine = 0; machine < MSDOS_M_COUNT; machine++) {
        float w, h;
        if (msdossession_keyboard_skin_layout(machine, NULL, &w, &h)) {
            if (w > *width) *width = w;
            if (h > *height) *height = h;
        }
    }
}

static void relayout_keyboard(void)
{
    kbd_release_mouse();
    /* the session builds the layout that key indexes refer to on this call */
    msdossession_keyboard_layout(g_session, NULL);
    g_skin_machine = msdossession_machine(g_session);
    g_skin = msdossession_keyboard_skin_layout(g_skin_machine, &g_skin_count, &g_skin_w, &g_skin_h);
    g_kbd_sig = 0;
    if (g_kbd_area) gtk_widget_queue_draw(g_kbd_area);
}

static guint64 kbd_signature(void)
{
    guint64 sig = (guint64)(g_skin_machine + 1) * 31 + msdossession_lock_lights(g_session);
    for (int i = 0; i < g_skin_count; i++)
        if (g_skin[i].kind == MSDOS_SKIN_KEYCAP)
            sig = sig * 1099511628211ull +
                  (guint64)(msdossession_keyboard_held(g_session, g_skin[i].key) ? i + 7 : 3);
    return sig;
}

static gboolean kbd_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    guint64 sig;
    (void)widget; (void)clock; (void)data;
    if (msdossession_machine(g_session) != g_skin_machine)
        relayout_keyboard();
    sig = kbd_signature();
    if (sig != g_kbd_sig) {
        g_kbd_sig = sig;
        gtk_widget_queue_draw(g_kbd_area);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- the Kraft sticks ---------------------------------------------------------------- */

static double ctl_x(double unit) { return g_ctl_offx + unit * CTL_SCALE; }
static double ctl_y(double unit) { return CTL_MARGIN + unit * CTL_SCALE; }

static const msdos_control *part(int idx)
{
    return idx >= 0 && idx < g_layout_count ? &g_layout[idx] : NULL;
}

/* The knob's place now (merged from every source) in canvas pixels. */
static void knob_position(int stick, double *cx, double *cy)
{
    int xpos = 0, ypos = 0;
    unsigned buttons;
    *cx = *cy = 0;
    msdossession_joystick_state(g_session, stick, &xpos, &ypos, &buttons);
    for (int i = 0; i < g_layout_count; i++)
        if (g_layout[i].kind == MSDOS_CTL_STICK && g_layout[i].stick == stick) {
            *cx = ctl_x(g_layout[i].cenx + xpos / 32768.0 * g_layout[i].reach);
            *cy = ctl_y(g_layout[i].ceny + ypos / 32768.0 * g_layout[i].reach);
            return;
        }
}

/* What a target shows in Map mode and with Show bindings. */
static void binding_text(int target, char *key, int keysz, char *pad, int padsz)
{
    const msdos_binding bind = msdossession_binding_get(g_session, target);
    const char *name = bind.key ? msdossession_hid_name(bind.key) : NULL;
    key[0] = pad[0] = 0;
    g_strlcpy(key, name ? name : "\xe2\x80\x94", keysz);
    if (bind.button != MSDOS_PAD_BTN_NONE)
        g_strlcpy(pad, msdos_pad_button_name(bind.button), padsz);
}

static gboolean bindings_on(void)
{
    return g_map_state != -2 || g_show_bindings;
}

/* A binding label: the key over the gamepad input (compact: both on one line). */
static void bubble(cairo_t *cr, int target, double cx, double cy, double width, gboolean compact)
{
    char key[48], pad[48], line[100];
    const double height = compact ? 18 : 24;
    const gboolean picked = target == g_map_state;
    binding_text(target, key, sizeof key, pad, sizeof pad);
    rgba(cr, picked ? 0x401014 : 0x202428, 0.90);
    rrect(cr, cx - width / 2, cy - height / 2, width, height, 5);
    cairo_fill(cr);
    if (picked) {
        rgb(cr, MSDOS_HELD_RGB);
        cairo_set_line_width(cr, 2);
        rrect(cr, cx - width / 2 + 1, cy - height / 2 + 1, width - 2, height - 2, 4);
        cairo_stroke(cr);
    }
    rgb(cr, 0xFFFFFF);
    if (compact) {
        g_snprintf(line, sizeof line, "%s \xc2\xb7 %s", key, pad[0] ? pad : "\xe2\x80\x94");
        text_put(cr, line, cx, cy, 0.5, 0.5, 9, width - 6, TRUE, FALSE);
        return;
    }
    text_put(cr, key, cx, cy - 5.5, 0.5, 0.5, 9, width - 6, TRUE, FALSE);
    rgba(cr, 0xFFFFFF, 0.75);
    text_put(cr, pad[0] ? pad : "\xe2\x80\x94", cx, cy + 6, 0.5, 0.5, 8, width - 6, FALSE, FALSE);
}

static void draw_ctl_part(cairo_t *cr, const msdos_control *ctl, const GdkRGBA *fg,
                          gboolean held, double alpha)
{
    const double x = ctl_x(ctl->x), y = ctl_y(ctl->y);
    double w = ctl->w * CTL_SCALE, h = ctl->h * CTL_SCALE;
    const char *name = ctl->label ? ctl->label : "";

    (void)alpha;
    switch (ctl->kind) {
    case MSDOS_CTL_CAPTION:
        if (!strcmp(name, "KRAFT")) {
            rgb(cr, MSDOS_KRAFT_LOGO_RGB);
            text_put(cr, "KRAFT", x + w / 2, y + h / 2, 0.5, 0.5, h * 0.9, w - 6, TRUE, TRUE);
        } else {
            cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.9);
            text_put(cr, name, x + w / 2, y + h / 2, 0.5, 0.5, 13, w, TRUE, FALSE);
        }
        break;
    case MSDOS_CTL_BODY:
        if (!strcmp(name, "Base")) {
            rgba(cr, 0x000000, 0.25);
            rrect(cr, x, y + 3, w, h, 0.9 * CTL_SCALE);
            cairo_fill(cr);
            rgb(cr, MSDOS_KRAFT_BODY_RGB);
            rrect(cr, x, y, w, h, 0.9 * CTL_SCALE);
            cairo_fill_preserve(cr);
            rgb(cr, MSDOS_KRAFT_RIM_RGB);
            cairo_set_line_width(cr, 3);
            cairo_stroke(cr);
        } else if (!strcmp(name, "Plate")) {
            rgb(cr, MSDOS_KRAFT_PLATE_RGB);
            rrect(cr, x, y, w, h, 0.5 * CTL_SCALE);
            cairo_fill(cr);
            rgba(cr, 0x000000, 0.18);
            cairo_set_line_width(cr, 1);
            rrect(cr, x + 0.5, y + 0.5, w - 1, h - 1, 0.5 * CTL_SCALE);
            cairo_stroke(cr);
        } else if (!strcmp(name, "Well")) {
            rgba(cr, 0x000000, 0.25);
            disc(cr, x + w / 2, y + h / 2 + 1.5, w / 2);
            cairo_fill(cr);
            rgb(cr, MSDOS_KRAFT_WELL_RGB);
            disc(cr, x + w / 2, y + h / 2, w / 2);
            cairo_fill_preserve(cr);
            rgba(cr, 0x000000, 0.30);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
        } else if (!strcmp(name, "Label")) {
            rgb(cr, 0xF4F4EE);
            rrect(cr, x, y, w, h, 3);
            cairo_fill_preserve(cr);
            rgba(cr, 0x000000, 0.25);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
        break;
    case MSDOS_CTL_TRIM: {
        const gboolean vertical = h > w;
        rgba(cr, 0x000000, 0.35);
        rrect(cr, x, y, w, h, fmin(w, h) / 2);
        cairo_fill(cr);
        rgb(cr, MSDOS_KRAFT_TRIM_RGB);
        if (vertical)
            rrect(cr, x - 2, y + h / 2 - 5, w + 4, 10, 2);
        else
            rrect(cr, x + w / 2 - 5, y - 2, 10, h + 4, 2);
        cairo_fill(cr);
        break;
    }
    case MSDOS_CTL_STICK: {
        double kx, ky;
        knob_position(ctl->stick, &kx, &ky);
        /* the shaft from the pivot to the grip, then the ribbed grip */
        rgb(cr, MSDOS_KRAFT_SHAFT_RGB);
        cairo_set_line_width(cr, 7);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, ctl_x(ctl->cenx), ctl_y(ctl->ceny));
        cairo_line_to(cr, kx, ky);
        cairo_stroke(cr);
        rgba(cr, 0x000000, 0.25);
        disc(cr, kx + 1.5, ky + 3, 1.25 * CTL_SCALE);
        cairo_fill(cr);
        rgb(cr, MSDOS_KRAFT_GRIP_RGB);
        disc(cr, kx, ky, 1.25 * CTL_SCALE);
        cairo_fill_preserve(cr);
        rgba(cr, 0x000000, 0.40);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
        rgba(cr, 0x000000, 0.12);
        cairo_set_line_width(cr, 1);
        disc(cr, kx, ky, 1.0 * CTL_SCALE);
        cairo_stroke(cr);
        break;
    }
    case MSDOS_CTL_BUTTON:
    case MSDOS_CTL_PILL: {
        const gboolean round = ctl->reach > 0;
        double bx = x, by = y;
        if (round) {
            knob_position(ctl->stick, &bx, &by);
            bx -= ctl->reach * CTL_SCALE;
            by -= ctl->reach * CTL_SCALE;
            w = h = 2 * ctl->reach * CTL_SCALE;
        }
        rgb(cr, round ? MSDOS_KRAFT_CAP_RGB : MSDOS_KRAFT_SIDE_RGB);
        if (round)
            disc(cr, bx + w / 2, by + h / 2, w / 2);
        else
            rrect(cr, bx, by, w, h, ctl->kind == MSDOS_CTL_PILL ? h / 2 : 4);
        cairo_fill_preserve(cr);
        rgba(cr, 0x000000, 0.35);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        if (ctl->legend && !bindings_on()) {
            rgb(cr, round ? 0xF2F2EE : 0x303030);
            text_put(cr, ctl->legend, bx + w / 2, by + h / 2, 0.5, 0.5, fmin(w, h) * 0.6, w, TRUE, FALSE);
        }
        if (held)
            held_light(cr, round, bx, by, w, h, 4, ctl->kind == MSDOS_CTL_PILL ? h / 2 : 4,
                       ctl->kind == MSDOS_CTL_PILL ? h / 2 : 4,
                       ctl->kind == MSDOS_CTL_PILL ? h / 2 : 4,
                       ctl->kind == MSDOS_CTL_PILL ? h / 2 : 4);
        break;
    }
    }
}

/* The binding labels of one stick: its four directions round the plate, button
 * 1 on the knob, button 2 on its pad and buttons 3 and 4 on their pills. */
static void draw_stick_bindings(cairo_t *cr, int stick)
{
    for (int i = 0; i < g_layout_count; i++) {
        const msdos_control *ctl = &g_layout[i];
        if (ctl->stick != stick || !ctl->enabled)
            continue;
        if (ctl->kind == MSDOS_CTL_STICK) {
            const double cx = ctl_x(ctl->cenx), cy = ctl_y(ctl->ceny);
            bubble(cr, MSDOS_TARGET(stick, MSDOS_ACT_UP), cx, ctl_y(ctl->y) + 16, 56, FALSE);
            bubble(cr, MSDOS_TARGET(stick, MSDOS_ACT_DOWN), cx, ctl_y(ctl->y + ctl->h) - 16, 56, FALSE);
            bubble(cr, MSDOS_TARGET(stick, MSDOS_ACT_LEFT), ctl_x(ctl->x) + 29, cy, 54, FALSE);
            bubble(cr, MSDOS_TARGET(stick, MSDOS_ACT_RIGHT), ctl_x(ctl->x + ctl->w) - 29, cy, 54, FALSE);
        } else if (ctl->kind == MSDOS_CTL_BUTTON && ctl->reach > 0) {
            bubble(cr, ctl->target, ctl_x(ctl->cenx), ctl_y(ctl->ceny), 56, FALSE);
        } else if (ctl->kind == MSDOS_CTL_BUTTON) {
            bubble(cr, ctl->target, ctl_x(ctl->x + ctl->w / 2), ctl_y(ctl->y + ctl->h / 2), 50, FALSE);
        } else if (ctl->kind == MSDOS_CTL_PILL) {
            bubble(cr, ctl->target, ctl_x(ctl->x + ctl->w / 2), ctl_y(ctl->y + ctl->h / 2), 84, TRUE);
        }
    }
}

static void ctl_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    GdkRGBA fg;
    (void)data; (void)height;
    if (!g_layout)
        return;
    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    g_ctl_offx = (width - g_layout_w * CTL_SCALE) / 2;
    for (int i = 0; i < g_layout_count; i++) {
        const msdos_control *ctl = &g_layout[i];
        const gboolean held = ctl->target >= 0 && msdossession_target_held(g_session, ctl->target);
        const gboolean picked = g_map_state >= 0 && ctl->target == g_map_state;
        double alpha = ctl->enabled ? 1.0 : 0.35;
        if (g_map_state >= 0 && ctl->target >= 0 && !picked)
            alpha *= 0.55;
        cairo_push_group(cr);
        draw_ctl_part(cr, ctl, &fg, held || picked, alpha);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, alpha);
    }
    if (bindings_on())
        for (int stick = 0; stick < msdossession_joystick_count(g_session) && stick < MSDOS_JOYSTICKS; stick++)
            draw_stick_bindings(cr, stick);
}

static void relayout_controls(void)
{
    g_layout_mode = msdossession_game_port_mode(g_session);
    g_layout = msdossession_controls_layout(g_layout_mode, &g_layout_count, &g_layout_w, &g_layout_h);
    g_ctl_sig = 0;
    if (g_ctl_area) gtk_widget_queue_draw(g_ctl_area);
}

static guint64 ctl_signature(void)
{
    guint64 sig = (guint64)(g_layout_mode + 1) * 7 + (guint64)(g_map_state + 3) * 131;
    for (int stick = 0; stick < MSDOS_JOYSTICKS; stick++) {
        int xpos, ypos;
        unsigned buttons;
        msdossession_joystick_state(g_session, stick, &xpos, &ypos, &buttons);
        sig = sig * 1099511628211ull + (guint64)(xpos + 40000);
        sig = sig * 1099511628211ull + (guint64)(ypos + 40000);
        sig = sig * 1099511628211ull + buttons;
    }
    return sig;
}

static void release_pointer(void)
{
    for (int stick = 0; stick < MSDOS_JOYSTICKS; stick++) {
        g_pointer_buttons[stick] = 0;
        if (g_session)
            msdossession_pointer_set(g_session, stick, 0, 0, 0);
    }
    g_drag_idx = -1;
    if (g_ctl_area) gtk_widget_queue_draw(g_ctl_area);
}

static void steer(const msdos_control *ctl, double x, double y)
{
    int xpos, ypos;
    msdossession_stick_from_vector((float)((x - ctl_x(ctl->cenx)) / CTL_SCALE),
                                   (float)((y - ctl_y(ctl->ceny)) / CTL_SCALE), ctl->reach,
                                   &xpos, &ypos);
    msdossession_pointer_set(g_session, ctl->stick, xpos, ypos, g_pointer_buttons[ctl->stick]);
}

/* Map: the target under a click. On the plate, the direction the click lies
 * in from the knob's rest centre (-1 within the dead centre). */
static int map_target_at(const msdos_control *ctl, double x, double y)
{
    if (ctl->kind != MSDOS_CTL_STICK)
        return ctl->target;
    {
        const double dx = x - ctl_x(ctl->cenx), dy = y - ctl_y(ctl->ceny);
        if (hypot(dx, dy) < 0.5 * CTL_SCALE)
            return -1;
        if (fabs(dx) >= fabs(dy))
            return MSDOS_TARGET(ctl->stick, dx < 0 ? MSDOS_ACT_LEFT : MSDOS_ACT_RIGHT);
        return MSDOS_TARGET(ctl->stick, dy < 0 ? MSDOS_ACT_UP : MSDOS_ACT_DOWN);
    }
}

static void set_map_state(int state);

static void on_ctl_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data)
{
    const int idx = g_layout ? msdossession_controls_hit(g_layout_mode,
                                   (float)((x - g_ctl_offx) / CTL_SCALE),
                                   (float)((y - CTL_MARGIN) / CTL_SCALE)) : -1;
    const msdos_control *ctl = part(idx);
    (void)gesture; (void)data;
    if (!ctl)
        return;
    if (g_map_state != -2) {
        const int target = map_target_at(ctl, x, y);
        if (target >= 0)
            set_map_state(target);
        return;
    }
    release_pointer();
    g_drag_idx = idx;
    if (ctl->kind == MSDOS_CTL_STICK) {
        steer(ctl, x, y);
    } else {
        g_pointer_buttons[ctl->stick] |= 1u << ((ctl->target % MSDOS_ACT_COUNT) - MSDOS_ACT_B1);
        msdossession_pointer_set(g_session, ctl->stick, 0, 0, g_pointer_buttons[ctl->stick]);
    }
    gtk_widget_queue_draw(g_ctl_area);
}

static void on_ctl_drag_update(GtkGestureDrag *gesture, double dx, double dy, gpointer data)
{
    const msdos_control *ctl = part(g_drag_idx);
    double sx, sy;
    (void)data;
    if (!ctl || ctl->kind != MSDOS_CTL_STICK || g_map_state != -2)
        return;
    gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
    steer(ctl, sx + dx, sy + dy);
}

static void on_ctl_drag_end(GtkGestureDrag *gesture, double dx, double dy, gpointer data)
{
    (void)gesture; (void)dx; (void)dy; (void)data;
    release_pointer();          /* a joystick springs back to the middle */
}

static void on_ctl_cancelled(GtkGesture *gesture, GdkEventSequence *seq, gpointer data)
{
    (void)gesture; (void)seq; (void)data;
    release_pointer();
}

static gboolean ctl_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    guint64 sig;
    (void)widget; (void)clock; (void)data;
    if (msdossession_game_port_mode(g_session) != g_layout_mode) {
        release_pointer();
        relayout_controls();
        g_pad_generation = (unsigned)-1;    /* the pad rows follow the port */
    }
    sig = ctl_signature();
    if (sig != g_ctl_sig) {
        g_ctl_sig = sig;
        gtk_widget_queue_draw(g_ctl_area);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- Map ------------------------------------------------------------------------------ */

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    msdossession_gamepad_capture_cancel(g_session);
}

static gboolean capture_tick(gpointer data)
{
    int button;
    (void)data;
    if (g_map_state < 0) {
        g_capture_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (msdossession_gamepad_capture_poll(g_session, &button)) {
        char stolen[128], msg[256];
        stolen[0] = '\0';
        msdossession_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", msdos_pad_button_name(button), stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", msdos_pad_button_name(button));
        g_capture_timer = 0;
        set_map_state(-1);          /* back to armed: remapping several in a row is normal */
        set_hint(msg);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void idle_hint(void)
{
    set_hint(msdossession_game_port_mode(g_session) == MSDOS_PORT_NONE
             ? "No game port: the joysticks are off (Preferences \xe2\x96\xb8 Ports)"
             : "Drag the plate to steer, click the buttons. Clicking the keyboard types on the PC.");
}

static void set_map_state(int state)
{
    if (state != -2) {
        release_pointer();                  /* a part held into Map would never be released */
        kbd_release_mouse();
        msdossession_release_all(g_session);    /* Map swallows key-ups */
    }
    g_map_state = state;
    stop_capture();
    if (state == -2) {
        g_swallow_code = 0;
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "suggested-action");
        idle_hint();
    } else if (state == -1) {
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "suggested-action");
        set_hint("Click a control to remap (on the plate, a direction). Esc backs out");
    } else {
        char msg[160];
        g_snprintf(msg, sizeof msg, "Press a key or gamepad button for %s (Esc cancels)",
                   msdos_target_name(state));
        set_hint(msg);
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "suggested-action");
        msdossession_gamepad_capture_begin(g_session);
        g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    gtk_widget_set_sensitive(g_clear_button, g_map_state >= 0);
    redraw();
}

/* ---- the gamepads ------------------------------------------------------------------------ */

static void drives_text(int idx, char *text, int size)
{
    const int stick = msdossession_gamepad_effective_stick(g_session, idx);
    if (msdossession_game_port_mode(g_session) == MSDOS_PORT_NONE)
        g_snprintf(text, size, "no game port");
    else if (stick < 0)
        g_snprintf(text, size, "not driving");
    else
        g_snprintf(text, size, "drives Joystick %c", 'A' + stick);
}

static void refresh_pad_drives(void)
{
    for (int i = 0; i < g_pad_rows; i++) {
        char text[40];
        drives_text(i, text, sizeof text);
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_drives[i])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_drives[i]), text);
    }
}

static void on_assign_changed(GObject *obj, GParamSpec *spec, gpointer user_data)
{
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    (void)spec;
    if (g_syncing)
        return;
    msdossession_gamepad_assign(g_session, GPOINTER_TO_INT(user_data), sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_drives();
}

/* Rebuilt whenever a pad comes or goes, or the game port changes: one row per
 * pad, its name, the joystick it drives and the joystick it is set to. */
static void refresh_pads(void)
{
    static const char *const both[] = { "Automatic", "Joystick A", "Joystick B", NULL };
    static const char *const only_a[] = { "Automatic", "Joystick A", NULL };
    const int mode = msdossession_game_port_mode(g_session);
    const int count = msdossession_gamepad_count(g_session);
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_pad_rows = 0;
    if (count == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (int i = 0; i < count; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dropdown, *drives;
        char text[128], effective[40];
        const int assign = msdossession_gamepad_assignment(g_session, i);
        /* Joystick B is not offered when the port has one stick */
        const gboolean two = mode != MSDOS_PORT_ONE_4BTN;

        msdossession_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(name), 24);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 40);
        gtk_widget_set_tooltip_text(name, text);
        dropdown = gtk_drop_down_new_from_strings(two ? both : only_a);
        g_syncing = TRUE;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown),
                                   assign < 0 || assign > (two ? 1 : 0) ? 0 : (guint)assign + 1);
        g_syncing = FALSE;
        gtk_widget_set_focus_on_click(dropdown, FALSE);
        g_signal_connect(dropdown, "notify::selected", G_CALLBACK(on_assign_changed), GINT_TO_POINTER(i));
        drives_text(i, effective, sizeof effective);
        drives = gtk_label_new(effective);
        gtk_widget_add_css_class(drives, "dim-label");
        gtk_label_set_width_chars(GTK_LABEL(drives), 18);
        gtk_label_set_xalign(GTK_LABEL(drives), 0);
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), drives);
        gtk_box_append(GTK_BOX(row), dropdown);
        /* no game port: the rows stay, inert */
        gtk_widget_set_sensitive(row, mode != MSDOS_PORT_NONE);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        if (g_pad_rows < MAX_PADS)
            g_pad_drives[g_pad_rows++] = drives;
    }
}

static gboolean info_tick(gpointer data)
{
    unsigned gen;
    (void)data;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    gen = msdossession_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_drives();
    }
    return G_SOURCE_CONTINUE;
}

static void hide_window(void);

/* ---- the keys ---------------------------------------------------------------------------- */

static gboolean on_key(guint keyval, guint code, GdkModifierType state, gboolean down)
{
    uint32_t hid = code >= 8 ? msdossession_hid_from_evdev(code - 8) : 0;
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));

    /* an open drop-down (a gamepad's joystick) has the arrows and Return */
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    /* Ctrl+Shift+K toggles the panel from the panel too (the application's
     * accelerator belongs to the main window's actions) */
    if (down && (state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) &&
        (keyval == GDK_KEY_K || keyval == GDK_KEY_k)) {
        hide_window();
        return TRUE;
    }
    /* Ctrl chords (Ctrl+Shift+K among them) are never Map's */
    if (g_map_state != -2 && !(state & GDK_CONTROL_MASK)) {
        if (g_swallow_code && code == g_swallow_code) {
            if (!down) g_swallow_code = 0;
            return TRUE;
        }
        if (!down)
            return TRUE;
        if (keyval == GDK_KEY_Escape) {
            set_map_state(g_map_state >= 0 ? -1 : -2);
            g_swallow_code = code;          /* after: leaving Map clears it */
            return TRUE;
        }
        if (g_map_state < 0)
            return TRUE;                    /* armed, nothing picked: keys do nothing */
        if (!hid) {
            set_hint("The PC has no such key: pick another");
            g_swallow_code = code;
            return TRUE;
        }
        {
            char stolen[128], msg[256];
            const char *name = msdossession_hid_name(hid);
            stolen[0] = '\0';
            msdossession_binding_set_key(g_session, g_map_state, hid, stolen, sizeof stolen);
            g_swallow_code = code;
            set_map_state(-1);              /* back to armed */
            if (stolen[0])
                g_snprintf(msg, sizeof msg, "Bound %s (was %s)", name ? name : "key", stolen);
            else
                g_snprintf(msg, sizeof msg, "Bound %s", name ? name : "key");
            set_hint(msg);
            return TRUE;
        }
    }
    /* otherwise behave exactly like the main window */
    return g_parent ? msdos_window_key(MSDOS_WINDOW(g_parent), keyval, code, state, down) : FALSE;
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint code,
                               GdkModifierType state, gpointer data)
{
    (void)ctrl; (void)data;
    return on_key(keyval, code, state, TRUE);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval, guint code,
                            GdkModifierType state, gpointer data)
{
    (void)ctrl; (void)data;
    on_key(keyval, code, state, FALSE);
}

/* Anything held into a focus change must not stay down: the release goes to
 * another window. */
static void let_go(void)
{
    release_pointer();
    kbd_release_mouse();
    msdossession_release_all(g_session);
}

static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer data)
{
    (void)ctrl; (void)data;
    let_go();
    g_swallow_code = 0;
}

static void on_active_changed(GObject *obj, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    if (!gtk_window_is_active(GTK_WINDOW(obj)))
        let_go();
}

/* ---- the buttons ------------------------------------------------------------------------- */

static void on_map_clicked(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_clear_clicked(GtkButton *button, gpointer data)
{
    char msg[160];
    (void)button; (void)data;
    if (g_map_state < 0)
        return;
    msdossession_binding_clear(g_session, g_map_state);
    g_snprintf(msg, sizeof msg, "Cleared %s", msdos_target_name(g_map_state));
    set_hint(msg);
    redraw();
}

static void on_defaults_clicked(GtkButton *button, gpointer data)
{
    const int count = msdossession_gamepad_count(g_session);
    (void)button; (void)data;
    msdossession_bindings_reset(g_session);
    for (int i = 0; i < count; i++)
        msdossession_gamepad_assign(g_session, i, -1);
    refresh_pads();
    set_hint("Back to the defaults");
    redraw();
}

static void on_show_toggled(GtkCheckButton *button, gpointer data)
{
    (void)data;
    g_show_bindings = gtk_check_button_get_active(button);
    msdossession_set_controls_show_bindings(g_session, g_show_bindings);
    redraw();
}

static GtkWidget *plain_button(const char *label, GCallback callback)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_focus_on_click(button, FALSE);
    g_signal_connect(button, "clicked", callback, NULL);
    return button;
}

/* ---- the window --------------------------------------------------------------------------- */

static void hide_window(void)
{
    set_map_state(-2);
    let_go();
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *window, gpointer data)
{
    (void)window; (void)data;
    hide_window();
    return TRUE;
}

/* The info timer runs only while the window is on screen. */
static void on_window_map(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    release_pointer();
    if (!g_info_timer)
        g_info_timer = g_timeout_add(250, info_tick, NULL);
}

static void on_window_unmap(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    let_go();
    if (g_info_timer)
        g_source_remove(g_info_timer);
    g_info_timer = 0;
}

static void on_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_info_timer) g_source_remove(g_info_timer);
    g_capture_timer = g_info_timer = 0;
    g_window = NULL;
    g_kbd_area = g_ctl_area = NULL;
    g_pad_rows = 0;
    g_map_state = -2;
    g_skin_machine = g_layout_mode = -1;
    g_kbd_mouse = -1;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *toolbar, *header, *maprow, *padtitle, *padscroll;
    GtkEventController *keys, *focus;
    GtkGesture *click, *drag;
    float kbd_w, kbd_h;

    g_parent = parent;
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    /* a fixed size floats on a tiling compositor */
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);
    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 8);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    /* the keyboard: sized for the largest of the three skins */
    largest_skin(&kbd_w, &kbd_h);
    g_kbd_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_kbd_area), (int)(kbd_w * KEY_SCALE + 2 * KEY_MARGIN));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_kbd_area), (int)(kbd_h * KEY_SCALE + 2 * KEY_MARGIN));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_kbd_area), kbd_draw, NULL, NULL);
    gtk_widget_set_halign(g_kbd_area, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(g_kbd_area, FALSE);
    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_kbd_pressed), NULL);
    g_signal_connect(click, "released", G_CALLBACK(on_kbd_released), NULL);
    g_signal_connect(click, "cancel", G_CALLBACK(on_kbd_cancelled), NULL);
    gtk_widget_add_controller(g_kbd_area, GTK_EVENT_CONTROLLER(click));
    gtk_widget_add_tick_callback(g_kbd_area, kbd_tick, NULL, NULL);
    gtk_box_append(GTK_BOX(root), g_kbd_area);

    /* the two Kraft sticks */
    g_ctl_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_ctl_area),
                                       (int)(MSDOS_CONTROLS_W * CTL_SCALE + 2 * CTL_MARGIN));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_ctl_area),
                                        (int)(MSDOS_CONTROLS_H * CTL_SCALE + 2 * CTL_MARGIN));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_ctl_area), ctl_draw, NULL, NULL);
    gtk_widget_set_halign(g_ctl_area, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(g_ctl_area, FALSE);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_ctl_drag_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_ctl_drag_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_ctl_drag_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_ctl_cancelled), NULL);
    gtk_widget_add_controller(g_ctl_area, GTK_EVENT_CONTROLLER(drag));
    gtk_widget_add_tick_callback(g_ctl_area, ctl_tick, NULL, NULL);
    gtk_box_append(GTK_BOX(root), g_ctl_area);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = plain_button("Map", G_CALLBACK(on_map_clicked));
    gtk_widget_set_size_request(g_map_button, 72, -1);
    g_clear_button = plain_button("Clear", G_CALLBACK(on_clear_clicked));
    g_defaults_button = plain_button("Defaults", G_CALLBACK(on_defaults_clicked));
    g_show_check = gtk_check_button_new_with_label("Show bindings");
    g_show_bindings = msdossession_controls_show_bindings(g_session) != 0;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_check), g_show_bindings);
    gtk_widget_set_focus_on_click(g_show_check, FALSE);
    g_signal_connect(g_show_check, "toggled", G_CALLBACK(on_show_toggled), NULL);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), g_defaults_button);
    gtk_box_append(GTK_BOX(maprow), g_show_check);
    gtk_box_append(GTK_BOX(root), maprow);

    g_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_hint, "dim-label");
    gtk_widget_set_hexpand(g_hint, TRUE);
    /* a long hint must not widen the window */
    gtk_label_set_ellipsize(GTK_LABEL(g_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(g_hint), 24);
    gtk_label_set_max_width_chars(GTK_LABEL(g_hint), 90);
    gtk_label_set_xalign(GTK_LABEL(g_hint), 0);
    gtk_box_append(GTK_BOX(root), g_hint);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_label_set_xalign(GTK_LABEL(padtitle), 0);
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    /* two pad rows tall whatever is plugged in (more scroll), so a pad coming
     * or going never resizes the window */
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll), 84);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll), 84);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(root), padscroll);

    header = adw_header_bar_new();
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), msdos_help_button("controllers"));
    msdos_help_set_context(GTK_WINDOW(g_window), "controllers");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    relayout_keyboard();
    relayout_controls();
    set_map_state(-2);
}

static void show_window(void)
{
    if (msdossession_machine(g_session) != g_skin_machine)
        relayout_keyboard();
    if (msdossession_game_port_mode(g_session) != g_layout_mode)
        relayout_controls();
    g_pad_generation = msdossession_gamepad_generation(g_session);
    refresh_pads();
    if (g_map_state == -2)
        idle_hint();
    gtk_window_present(GTK_WINDOW(g_window));
}

void msdos_controllers_toggle(GtkWindow *parent, msdossession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);
    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        show_window();
}

gboolean msdos_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}

/* The machine or the game port changed, or Preferences reset the controls:
 * rebuild the keyboard and sticks and the pad rows. */
void msdos_controllers_refresh(void)
{
    if (!g_window)
        return;
    g_pad_generation = (unsigned)-1;
    if (gtk_widget_get_visible(g_window)) {
        if (msdossession_machine(g_session) != g_skin_machine)
            relayout_keyboard();
        if (msdossession_game_port_mode(g_session) != g_layout_mode) {
            release_pointer();
            relayout_controls();
        }
        redraw();
    } else {
        g_skin_machine = -1;
        g_layout_mode = -1;
    }
}
