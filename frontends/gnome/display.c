/*
 * display.c -- see display.h.
 *
 * Frames are pulled by serial, not pushed: copy_frame does nothing when the
 * PC has not drawn anything new, so a tick between changes costs a mutex
 * and no memcpy.
 *
 * The frame's size follows the video mode (CGA's 640x200, VGA's 720x400 for
 * text, SVGA up to 1024x768); its pixel aspect (x1000) is what the 4:3
 * setting asks for, so a 200-line mode is shown on a monitor's shape.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

#define FB_W MSDOSSESSION_FB_MAX_WIDTH
#define FB_H MSDOSSESSION_FB_MAX_HEIGHT

struct _MsdosDisplay {
    GtkWidget parent_instance;

    msdossession *session;
    guint32 *fb;            /* XRGB8888 straight from the session */
    GdkTexture *texture;
    int fw, fh, par;        /* the frame's size and pixel aspect */
    guint64 serial;
    guint64 frames;
    guint tick_id;
    gboolean smooth;
    gboolean integer;
    gboolean captured;
    double last_x, last_y;
    gboolean have_last;
    unsigned buttons;
};

G_DEFINE_FINAL_TYPE(MsdosDisplay, msdos_display, GTK_TYPE_WIDGET)

enum { SIGNAL_CAPTURE_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void rebuild_texture(MsdosDisplay *self)
{
    GBytes *bytes;
    int i, n = self->fw * self->fh;

    /* 0x00RRGGBB read as B,G,R,A on a little-endian host: only the alpha
     * needs setting; the texture takes its own copy */
    for (i = 0; i < n; i++)
        self->fb[i] |= 0xFF000000u;
    bytes = g_bytes_new(self->fb, (gsize)n * 4);
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_new(self->fw, self->fh, GDK_MEMORY_B8G8R8A8, bytes,
                                           (gsize)self->fw * 4);
    g_bytes_unref(bytes);
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    MsdosDisplay *self = MSDOS_DISPLAY(widget);
    int w, h, par;
    (void)clock; (void)user_data;

    if (msdossession_copy_frame(self->session, self->fb, &w, &h, &par, &self->serial) &&
        w > 0 && h > 0) {
        self->fw = w;
        self->fh = h;
        self->par = par > 0 ? par : 1000;
        rebuild_texture(self);
        self->frames++;
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

static void msdos_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    MsdosDisplay *self = MSDOS_DISPLAY(widget);
    int w = gtk_widget_get_width(widget);
    int h = gtk_widget_get_height(widget);
    double pw, ph, sw, sh;
    graphene_rect_t r;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){ 0, 0, 0, 1 },
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w <= 0 || h <= 0)
        return;

    pw = self->fw * self->par / 1000.0;
    ph = self->fh;
    if (self->integer && pw <= w && ph <= h) {
        int k = (int)MIN(w / pw, h / ph);
        sw = pw * k;
        sh = ph * k;
    } else if ((double)w / (double)h > pw / ph) {
        sh = h;
        sw = sh * pw / ph;
    } else {
        sw = w;
        sh = sw * ph / pw;
    }
    graphene_rect_init(&r, (float)((w - sw) / 2.0), (float)((h - sh) / 2.0), (float)sw, (float)sh);
    gtk_snapshot_append_scaled_texture(
        snapshot, self->texture,
        self->smooth ? GSK_SCALING_FILTER_LINEAR : GSK_SCALING_FILTER_NEAREST, &r);
}

/* ---- the mouse ----------------------------------------------------------------- */

void msdos_display_set_captured(MsdosDisplay *self, gboolean on)
{
    on = on && msdossession_mouse_fitted(self->session);
    if (on == self->captured)
        return;
    self->captured = on;
    self->have_last = FALSE;
    if (!on && self->buttons) {
        self->buttons = 0;
        msdossession_mouse_buttons(self->session, 0);
    }
    gtk_widget_set_cursor_from_name(GTK_WIDGET(self), on ? "none" : NULL);
    g_signal_emit(self, signals[SIGNAL_CAPTURE_CHANGED], 0, on);
}

gboolean msdos_display_captured(MsdosDisplay *self)
{
    return self->captured;
}

/* Movement while captured, as the host pointer's. GTK has no pointer lock,
 * so the mickeys stop where the host pointer meets the window's edge. */
static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer user_data)
{
    MsdosDisplay *self = user_data;
    (void)c;
    if (!self->captured) {
        self->have_last = FALSE;
        return;
    }
    if (self->have_last) {
        int dx = (int)(x - self->last_x), dy = (int)(y - self->last_y);
        if (dx || dy)
            msdossession_mouse_motion(self->session, dx, dy);
        if (!dx)
            x = self->last_x;
        if (!dy)
            y = self->last_y;
    }
    self->last_x = x;
    self->last_y = y;
    self->have_last = TRUE;
}

static void on_leave(GtkEventControllerMotion *c, gpointer user_data)
{
    (void)c;
    ((MsdosDisplay *)user_data)->have_last = FALSE;
}

static unsigned button_bit(guint b)
{
    return b == GDK_BUTTON_PRIMARY ? 1u : b == GDK_BUTTON_SECONDARY ? 2u : b == GDK_BUTTON_MIDDLE ? 4u : 0u;
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    MsdosDisplay *self = user_data;
    guint b = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    (void)n; (void)x; (void)y;
    gtk_widget_grab_focus(GTK_WIDGET(self));
    if (!self->captured) {
        if (b == GDK_BUTTON_PRIMARY)
            msdos_display_set_captured(self, TRUE);
        return;
    }
    if (b == GDK_BUTTON_MIDDLE) {
        msdos_display_set_captured(self, FALSE);
        return;
    }
    self->buttons |= button_bit(b);
    msdossession_mouse_buttons(self->session, self->buttons);
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    MsdosDisplay *self = user_data;
    guint b = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    (void)n; (void)x; (void)y;
    if (!self->captured || !(self->buttons & button_bit(b)))
        return;
    self->buttons &= ~button_bit(b);
    msdossession_mouse_buttons(self->session, self->buttons);
}

static gboolean on_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer user_data)
{
    MsdosDisplay *self = user_data;
    (void)c; (void)dx;
    if (!self->captured)
        return FALSE;
    msdossession_mouse_wheel(self->session, dy > 0 ? -1 : dy < 0 ? 1 : 0);
    return TRUE;
}

/* ---- the widget -------------------------------------------------------------- */

static void msdos_display_dispose(GObject *object)
{
    MsdosDisplay *self = MSDOS_DISPLAY(object);

    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    g_clear_pointer(&self->fb, g_free);

    G_OBJECT_CLASS(msdos_display_parent_class)->dispose(object);
}

static void msdos_display_class_init(MsdosDisplayClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = msdos_display_dispose;
    GTK_WIDGET_CLASS(klass)->snapshot = msdos_display_snapshot;
    signals[SIGNAL_CAPTURE_CHANGED] =
        g_signal_new("capture-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                     NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void msdos_display_init(MsdosDisplay *self)
{
    GtkEventController *motion, *scroll;
    GtkGesture *click;

    self->fb = g_new0(guint32, FB_W * FB_H);
    self->par = 1000;
    self->smooth = TRUE;
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);

    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), self);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), motion);
    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), self);
    g_signal_connect(click, "released", G_CALLBACK(on_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
    scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self), scroll);
}

GtkWidget *msdos_display_new(msdossession *session)
{
    MsdosDisplay *self = g_object_new(MSDOS_TYPE_DISPLAY, NULL);
    self->session = session;
    self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick, NULL, NULL);
    return GTK_WIDGET(self);
}

void msdos_display_set_smooth(MsdosDisplay *self, gboolean smooth)
{
    self->smooth = smooth;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void msdos_display_set_integer_scale(MsdosDisplay *self, gboolean on)
{
    self->integer = on;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

gboolean msdos_display_save_png(MsdosDisplay *self, const char *path)
{
    return self->texture && gdk_texture_save_to_png(self->texture, path);
}

guint64 msdos_display_frames(MsdosDisplay *self)
{
    return self->frames;
}
