/*
 * The session's private state, shared by the core's C and C++ files.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_SESSION_INTERNAL_H
#define MSDOS_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>

#include "msdossession.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MSDOS_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct msdossession {
    char config_dir[MSDOS_PATH_MAX];
    char data_dir[MSDOS_PATH_MAX];
    char media_dir[MSDOS_PATH_MAX];         /* blank disks the user makes */
    char roms_dir[MSDOS_PATH_MAX];          /* an 86Box ROM tree */
    char vm_dir[MSDOS_PATH_MAX];            /* 86box.cfg, nvr/ */
    char disks_dir[MSDOS_PATH_MAX];         /* the FujiNet boot floppies */
    char settings_file[MSDOS_PATH_MAX];
    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;
    char last_error[256];

    char fujinet_root[MSDOS_PATH_MAX];      /* <data>/fujinet */
    char fujinet_config[MSDOS_PATH_MAX];    /* .../fnconfig.ini */
    char fujinet_sd[MSDOS_PATH_MAX];        /* .../SD */
    char fujinet_data[MSDOS_PATH_MAX];      /* .../data */
    char fujinet_lib[MSDOS_PATH_MAX];       /* resolved libfujinet, "" until then */
    char fujinet_runtime_src[MSDOS_PATH_MAX];
    char webui_url[64];
    int  fujinet_running;
    int  fujinet_wanted;                    /* FujiNet enabled for this run */
    int  fujinet_disabled;                  /* the caller said no FujiNet ("" lib) */

    msdossession_start_opts opts;
    int running;

    void *impl;             /* session.cpp's SessionImpl */
    void *audio;            /* audio_sdl.c, NULL until started */
    void *gamepad;          /* gamepad_sdl.c, NULL until started */
    void *debugger;         /* msdosdebug, lazily created */

    pthread_mutex_t pad_event_mtx;
    char pad_event[128];
};

void settings_init(struct msdossession *s);
void settings_free_all(struct msdossession *s);

int  paths_init(struct msdossession *s, const char *config_dir, const char *data_dir);
int  paths_provision_fujinet(struct msdossession *s);
int  paths_mkdir_p(const char *path);

void session_set_error(struct msdossession *s, const char *fmt, ...);

/* machines.c: the seven profiles and what 86Box offers on each */
typedef struct {
    const char *name;           /* "IBM 5160 PC/XT" */
    const char *internal;       /* 86Box machine */
    const char *rom_item;       /* rom_table key of its BIOS */
    const char *cpu_family;     /* default */
    uint32_t    cpu_speed;      /* default, Hz */
    int         ram_kb;         /* default */
    int         ram_min, ram_max, ram_step;   /* what we offer */
    int         board_ram_max;  /* above it, an ISA memory card */
    const char *video;          /* default card */
    const char *sound;
    const char *fdd_a, *fdd_b;  /* default drive types */
    int         xt_hdc;         /* offers the XT disk controllers */
    int         at_ide;         /* AT: ide_isa unless the board has IDE */
    int         pcjr;
    int         eight_bit;      /* 8-bit ISA only */
} msdos_profile;

const msdos_profile *msdos_profile_get(int m);
/* The 86Box machine a profile runs: its own, except that with
 * MSDOS_TEST_GLABIOS set (tests only) the 5150 and 5160 run 86Box's
 * "glabios" XT, whose GPL BIOS CI can carry. */
const char *msdos_profile_machine86(int m);
int msdos_test_glabios(int m);

typedef struct {
    char family[32];            /* 86Box internal name */
    uint32_t speed;             /* rspeed */
    double multi;
    char label[64];             /* "Intel 486DX2 66 MHz" */
} msdos_cpu_choice;
/* The CPUs we offer on a machine (typical families, every speed 86Box
 * allows there). Returns the count. */
int msdos_cpu_choices(int m, msdos_cpu_choice *out, int max);

/* prefs.c */
int  prefs_machine(struct msdossession *s);
const char *prefs_value(struct msdossession *s, const char *key);   /* resolved */
int  prefs_int(struct msdossession *s, const char *key);
int  prefs_cpu(struct msdossession *s, msdos_cpu_choice *out);
int  prefs_ram_kb(struct msdossession *s);
int  prefs_fpu(struct msdossession *s);
const char *prefs_cpu_family_fpu(struct msdossession *s);
const char *prefs_video(struct msdossession *s);
const char *prefs_sound(struct msdossession *s);
const char *prefs_midi(struct msdossession *s);
const char *prefs_fdd_type(struct msdossession *s, int drive);
const char *prefs_hdc(struct msdossession *s);
int  prefs_hd_type(struct msdossession *s, int drive, int *spt, int *heads, int *cyls,
                   const char **bus);
int  prefs_fujinet_com(struct msdossession *s);
int  prefs_fujinet_bps(struct msdossession *s);
const char *prefs_mouse(struct msdossession *s);
const char *prefs_joystick(struct msdossession *s);
/* A signature of everything a hard reset takes in. */
void prefs_reset_signature(struct msdossession *s, char *out, int outsz);

/* cfg86.c: 86box.cfg from the settings and the drives */
int  cfg86_write(struct msdossession *s, const char *path);

/* roms.c */
int  roms_item_index(const char *key);
int  roms_item_complete(struct msdossession *s, int item, int *variant);
uint32_t roms_crc32(const uint8_t *p, size_t n, uint32_t crc);

/* bootdisk.c: the FujiNet boot floppy for a drive type, CONFIG.SYS up to date */
int  bootdisk_path(struct msdossession *s, const char *fdd_type, char *out, int outsz);

/* media.cpp: the drives' contents, kept in the settings */
const char *media_floppy(struct msdossession *s, int drive, int *read_only);
const char *media_hd(struct msdossession *s, int drive);

/* fujinet_runtime.c */
int  session_fujinet_available(struct msdossession *s);
int  session_fujinet_start(struct msdossession *s);
void session_fujinet_stop(struct msdossession *s);
void session_fujinet_restart_for_power(struct msdossession *s);
int  session_fujinet_wait_link(struct msdossession *s, int timeout_ms);
int  session_fujinet_wait_listener(struct msdossession *s, int timeout_ms);
void session_fujinet_mix(struct msdossession *s, float *stereo, int frames, float gain);
void session_fujinet_link_lost(struct msdossession *s);

/* audio_sdl.c, gamepad_sdl.c */
int  audio_start(struct msdossession *s);
void audio_stop(struct msdossession *s);
int  gamepad_start(struct msdossession *s);
void gamepad_stop(struct msdossession *s);
void session_gamepad_stick(struct msdossession *s, int stick, int x, int y, unsigned buttons,
                           int present);
void session_gamepad_event(struct msdossession *s, const char *text);

#ifdef __cplusplus
}
#endif

#endif
