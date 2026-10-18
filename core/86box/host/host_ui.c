/*
 * The 86Box ui.h / video side of the embedded host: no window of its own.
 * Pictures, drive activity and messages go to PcHost through
 * msdos_host_callbacks; the blit lock is ours.
 *
 * Derived in part from 86Box src/android/android_ui.c and src/unix/sdl_ui.c
 * (GPL-2.0-or-later).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/thread.h>
#include <86box/ui.h>
#include <86box/version.h>
#include <86box/video.h>

#include "msdos_host.h"

/* Globals 86Box expects a frontend to define. */
int          rctrl_is_lalt   = 0;
int          update_icons    = 0;
int          kbd_req_capture = 0;
int          hide_status_bar = 1;
int          hide_tool_bar   = 1;
int          fixed_size_x    = 640;
int          fixed_size_y    = 480;
int          mouse_capture   = 0;
bool         fast_forward    = false;
volatile int cpu_thread_run  = 1;
int          slirp_card_num  = 0;   /* no SLiRP in the embedded build */

static msdos_host_callbacks cb;

void
msdos_host_set_callbacks(const msdos_host_callbacks *c)
{
    if (c)
        cb = *c;
    else
        memset(&cb, 0, sizeof(cb));
}

/* ---- messages ----------------------------------------------------------- */

int
ui_msgbox_header(int flags, char *header, char *message)
{
    if (!header)
        header = (flags & MBX_FATAL) ? "Fatal error" : (flags & MBX_ERROR) ? "Error" : EMU_NAME;
    if (cb.message)
        cb.message(cb.user, flags, header, message ? message : "");
    else
        fprintf(stderr, "86Box: %s: %s\n", header, message ? message : "");
    /* A question's default; nobody is there to answer it. */
    return 0;
}

int
ui_msgbox(int flags, char *message)
{
    return ui_msgbox_header(flags, NULL, message);
}

/* Hardware with missing ROMs: drop it and carry on, saying what went. */
int
ui_confirm_unsupported_hardware(const ui_unsupported_hardware_t *items, int count, int machine_missing)
{
    char   text[1024];
    size_t len = 0;

    if (count <= 0)
        return 1;
    text[0] = '\0';
    for (int i = 0; i < count && len < sizeof(text) - 1; i++) {
        int n = snprintf(text + len, sizeof(text) - len, "%s%s", i ? ", " : "", items[i].name);
        if (n > 0)
            len = ((size_t) n < sizeof(text) - len) ? len + (size_t) n : sizeof(text) - 1;
    }
    if (cb.message)
        cb.message(cb.user, MBX_WARNING,
                   machine_missing ? "Missing BIOS" : "Missing ROMs", text);
    return 1;
}

void
ui_progress_wait(const char *message, int total, int (*poll)(void *arg), void *arg)
{
    (void) message;
    while (poll(arg) < total)
        plat_delay_ms(50);
}

/* ---- status bar: only drive activity matters to us ---------------------- */

void
ui_sb_update_icon(int tag, int active)
{
    if (cb.activity)
        cb.activity(cb.user, tag, active);
}

void
ui_sb_update_icon_write(int tag, int active)
{
    if (cb.activity)
        cb.activity(cb.user, tag, active);
}

void ui_sb_update_icon_state(int tag, int state) { (void) tag; (void) state; }
void ui_sb_update_icon_wp(int tag, int state) { (void) tag; (void) state; }
void ui_sb_update_tip(int meaning) { (void) meaning; }
void ui_sb_update_panes(void) { }
void ui_sb_update_text(void) { }
void ui_sb_set_text(char *str) { (void) str; }
void ui_sb_bugui(char *str) { (void) str; }
void ui_sb_set_ready(int ready) { (void) ready; }
void ui_sb_mt32lcd(char *str) { (void) str; }
void ui_emu_status(int speed_percent) { (void) speed_percent; }
void ui_hard_reset_completed(void) { }
void ui_update_force_interpreter(void) { }
void ui_init_monitor(int monitor_index) { (void) monitor_index; }
void ui_deinit_monitor(int monitor_index) { (void) monitor_index; }

static char window_title[512] = EMU_NAME;

char *
ui_window_title(char *str)
{
    if (!str)
        return window_title;
    strncpy(window_title, str, sizeof(window_title) - 1);
    window_title[sizeof(window_title) - 1] = '\0';
    return str;
}

/* fatal() calls this, then exit(): the message has already gone out. */
void
do_stop(void)
{
    cpu_thread_run = 0;
}

/* ---- video -------------------------------------------------------------- */

static void
host_blit(int x, int y, int w, int h, int monitor_index)
{
    if (w > 0 && h > 0 && buffer32 && cb.frame)
        cb.frame(cb.user, &buffer32->line[y][x], buffer32->w, w, h);
    video_blit_complete_monitor(monitor_index);
}

void
plat_resize(int w, int h, int monitor_index)
{
    (void) w;
    (void) h;
    (void) monitor_index;
}

void
plat_resize_request(int w, int h, int monitor_index)
{
    plat_resize(w, h, monitor_index);
}

int
plat_vidapi(const char *api)
{
    (void) api;
    return 0;
}

void
plat_mouse_capture(int on)
{
    mouse_capture = on;
}

/* ---- the blit lock: held by pc_run() while the CPU runs ----------------- */

static mutex_t *blit_mutex;

void
msdos_host_blit_init(void)
{
    /* A fresh one each start: a stopped session can leave the old one held. */
    blit_mutex = thread_create_mutex();
    video_setblit(host_blit);
}

void
startblit(void)
{
    if (blit_mutex)
        thread_wait_mutex(blit_mutex);
}

void
endblit(void)
{
    if (blit_mutex)
        thread_release_mutex(blit_mutex);
}

/* ---- audio: host_audio.c hands its buffers here ------------------------- */

void
msdos_host_deliver_audio(int stream, const void *buf, int frames, int is_float, int rate)
{
    if (cb.audio)
        cb.audio(cb.user, stream, buf, frames, is_float, rate);
}
