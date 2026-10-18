/*
 * The Controllers panel: the machine's own keyboard on screen (the keys a
 * desktop keeps for itself -- SysRq, Pause, Scroll Lock -- included), the
 * game port's two joysticks, and the gamepads that drive them.
 *
 * A fixed-size floating window, hidden at start and hidden (not destroyed)
 * when closed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../window.h"

#include <math.h>
#include <string.h>

#define UNIT 34                 /* a key's width in pixels */
#define STICK 120               /* a joystick's square */
#define MAX_KEYS 128
#define MAX_PADS 8

static msdossession *g_session;
static GtkWindow *g_parent;
static GtkWidget *g_window;
static GtkWidget *g_kbd, *g_lights, *g_sticks, *g_pad_box, *g_stick_note;
static GtkWidget *g_keys[MAX_KEYS];
static int g_nkeys;
static int g_built_machine = -1;
static guint g_tick;
static unsigned g_pad_generation;

/* a joystick drawn and dragged here */
typedef struct {
    GtkWidget *area;
    GtkWidget *buttons[4];
    int x, y;                   /* -32768..32767 */
    unsigned held;
    gboolean dragging;
} StickUi;
static StickUi g_stick[MSDOS_JOYSTICKS];

/* Panel-only layout: key caps and controls without the theme's padding and
 * minimum sizes, so they are exactly the size given. The colours are the
 * app's (.msdos-held from main.c). */
static void install_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    if (done) return;
    done = TRUE;
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "button.msdos-key { padding: 0 1px; min-width: 0; min-height: 0;"
        " font-size: 8pt; font-weight: bold; }\n"
        "button.msdos-ctl { padding: 2px 3px; min-width: 0; min-height: 0; }\n");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ---- the keyboard ---------------------------------------------------------- */

static void on_key_pressed_g(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    (void)g; (void)n; (void)x; (void)y;
    msdossession_keyboard_press(g_session, GPOINTER_TO_INT(ud), 1);
}

static void on_key_released_g(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    (void)g; (void)n; (void)x; (void)y;
    msdossession_keyboard_press(g_session, GPOINTER_TO_INT(ud), 0);
}

static void build_keyboard(void)
{
    const msdos_kbd_key *keys;
    GtkWidget *child;
    int n = msdossession_keyboard_layout(g_session, &keys);

    while ((child = gtk_widget_get_first_child(g_kbd)))
        gtk_fixed_remove(GTK_FIXED(g_kbd), child);
    g_nkeys = 0;
    for (int i = 0; i < n && i < MAX_KEYS; i++) {
        GtkWidget *b = gtk_button_new();
        GtkWidget *label;
        GtkGesture *click = gtk_gesture_click_new();
        char text[64];
        if (keys[i].shifted)
            g_snprintf(text, sizeof text, "%s\n%s", keys[i].shifted, keys[i].label);
        else
            g_snprintf(text, sizeof text, "%s", keys[i].label);
        label = gtk_label_new(text);
        gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_button_set_child(GTK_BUTTON(b), label);
        gtk_widget_add_css_class(b, "msdos-key");
        gtk_widget_set_size_request(b, (int)(keys[i].w * UNIT) - 3, (int)(keys[i].h * UNIT) - 3);
        gtk_widget_set_focusable(b, FALSE);
        gtk_widget_set_focus_on_click(b, FALSE);
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
        g_signal_connect(click, "pressed", G_CALLBACK(on_key_pressed_g), GINT_TO_POINTER(i));
        g_signal_connect(click, "released", G_CALLBACK(on_key_released_g), GINT_TO_POINTER(i));
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
        gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(click));
        gtk_fixed_put(GTK_FIXED(g_kbd), b, keys[i].x * UNIT, keys[i].y * UNIT);
        g_keys[g_nkeys++] = b;
    }
    g_built_machine = msdossession_machine(g_session);
}

/* ---- the joysticks ------------------------------------------------------------ */

static void draw_stick(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer ud)
{
    StickUi *s = ud;
    const double cx = w / 2.0 + s->x / 32768.0 * (w / 2.0 - 12);
    const double cy = h / 2.0 + s->y / 32768.0 * (h / 2.0 - 12);
    GdkRGBA fg;
    (void)a;
    gtk_widget_get_color(GTK_WIDGET(a), &fg);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.15);
    cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.4);
    cairo_move_to(cr, w / 2.0, 0);
    cairo_line_to(cr, w / 2.0, h);
    cairo_move_to(cr, 0, h / 2.0);
    cairo_line_to(cr, w, h / 2.0);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, ((MSDOSSESSION_ACCENT_RGB >> 16) & 255) / 255.0,
                         ((MSDOSSESSION_ACCENT_RGB >> 8) & 255) / 255.0,
                         (MSDOSSESSION_ACCENT_RGB & 255) / 255.0);
    cairo_arc(cr, cx, cy, 10, 0, 2 * M_PI);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.8);
    cairo_stroke(cr);
}

static int stick_index(StickUi *s)
{
    return (int)(s - g_stick);
}

static void send_stick(StickUi *s)
{
    msdossession_joystick(g_session, stick_index(s), s->x, s->y, s->held);
}

static void drag_to(StickUi *s, double x, double y)
{
    const int w = gtk_widget_get_width(s->area), h = gtk_widget_get_height(s->area);
    s->x = (int)CLAMP((x - w / 2.0) / (w / 2.0) * 32767, -32768, 32767);
    s->y = (int)CLAMP((y - h / 2.0) / (h / 2.0) * 32767, -32768, 32767);
    send_stick(s);
    gtk_widget_queue_draw(s->area);
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer ud)
{
    StickUi *s = ud;
    (void)g;
    s->dragging = TRUE;
    drag_to(s, x, y);
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer ud)
{
    StickUi *s = ud;
    double x0, y0;
    gtk_gesture_drag_get_start_point(g, &x0, &y0);
    drag_to(s, x0 + dx, y0 + dy);
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer ud)
{
    StickUi *s = ud;
    (void)g; (void)dx; (void)dy;
    s->dragging = FALSE;
    s->x = s->y = 0;      /* a joystick springs back to the middle */
    send_stick(s);
    gtk_widget_queue_draw(s->area);
}

static void on_button_pressed(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    StickUi *s = g_object_get_data(G_OBJECT(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g))),
                                   "msdos-stick");
    (void)n; (void)x; (void)y;
    s->held |= 1u << GPOINTER_TO_INT(ud);
    send_stick(s);
}

static void on_button_released(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    StickUi *s = g_object_get_data(G_OBJECT(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g))),
                                   "msdos-stick");
    (void)n; (void)x; (void)y;
    s->held &= ~(1u << GPOINTER_TO_INT(ud));
    send_stick(s);
}

static GtkWidget *build_stick(int i)
{
    StickUi *s = &g_stick[i];
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *title = gtk_label_new(i ? "Joystick B" : "Joystick A");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkGesture *drag = gtk_gesture_drag_new();

    gtk_widget_add_css_class(title, "heading");
    gtk_box_append(GTK_BOX(box), title);
    s->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s->area, STICK, STICK);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s->area), draw_stick, s, NULL);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), s);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), s);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), s);
    gtk_widget_add_controller(s->area, GTK_EVENT_CONTROLLER(drag));
    gtk_widget_set_halign(s->area, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), s->area);
    for (int b = 0; b < 4; b++) {
        char label[8];
        GtkGesture *click = gtk_gesture_click_new();
        g_snprintf(label, sizeof label, "%d", b + 1);
        s->buttons[b] = gtk_button_new_with_label(label);
        gtk_widget_set_size_request(s->buttons[b], 28, 24);
        gtk_widget_add_css_class(s->buttons[b], "msdos-ctl");
        gtk_widget_set_focusable(s->buttons[b], FALSE);
        g_object_set_data(G_OBJECT(s->buttons[b]), "msdos-stick", s);
        g_signal_connect(click, "pressed", G_CALLBACK(on_button_pressed), GINT_TO_POINTER(b));
        g_signal_connect(click, "released", G_CALLBACK(on_button_released), GINT_TO_POINTER(b));
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
        gtk_widget_add_controller(s->buttons[b], GTK_EVENT_CONTROLLER(click));
        gtk_box_append(GTK_BOX(row), s->buttons[b]);
    }
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), row);
    return box;
}

/* ---- the gamepads ---------------------------------------------------------------- */

static void on_assign(GObject *dd, GParamSpec *ps, gpointer ud)
{
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    (void)ps;
    msdossession_gamepad_assign(g_session, GPOINTER_TO_INT(ud), sel == 0 ? -1 : (int)sel - 1);
}

static void refresh_pads(void)
{
    GtkWidget *child;
    int n = msdossession_gamepad_count(g_session);
    while ((child = gtk_widget_get_first_child(g_pad_box)))
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No gamepads. One plugged in drives Joystick A, the next B.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_box_append(GTK_BOX(g_pad_box), l);
        return;
    }
    for (int i = 0; i < n && i < MAX_PADS; i++) {
        static const char *const choices[] = { "Automatic", "Joystick A", "Joystick B", NULL };
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dd;
        char text[128];
        int a = msdossession_gamepad_assignment(g_session, i);
        msdossession_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 32);
        dd = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), a < 0 ? 0 : (guint)a + 1);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(on_assign), GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), dd);
        gtk_box_append(GTK_BOX(g_pad_box), row);
    }
}

/* ---- refreshing ------------------------------------------------------------------- */

static void rebuild(void)
{
    const gboolean fitted = msdossession_joystick_fitted(g_session);
    build_keyboard();
    gtk_widget_set_sensitive(g_sticks, fitted);
    gtk_label_set_text(GTK_LABEL(g_stick_note), fitted ? ""
                       : "No joystick on the game port (Preferences \xe2\x96\xb8 Ports)");
    gtk_widget_set_visible(g_stick_note, !fitted);
    refresh_pads();
}

static gboolean tick(gpointer ud)
{
    const unsigned lights = msdossession_lock_lights(g_session);
    char text[96];
    unsigned gen;
    (void)ud;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    for (int i = 0; i < g_nkeys; i++) {
        if (msdossession_keyboard_held(g_session, i))
            gtk_widget_add_css_class(g_keys[i], "msdos-held");
        else
            gtk_widget_remove_css_class(g_keys[i], "msdos-held");
    }
    g_snprintf(text, sizeof text, "%s   %s   %s", lights & 1 ? "\xe2\x97\x8f Num Lock" : "\xe2\x97\x8b Num Lock",
               lights & 2 ? "\xe2\x97\x8f Caps Lock" : "\xe2\x97\x8b Caps Lock",
               lights & 4 ? "\xe2\x97\x8f Scroll Lock" : "\xe2\x97\x8b Scroll Lock");
    gtk_label_set_text(GTK_LABEL(g_lights), text);
    for (int i = 0; i < MSDOS_JOYSTICKS; i++) {
        StickUi *s = &g_stick[i];
        int x, y;
        unsigned b;
        if (s->dragging)
            continue;
        msdossession_joystick_state(g_session, i, &x, &y, &b);
        if (x != s->x || y != s->y) {
            s->x = x;
            s->y = y;
            gtk_widget_queue_draw(s->area);
        }
        for (int k = 0; k < 4; k++) {
            if ((b | s->held) & (1u << k))
                gtk_widget_add_css_class(s->buttons[k], "msdos-held");
            else
                gtk_widget_remove_css_class(s->buttons[k], "msdos-held");
        }
    }
    gen = msdossession_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    }
    if (msdossession_machine(g_session) != g_built_machine)
        rebuild();
    return G_SOURCE_CONTINUE;
}

/* ---- the window --------------------------------------------------------------------- */

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval, guint keycode,
                               GdkModifierType state, gpointer ud)
{
    (void)c; (void)ud;
    return g_parent ? msdos_window_key(MSDOS_WINDOW(g_parent), keyval, keycode, state, TRUE) : FALSE;
}

static void on_key_released(GtkEventControllerKey *c, guint keyval, guint keycode,
                            GdkModifierType state, gpointer ud)
{
    (void)c; (void)ud;
    if (g_parent)
        msdos_window_key(MSDOS_WINDOW(g_parent), keyval, keycode, state, FALSE);
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)w; (void)d;
    gtk_widget_set_visible(g_window, FALSE);
    return TRUE;
}

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_tick)
        g_source_remove(g_tick);
    g_tick = 0;
    g_window = NULL;
    g_nkeys = 0;
    g_built_machine = -1;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *toolbar, *header, *padtitle;
    GtkEventController *keys;

    install_css();
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

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    g_kbd = gtk_fixed_new();
    gtk_widget_set_halign(g_kbd, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), g_kbd);
    g_lights = gtk_label_new("");
    gtk_widget_add_css_class(g_lights, "dim-label");
    gtk_box_append(GTK_BOX(root), g_lights);

    g_sticks = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
    gtk_widget_set_halign(g_sticks, GTK_ALIGN_CENTER);
    for (int i = 0; i < MSDOS_JOYSTICKS; i++)
        gtk_box_append(GTK_BOX(g_sticks), build_stick(i));
    gtk_box_append(GTK_BOX(root), g_sticks);
    g_stick_note = gtk_label_new("");
    gtk_widget_add_css_class(g_stick_note, "dim-label");
    gtk_box_append(GTK_BOX(root), g_stick_note);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(root), g_pad_box);

    header = adw_header_bar_new();
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    rebuild();
    g_tick = g_timeout_add(50, tick, NULL);
}

void msdos_controllers_toggle(GtkWindow *parent, msdossession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);
    if (gtk_widget_get_visible(g_window)) {
        gtk_widget_set_visible(g_window, FALSE);
        return;
    }
    if (msdossession_machine(g_session) != g_built_machine)
        rebuild();
    g_pad_generation = msdossession_gamepad_generation(g_session);
    refresh_pads();
    gtk_window_present(GTK_WINDOW(g_window));
}

gboolean msdos_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}

void msdos_controllers_refresh(void)
{
    if (!g_window)
        return;
    if (gtk_widget_get_visible(g_window))
        rebuild();
    else
        g_built_machine = -1;
}
