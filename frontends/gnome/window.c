/*
 * window.c -- the main window: the PC's picture, the Ctrl+Alt+Del and Reset
 * buttons, the header-bar menu, the keyboard and mouse, the drives, the
 * FujiNet status, and the timer that brings the other threads' news to the
 * UI. With no BIOS imported yet it shows how to import one instead.
 *
 * Keys go to the PC pressed and released, by physical key (GDK's hardware
 * keycode is evdev + 8): the PC keyboard is positional, F1-F12 and the
 * Control and Alt combinations included. The app's own shortcuts are
 * Ctrl+Shift ones, plus Ctrl+End (let the mouse go) and Ctrl+Alt+PgDn
 * (fullscreen), as in 86Box.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include "controllers/controllers_window.h"
#include "debugger/dbg_window.h"
#include "display.h"
#include "fujilog.h"
#include "help/help_window.h"
#include "prefs.h"

#include <string.h>

struct _MsdosWindow {
    AdwApplicationWindow parent_instance;

    msdossession *session;
    GtkWidget *stack;
    GtkWidget *display;
    GtkWidget *toast_overlay;
    GtkWidget *status;
    GtkWidget *status_dot;
    GtkWidget *lights;
    GtkWidget *menu_button;
    GtkWidget *cad_button, *reset_button;
    char menu_sig[4096];        /* what the media menus last showed */
    guint status_id;
    guint fast_id;
    unsigned pad_generation;
    unsigned fujinet_generation;
    gboolean fullscreen;
    /* MSDOS_SCREENSHOT / MSDOS_WINDOW_SHOTS / MSDOS_AUTOQUIT_FRAMES */
    char *shot_path, *window_shots;
    guint64 shot_frames, window_shot_frames, quit_frames;
};

G_DEFINE_FINAL_TYPE(MsdosWindow, msdos_window, ADW_TYPE_APPLICATION_WINDOW)

static void rebuild_menu(MsdosWindow *self, gboolean force);
static void update_status(MsdosWindow *self);
static void save_window_shots(const char *dir);

void msdos_window_toast(MsdosWindow *self, const char *text)
{
    AdwToast *t = adw_toast_new(text);
    adw_toast_set_timeout(t, strlen(text) > 80 ? 6 : 3);
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay), t);
}

/* ---- the accent ---------------------------------------------------------------- */

static GtkCssProvider *s_accent_css;

void msdos_install_accent_css(void)
{
    char buf[1024];
    if (s_accent_css)
        return;
    s_accent_css = gtk_css_provider_new();
    /* the badge's silver: light enough that its text stays the badge's ink */
    g_snprintf(buf, sizeof buf,
        ".msdos-accent, .msdos-held { background: #%06x; color: #1e1e1e; }\n"
        ".msdos-accent:hover { background: shade(#%06x, 1.08); }\n"
        ".msdos-dot { border-radius: 6px; min-width: 12px; min-height: 12px; }\n"
        ".msdos-dot-on { background: #%06x; box-shadow: 0 0 0 1px alpha(currentColor, 0.4); }\n"
        ".msdos-dot-off { background: alpha(currentColor, 0.25); }\n"
        ".msdos-light { font-family: monospace; font-size: smaller; padding: 0 3px; "
        "border-radius: 3px; }\n"
        ".msdos-light-on { background: #%06x; color: #1e1e1e; }\n",
        MSDOSSESSION_ACCENT_RGB, MSDOSSESSION_ACCENT_RGB, MSDOSSESSION_ACCENT_RGB,
        MSDOSSESSION_ACCENT_RGB);
    gtk_css_provider_load_from_string(s_accent_css, buf);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(s_accent_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* ---- small helpers ---------------------------------------------------------------- */

static void show_message(GtkWidget *over, const char *heading, const char *body)
{
    AdwDialog *dlg = adw_alert_dialog_new(heading, body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "ok", "OK");
    adw_dialog_present(dlg, over);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* GMenu labels take _ as a mnemonic: a file name keeps its own. */
static char *menu_escape(const char *s)
{
    GString *g = g_string_new(NULL);
    for (; *s; s++) {
        if (*s == '_')
            g_string_append_c(g, '_');
        g_string_append_c(g, *s);
    }
    return g_string_free(g, FALSE);
}

static GtkFileDialog *file_dialog(const char *title, const char *filter_name,
                                  const char *const *exts, const char *folder)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *these = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();

    gtk_file_filter_set_name(these, filter_name);
    for (int i = 0; exts && exts[i]; i++)
        gtk_file_filter_add_suffix(these, exts[i]);   /* any case */
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, these);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    if (folder && *folder && g_file_test(folder, G_FILE_TEST_IS_DIR)) {
        g_autoptr(GFile) dir = g_file_new_for_path(folder);
        gtk_file_dialog_set_initial_folder(dlg, dir);
    }
    g_object_unref(these);
    g_object_unref(all);
    g_object_unref(filters);
    return dlg;
}

static const char *const FLOPPY_EXTS[] = { "img", "ima", "vfd", "flp", "dsk", "86f", "imd", "td0",
                                           "fdi", NULL };
static const char *const HD_EXTS[] = { "vhd", "img", "hdi", "hdx", NULL };
static const char *const ANY_EXTS[] = { "img", "ima", "vfd", "flp", "dsk", "86f", "imd", "td0",
                                        "fdi", "vhd", "hdi", "hdx", "zip", "rom", "bin", "exe",
                                        "com", NULL };
static const char *const ROM_EXTS[] = { "zip", "rom", "bin", "awd", "bio", "vbi", "raw", NULL };

/* Where to look for media: the folder the last file came from. */
static char *media_folder(MsdosWindow *self)
{
    const char *last = msdossession_get_str(self->session, "last_dir", "");
    if (last && *last && g_file_test(last, G_FILE_TEST_IS_DIR))
        return g_strdup(last);
    return g_strdup(msdossession_media_path(self->session));
}

static void remember_folder(MsdosWindow *self, const char *path)
{
    g_autofree char *dir = g_path_get_dirname(path);
    msdossession_set_str(self->session, "last_dir", dir);
}

static void open_folder(MsdosWindow *self, const char *path)
{
    g_autoptr(GFile) dir = g_file_new_for_path(path);
    GtkFileLauncher *l = gtk_file_launcher_new(dir);
    g_mkdir_with_parents(path, 0755);
    gtk_file_launcher_launch(l, GTK_WINDOW(self), NULL, NULL, NULL);
    g_object_unref(l);
}

/* ---- the keyboard ------------------------------------------------------------------ */

static void set_captured(MsdosWindow *self, gboolean on)
{
    msdos_display_set_captured(MSDOS_DISPLAY(self->display), on);
}

gboolean msdos_window_key(MsdosWindow *self, guint keyval, guint keycode,
                          GdkModifierType state, gboolean down)
{
    const gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    const gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    const gboolean alt = (state & GDK_ALT_MASK) != 0;
    uint32_t hid = keycode >= 8 ? msdossession_hid_from_evdev(keycode - 8) : 0;

    if (down) {
        /* Ctrl+End lets the mouse go (86Box's own key for it) */
        if (ctrl && !alt && keyval == GDK_KEY_End
            && msdos_display_captured(MSDOS_DISPLAY(self->display))) {
            set_captured(self, FALSE);
            return TRUE;
        }
        if (ctrl && alt && (keyval == GDK_KEY_Page_Down || keyval == GDK_KEY_KP_Page_Down)) {
            gtk_widget_activate_action(GTK_WIDGET(self), "win.fullscreen", NULL);
            return TRUE;
        }
        /* Ctrl+Shift+letter is the app's (and the desktop's Super) */
        if ((ctrl && shift && keyval != GDK_KEY_Shift_L && keyval != GDK_KEY_Shift_R
             && keyval != GDK_KEY_Control_L && keyval != GDK_KEY_Control_R
             && gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval)) >= 'a'
             && gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval)) <= 'z')
            || (state & (GDK_SUPER_MASK | GDK_META_MASK)))
            return FALSE;
    }
    if (!hid)
        return FALSE;
    return msdossession_key(self->session, hid, down) != 0;
}

/* An open dialog (Preferences, About, an alert) or menu has the keyboard:
 * Escape closes it and the arrows and Return move through it, instead of
 * going to the PC. */
static gboolean ui_has_keyboard(MsdosWindow *self)
{
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self));
    if (adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self)))
        return TRUE;
    if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(self->stack)), "pc") != 0)
        return TRUE;
    return focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER) != NULL;
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)ctrl;
    if (ui_has_keyboard(self)) {
        /* F1 is the PC's, but not while Preferences or a dialog is open */
        if (keyval == GDK_KEY_F1 && !(state & GDK_MODIFIER_MASK & ~GDK_LOCK_MASK)) {
            msdos_help_for(GTK_WINDOW(self));
            return TRUE;
        }
        return FALSE;
    }
    return msdos_window_key(self, keyval, keycode, state, TRUE);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                            GdkModifierType state, gpointer user_data)
{
    (void)ctrl;
    msdos_window_key(user_data, keyval, keycode, state, FALSE);
}

/* Focus gone: nothing stays held, and the mouse is the desktop's again. */
static void on_active_changed(GObject *win, GParamSpec *ps, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)ps;
    if (!gtk_window_is_active(GTK_WINDOW(win))) {
        if (!msdos_controllers_visible())
            msdossession_release_keys(self->session);
        set_captured(self, FALSE);
    }
}

static void on_capture_changed(MsdosDisplay *d, gboolean on, gpointer user_data)
{
    (void)d;
    if (on)
        msdos_window_toast(user_data, "Mouse captured: Ctrl+End or the middle button lets it go");
}

/* ---- starting ------------------------------------------------------------------------ */

static void show_page(MsdosWindow *self)
{
    const gboolean bios = msdossession_has_bios(self->session);
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), bios ? "pc" : "nobios");
    gtk_widget_set_sensitive(self->cad_button, msdossession_is_running(self->session));
    gtk_widget_set_sensitive(self->reset_button, msdossession_is_running(self->session));
}

void msdos_window_start(MsdosWindow *self)
{
    msdossession_start_opts opts;

    show_page(self);
    if (msdossession_is_running(self->session) || !msdossession_has_bios(self->session))
        return;
    msdossession_default_opts(self->session, &opts);
    if (msdossession_start(self->session, &opts) != 0) {
        show_message(GTK_WIDGET(self), "Cannot Start the PC", msdossession_last_error(self->session));
        update_status(self);
        return;
    }
    self->menu_sig[0] = '\0';
    rebuild_menu(self, TRUE);
    update_status(self);
    show_page(self);
}

/* ---- status, and news from the other threads ------------------------------------------- */

static void set_dot(GtkWidget *dot, gboolean on)
{
    gtk_widget_remove_css_class(dot, on ? "msdos-dot-off" : "msdos-dot-on");
    gtk_widget_add_css_class(dot, on ? "msdos-dot-on" : "msdos-dot-off");
}

static void update_status(MsdosWindow *self)
{
    char text[512];
    if (!msdossession_is_running(self->session)) {
        gtk_label_set_text(GTK_LABEL(self->status),
                           msdossession_has_bios(self->session) ? "Off" : "No BIOS");
        set_dot(self->status_dot, FALSE);
        return;
    }
    msdossession_status(self->session, text, sizeof text);
    gtk_label_set_text(GTK_LABEL(self->status), text);
    set_dot(self->status_dot, msdossession_fujinet_link(self->session));
}

/* The drives' lights. */
static void update_lights(MsdosWindow *self)
{
    static const char *const names[4] = { "A:", "B:", "C:", "D:" };
    GtkWidget *child = gtk_widget_get_first_child(self->lights);
    for (int i = 0; i < 4 && child; i++, child = gtk_widget_get_next_sibling(child)) {
        gboolean present, on;
        if (i < 2) {
            msdos_floppy_info fi;
            msdossession_floppy_info(self->session, i, &fi);
            present = fi.present;
            on = fi.active;
        } else {
            msdos_hd_info hi;
            msdossession_hd_info(self->session, i - 2, &hi);
            present = hi.path[0] != 0;
            on = hi.active;
        }
        gtk_label_set_text(GTK_LABEL(child), names[i]);
        gtk_widget_set_visible(child, present && msdossession_is_running(self->session));
        if (on)
            gtk_widget_add_css_class(child, "msdos-light-on");
        else
            gtk_widget_remove_css_class(child, "msdos-light-on");
    }
}

static gboolean status_tick(gpointer user_data)
{
    MsdosWindow *self = user_data;
    update_status(self);
    rebuild_menu(self, FALSE);
    return G_SOURCE_CONTINUE;
}

/* Gamepads come and go, FujiNet restarts itself, 86Box has something to
 * say: the other threads cannot call into GTK, so this timer asks. */
static gboolean fast_tick(gpointer user_data)
{
    MsdosWindow *self = user_data;
    char text[1024];
    unsigned gen;

    gen = msdossession_gamepad_generation(self->session);
    if (gen != self->pad_generation) {
        self->pad_generation = gen;
        if (msdossession_gamepad_last_event(self->session, text, sizeof text) > 0)
            msdos_window_toast(self, text);
    }
    gen = msdossession_fujinet_generation(self->session);
    if (gen != self->fujinet_generation) {
        self->fujinet_generation = gen;
        if (msdossession_fujinet_last_event(self->session, text, sizeof text) > 0)
            msdos_window_toast(self, text);
        update_status(self);
    }
    if (msdossession_take_notice(self->session, text, sizeof text) > 0)
        msdos_window_toast(self, text);
    update_lights(self);

    if (self->shot_path &&
        msdos_display_frames(MSDOS_DISPLAY(self->display)) >= self->shot_frames) {
        if (!msdos_display_save_png(MSDOS_DISPLAY(self->display), self->shot_path))
            g_printerr("MSDOS_SCREENSHOT: could not write %s\n", self->shot_path);
        g_clear_pointer(&self->shot_path, g_free);
    }
    if (self->window_shots &&
        msdos_display_frames(MSDOS_DISPLAY(self->display)) >= self->window_shot_frames) {
        save_window_shots(self->window_shots);
        g_clear_pointer(&self->window_shots, g_free);
    }
    if (self->quit_frames && msdos_display_frames(MSDOS_DISPLAY(self->display)) >= self->quit_frames) {
        self->quit_frames = 0;
        gtk_window_close(GTK_WINDOW(self));
    }
    return G_SOURCE_CONTINUE;
}

/* MSDOS_WINDOW_SHOTS: every window of the app, as GTK draws it, into a
 * PNG each in that directory (named for its title) -- the metainfo
 * screenshots, with nothing else on the screen in them. */
static void save_window_shots(const char *dir)
{
    GList *wins = gtk_window_list_toplevels();
    g_mkdir_with_parents(dir, 0755);
    for (GList *l = wins; l; l = l->next) {
        GtkWidget *w = l->data;
        const char *title = gtk_window_get_title(GTK_WINDOW(w));
        int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
        GdkSurface *surface;
        GdkPaintable *paint;
        GtkSnapshot *snap;
        GskRenderNode *node;
        GdkTexture *tex;
        double scale;
        char *name, *path;
        if (!gtk_widget_get_visible(w) || !title || width <= 0 || height <= 0)
            continue;
        surface = gtk_native_get_surface(GTK_NATIVE(w));
        scale = surface ? gdk_surface_get_scale(surface) : 1.0;
        paint = gtk_widget_paintable_new(w);
        snap = gtk_snapshot_new();
        gtk_snapshot_scale(snap, (float)scale, (float)scale);
        gdk_paintable_snapshot(paint, snap, width, height);
        node = gtk_snapshot_free_to_node(snap);
        if (node) {
            graphene_rect_t vp = GRAPHENE_RECT_INIT(0, 0, (float)(width * scale), (float)(height * scale));
            tex = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(w)), node, &vp);
            name = g_strcanon(g_strdup(title), "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-", '_');
            path = g_strdup_printf("%s/%s.png", dir, name);
            if (!gdk_texture_save_to_png(tex, path))
                g_printerr("MSDOS_WINDOW_SHOTS: could not write %s\n", path);
            g_free(path);
            g_free(name);
            g_object_unref(tex);
            gsk_render_node_unref(node);
        }
        g_object_unref(paint);
    }
    g_list_free(wins);
}

/* ---- opening things ------------------------------------------------------------------ */

typedef struct {
    MsdosWindow *self;
    char *path;
} HdCtx;

static void attach_hd(MsdosWindow *self, int drive, const char *path)
{
    char msg[1024];
    if (msdossession_hd_attach(self->session, drive, path, msg, sizeof msg) != 0) {
        show_message(GTK_WIDGET(self), "Cannot Attach the Disk", msg);
        return;
    }
    remember_folder(self, path);
    msdos_window_toast(self, msg);
    rebuild_menu(self, FALSE);
}

static void on_hd_drive(AdwAlertDialog *dlg, const char *response, gpointer user_data)
{
    HdCtx *c = user_data;
    (void)dlg;
    if (!g_strcmp0(response, "c") || !g_strcmp0(response, "d"))
        attach_hd(c->self, response[0] == 'd', c->path);
    g_free(c->path);
    g_free(c);
}

static void ask_hd_drive(MsdosWindow *self, const char *path)
{
    HdCtx *c = g_new0(HdCtx, 1);
    char body[600];
    AdwDialog *dlg;
    g_snprintf(body, sizeof body, "%s is a hard disk image. It takes effect from the next reset.",
               base_name(path));
    dlg = adw_alert_dialog_new("Which Drive?", body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "d", "D:");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "c", "C:");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "c", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "c");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");
    c->self = self;
    c->path = g_strdup(path);
    g_signal_connect(dlg, "response", G_CALLBACK(on_hd_drive), c);
    adw_dialog_present(dlg, GTK_WIDGET(self));
}

void msdos_window_load_media(MsdosWindow *self, const char *path)
{
    char msg[2048];
    int rc = msdossession_open(self->session, path, msg, sizeof msg);
    if (rc == 1) {
        ask_hd_drive(self, path);
        return;
    }
    if (rc != 0) {
        show_message(GTK_WIDGET(self), "Cannot Open It",
                     msg[0] ? msg : msdossession_last_error(self->session));
        return;
    }
    remember_folder(self, path);
    msdos_window_toast(self, msg[0] ? msg : base_name(path));
    msdos_window_settings_changed(self);
    msdos_window_start(self);
}

static void on_open_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    MsdosWindow *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    if (path)
        msdos_window_load_media(self, path);
}

static void action_open(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    g_autofree char *folder = media_folder(self);
    GtkFileDialog *dlg = file_dialog("Open", "Disk images and programs", ANY_EXTS, folder);
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_open_chosen, self);
    g_object_unref(dlg);
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x, double y,
                        gpointer user_data)
{
    MsdosWindow *self = user_data;
    g_autofree char *path = NULL;
    (void)t; (void)x; (void)y;
    if (!G_VALUE_HOLDS(value, G_TYPE_FILE))
        return FALSE;
    path = g_file_get_path(G_FILE(g_value_get_object(value)));
    if (!path)
        return FALSE;
    msdos_window_load_media(self, path);
    return TRUE;
}

/* ---- the BIOS ------------------------------------------------------------------------------ */

static void on_bios_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    MsdosWindow *self = user_data;
    const gboolean folder = GPOINTER_TO_INT(g_object_get_data(src, "msdos-folder"));
    g_autoptr(GListModel) files = NULL;
    g_autoptr(GFile) dir = NULL;
    GString *log = g_string_new(NULL);
    int imported = 0;

    if (folder) {
        dir = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    } else {
        files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    }
    for (guint i = 0; ; i++) {
        g_autoptr(GFile) f = NULL;
        if (folder) {
            if (i > 0 || !dir)
                break;
            f = g_object_ref(dir);
        } else {
            if (!files || i >= g_list_model_get_n_items(files))
                break;
            f = g_list_model_get_item(files, i);
        }
        g_autofree char *path = g_file_get_path(f);
        char msg[2048];
        if (!path)
            continue;
        if (msdossession_rom_import(self->session, path, msg, sizeof msg) == 0)
            imported++;
        g_string_append_printf(log, "%s%s", log->len ? "\n" : "", msg);
    }
    if (log->len)
        show_message(GTK_WIDGET(self), imported ? "BIOS Imported" : "Nothing Imported", log->str);
    g_string_free(log, TRUE);
    msdos_window_settings_changed(self);
    msdos_window_start(self);
}

static void import_bios(MsdosWindow *self, gboolean folder)
{
    GtkFileDialog *dlg = file_dialog(folder ? "Import BIOS from a Folder" : "Import BIOS",
                                     "BIOS images and ROM sets", ROM_EXTS, NULL);
    g_object_set_data(G_OBJECT(dlg), "msdos-folder", GINT_TO_POINTER(folder));
    if (folder)
        gtk_file_dialog_select_folder(dlg, GTK_WINDOW(self), NULL, on_bios_chosen, self);
    else
        gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(self), NULL, on_bios_chosen, self);
    g_object_unref(dlg);
}

static void action_import_bios(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)a; (void)p;
    import_bios(u, FALSE);
}

static void action_import_bios_folder(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)a; (void)p;
    import_bios(u, TRUE);
}

/* ---- floppies ------------------------------------------------------------------------------ */

typedef struct {
    MsdosWindow *self;
    int drive;
} DriveCtx;

static void on_floppy_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    DriveCtx *c = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    char msg[1024];
    if (path) {
        if (msdossession_floppy_insert(c->self->session, c->drive, path, 0, msg, sizeof msg) == 0) {
            remember_folder(c->self, path);
            msdos_window_toast(c->self, msg);
        } else {
            show_message(GTK_WIDGET(c->self), "Cannot Insert the Disk", msg);
        }
        rebuild_menu(c->self, FALSE);
    }
    g_free(c);
}

static void action_floppy_insert(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    DriveCtx *c = g_new0(DriveCtx, 1);
    g_autofree char *folder = media_folder(self);
    GtkFileDialog *dlg = file_dialog(g_variant_get_int32(p) ? "Insert in B:" : "Insert in A:",
                                     "Floppy images", FLOPPY_EXTS, folder);
    (void)a;
    c->self = self;
    c->drive = g_variant_get_int32(p);
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_floppy_chosen, c);
    g_object_unref(dlg);
}

static void action_floppy_eject(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a;
    msdossession_floppy_eject(self->session, g_variant_get_int32(p));
    rebuild_menu(self, FALSE);
}

static void action_floppy_protect(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    msdos_floppy_info fi;
    int d = g_variant_get_int32(p);
    (void)a;
    msdossession_floppy_info(self->session, d, &fi);
    msdossession_floppy_set_read_only(self->session, d, !fi.read_only);
    rebuild_menu(self, TRUE);
}

static void action_floppy_boot(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a;
    msdossession_floppy_boot_disk(self->session, g_variant_get_int32(p));
    msdos_window_toast(self, "The FujiNet boot floppy is in the drive");
    rebuild_menu(self, FALSE);
}

static void on_blank_floppy_saved(GObject *src, GAsyncResult *res, gpointer user_data)
{
    DriveCtx *c = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    char msg[512], ins[512];
    if (path) {
        if (msdossession_new_blank_floppy(c->self->session, c->drive, path, msg, sizeof msg) == 0
            && msdossession_floppy_insert(c->self->session, c->drive, path, 0, ins, sizeof ins) == 0)
            msdos_window_toast(c->self, ins);
        else
            show_message(GTK_WIDGET(c->self), "Cannot Make the Disk", msg);
        rebuild_menu(c->self, FALSE);
    }
    g_free(c);
}

static void action_floppy_blank(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    DriveCtx *c = g_new0(DriveCtx, 1);
    GtkFileDialog *dlg = gtk_file_dialog_new();
    g_autofree char *folder = media_folder(self);
    g_autoptr(GFile) dir = g_file_new_for_path(folder);
    (void)a;
    c->self = self;
    c->drive = g_variant_get_int32(p);
    gtk_file_dialog_set_title(dlg, "New Blank Floppy");
    gtk_file_dialog_set_initial_folder(dlg, dir);
    gtk_file_dialog_set_initial_name(dlg, "blank.img");
    gtk_file_dialog_save(dlg, GTK_WINDOW(self), NULL, on_blank_floppy_saved, c);
    g_object_unref(dlg);
}

/* ---- hard disks ------------------------------------------------------------------------------ */

static void on_hd_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    DriveCtx *c = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    if (path)
        attach_hd(c->self, c->drive, path);
    g_free(c);
}

static void action_hd_attach(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    DriveCtx *c = g_new0(DriveCtx, 1);
    g_autofree char *folder = media_folder(self);
    GtkFileDialog *dlg = file_dialog(g_variant_get_int32(p) ? "Attach as D:" : "Attach as C:",
                                     "Hard disk images", HD_EXTS, folder);
    (void)a;
    c->self = self;
    c->drive = g_variant_get_int32(p);
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_hd_chosen, c);
    g_object_unref(dlg);
}

static void action_hd_detach(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a;
    msdossession_hd_detach(self->session, g_variant_get_int32(p));
    msdos_window_toast(self, "Detached from the next reset");
    rebuild_menu(self, FALSE);
}

static void on_blank_hd_saved(GObject *src, GAsyncResult *res, gpointer user_data)
{
    DriveCtx *c = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    char msg[512];
    if (path) {
        if (msdossession_new_blank_hd(c->self->session, c->drive, path, msg, sizeof msg) == 0) {
            attach_hd(c->self, c->drive, path);
            show_message(GTK_WIDGET(c->self), "New Hard Disk", msg);
        } else {
            show_message(GTK_WIDGET(c->self), "Cannot Make the Disk", msg);
        }
    }
    g_free(c);
}

static void action_hd_blank(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    DriveCtx *c = g_new0(DriveCtx, 1);
    GtkFileDialog *dlg = gtk_file_dialog_new();
    g_autofree char *folder = media_folder(self);
    g_autoptr(GFile) dir = g_file_new_for_path(folder);
    (void)a;
    c->self = self;
    c->drive = g_variant_get_int32(p);
    gtk_file_dialog_set_title(dlg, "New Blank Hard Disk");
    gtk_file_dialog_set_initial_folder(dlg, dir);
    gtk_file_dialog_set_initial_name(dlg, c->drive ? "drive-d.vhd" : "drive-c.vhd");
    gtk_file_dialog_save(dlg, GTK_WINDOW(self), NULL, on_blank_hd_saved, c);
    g_object_unref(dlg);
}

/* ---- FujiNet's SD card ------------------------------------------------------------------------ */

static void on_import_sd_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    MsdosWindow *self = user_data;
    g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    int n = 0;
    if (!files)
        return;
    for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
        g_autoptr(GFile) f = g_list_model_get_item(files, i);
        g_autofree char *path = g_file_get_path(f);
        char dest[1024];
        if (path && msdossession_import_to_sd(self->session, path, dest, sizeof dest) == 0)
            n++;
    }
    if (n) {
        char t[200];
        g_snprintf(t, sizeof t, "%d file%s on FujiNet's SD card: mount %s in CONFIG", n,
                   n == 1 ? " is" : "s are", n == 1 ? "it" : "them");
        msdos_window_toast(self, t);
    } else {
        show_message(GTK_WIDGET(self), "Nothing Imported", "The files could not be copied.");
    }
}

static void action_import_sd(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    g_autofree char *folder = media_folder(self);
    GtkFileDialog *dlg = file_dialog("Import to FujiNet's SD Card", "Disk images and programs",
                                     ANY_EXTS, folder);
    (void)a; (void)p;
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(self), NULL, on_import_sd_chosen, self);
    g_object_unref(dlg);
}

static void action_sd_folder(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a; (void)p;
    open_folder(self, msdossession_sd_path(self->session));
}

/* ---- the machine ---------------------------------------------------------------------------- */

static void action_cad(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a; (void)p;
    msdossession_ctrl_alt_del(self->session);
}

static void action_reset(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a; (void)p;
    if (!msdossession_is_running(self->session)) {
        msdos_window_start(self);
        return;
    }
    msdossession_reset(self->session);
    update_status(self);
}

static void action_pause(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    gboolean on = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(on));
    msdossession_set_paused(self->session, on);
    update_status(self);
}

static void action_warp(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    gboolean on = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(on));
    msdossession_set_warp(self->session, on);
    update_status(self);
}

static void action_prefs(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a; (void)p;
    msdos_prefs_show(self, self->session);
}

static void action_quit(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    gtk_window_close(GTK_WINDOW(user_data));
}

/* ---- text in and out -------------------------------------------------------------------------- */

static void action_copy_screen(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    static char buf[16384];
    (void)a; (void)p;
    if (msdossession_copy_screen_text(self->session, buf, sizeof buf) <= 0) {
        msdos_window_toast(self, "No text screen to copy");
        return;
    }
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), buf);
    msdos_window_toast(self, "The screen's text is on the clipboard");
}

static void on_paste_text(GObject *src, GAsyncResult *res, gpointer user_data)
{
    MsdosWindow *self = user_data;
    g_autofree char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (text && *text)
        msdossession_paste(self->session, text);
}

static void action_paste(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a; (void)p;
    gdk_clipboard_read_text_async(gtk_widget_get_clipboard(GTK_WIDGET(self)), NULL,
                                  on_paste_text, self);
}

/* ---- the view ---------------------------------------------------------------------------------- */

static void action_controllers(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a; (void)p;
    msdos_controllers_toggle(GTK_WINDOW(self), self->session);
}

static void action_debugger(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a; (void)p;
    msdos_debugger_toggle(GTK_WINDOW(self), self->session);
}

static void action_fullscreen(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a; (void)p;
    self->fullscreen = !self->fullscreen;
    if (self->fullscreen)
        gtk_window_fullscreen(GTK_WINDOW(self));
    else
        gtk_window_unfullscreen(GTK_WINDOW(self));
}

static void toggle_pref(MsdosWindow *self, GSimpleAction *a, const char *key)
{
    gboolean on = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    int i = msdossession_pref_find(key);
    if (i >= 0)
        msdossession_pref_set_int(self->session, i, on);
    msdossession_settings_flush(self->session);
    msdos_window_apply_view(self);
}

static void action_aspect(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)p;
    toggle_pref(u, a, "aspect43");
}

static void action_integer(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)p;
    toggle_pref(u, a, "integer_scale");
}

static void action_smooth(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)p;
    toggle_pref(u, a, "smooth");
}

static void set_toggle(MsdosWindow *self, const char *name, gboolean on)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), name);
    if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(on));
}

static void set_enabled(MsdosWindow *self, const char *name, gboolean on)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), name);
    if (a)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

void msdos_window_apply_view(MsdosWindow *self)
{
    const gboolean smooth = msdossession_smooth(self->session) != 0;
    const gboolean integer = msdossession_integer_scale(self->session) != 0;
    msdos_display_set_smooth(MSDOS_DISPLAY(self->display), smooth);
    msdos_display_set_integer_scale(MSDOS_DISPLAY(self->display), integer);
    set_toggle(self, "aspect", msdossession_get_int(self->session, "aspect43", 1) != 0);
    set_toggle(self, "integer", integer);
    set_toggle(self, "smooth", smooth);
}

void msdos_window_settings_changed(MsdosWindow *self)
{
    msdos_window_apply_view(self);
    msdos_controllers_refresh();
    rebuild_menu(self, TRUE);
    update_status(self);
    show_page(self);
}

/* ---- FujiNet ------------------------------------------------------------------------------------- */

static void action_fujinet_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a; (void)p;
    if (!msdossession_fujinet_running(self->session)) {
        msdos_window_toast(self, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet)");
        return;
    }
    msdos_fujiconfig_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a; (void)p;
    msdos_fujilog_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_restart(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    (void)a; (void)p;
    if (msdossession_fujinet_restart(self->session) != 0)
        show_message(GTK_WIDGET(self), "Cannot Restart FujiNet",
                     msdossession_last_error(self->session));
    else
        msdos_window_toast(self, "FujiNet restarted");
}

static void action_about(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    MsdosWindow *self = user_data;
    AdwDialog *about;
    (void)a; (void)p;
    about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "FujiNet Go MS-DOS");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), msdos_icon_name());
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), MSDOS_VERSION_STRING);
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "Thomas Cherryhomes");
    adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), "https://fujinet.online/");
    adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about),
        "https://github.com/FujiNetWIFI/fujinet-go-msdos-desktop/issues");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
        "IBM PCs and compatibles, from the 5150 to a Pentium, with a FujiNet on a COM port. "
        "The emulator is 86Box (GPL-2.0-or-later, the 86Box team), with the FujiNet "
        "firmware (GPL-3.0-or-later) talking to FUJINET.SYS. The boot floppy is FreeDOS "
        "(GPL). The BIOS ROMs are imported.");
    adw_dialog_present(about, GTK_WIDGET(self));
}

static void noop(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)a; (void)p; (void)u;
}

/* app.help: every window's Ctrl+Shift+H, for what it shows */
static void action_help(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    msdos_help_for(gtk_application_get_active_window(GTK_APPLICATION(user_data)));
}

static const GActionEntry app_actions[] = {
    { "help", action_help, NULL, NULL, NULL, { 0 } },
};

const char *msdos_window_help_context(MsdosWindow *self)
{
    static char ctx[64];
    AdwDialog *d = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self));
    if (d && ADW_IS_PREFERENCES_DIALOG(d)) {
        const char *page = adw_preferences_dialog_get_visible_page_name(ADW_PREFERENCES_DIALOG(d));
        if (page) {
            g_snprintf(ctx, sizeof ctx, "prefs/%s", page);
            return ctx;
        }
    }
    return g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(self->stack)), "nobios") == 0
        ? "nobios" : "main";
}

static const GActionEntry win_actions[] = {
    { "open", action_open, NULL, NULL, NULL, { 0 } },
    { "cad", action_cad, NULL, NULL, NULL, { 0 } },
    { "reset", action_reset, NULL, NULL, NULL, { 0 } },
    { "pause", action_pause, NULL, "false", NULL, { 0 } },
    { "warp", action_warp, NULL, "false", NULL, { 0 } },
    { "import-bios", action_import_bios, NULL, NULL, NULL, { 0 } },
    { "import-bios-folder", action_import_bios_folder, NULL, NULL, NULL, { 0 } },
    { "floppy-insert", action_floppy_insert, "i", NULL, NULL, { 0 } },
    { "floppy-eject", action_floppy_eject, "i", NULL, NULL, { 0 } },
    { "floppy-protect", action_floppy_protect, "i", NULL, NULL, { 0 } },
    { "floppy-boot", action_floppy_boot, "i", NULL, NULL, { 0 } },
    { "floppy-blank", action_floppy_blank, "i", NULL, NULL, { 0 } },
    { "hd-attach", action_hd_attach, "i", NULL, NULL, { 0 } },
    { "hd-detach", action_hd_detach, "i", NULL, NULL, { 0 } },
    { "hd-blank", action_hd_blank, "i", NULL, NULL, { 0 } },
    { "import-sd", action_import_sd, NULL, NULL, NULL, { 0 } },
    { "sd-folder", action_sd_folder, NULL, NULL, NULL, { 0 } },
    { "copy-screen", action_copy_screen, NULL, NULL, NULL, { 0 } },
    { "paste", action_paste, NULL, NULL, NULL, { 0 } },
    { "controllers", action_controllers, NULL, NULL, NULL, { 0 } },
    { "debugger", action_debugger, NULL, NULL, NULL, { 0 } },
    { "fullscreen", action_fullscreen, NULL, NULL, NULL, { 0 } },
    { "aspect", action_aspect, NULL, "true", NULL, { 0 } },
    { "integer", action_integer, NULL, "false", NULL, { 0 } },
    { "smooth", action_smooth, NULL, "true", NULL, { 0 } },
    { "fujinet-config", action_fujinet_config, NULL, NULL, NULL, { 0 } },
    { "fujinet-log", action_fujinet_log, NULL, NULL, NULL, { 0 } },
    { "fujinet-restart", action_fujinet_restart, NULL, NULL, NULL, { 0 } },
    { "prefs", action_prefs, NULL, NULL, NULL, { 0 } },
    { "about", action_about, NULL, NULL, NULL, { 0 } },
    { "quit", action_quit, NULL, NULL, NULL, { 0 } },
    { "info", noop, NULL, NULL, NULL, { 0 } },
};

/* ---- the menu ----------------------------------------------------------------------------------- */

static void append_target(GMenu *m, const char *label, const char *action, int target)
{
    GMenuItem *item = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target_value(item, action, g_variant_new_int32(target));
    g_menu_append_item(m, item);
    g_object_unref(item);
}

static GMenu *drives_menu(MsdosWindow *self)
{
    GMenu *menu = g_menu_new(), *drives = g_menu_new(), *more = g_menu_new();
    char label[1200];

    for (int d = 0; d < MSDOS_FLOPPIES; d++) {
        msdos_floppy_info fi;
        GMenu *sub, *info;
        g_autofree char *name = NULL;
        msdossession_floppy_info(self->session, d, &fi);
        if (!fi.present)
            continue;
        sub = g_menu_new();
        info = g_menu_new();
        name = menu_escape(fi.boot_disk ? "FujiNet boot floppy" : fi.path[0] ? base_name(fi.path)
                                                                             : "Empty");
        g_snprintf(label, sizeof label, "%s%s", name, fi.read_only ? " (write-protected)" : "");
        g_menu_append(info, label, "win.info");
        g_menu_append_section(sub, NULL, G_MENU_MODEL(info));
        append_target(sub, "_Insert\xe2\x80\xa6", "win.floppy-insert", d);
        append_target(sub, "_Eject", "win.floppy-eject", d);
        append_target(sub, fi.read_only ? "Allow _Writing" : "_Write-Protect", "win.floppy-protect", d);
        append_target(sub, "FujiNet _Boot Floppy", "win.floppy-boot", d);
        append_target(sub, "_New Blank Disk\xe2\x80\xa6", "win.floppy-blank", d);
        g_snprintf(label, sizeof label, "%s %s", d ? "_B:" : "_A:", fi.type);
        g_menu_append_submenu(drives, label, G_MENU_MODEL(sub));
        g_object_unref(info);
        g_object_unref(sub);
    }
    for (int d = 0; d < MSDOS_HARD_DISKS; d++) {
        msdos_hd_info hi;
        GMenu *sub, *info;
        g_autofree char *name = NULL;
        msdossession_hd_info(self->session, d, &hi);
        if (!hi.present)
            continue;
        sub = g_menu_new();
        info = g_menu_new();
        name = menu_escape(hi.path[0] ? base_name(hi.path) : "None");
        g_menu_append(info, name, "win.info");
        g_menu_append_section(sub, NULL, G_MENU_MODEL(info));
        append_target(sub, "_Attach\xe2\x80\xa6", "win.hd-attach", d);
        append_target(sub, "_Detach", "win.hd-detach", d);
        append_target(sub, "_New Blank Disk\xe2\x80\xa6", "win.hd-blank", d);
        g_menu_append_submenu(drives, d ? "_D: hard disk" : "_C: hard disk", G_MENU_MODEL(sub));
        g_object_unref(info);
        g_object_unref(sub);
    }
    g_menu_append_section(menu, "The PC's drives", G_MENU_MODEL(drives));
    g_menu_append(more, "_Import to SD Card\xe2\x80\xa6", "win.import-sd");
    g_menu_append(more, "Open _SD Folder", "win.sd-folder");
    g_menu_append_section(menu, "FujiNet's SD card", G_MENU_MODEL(more));
    g_object_unref(drives);
    g_object_unref(more);
    return menu;
}

static GMenu *build_menu(MsdosWindow *self)
{
    GMenu *menu = g_menu_new();
    GMenu *machine = g_menu_new(), *media = g_menu_new(), *edit = g_menu_new();
    GMenu *view = g_menu_new(), *fujim = g_menu_new(), *app = g_menu_new();
    GMenu *drives = drives_menu(self);

    g_menu_append(machine, "_Open\xe2\x80\xa6", "win.open");
    g_menu_append(machine, "Ctrl+_Alt+Del", "win.cad");
    g_menu_append(machine, "_Reset (Ctrl+Shift+R)", "win.reset");
    g_menu_append(machine, "_Pause", "win.pause");
    g_menu_append(machine, "_Warp Speed", "win.warp");
    g_menu_append(machine, "Import _BIOS\xe2\x80\xa6", "win.import-bios");
    g_menu_append(machine, "Import BIOS from a _Folder\xe2\x80\xa6", "win.import-bios-folder");
    g_menu_append_section(menu, "Machine", G_MENU_MODEL(machine));
    g_menu_append_submenu(media, "_Drives", G_MENU_MODEL(drives));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(media));
    g_menu_append(edit, "_Copy Screen Text", "win.copy-screen");
    g_menu_append(edit, "P_aste", "win.paste");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(edit));
    g_menu_append(view, "C_ontrollers (Ctrl+Shift+K)", "win.controllers");
    g_menu_append(view, "De_bugger (Ctrl+Shift+D)", "win.debugger");
    g_menu_append(view, "_4:3 Picture", "win.aspect");
    g_menu_append(view, "_Whole-Number Scaling", "win.integer");
    g_menu_append(view, "S_mooth Scaling", "win.smooth");
    g_menu_append(view, "_Fullscreen (Ctrl+Alt+PgDn)", "win.fullscreen");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(view));
    g_menu_append(fujim, "FujiNet _Web UI", "win.fujinet-config");
    g_menu_append(fujim, "Console _Log", "win.fujinet-log");
    g_menu_append(fujim, "R_estart FujiNet", "win.fujinet-restart");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fujim));
    g_menu_append(app, "_Preferences", "win.prefs");
    g_menu_append(app, "_Help", "app.help");
    g_menu_append(app, "_About FujiNet Go MS-DOS", "win.about");
    g_menu_append(app, "_Quit", "win.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app));

    g_object_unref(machine);
    g_object_unref(media);
    g_object_unref(drives);
    g_object_unref(edit);
    g_object_unref(view);
    g_object_unref(fujim);
    g_object_unref(app);
    return menu;
}

/* What the drive menus show, to rebuild only on a change: GtkPopoverMenu
 * names its submenu pages by label, and changing the model it shows in
 * place re-adds pages whose names are still taken. */
static void rebuild_menu(MsdosWindow *self, gboolean force)
{
    GString *sig = g_string_new(NULL);
    const gboolean up = msdossession_is_running(self->session);

    for (int d = 0; d < MSDOS_FLOPPIES; d++) {
        msdos_floppy_info fi;
        msdossession_floppy_info(self->session, d, &fi);
        g_string_append_printf(sig, "%d%s%s%d|", fi.present, fi.type, fi.path, fi.read_only);
    }
    for (int d = 0; d < MSDOS_HARD_DISKS; d++) {
        msdos_hd_info hi;
        msdossession_hd_info(self->session, d, &hi);
        g_string_append_printf(sig, "%d%s|", hi.present, hi.path);
    }
    set_enabled(self, "fujinet-config", msdossession_fujinet_running(self->session));
    set_enabled(self, "fujinet-restart", up && msdossession_get_int(self->session, "fujinet", 1));
    set_enabled(self, "cad", up);
    set_enabled(self, "pause", up);
    set_enabled(self, "warp", up);
    set_enabled(self, "copy-screen", up);
    set_enabled(self, "paste", up);
    set_enabled(self, "info", FALSE);

    if (force || strcmp(sig->str, self->menu_sig) != 0) {
        GMenu *menu = build_menu(self);
        g_strlcpy(self->menu_sig, sig->str, sizeof self->menu_sig);
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(self->menu_button), G_MENU_MODEL(menu));
        g_object_unref(menu);
    }
    g_string_free(sig, TRUE);
}

/* ---- construction ---------------------------------------------------------------------------------- */

static void msdos_window_dispose(GObject *object)
{
    MsdosWindow *self = MSDOS_WINDOW(object);
    if (self->status_id) {
        g_source_remove(self->status_id);
        self->status_id = 0;
    }
    if (self->fast_id) {
        g_source_remove(self->fast_id);
        self->fast_id = 0;
    }
    g_clear_pointer(&self->shot_path, g_free);
    g_clear_pointer(&self->window_shots, g_free);
    G_OBJECT_CLASS(msdos_window_parent_class)->dispose(object);
}

static void msdos_window_class_init(MsdosWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = msdos_window_dispose;
}

static void msdos_window_init(MsdosWindow *self)
{
    (void)self;
}

static GtkWidget *dot(const char *tip)
{
    GtkWidget *d = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(d, "msdos-dot");
    gtk_widget_add_css_class(d, "msdos-dot-off");
    gtk_widget_set_valign(d, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(d, tip);
    return d;
}

static gboolean env_on(const char *name)
{
    const char *v = g_getenv(name);
    return v && *v && *v != '0';
}

static void on_nobios_import(GtkButton *b, gpointer user_data)
{
    import_bios(user_data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "msdos-folder")));
}

static void on_nobios_help(GtkButton *b, gpointer user_data)
{
    (void)b;
    msdos_help_show(GTK_WINDOW(user_data), "nobios");
}

static GtkWidget *nobios_page(MsdosWindow *self)
{
    GtkWidget *page = adw_status_page_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *files = gtk_button_new_with_label("Import BIOS\xe2\x80\xa6");
    GtkWidget *folder = gtk_button_new_with_label("Import a Folder\xe2\x80\xa6");
    GtkWidget *help = gtk_button_new_with_label("Help");

    adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), "computer-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(page), "No PC BIOS");
    adw_status_page_set_description(ADW_STATUS_PAGE(page),
        "IBM's and the clone makers' BIOS ROMs are not part of the app. Import the ones "
        "you have -- single images, a zip, or a folder such as an 86Box ROM set -- and "
        "the PC starts.");
    gtk_widget_add_css_class(files, "pill");
    gtk_widget_add_css_class(files, "suggested-action");
    gtk_widget_add_css_class(folder, "pill");
    g_object_set_data(G_OBJECT(folder), "msdos-folder", GINT_TO_POINTER(1));
    g_signal_connect(files, "clicked", G_CALLBACK(on_nobios_import), self);
    g_signal_connect(folder, "clicked", G_CALLBACK(on_nobios_import), self);
    gtk_widget_add_css_class(help, "pill");
    g_signal_connect(help, "clicked", G_CALLBACK(on_nobios_help), self);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), files);
    gtk_box_append(GTK_BOX(box), folder);
    gtk_box_append(GTK_BOX(box), help);
    {
        GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        GtkWidget *roms = gtk_link_button_new_with_label(MSDOSSESSION_ROMS_URL, "Get the 86Box ROM set");
        gtk_box_append(GTK_BOX(outer), box);
        gtk_box_append(GTK_BOX(outer), roms);
        box = outer;
    }
    adw_status_page_set_child(ADW_STATUS_PAGE(page), box);
    return page;
}

static GtkWidget *quick_button(const char *label, const char *action, const char *tip)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_focusable(b, FALSE);
    return b;
}

GtkWidget *msdos_window_new(AdwApplication *app, msdossession *session)
{
    MsdosWindow *self = g_object_new(MSDOS_TYPE_WINDOW, "application", app, NULL);
    GtkWidget *header, *menu_button, *toolbar, *status_box;
    GtkEventController *keys;

    self->session = session;

    gtk_window_set_title(GTK_WINDOW(self), "FujiNet Go MS-DOS");
    gtk_window_set_icon_name(GTK_WINDOW(self), msdos_icon_name());
    /* 640 x 480 at one and a half */
    gtk_window_set_default_size(GTK_WINDOW(self), 960, 720 + 46);

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions, G_N_ELEMENTS(win_actions),
                                    self);
    g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions, G_N_ELEMENTS(app_actions),
                                    app);
    {
        /* Ctrl+Shift: Ctrl alone is the PC's */
        static const char *const prefs[] = { "<Control><Shift>p", NULL };
        static const char *const quit[] = { "<Control><Shift>q", NULL };
        static const char *const open[] = { "<Control><Shift>o", NULL };
        static const char *const copy[] = { "<Control><Shift>c", NULL };
        static const char *const paste[] = { "<Control><Shift>v", NULL };
        static const char *const reset[] = { "<Control><Shift>r", NULL };
        static const char *const ctl[] = { "<Control><Shift>k", NULL };
        static const char *const dbg[] = { "<Control><Shift>d", NULL };
        static const char *const full[] = { "<Control><Shift>f", NULL };
        static const char *const warp[] = { "<Control><Shift>w", NULL };
        static const char *const help[] = { "<Control><Shift>h", NULL };
        GtkApplication *a = GTK_APPLICATION(app);
        gtk_application_set_accels_for_action(a, "win.prefs", prefs);
        gtk_application_set_accels_for_action(a, "win.quit", quit);
        gtk_application_set_accels_for_action(a, "win.open", open);
        gtk_application_set_accels_for_action(a, "win.copy-screen", copy);
        gtk_application_set_accels_for_action(a, "win.paste", paste);
        gtk_application_set_accels_for_action(a, "win.reset", reset);
        gtk_application_set_accels_for_action(a, "win.controllers", ctl);
        gtk_application_set_accels_for_action(a, "win.debugger", dbg);
        gtk_application_set_accels_for_action(a, "win.fullscreen", full);
        gtk_application_set_accels_for_action(a, "win.warp", warp);
        gtk_application_set_accels_for_action(a, "app.help", help);
    }

    header = adw_header_bar_new();
    menu_button = gtk_menu_button_new();
    self->menu_button = menu_button;
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_button), TRUE);
    gtk_widget_set_tooltip_text(menu_button, "Main Menu");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

    self->cad_button = quick_button("Ctrl+Alt+Del", "win.cad",
                                    "Send Ctrl+Alt+Del: DOS restarts (a warm boot)");
    self->reset_button = quick_button("Reset", "win.reset",
                                      "Press the reset button: the PC starts again from cold "
                                      "(Ctrl+Shift+R)");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), self->cad_button);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), self->reset_button);

    status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    self->status_dot = dot("FujiNet link");
    self->status = gtk_label_new("Starting\xe2\x80\xa6");
    gtk_widget_add_css_class(self->status, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(self->status), PANGO_ELLIPSIZE_END);
    self->lights = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    for (int i = 0; i < 4; i++) {
        GtkWidget *l = gtk_label_new("");
        gtk_widget_add_css_class(l, "msdos-light");
        gtk_widget_set_tooltip_text(l, "Drive activity");
        gtk_box_append(GTK_BOX(self->lights), l);
    }
    gtk_widget_set_margin_start(status_box, 8);
    gtk_box_append(GTK_BOX(status_box), self->status_dot);
    gtk_box_append(GTK_BOX(status_box), self->status);
    gtk_box_append(GTK_BOX(status_box), self->lights);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), status_box);

    self->display = msdos_display_new(session);
    g_signal_connect(self->display, "capture-changed", G_CALLBACK(on_capture_changed), self);
    {
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
        g_signal_connect(drop, "drop", G_CALLBACK(on_drop), self);
        gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drop));
    }

    self->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(self->stack), self->display, "pc");
    gtk_stack_add_named(GTK_STACK(self->stack), nobios_page(self), "nobios");
    self->toast_overlay = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay), self->stack);
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->toast_overlay);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), toolbar);
    msdos_window_apply_view(self);

    /* capture on the WINDOW so typing works whatever has focus */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);
    g_signal_connect(self, "notify::is-active", G_CALLBACK(on_active_changed), self);

    /* the current pads and FujiNet state are the baseline: no toasts for them */
    self->pad_generation = msdossession_gamepad_generation(session);
    self->fujinet_generation = msdossession_fujinet_generation(session);
    rebuild_menu(self, TRUE);
    self->status_id = g_timeout_add_seconds(1, status_tick, self);
    self->fast_id = g_timeout_add(100, fast_tick, self);
    update_status(self);
    show_page(self);

    /* MSDOS_OPEN_CONTROLLERS / _DEBUGGER / _SETTINGS open those windows at
     * launch, MSDOS_OPEN_HELP=<slug, context or ?words> the Help ("1": its
     * start). MSDOS_SCREENSHOT=file.png saves the picture after
     * MSDOS_SCREENSHOT_FRAMES frames (300), MSDOS_WINDOW_SHOTS=dir every
     * window after MSDOS_WINDOW_SHOTS_FRAMES; MSDOS_AUTOQUIT_FRAMES quits after
     * that many. */
    if (env_on("MSDOS_OPEN_CONTROLLERS"))
        msdos_controllers_toggle(GTK_WINDOW(self), session);
    if (env_on("MSDOS_OPEN_DEBUGGER"))
        msdos_debugger_show(GTK_WINDOW(self), session);
    if (env_on("MSDOS_OPEN_SETTINGS"))
        msdos_prefs_show(self, session);
    if (env_on("MSDOS_OPEN_HELP"))
        msdos_help_show(GTK_WINDOW(self),
                        strcmp(g_getenv("MSDOS_OPEN_HELP"), "1") ? g_getenv("MSDOS_OPEN_HELP") : NULL);
    if (g_getenv("MSDOS_SCREENSHOT") && *g_getenv("MSDOS_SCREENSHOT")) {
        const char *n = g_getenv("MSDOS_SCREENSHOT_FRAMES");
        self->shot_path = g_strdup(g_getenv("MSDOS_SCREENSHOT"));
        self->shot_frames = n ? g_ascii_strtoull(n, NULL, 10) : 300;
    }
    if (g_getenv("MSDOS_WINDOW_SHOTS") && *g_getenv("MSDOS_WINDOW_SHOTS")) {
        const char *n = g_getenv("MSDOS_WINDOW_SHOTS_FRAMES");
        self->window_shots = g_strdup(g_getenv("MSDOS_WINDOW_SHOTS"));
        self->window_shot_frames = n ? g_ascii_strtoull(n, NULL, 10) : 300;
    }
    if (g_getenv("MSDOS_AUTOQUIT_FRAMES"))
        self->quit_frames = g_ascii_strtoull(g_getenv("MSDOS_AUTOQUIT_FRAMES"), NULL, 10);
    return GTK_WIDGET(self);
}
