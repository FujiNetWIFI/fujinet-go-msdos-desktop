/*
 * msdos_session_run -- the whole session with no window: what the frontends
 * drive, for tests and bring-up.
 *
 *   msdos_session_run [--data DIR] [--config DIR] [--import PATH]...
 *                     [--set key=value]... [--no-fujinet] [--fujinet-lib PATH]
 *                     [--seconds N] [--expect TEXT] [--type TEXT] [--screen-text]
 *                     [--ppm FILE] [--apply-after N key=value]
 *
 * --expect: exit 0 when the text screen shows it, 1 if it never does.
 * --type: typed (paste) once --expect is seen, or after a second.
 * --apply-after N k=v: after N seconds, change a setting and apply it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "msdossession.h"

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD) ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) { usleep((useconds_t) ms * 1000); }
#endif

static double now_s(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: msdos_session_run [--data DIR] [--config DIR] [--import PATH]...\n"
            "       [--set key=value]... [--no-fujinet] [--fujinet-lib PATH] [--seconds N]\n"
            "       [--expect TEXT] [--type TEXT] [--screen-text] [--ppm FILE]\n"
            "       [--apply-after N key=value]\n");
}

static void set_kv(msdossession *s, const char *kv)
{
    char key[128];
    const char *eq = strchr(kv, '=');
    if (!eq || eq - kv >= (int) sizeof key)
        return;
    memcpy(key, kv, (size_t) (eq - kv));
    key[eq - kv] = 0;
    msdossession_set_str(s, key, eq + 1);
}

int main(int argc, char **argv)
{
    const char *data = NULL, *config = NULL, *expect = NULL, *type = NULL, *ppm = NULL;
    const char *fnlib = NULL, *apply_kv = NULL;
    const char *imports[8], *sets[32];
    int nimports = 0, nsets = 0, no_fn = 0, screen = 0;
    double seconds = 30, apply_after = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(), exit(2), ""))
        if (!strcmp(a, "--data")) data = NEXT();
        else if (!strcmp(a, "--config")) config = NEXT();
        else if (!strcmp(a, "--import") && nimports < 8) imports[nimports++] = NEXT();
        else if (!strcmp(a, "--set") && nsets < 32) sets[nsets++] = NEXT();
        else if (!strcmp(a, "--no-fujinet")) no_fn = 1;
        else if (!strcmp(a, "--fujinet-lib")) fnlib = NEXT();
        else if (!strcmp(a, "--seconds")) seconds = atof(NEXT());
        else if (!strcmp(a, "--expect")) expect = NEXT();
        else if (!strcmp(a, "--type")) type = NEXT();
        else if (!strcmp(a, "--screen-text")) screen = 1;
        else if (!strcmp(a, "--ppm")) ppm = NEXT();
        else if (!strcmp(a, "--apply-after")) {
            apply_after = atof(NEXT());
            apply_kv = NEXT();
        } else {
            usage();
            return 2;
        }
    }

    msdossession_paths paths = { config, data, no_fn ? "" : fnlib, NULL };
    msdossession *s = msdossession_new(&paths);
    if (!s) {
        fprintf(stderr, "session: cannot set up its directories\n");
        return 3;
    }
    for (int i = 0; i < nimports; i++) {
        char msg[1024];
        msdossession_rom_import(s, imports[i], msg, sizeof msg);
        fprintf(stderr, "import: %s\n", msg);
    }
    for (int i = 0; i < nsets; i++)
        set_kv(s, sets[i]);

    msdossession_start_opts opts;
    msdossession_default_opts(s, &opts);
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    if (no_fn)
        opts.enable_fujinet = 0;
    if (msdossession_start(s, &opts) != 0) {
        fprintf(stderr, "start: %s\n", msdossession_last_error(s));
        msdossession_free(s);
        return 3;
    }

    static char text[16384];
    const double t0 = now_s();
    int found = expect == NULL, typed = 0, applied = 0;
    while (now_s() - t0 < seconds) {
        sleep_ms(250);
        char notice[512];
        if (msdossession_take_notice(s, notice, sizeof notice))
            fprintf(stderr, "notice: %s\n", notice);
        if (apply_kv && !applied && now_s() - t0 >= apply_after) {
            set_kv(s, apply_kv);
            fprintf(stderr, "apply: %s -> %d\n", apply_kv, msdossession_apply_settings(s));
            applied = 1;
        }
        if (expect && !found && msdossession_copy_screen_text(s, text, sizeof text) >= 0
            && strstr(text, expect)) {
            found = 1;
            if (!type)
                break;
        }
        if (type && !typed && (found || (!expect && now_s() - t0 > 1))) {
            msdossession_paste(s, type);
            typed = 1;
        }
        if (typed && !msdossession_paste_pending(s) && found && expect) {
            sleep_ms(1500);
            break;
        }
    }

    char status[256];
    msdossession_status(s, status, sizeof status);
    fprintf(stderr, "status: %s\n", status);
    if (screen && msdossession_copy_screen_text(s, text, sizeof text) >= 0)
        printf("%s", text);
    if (ppm) {
        static uint32_t px[MSDOSSESSION_FB_MAX_WIDTH * MSDOSSESSION_FB_MAX_HEIGHT];
        int w = 0, h = 0, par = 1000;
        uint64_t serial = 0;
        msdossession_copy_frame(s, px, &w, &h, &par, &serial);
        FILE *f = fopen(ppm, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; i++) {
                unsigned char rgb[3] = { (unsigned char) (px[i] >> 16), (unsigned char) (px[i] >> 8),
                                         (unsigned char) px[i] };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
    }
    msdossession_free(s);
    return found ? 0 : 1;
}
