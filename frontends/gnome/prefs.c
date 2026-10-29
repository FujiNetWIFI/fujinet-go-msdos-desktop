/*
 * The Preferences dialog: an AdwPreferencesDialog built from the session's
 * preference table (msdossession_pref), so the four frontends show the same
 * pages, groups and rows. No .ui file.
 *
 * Rows that apply LIVE (the picture, the volume) take effect at once. The
 * machine -- model, processor, memory, cards, drives, ports -- is the
 * computer you would switch off to change: those rows wait, and closing the
 * dialog applies them together with one reset. Rows follow each other as
 * they change: processors and memory by machine, cards by what the machine
 * can take and what has been imported.
 *
 * A ROMs page lists what each machine and card needs, imported or not.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "controllers/controllers_window.h"
#include "help/help_window.h"

#include <string.h>

#define MAX_ROWS 128
#define MAX_ROM_ROWS 64

typedef struct {
    MsdosWindow *window;
    msdossession *session;
    AdwDialog *dialog;
    GtkWidget *rows[MAX_ROWS];          /* by pref index */
    char *labels[MAX_ROWS];             /* a choice row's labels, as shown */
    GtkWidget *rom_rows[MAX_ROM_ROWS];
    gboolean syncing;
    int pending;                        /* the heaviest change not applied yet */
} PrefsState;

static void refresh_rows(PrefsState *st);

static const char *page_icon(const char *page)
{
    if (!strcmp(page, "Machine"))
        return "computer-symbolic";
    if (!strcmp(page, "Storage"))
        return "drive-harddisk-symbolic";
    if (!strcmp(page, "Display"))
        return "video-display-symbolic";
    if (!strcmp(page, "Sound"))
        return "audio-speakers-symbolic";
    if (!strcmp(page, "Ports"))
        return "input-gaming-symbolic";
    if (!strcmp(page, "FujiNet"))
        return "network-wireless-symbolic";
    return "applications-system-symbolic";
}

/* A change to pref i: LIVE ones apply now unless something heavier waits
 * (applying then would power-cycle mid-edit). */
static void changed(PrefsState *st, int i)
{
    const msdos_pref *p = msdossession_pref(i);
    if (p->applies > st->pending)
        st->pending = p->applies;
    if (st->pending == MSDOS_APPLY_LIVE)
        msdossession_apply_settings(st->session);
    if (p->applies == MSDOS_APPLY_LIVE)
        msdos_window_apply_view(st->window);
    if (!strcmp(p->key, "machine"))
        msdos_controllers_refresh();
    refresh_rows(st);
}

/* ---- rows -------------------------------------------------------------------- */

static void on_switch(GObject *row, GParamSpec *ps, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(row, "msdos-pref"));
    (void)ps;
    if (st->syncing)
        return;
    msdossession_pref_set_int(st->session, i, adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
    changed(st, i);
}

static void on_spin(GObject *row, GParamSpec *ps, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(row, "msdos-pref"));
    (void)ps;
    if (st->syncing)
        return;
    msdossession_pref_set_int(st->session, i, (int)adw_spin_row_get_value(ADW_SPIN_ROW(row)));
    changed(st, i);
}

static void on_combo(GObject *row, GParamSpec *ps, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(row, "msdos-pref"));
    guint c = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)ps;
    if (st->syncing || c == GTK_INVALID_LIST_POSITION)
        return;
    if (!msdossession_pref_choice_enabled(st->session, i, (int)c)) {
        /* not imported, or not on this machine: put it back */
        msdos_window_toast(st->window, "Not available \xe2\x80\x94 import its ROM first, or "
                                      "choose a machine that takes it");
        refresh_rows(st);
        return;
    }
    msdossession_pref_set_choice(st->session, i, (int)c);
    changed(st, i);
}

/* A text row: valid text is saved; anything else is flagged and left unsaved. */
static void on_text(GtkEditable *entry, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "msdos-pref"));
    const char *text = gtk_editable_get_text(entry);
    char saved[256];
    g_autofree char *trimmed = g_strstrip(g_strdup(text));

    if (st->syncing)
        return;
    if (!msdossession_pref_text_valid(i, text)) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    msdossession_pref_get_text(st->session, i, saved, sizeof saved);
    if (!g_strcmp0(saved, trimmed))
        return;
    msdossession_pref_set_text(st->session, i, text);
    changed(st, i);
}

static GtkWidget *make_row(PrefsState *st, int i)
{
    const msdos_pref *p = msdossession_pref(i);
    GtkWidget *row;

    switch (p->type) {
    case MSDOS_PREF_BOOL:
        row = adw_switch_row_new();
        g_signal_connect(row, "notify::active", G_CALLBACK(on_switch), st);
        break;
    case MSDOS_PREF_INT:
        row = adw_spin_row_new_with_range(p->min, p->max, p->step > 0 ? p->step : 1);
        g_signal_connect(row, "notify::value", G_CALLBACK(on_spin), st);
        break;
    case MSDOS_PREF_CHOICE:
        row = adw_combo_row_new();
        g_signal_connect(row, "notify::selected", G_CALLBACK(on_combo), st);
        break;
    case MSDOS_PREF_TEXT:
        row = adw_entry_row_new();
        g_signal_connect(row, "changed", G_CALLBACK(on_text), st);
        break;
    default:
        row = adw_action_row_new();
        break;
    }
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), p->label);
    if (p->help && p->type != MSDOS_PREF_TEXT)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), p->help);
    else if (p->help)
        gtk_widget_set_tooltip_text(row, p->help);   /* an entry row has no subtitle */
    g_object_set_data(G_OBJECT(row), "msdos-pref", GINT_TO_POINTER(i));
    return row;
}

/* A choice row's labels joined, to see when they change. */
static char *choice_labels(PrefsState *st, int i)
{
    GString *g = g_string_new(NULL);
    const char *l;
    for (int c = 0; (l = msdossession_pref_choice(st->session, i, c)) != NULL; c++) {
        g_string_append(g, l);
        if (!msdossession_pref_choice_enabled(st->session, i, c))
            g_string_append(g, " (not available)");
        g_string_append_c(g, '\n');
    }
    return g_string_free(g, FALSE);
}

/* Every row from the store: values, labels that depend on others, and
 * which rows apply to this machine. */
static void refresh_rows(PrefsState *st)
{
    st->syncing = TRUE;
    for (int i = 0; i < msdossession_pref_count() && i < MAX_ROWS; i++) {
        const msdos_pref *p = msdossession_pref(i);
        GtkWidget *row = st->rows[i];
        if (!row)
            continue;
        gtk_widget_set_sensitive(row, msdossession_pref_enabled(st->session, i));
        switch (p->type) {
        case MSDOS_PREF_BOOL:
            adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                                      msdossession_pref_get_int(st->session, i) != 0);
            break;
        case MSDOS_PREF_INT: {
            int lo, hi, step;
            msdossession_pref_int_range(st->session, i, &lo, &hi, &step);
            adw_spin_row_set_range(ADW_SPIN_ROW(row), lo, hi);
            gtk_adjustment_set_step_increment(adw_spin_row_get_adjustment(ADW_SPIN_ROW(row)),
                                              step > 0 ? step : 1);
            adw_spin_row_set_value(ADW_SPIN_ROW(row), msdossession_pref_get_int(st->session, i));
            break;
        }
        case MSDOS_PREF_CHOICE: {
            char *labels = choice_labels(st, i);
            int sel = msdossession_pref_get_choice(st->session, i);
            if (g_strcmp0(labels, st->labels[i]) != 0) {
                char **parts = g_strsplit(labels, "\n", -1);
                guint n = g_strv_length(parts);
                if (n && !*parts[n - 1]) {
                    g_free(parts[n - 1]);
                    parts[n - 1] = NULL;
                }
                GtkStringList *model = gtk_string_list_new((const char *const *)parts);
                adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
                g_object_unref(model);
                g_strfreev(parts);
                g_free(st->labels[i]);
                st->labels[i] = labels;
            } else {
                g_free(labels);
            }
            adw_combo_row_set_selected(ADW_COMBO_ROW(row), sel >= 0 ? (guint)sel : 0);
            break;
        }
        case MSDOS_PREF_TEXT:
            /* Set once: later refreshes must not undo text being typed. */
            if (!g_object_get_data(G_OBJECT(row), "msdos-text-set")) {
                char text[256];
                msdossession_pref_get_text(st->session, i, text, sizeof text);
                gtk_editable_set_text(GTK_EDITABLE(row), text);
                g_object_set_data(G_OBJECT(row), "msdos-text-set", GINT_TO_POINTER(1));
            }
            break;
        default:
            break;
        }
    }
    for (int r = 0; r < msdossession_rom_item_count() && r < MAX_ROM_ROWS; r++)
        if (st->rom_rows[r])
            adw_action_row_set_subtitle(ADW_ACTION_ROW(st->rom_rows[r]),
                                        msdossession_rom_item_available(st->session, r)
                                            ? "Imported" : "Not imported");
    st->syncing = FALSE;
}

/* ---- the ROMs page -------------------------------------------------------------- */

static void on_rom_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    PrefsState *st = ud;
    gboolean folder = GPOINTER_TO_INT(g_object_get_data(src, "msdos-folder"));
    g_autoptr(GFile) f = folder
        ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL)
        : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = f ? g_file_get_path(f) : NULL;
    char msg[2048];
    if (!path)
        return;
    msdossession_rom_import(st->session, path, msg, sizeof msg);
    msdos_window_toast(st->window, msg);
    msdos_window_settings_changed(st->window);
    refresh_rows(st);
}

static void on_rom_import(GtkButton *b, gpointer ud)
{
    PrefsState *st = ud;
    gboolean folder = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "msdos-folder"));
    GtkFileDialog *dlg = gtk_file_dialog_new();
    g_object_set_data(G_OBJECT(dlg), "msdos-folder", GINT_TO_POINTER(folder));
    gtk_file_dialog_set_title(dlg, folder ? "Import BIOS from a Folder" : "Import BIOS");
    if (folder)
        gtk_file_dialog_select_folder(dlg, GTK_WINDOW(st->window), NULL, on_rom_chosen, st);
    else
        gtk_file_dialog_open(dlg, GTK_WINDOW(st->window), NULL, on_rom_chosen, st);
    g_object_unref(dlg);
}

static AdwPreferencesPage *build_roms_page(PrefsState *st)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *group = NULL;
    const char *last = NULL;
    char desc[1200];

    adw_preferences_page_set_title(page, "ROMs");
    adw_preferences_page_set_name(page, "ROMs");
    adw_preferences_page_set_icon_name(page, "media-flash-symbolic");
    for (int r = 0; r < msdossession_rom_item_count() && r < MAX_ROM_ROWS; r++) {
        const msdos_rom_item *item = msdossession_rom_item(r);
        GtkWidget *row;
        if (!last || strcmp(last, item->group) != 0) {
            group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(group, item->group);
            if (!last) {
                GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                GtkWidget *files = gtk_button_new_with_label("Import\xe2\x80\xa6");
                GtkWidget *dir = gtk_button_new_with_label("Import a Folder\xe2\x80\xa6");
                g_snprintf(desc, sizeof desc,
                    "The machines' BIOS chips and the cards' ROMs are not part of the app. "
                    "Import them from files, a zip or a folder (an 86Box ROM set works); "
                    "they are recognised by their checksums. Kept in %s.",
                    msdossession_roms_path(st->session));
                adw_preferences_group_set_description(group, desc);
                g_object_set_data(G_OBJECT(dir), "msdos-folder", GINT_TO_POINTER(1));
                g_signal_connect(files, "clicked", G_CALLBACK(on_rom_import), st);
                g_signal_connect(dir, "clicked", G_CALLBACK(on_rom_import), st);
                gtk_widget_set_valign(files, GTK_ALIGN_CENTER);
                gtk_widget_set_valign(dir, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(box), files);
                gtk_box_append(GTK_BOX(box), dir);
                gtk_box_append(GTK_BOX(box),
                    gtk_link_button_new_with_label(MSDOSSESSION_ROMS_URL, "Get ROMs"));
                gtk_box_append(GTK_BOX(box), msdos_help_button("prefs/ROMs"));
                adw_preferences_group_set_header_suffix(group, box);
            }
            adw_preferences_page_add(page, group);
            last = item->group;
        }
        row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), item->name);
        adw_preferences_group_add(group, row);
        st->rom_rows[r] = row;
    }
    return page;
}

/* ---- the dialog --------------------------------------------------------------------- */

static void on_closed(AdwDialog *dlg, gpointer ud)
{
    PrefsState *st = ud;
    int did;
    (void)dlg;
    msdossession_settings_flush(st->session);
    did = st->pending > MSDOS_APPLY_LIVE ? msdossession_apply_settings(st->session) : 0;
    msdos_window_settings_changed(st->window);
    if (did == MSDOS_APPLY_RESET)
        msdos_window_toast(st->window, "Reset with the new configuration");
    else if (did == MSDOS_APPLY_RESTART)
        msdos_window_toast(st->window, "FujiNet restarted");
    for (int i = 0; i < MAX_ROWS; i++)
        g_free(st->labels[i]);
    g_free(st);
}

void msdos_prefs_show(MsdosWindow *parent, msdossession *session)
{
    PrefsState *st = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dlg = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page = NULL;
    AdwPreferencesGroup *group = NULL;
    const char *last_page = NULL, *last_group = NULL;

    st->window = parent;
    st->session = session;
    st->dialog = ADW_DIALOG(dlg);
    adw_dialog_set_title(ADW_DIALOG(dlg), "Preferences");
    adw_preferences_dialog_set_search_enabled(dlg, TRUE);

    for (int i = 0; i < msdossession_pref_count() && i < MAX_ROWS; i++) {
        const msdos_pref *p = msdossession_pref(i);
        if (!last_page || strcmp(last_page, p->page) != 0) {
            page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
            adw_preferences_page_set_title(page, p->page);
            adw_preferences_page_set_name(page, p->page);
            adw_preferences_page_set_icon_name(page, page_icon(p->page));
            adw_preferences_dialog_add(dlg, page);
            last_page = p->page;
            last_group = NULL;

        }
        if (!last_group || strcmp(last_group, p->group) != 0) {
            group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(group, p->group);
            if (!last_group) {
                char ctx[64];
                g_snprintf(ctx, sizeof ctx, "prefs/%s", p->page);
                adw_preferences_group_set_header_suffix(group, msdos_help_button(ctx));
            }
            if (!strcmp(p->page, "Machine") && !strcmp(p->group, "Machine"))
                adw_preferences_group_set_description(group,
                    "The machine's settings apply together when Preferences closes: the PC "
                    "resets once.");
            adw_preferences_page_add(page, group);
            last_group = p->group;
        }
        st->rows[i] = make_row(st, i);
        adw_preferences_group_add(group, st->rows[i]);
    }
    adw_preferences_dialog_add(dlg, build_roms_page(st));
    refresh_rows(st);

    g_signal_connect(dlg, "closed", G_CALLBACK(on_closed), st);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(parent));
}
