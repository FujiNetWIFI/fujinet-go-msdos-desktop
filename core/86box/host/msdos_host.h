/*
 * The embedded 86Box's side of the bridge to PcHost: what the platform
 * layer (host_*.c) hands out, and the callbacks it calls.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_HOST_H
#define MSDOS_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio streams 86Box produces, each at its own rate. */
enum {
    MSDOS_AUDIO_MAIN = 0,   /* 48000: cards, PC speaker */
    MSDOS_AUDIO_MUSIC,      /* MUSIC_FREQ: MT-32 (munt) */
    MSDOS_AUDIO_WT,         /* 44100: wavetable daughterboards */
    MSDOS_AUDIO_CD,         /* 44100: CD audio */
    MSDOS_AUDIO_FDD,        /* 48000: floppy drive sounds */
    MSDOS_AUDIO_HDD,        /* 48000: hard disk sounds */
    MSDOS_AUDIO_STREAMS
};

typedef struct {
    void *user;
    /* A finished picture, w x h at (x, y) of a row-major buffer whose rows
     * are stride pixels apart. On 86Box's blit thread. */
    void (*frame)(void *user, const uint32_t *pixels, int stride, int w, int h);
    /* Interleaved stereo; float or int16. On the CPU thread. */
    void (*audio)(void *user, int stream, const void *buf, int frames, int is_float, int rate);
    /* Drive activity (ui_sb_update_icon): tag is SB_FLOPPY | n, SB_HDD | bus... */
    void (*activity)(void *user, int tag, int active);
    /* A message 86Box wants shown. Never blocks; the answer to a question is
     * always its default. */
    void (*message)(void *user, int flags, const char *header, const char *text);
} msdos_host_callbacks;

void msdos_host_set_callbacks(const msdos_host_callbacks *cb);

/* 86Box's frontends declare this privately. */
char *ui_window_title(char *str);

/* Where 86Box keeps its data and finds ROMs and assets: only these. */
void msdos_host_set_dirs(const char *data_dir, const char *const *rom_dirs, int nrom_dirs);
void msdos_host_data_dir(char *out, size_t len);
void msdos_host_add_rom_paths(void);
void msdos_host_add_asset_paths(void);

/* The blit lock 86Box takes around each pc_run(). */
void msdos_host_blit_init(void);

/* The joysticks the game port sees. Axes -32768..32767; pov in degrees or -1. */
#define MSDOS_HOST_STICKS 4
void msdos_host_joystick_set(int stick, const int axes[4], unsigned buttons, int pov);
void msdos_host_joystick_clear(void);

/* The debugger's hooks (host_debug.c). The CPU thread calls these. */
typedef struct {
    void *user;
    /* After every instruction while installed: return 1 to stop before
     * cs:ip. at_slice_start: called before a slice runs (nothing has
     * executed since the last call), for breakpoints but not for steps. */
    int  (*instruction)(void *user, int at_slice_start);
    /* A watched memory access (linear addresses) or I/O port. */
    void (*mem_access)(void *user, const uint32_t *addrs, int access);
    void (*io_access)(void *user, uint16_t port, int write, int width, uint32_t val);
} msdos_host_debug_hooks;

void msdos_host_debug_install(const msdos_host_debug_hooks *hooks);   /* NULL removes */
void msdos_host_debug_set_stopped(int stopped);
int  msdos_host_debug_stopped(void);
/* The watch bitmaps: set or clear one linear granule / port. */
void msdos_host_debug_watch_page(uint32_t addr, int on);
void msdos_host_debug_watch_io(uint16_t port, int on);
void msdos_host_debug_watch_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* MSDOS_HOST_H */
