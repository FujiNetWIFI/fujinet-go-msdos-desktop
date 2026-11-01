/*
 * display.c -- see display.h.
 *
 * Frames are pulled by serial, not pushed: copy_frame does nothing when the
 * PC has not drawn anything new, so a tick between changes costs a mutex
 * and no memcpy. The tick runs only while the window is mapped and not
 * suspended; a new frame is copied once, into a pooled buffer the texture
 * borrows (GDK_MEMORY_B8G8R8X8, so no alpha pass and no second copy).
 *
 * The frame's size follows the video mode (CGA's 640x200, VGA's 720x400 for
 * text, SVGA up to 1024x768); its pixel aspect (x1000) is what the 4:3
 * setting asks for, so a 200-line mode is shown on a monitor's shape.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

/* A smallest buffer worth having; it grows to the largest frame seen. */
#define POOL_MIN_PIXELS (1024 * 768)
#define POOL_KEEP 4

/* ---- frame buffer pool ----------------------------------------------------
 * The session copies the frame straight into a pooled buffer and the texture
 * borrows it: no allocation in steady state. The buffer comes back to the
 * pool when GSK drops the texture's bytes, possibly after the widget is gone,
 * so the pool is reference counted and each outstanding buffer holds a
 * reference. Buffers are sized for the largest frame in use (the frame
 * follows the video mode); a smaller one is dropped when the size grows. */
typedef struct {
    gatomicrefcount refs;
    GMutex lock;
    GPtrArray *spare;      /* idle FrameBuf */
    gsize pixels;          /* capacity new buffers get */
} FramePool;

typedef struct {
    guint32 *px;
    gsize pixels;
} FrameBuf;

typedef struct {
    FramePool *pool;
    FrameBuf buf;
} FrameLoan;

static void framebuf_free(gpointer p)
{
    FrameBuf *b = p;
    g_free(b->px);
    g_free(b);
}

static FramePool *pool_new(void)
{
    FramePool *p = g_new0(FramePool, 1);
    g_atomic_ref_count_init(&p->refs);
    g_mutex_init(&p->lock);
    p->spare = g_ptr_array_new_with_free_func(framebuf_free);
    p->pixels = POOL_MIN_PIXELS;
    return p;
}

static void pool_unref(FramePool *p)
{
    if (!g_atomic_ref_count_dec(&p->refs))
        return;
    g_ptr_array_free(p->spare, TRUE);
    g_mutex_clear(&p->lock);
    g_free(p);
}

/* A buffer of at least `want` pixels (the pool's size grows to it). */
static FrameBuf pool_take(FramePool *p, gsize want)
{
    FrameBuf out = { NULL, 0 };

    g_mutex_lock(&p->lock);
    if (want > p->pixels) {
        p->pixels = want;
        g_ptr_array_set_size(p->spare, 0);   /* all too small now */
    }
    if (p->spare->len > 0) {
        FrameBuf *b = g_ptr_array_steal_index(p->spare, p->spare->len - 1);
        out = *b;
        g_free(b);
    }
    want = p->pixels;
    g_mutex_unlock(&p->lock);
    if (!out.px) {
        out.px = g_malloc(want * sizeof(guint32));
        out.pixels = want;
    }
    return out;
}

static void pool_give(FramePool *p, FrameBuf buf)
{
    gboolean kept = FALSE;

    g_mutex_lock(&p->lock);
    if (buf.pixels >= p->pixels && p->spare->len < POOL_KEEP) {
        FrameBuf *b = g_new(FrameBuf, 1);
        *b = buf;
        g_ptr_array_add(p->spare, b);
        kept = TRUE;
    }
    g_mutex_unlock(&p->lock);
    if (!kept)
        g_free(buf.px);
}

static void loan_release(gpointer data)
{
    FrameLoan *loan = data;

    pool_give(loan->pool, loan->buf);
    pool_unref(loan->pool);
    g_free(loan);
}

struct _MsdosDisplay {
    GtkWidget parent_instance;

    msdossession *session;
    FramePool *pool;
    GdkTexture *texture;
    int fw, fh, par;        /* the frame's size and pixel aspect */
    uint64_t serial;
    guint64 frames;
    guint tick_id;
    GtkWindow *toplevel;    /* while mapped, for notify::suspended */
    gulong suspended_id;
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

/* Wrap buf (the XRGB frame, 0x00RRGGBB = B,G,R,X on a little-endian host; the
 * X byte is ignored, so no alpha pass) in the texture. Only the frame's own
 * w * h pixels are uploaded. The previous texture goes in as the update
 * source so GSK can reuse its GPU texture, but only when the size is
 * unchanged. Takes ownership of buf. */
static void publish_texture(MsdosDisplay *self, FrameBuf buf, int w, int h)
{
    GdkMemoryTextureBuilder *b = gdk_memory_texture_builder_new();
    FrameLoan *loan = g_new(FrameLoan, 1);
    GBytes *bytes;
    gboolean same = self->texture && gdk_texture_get_width(self->texture) == w &&
                    gdk_texture_get_height(self->texture) == h;

    g_atomic_ref_count_inc(&self->pool->refs);
    loan->pool = self->pool;
    loan->buf = buf;
    bytes = g_bytes_new_with_free_func(buf.px, (gsize)w * h * sizeof(guint32), loan_release, loan);
    gdk_memory_texture_builder_set_bytes(b, bytes);
    gdk_memory_texture_builder_set_stride(b, (gsize)w * 4);
    gdk_memory_texture_builder_set_width(b, w);
    gdk_memory_texture_builder_set_height(b, h);
    gdk_memory_texture_builder_set_format(b, GDK_MEMORY_B8G8R8X8);
    if (same) {
        cairo_rectangle_int_t all = { 0, 0, w, h };
        cairo_region_t *region = cairo_region_create_rectangle(&all);

        gdk_memory_texture_builder_set_update_texture(b, self->texture);
        gdk_memory_texture_builder_set_update_region(b, region);
        cairo_region_destroy(region);
    }
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_builder_build(b);
    g_bytes_unref(bytes);
    g_object_unref(b);
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    MsdosDisplay *self = MSDOS_DISPLAY(widget);
    FrameBuf buf = pool_take(self->pool, 0);
    int w = 0, h = 0, par = 0, got;
    (void)clock; (void)user_data;

    got = msdossession_copy_frame_n(self->session, buf.px, buf.pixels, &w, &h, &par, &self->serial);
    if (!got && w > 0 && h > 0 && (gsize)w * h > buf.pixels) {
        /* a bigger video mode: a bigger buffer, then ask again */
        pool_give(self->pool, buf);
        buf = pool_take(self->pool, (gsize)w * h);
        got = msdossession_copy_frame_n(self->session, buf.px, buf.pixels, &w, &h, &par,
                                        &self->serial);
    }
    if (got && w > 0 && h > 0) {
        self->fw = w;
        self->fh = h;
        self->par = par > 0 ? par : 1000;
        publish_texture(self, buf, w, h);
        self->frames++;
        gtk_widget_queue_draw(widget);
    } else {
        pool_give(self->pool, buf);
    }
    return G_SOURCE_CONTINUE;
}

/* The tick runs only while somebody can see the result. The PC does not care:
 * the session keeps its own clock. The first tick after the window comes
 * back copies whatever frame is newest. */
static void update_tick(MsdosDisplay *self)
{
    gboolean want = gtk_widget_get_mapped(GTK_WIDGET(self)) &&
                    !(self->toplevel && gtk_window_is_suspended(self->toplevel));

    if (want && !self->tick_id)
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick, NULL, NULL);
    else if (!want && self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
}

static void on_suspended(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj; (void)pspec;
    update_tick(MSDOS_DISPLAY(user_data));
}

static void msdos_display_map(GtkWidget *widget)
{
    MsdosDisplay *self = MSDOS_DISPLAY(widget);
    GtkRoot *root;

    GTK_WIDGET_CLASS(msdos_display_parent_class)->map(widget);
    root = gtk_widget_get_root(widget);
    if (GTK_IS_WINDOW(root)) {
        self->toplevel = GTK_WINDOW(root);
        self->suspended_id = g_signal_connect(self->toplevel, "notify::suspended",
                                              G_CALLBACK(on_suspended), self);
    }
    update_tick(self);
}

static void msdos_display_unmap(GtkWidget *widget)
{
    MsdosDisplay *self = MSDOS_DISPLAY(widget);

    if (self->toplevel && self->suspended_id)
        g_signal_handler_disconnect(self->toplevel, self->suspended_id);
    self->suspended_id = 0;
    self->toplevel = NULL;
    GTK_WIDGET_CLASS(msdos_display_parent_class)->unmap(widget);
    update_tick(self);
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
    if (self->pool) {
        pool_unref(self->pool);
        self->pool = NULL;
    }

    G_OBJECT_CLASS(msdos_display_parent_class)->dispose(object);
}

static void msdos_display_class_init(MsdosDisplayClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = msdos_display_dispose;
    GTK_WIDGET_CLASS(klass)->snapshot = msdos_display_snapshot;
    GTK_WIDGET_CLASS(klass)->map = msdos_display_map;
    GTK_WIDGET_CLASS(klass)->unmap = msdos_display_unmap;
    signals[SIGNAL_CAPTURE_CHANGED] =
        g_signal_new("capture-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                     NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void msdos_display_init(MsdosDisplay *self)
{
    GtkEventController *motion, *scroll;
    GtkGesture *click;

    self->pool = pool_new();
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
