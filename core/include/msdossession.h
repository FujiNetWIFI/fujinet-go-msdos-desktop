/*
 * msdossession -- the toolkit-agnostic desktop session for FujiNet Go MS-DOS.
 *
 * Owns the emulator (86Box on a thread of its own, core/86box), the SDL
 * audio and gamepad backends, the in-process FujiNet runtime, the settings
 * store, imported ROMs, the FujiNet boot floppy and the drives. Frontends
 * (GTK4, Qt6, AppKit, Win32) do windowing, painting and event translation
 * only.
 *
 * 86Box can be brought up once per process: changing the machine is a hard
 * reset with the new configuration, not a restart.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOSSESSION_H
#define MSDOSSESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest picture: SVGA at 1024x768 with room to spare. */
#define MSDOSSESSION_FB_MAX_WIDTH  2048
#define MSDOSSESSION_FB_MAX_HEIGHT 1536

/* Where the 86Box ROM set, which Import BIOS reads, is downloaded. */
#define MSDOSSESSION_ROMS_URL "https://github.com/86Box/roms/releases"

/* FujiBus over SLIP (TCP; FujiNet listens, 86Box's COM device connects) and
 * the FujiNet web UI. Siblings use 11500-11515. */
#define MSDOSSESSION_BOIP_PORT  11516
#define MSDOSSESSION_WEBUI_PORT 11517

#define MSDOSSESSION_AUDIO_RATE 48000

/* Highlights (status dot, held keys, debugger PC line): the silver of the
 * IBM badge. A fill colour; text on it stays dark. */
#define MSDOSSESSION_ACCENT_RGB 0xA8A8A6

typedef struct msdossession msdossession;
typedef struct msdosdebug msdosdebug;

/* All optional (NULL = default).
 *  config_dir   default $XDG_CONFIG_HOME/fujinet-go-msdos (%APPDATA%)
 *  data_dir     default $XDG_DATA_HOME/fujinet-go-msdos (%LOCALAPPDATA%)
 *  fujinet_lib  libfujinet path; "" disables FujiNet
 *  fujinet_runtime_src  pristine fnconfig.ini + data/ + SD/ (macOS bundle) */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} msdossession_paths;

msdossession *msdossession_new(const msdossession_paths *paths);
void msdossession_free(msdossession *s);

/* ---- settings ------------------------------------------------------------ */
int         msdossession_get_int(msdossession *s, const char *key, int def);
void        msdossession_set_int(msdossession *s, const char *key, int value);
const char *msdossession_get_str(msdossession *s, const char *key, const char *def);
void        msdossession_set_str(msdossession *s, const char *key, const char *value);
void        msdossession_settings_flush(msdossession *s);

/* ---- the machines ---------------------------------------------------------- */
typedef enum {
    MSDOS_M_5150 = 0, MSDOS_M_5160, MSDOS_M_PCJR, MSDOS_M_286, MSDOS_M_386,
    MSDOS_M_486, MSDOS_M_PENTIUM,
    MSDOS_M_COUNT
} msdos_machine;
const char *msdos_machine_name(int m);          /* "IBM 5160 PC/XT" */
int msdossession_machine(msdossession *s);      /* the selected one */
/* Its BIOS is imported. */
int msdossession_machine_available(msdossession *s, int m);
/* Any machine can start. */
int msdossession_has_bios(msdossession *s);

/* ---- Preferences, as a table every frontend builds its dialog from -------
 * Each machine is a profile: most values live in the settings store under
 * "key.<machine>" (msdossession_pref_key), the app's own under .key. After
 * changing any, call msdossession_apply_settings. */
typedef enum {
    MSDOS_PREF_BOOL = 0, MSDOS_PREF_CHOICE, MSDOS_PREF_INT, MSDOS_PREF_FILE,
    MSDOS_PREF_TEXT         /* a line of text, a string setting */
} msdos_pref_type;

/* When a change takes effect. */
typedef enum {
    MSDOS_APPLY_LIVE = 0,   /* at once */
    MSDOS_APPLY_RESET,      /* a hard reset with the new configuration */
    MSDOS_APPLY_RESTART     /* the FujiNet runtime restarts */
} msdos_apply;

typedef struct {
    const char *key;        /* settings key */
    const char *page;       /* "Machine", "Display", "Sound", "Storage",
                               "Ports", "FujiNet" */
    const char *group;      /* group title on the page */
    const char *label;
    const char *help;       /* subtitle; may be NULL */
    int type;               /* msdos_pref_type */
    int def;                /* BOOL/CHOICE/INT default */
    int min, max, step;     /* INT */
    int applies;            /* msdos_apply */
} msdos_pref;

int msdossession_pref_count(void);
const msdos_pref *msdossession_pref(int i);
int msdossession_pref_find(const char *key);    /* index or -1 */
/* The settings key pref i is stored under on the selected machine. */
int msdossession_pref_key(msdossession *s, int i, char *out, int outsz);
/* CHOICE labels depend on the machine (CPUs, RAM, cards): label c of pref i,
 * or NULL past the end. */
const char *msdossession_pref_choice(msdossession *s, int i, int c);
int  msdossession_pref_get_choice(msdossession *s, int i);
void msdossession_pref_set_choice(msdossession *s, int i, int c);
/* BOOL and INT values: always through these (some are per machine). */
int  msdossession_pref_get_int(msdossession *s, int i);
void msdossession_pref_set_int(msdossession *s, int i, int v);
/* INT bounds that depend on the machine (RAM). */
void msdossession_pref_int_range(msdossession *s, int i, int *min, int *max, int *step);
/* TEXT values: always through these. The text is trimmed; invalid text is not
 * to be saved (msdossession_pref_text_valid says which is which). */
void msdossession_pref_get_text(msdossession *s, int i, char *out, int outsz);
void msdossession_pref_set_text(msdossession *s, int i, const char *text);
int  msdossession_pref_text_valid(int i, const char *text);
/* 0 = shown greyed (not on this machine, ROM not imported, ...). */
int msdossession_pref_enabled(msdossession *s, int i);
int msdossession_pref_choice_enabled(msdossession *s, int i, int c);

/* Applies every changed setting; returns the highest msdos_apply it needed. */
int msdossession_apply_settings(msdossession *s);

/* ---- lifecycle ----------------------------------------------------------- */
typedef struct {
    int enable_fujinet;     /* start the in-process FujiNet runtime */
    int enable_audio;       /* open the SDL audio device */
    int enable_gamepad;     /* start the SDL gamepad thread */
} msdossession_start_opts;

void msdossession_default_opts(msdossession *s, msdossession_start_opts *opts);
/* Fails (with last_error) when no BIOS is imported; start again after an
 * import. Once 86Box has run, stop is final for this process. */
int  msdossession_start(msdossession *s, const msdossession_start_opts *opts);
void msdossession_stop(msdossession *s);
int  msdossession_is_running(const msdossession *s);
const char *msdossession_last_error(const msdossession *s);

/* ---- the machine's buttons --------------------------------------------------- */
int  msdossession_reset(msdossession *s);           /* the reset button */
int  msdossession_ctrl_alt_del(msdossession *s);    /* the three-finger salute */
void msdossession_set_paused(msdossession *s, int on);
int  msdossession_paused(msdossession *s);
void msdossession_set_warp(msdossession *s, int on);
int  msdossession_warp(msdossession *s);
int  msdossession_speed_percent(msdossession *s);

/* ---- video --------------------------------------------------------------- */
/* XRGB8888 into dst (FB_MAX_WIDTH * FB_MAX_HEIGHT), its size and the pixel
 * aspect x1000 the frontend should draw it with (4:3 on, or 1000 square).
 * Returns 1 if a new frame was copied; *serial 0 forces a copy. */
int  msdossession_copy_frame(msdossession *s, uint32_t *dst, int *width, int *height,
                             int *par_milli, uint64_t *serial_inout);
/* View options the frontends paint with. */
int  msdossession_integer_scale(msdossession *s);
int  msdossession_smooth(msdossession *s);

/* ---- audio --------------------------------------------------------------- */
int  msdossession_render_audio(msdossession *s, float *stereo, int nframes);
void msdossession_set_volume(msdossession *s, int percent);

/* ---- the keyboard ------------------------------------------------------------
 * Keys are USB HID keyboard usages (0x04 'A' ... 0xE7 right GUI): the PC's
 * keyboard is positional, so position is what travels. Returns 1 if the PC
 * took the key. */
int  msdossession_key(msdossession *s, uint32_t hid_usage, int down);
/* Every key up (focus lost). */
void msdossession_release_keys(msdossession *s);
uint32_t msdossession_hid_from_win_scancode(unsigned scancode, int extended);
uint32_t msdossession_hid_from_evdev(unsigned code);
uint32_t msdossession_hid_from_macos_keycode(unsigned keycode);
const char *msdossession_hid_name(uint32_t usage);  /* "Left Ctrl"; NULL unknown */

/* The on-screen keyboard (the machine's own layout), drawn by every frontend
 * from this table. */
typedef struct {
    const char *label;      /* "Esc", "F1", "\xe2\x86\x90" ... (UTF-8) */
    const char *shifted;    /* the shifted legend, or NULL */
    uint32_t hid;           /* what it presses */
    float x, y, w, h;       /* key units from the top left */
} msdos_kbd_key;
int  msdossession_keyboard_layout(msdossession *s, const msdos_kbd_key **keys);
void msdossession_keyboard_press(msdossession *s, int index, int down);
int  msdossession_keyboard_held(msdossession *s, int index);
/* Lock lights: bit 0 Num, 1 Caps, 2 Scroll. */
unsigned msdossession_lock_lights(msdossession *s);

/* Type text as keystrokes, paced by the BIOS keyboard buffer. */
int  msdossession_paste(msdossession *s, const char *utf8);
void msdossession_paste_cancel(msdossession *s);
int  msdossession_paste_pending(msdossession *s);
/* The text screen as UTF-8 lines; returns its length, -1 in graphics. */
int  msdossession_copy_screen_text(msdossession *s, char *dst, int dstsz);

/* ---- the mouse --------------------------------------------------------------- */
/* A mouse is fitted (Preferences > Ports): frontends offer to capture. */
int  msdossession_mouse_fitted(msdossession *s);
void msdossession_mouse_motion(msdossession *s, int dx, int dy);
void msdossession_mouse_buttons(msdossession *s, unsigned buttons);  /* 1 L, 2 R, 4 M */
void msdossession_mouse_wheel(msdossession *s, int dz);

/* ---- the game port ------------------------------------------------------------ */
#define MSDOS_JOYSTICKS 2
/* What the game port has (Preferences > Ports): 0 none. */
int  msdossession_joystick_fitted(msdossession *s);
/* A joystick from the host (the Controllers window): axes -32768..32767,
 * buttons bit 0..3. */
void msdossession_joystick(msdossession *s, int stick, int x, int y, unsigned buttons);

/* ---- gamepads (hot-plugged; each drives a joystick) ----------------------- */
int  msdossession_gamepad_count(msdossession *s);
int  msdossession_gamepad_name(msdossession *s, int idx, char *dst, int dstsz);
void msdossession_gamepad_assign(msdossession *s, int idx, int stick); /* -1 auto */
int  msdossession_gamepad_assignment(msdossession *s, int idx);
int  msdossession_gamepad_effective_stick(msdossession *s, int idx);
unsigned msdossession_gamepad_generation(msdossession *s);
int  msdossession_gamepad_last_event(msdossession *s, char *dst, int dstsz);
/* What each joystick looks like right now (the Controllers window). */
void msdossession_joystick_state(msdossession *s, int stick, int *x, int *y, unsigned *buttons);

/* ---- media ------------------------------------------------------------------- */
typedef enum {
    MSDOS_MEDIA_UNKNOWN = 0, MSDOS_MEDIA_FLOPPY, MSDOS_MEDIA_HARD_DISK,
    MSDOS_MEDIA_ROM          /* a BIOS or a ROM set to import */
} msdos_media_kind;
int msdossession_media_kind(const char *path);

/* Open anything: a floppy into A:, a hard disk (asks: hd_needs_drive), a
 * BIOS to import, anything else onto FujiNet's SD card. msg says what
 * happened. Returns 0, -1, or 1 for a hard disk whose drive the user must
 * pick (then hd_attach). */
int msdossession_open(msdossession *s, const char *path, char *msg, int msgsz);

/* Floppy drives A: and B:. */
#define MSDOS_FLOPPIES 2
typedef struct {
    char path[1024];        /* "" = empty */
    char type[48];          /* "5.25\" 360K" */
    int  present;           /* the machine has this drive */
    int  read_only;
    int  boot_disk;         /* it is the FujiNet boot floppy */
    int  active;            /* the light is on */
} msdos_floppy_info;
int msdossession_floppy_info(msdossession *s, int drive, msdos_floppy_info *out);
int msdossession_floppy_insert(msdossession *s, int drive, const char *path, int read_only,
                               char *msg, int msgsz);
int msdossession_floppy_eject(msdossession *s, int drive);
int msdossession_floppy_set_read_only(msdossession *s, int drive, int ro);
/* The FujiNet boot floppy, sized for the drive. */
int msdossession_floppy_boot_disk(msdossession *s, int drive);
/* A blank formatted image of the drive's own size. */
int msdossession_new_blank_floppy(msdossession *s, int drive, const char *path,
                                  char *msg, int msgsz);

/* Hard disks C: and D:: the image takes effect with a reset. */
#define MSDOS_HARD_DISKS 2
typedef struct {
    char path[1024];        /* "" = none */
    char type[64];          /* "ST-225 (20 MB)" */
    int  present;           /* the machine has a controller for it */
    int  active;
} msdos_hd_info;
int msdossession_hd_info(msdossession *s, int drive, msdos_hd_info *out);
int msdossession_hd_attach(msdossession *s, int drive, const char *path, char *msg, int msgsz);
int msdossession_hd_detach(msdossession *s, int drive);
/* A blank image of the drive type in Preferences (.img raw, .vhd). */
int msdossession_new_blank_hd(msdossession *s, int drive, const char *path,
                              char *msg, int msgsz);

/* Copies into the FujiNet SD folder (archives unpacked). */
int msdossession_import_to_sd(msdossession *s, const char *src_path,
                              char *dest_out, int dest_sz);

/* ---- ROMs --------------------------------------------------------------------- */
/* Everything the offered machines and cards can use, grouped. */
typedef struct {
    const char *name;       /* "IBM 5160 PC/XT BIOS (1986)" */
    const char *group;      /* "Machines", "Video", "Sound", "Disk controllers" */
} msdos_rom_item;
int msdossession_rom_item_count(void);
const msdos_rom_item *msdossession_rom_item(int i);
int msdossession_rom_item_available(msdossession *s, int i);
/* Imports BIOS images from a file, a zip or a folder (an 86Box ROM set
 * works): every file that matches one 86Box needs. msg lists what came in. */
int msdossession_rom_import(msdossession *s, const char *path, char *msg, int msgsz);

/* ---- FujiNet ------------------------------------------------------------------- */
/* Parses the "Use external FujiNet" address field: "host:port" (host a name or
 * an IPv4 literal) or a bare "port". Empty text means the default,
 * 127.0.0.1:11516. Returns 0 and fills host/port, or -1 when the text is not
 * an address:port (nothing is written then). */
int         msdossession_parse_hostport(const char *text, char *host, int hostsz,
                                        int *port);
int         msdossession_fujinet_running(const msdossession *s);
/* 1 when the machine's FujiNet is another instance (the built-in one is not
 * started). */
int         msdossession_fujinet_external(const msdossession *s);
/* "host:port" the FujiNet COM device connects to. */
const char *msdossession_boip_endpoint(const msdossession *s);
int         msdossession_fujinet_link(msdossession *s);   /* the COM device is connected */
const char *msdossession_fujinet_webui_url(const msdossession *s);
int         msdossession_fujinet_copy_log(msdossession *s, char *dst, int max);
int         msdossession_fujinet_restart(msdossession *s);
unsigned    msdossession_fujinet_generation(msdossession *s);
int         msdossession_fujinet_last_event(msdossession *s, char *dst, int dstsz);
/* The COM port FujiNet is on: 1-4. */
int         msdossession_fujinet_com(msdossession *s);

/* ---- paths, status and the debugger ----------------------------------------- */
const char *msdossession_config_path(const msdossession *s);
const char *msdossession_data_path(const msdossession *s);
const char *msdossession_roms_path(const msdossession *s);
const char *msdossession_media_path(const msdossession *s);
const char *msdossession_sd_path(const msdossession *s);

/* A short status line: machine, FujiNet COM port and link, speed. */
int msdossession_status(msdossession *s, char *dst, int dstsz);
/* Something the user should hear about (one-shot); 0 if none. */
int msdossession_take_notice(msdossession *s, char *dst, int dstsz);

msdosdebug *msdossession_debugger(msdossession *s);

#ifdef __cplusplus
}
#endif

#endif /* MSDOSSESSION_H */
