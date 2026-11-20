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

#include <stddef.h>
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
/* Text on the accent (it is light), and the accent as a fill or text on a dark theme. */
#define MSDOSSESSION_ACCENT_INK_RGB 0x000000
#define MSDOSSESSION_ACCENT_LIGHT_RGB 0xA8A8A6

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
/* As copy_frame, into a dst of max_pixels (at least the frame's w * h; copy
 * only that much). Returns 0 and a width * height > max_pixels, copying
 * nothing and keeping the serial, when the frame does not fit: grow and ask
 * again. With no new frame it returns 0 and leaves width and height alone. */
int  msdossession_copy_frame_n(msdossession *s, uint32_t *dst, size_t max_pixels, int *width,
                               int *height, int *par_milli, uint64_t *serial_inout);
/* cb(user) runs after each new frame, on the thread that publishes frames, under
 * a lock: set(NULL, NULL) returns only after a call in flight, so `user` may be
 * freed after it. Keep cb short and never call set from inside it. A frame
 * identical to the previous one is not published. */
void msdossession_set_frame_callback(msdossession *s, void (*cb)(void *user), void *user);
/* Frames published so far: "the machine is running", unlike the serial, which
 * a test cannot rely on for a still picture. */
uint64_t msdossession_frame_count(msdossession *s);
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
/* A key bound to a game port target (see the bindings below) is taken by the
 * game port and does NOT reach 86Box; msdossession_key then returns 1 too
 * (the key was used). An auto-repeat of a bound key is ignored, and a release
 * lets go of exactly what its press held, whatever was rebound meanwhile. */
/* Every PC key up (focus lost). Leaves the bound keys: see release_keyboard. */
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
/* The same table for machine m (an msdos_machine), whatever is selected: the
 * 5150 and 5160 have the 83-key XT, the PCjr its 62-key Freeboard, the 286
 * and later the 101-key Enhanced. Static; valid for the process. */
int  msdossession_keyboard_layout_for_machine(int machine, const msdos_kbd_key **keys);
/* The on-screen keyboard's keys are the PC's own: a click presses the key
 * whatever the bindings say. index is into msdossession_keyboard_layout. */
void msdossession_keyboard_press(msdossession *s, int index, int down);
int  msdossession_keyboard_held(msdossession *s, int index);
/* Lock lights: bit 0 Num, 1 Caps, 2 Scroll. */
unsigned msdossession_lock_lights(msdossession *s);
/* The set 1 scan code a HID usage becomes (0x100 set for an E0 prefix), or 0
 * for a usage the PC has no key for. The PCjr's keyboard has no E0 codes: its
 * BIOS sees only codes up to 0x54, and 86Box folds the extended ones onto
 * them (the Freeboard's Fn is Right Alt, 0x138). */
unsigned msdossession_hid_to_scancode(uint32_t hid_usage);

/* ---- the keyboard skins (controls.c) -------------------------------------------
 * What the Controllers window draws for each machine's keyboard, in key units
 * (1 = one key pitch) from the top left of the case. Three skins: the Model F
 * XT (5150, 5160), the PCjr Freeboard, and the Model M (286 and later). Items
 * are in PAINT order. A KEYCAP's `key` is its index in
 * msdossession_keyboard_layout (for the same machine): pass it to
 * msdossession_keyboard_press / _held. Colours are ready to paint; a held key
 * is drawn with MSDOS_HELD_RGB. */
typedef enum {
    MSDOS_SKIN_BODY = 0,    /* a drawn part of the case: label says which */
    MSDOS_SKIN_KEYCAP,      /* a key */
    MSDOS_SKIN_LED          /* a lock light: key = bit of msdossession_lock_lights */
} msdos_skin_kind;
typedef enum {
    MSDOS_CAP_LIGHT = 0,    /* alphanumerics, Enter, Space */
    MSDOS_CAP_MODIFIER,     /* Esc, Shift, Ctrl, Alt, F-keys, locks ... the darker caps */
    MSDOS_CAP_CHICLET       /* every PCjr key */
} msdos_cap_class;
typedef enum {
    MSDOS_SHAPE_RECT = 0,
    MSDOS_SHAPE_JOIN_BELOW, /* the top half of the PCjr's L-shaped Enter: no seam at the bottom */
    MSDOS_SHAPE_JOIN_ABOVE  /* its lower half: no seam at the top (same key index) */
} msdos_cap_shape;
typedef struct {
    int kind;                   /* msdos_skin_kind */
    const char *label;          /* KEYCAP: the legend; LED: "Num" "Caps" "Scroll"; BODY: its name
                                   ("Case" first, "Slab", "Badge", "Plate", "Logo", "FnBracket",
                                   "AltBracket") */
    const char *shifted;        /* KEYCAP: the shifted legend or NULL; BODY: its text or NULL */
    const char *fn_legend;      /* KEYCAP on the PCjr: the Fn legend (drawn in alt_rgb) or NULL */
    int key;                    /* KEYCAP: layout index; LED: lock bit; else -1 */
    int cap_class;              /* msdos_cap_class (KEYCAP) */
    int shape;                  /* msdos_cap_shape (KEYCAP) */
    uint32_t fill_rgb;          /* BODY/KEYCAP fill; LED: unlit */
    uint32_t ink_rgb;           /* legend / text colour */
    uint32_t alt_rgb;           /* LED: lit; KEYCAP: fn_legend colour; BODY brackets: their colour */
    float x, y, w, h;
} msdos_skin_item;
/* NULL, count = 0 for a machine out of range. The first item is the BODY "Case",
 * which fills the extent. Static; valid for the process. */
const msdos_skin_item *msdossession_keyboard_skin_layout(int machine, int *count, float *width,
                                                         float *height);
/* The KEYCAP item at (x, y) (key units from the case's top left) or -1; the
 * topmost wins. */
int msdossession_keyboard_skin_hit(int machine, float xpos, float ypos);

/* Skin colours (DESIGN-msdos.md, read off the photographs). */
#define MSDOS_XT_CASE_RGB       0xD9D9CC
#define MSDOS_XT_SLAB_RGB       0xCBCBBD
#define MSDOS_XT_BADGE_RGB      0xA79C74
#define MSDOS_XT_LIGHT_RGB      0xD6D6C6
#define MSDOS_XT_MODIFIER_RGB   0xA9AB9A
#define MSDOS_XT_INK_RGB        0x303030
#define MSDOS_PCJR_CASE_RGB     0xEDEBE3
#define MSDOS_PCJR_PLATE_RGB    0xD3D1C9
#define MSDOS_PCJR_KEY_RGB      0xE6E4DA
#define MSDOS_PCJR_INK_RGB      0x303030
#define MSDOS_PCJR_LOGO_RGB     0x4A4A48
#define MSDOS_PCJR_FN_RGB       0x2E9E5B    /* the green Fn legends and the Fn bracket */
#define MSDOS_PCJR_ALT_RGB      0x4A7FC1    /* the blue Alt bracket */
#define MSDOS_MM_CASE_RGB       0xD9D9D2
#define MSDOS_MM_WELL_RGB       0xC9CBC9
#define MSDOS_MM_LIGHT_RGB      0xE1E1D9
#define MSDOS_MM_MODIFIER_RGB   0xA7A59F
#define MSDOS_MM_INK_RGB        0x303030
#define MSDOS_MM_LED_ON_RGB     0x3FE05A
#define MSDOS_MM_LED_OFF_RGB    0x2F7A3A
#define MSDOS_MM_BADGE_RGB      0x8C8C88
/* A held key or button: a 2 px outline of this and a 20 % wash of it. */
#define MSDOS_HELD_RGB          0xC0282D

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

/* ---- the game port ------------------------------------------------------------
 * Two analog joysticks, A (0) and B (1), each two axes and up to four buttons.
 * Every source of input is kept apart and merged: the Controllers window's
 * pointer, each gamepad's buttons, hat and analog stick, and the bound
 * keyboard keys. Axes: the strongest deflection wins, and a digital push
 * (key, D-pad, hat) is full deflection; opposed digital pushes cancel.
 * Buttons: OR. Letting go of one source never lets go of another. The sources
 * are state, so all of this works with no machine running. */
#define MSDOS_JOYSTICKS 2

/* A stick's targets, in a stick's own numbering. */
enum {
    MSDOS_ACT_UP = 0, MSDOS_ACT_DOWN, MSDOS_ACT_LEFT, MSDOS_ACT_RIGHT,
    MSDOS_ACT_B1, MSDOS_ACT_B2, MSDOS_ACT_B3, MSDOS_ACT_B4,
    MSDOS_ACT_COUNT
};
/* The flat target numbering the bindings use: stick 0's eight, then stick 1's. */
#define MSDOS_TARGET(stick, act) ((stick) * MSDOS_ACT_COUNT + (act))
#define MSDOS_TARGET_COUNT       (MSDOS_JOYSTICKS * MSDOS_ACT_COUNT)

/* What the game port has (Preferences > Ports: "joystick"). */
typedef enum {
    MSDOS_PORT_NONE = 0,    /* no game port: both sticks dimmed, gamepads inert */
    MSDOS_PORT_TWO,         /* "Standard (2 buttons)": A and B, buttons 1-2 each */
    MSDOS_PORT_ONE_4BTN     /* "Standard (4 buttons)": one stick, A, with buttons 1-4 */
} msdos_port_mode;
int  msdossession_game_port_mode(msdossession *s);     /* msdos_port_mode */
/* What the game port has (Preferences > Ports): 0 none. */
int  msdossession_joystick_fitted(msdossession *s);
/* The sticks the port has: 0, 1 (4 buttons) or 2. Joystick B cannot be
 * assigned a gamepad when this is 1. */
int  msdossession_joystick_count(msdossession *s);

/* The window's pointer source for one stick: axes -32768..32767 (+x right,
 * +y down; the knob's position) and buttons bit 0..3. Replaces the pointer's
 * whole state for that stick; (0, 0, 0) lets go. Any thread. */
void msdossession_pointer_set(msdossession *s, int stick, int xpos, int ypos, unsigned buttons);
/* The old name for msdossession_pointer_set. */
void msdossession_joystick(msdossession *s, int stick, int xpos, int ypos, unsigned buttons);
/* What a stick looks like right now, every source merged. */
void msdossession_joystick_state(msdossession *s, int stick, int *xpos, int *ypos, unsigned *buttons);
/* The merged buttons of a stick, bit 0..3 (the held lights). */
unsigned msdossession_buttons_held(msdossession *s, int stick);
/* 1 if target (MSDOS_TARGET) is held by any source; a direction counts when
 * the merged axis is past half deflection that way. */
int  msdossession_target_held(msdossession *s, int target);
/* Window focus loss, Map entry, hide: the pointer source and the keyboard
 * source (bound keys), and every PC key. Gamepads are left alone. */
void msdossession_release_all(msdossession *s);
/* The main window's focus loss: the keyboard source (bound keys) and every PC
 * key. Leaves the pointer and the gamepads. */
void msdossession_release_keyboard(msdossession *s);

/* ---- remappable bindings --------------------------------------------------------
 * Every target (16: two sticks x Up, Down, Left, Right, Button 1-4) has one
 * keyboard key and up to two gamepad inputs. The table is the app's (not a
 * machine's), kept in the settings under "bindings" as the entries that differ
 * from the defaults. There are no default keys (the PC owns its keyboard); the
 * default pad inputs are the D-pad for the directions and South, East, West,
 * North for buttons 1-4, with the left and right triggers also buttons 3 and
 * 4 (a target's second input). A key is identified by its HID usage.
 * Rebinding STEALS: a key drives one target; a pad input is unique within a
 * stick (Joystick A and B may both use South: a pad drives one stick). */
typedef enum {
    MSDOS_PAD_BTN_NONE = -1,
    MSDOS_PAD_BTN_SOUTH = 0, MSDOS_PAD_BTN_EAST, MSDOS_PAD_BTN_WEST, MSDOS_PAD_BTN_NORTH,
    MSDOS_PAD_BTN_BACK, MSDOS_PAD_BTN_GUIDE, MSDOS_PAD_BTN_START,
    MSDOS_PAD_BTN_LEFT_STICK, MSDOS_PAD_BTN_RIGHT_STICK,
    MSDOS_PAD_BTN_LEFT_SHOULDER, MSDOS_PAD_BTN_RIGHT_SHOULDER,
    MSDOS_PAD_BTN_DPAD_UP, MSDOS_PAD_BTN_DPAD_DOWN, MSDOS_PAD_BTN_DPAD_LEFT,
    MSDOS_PAD_BTN_DPAD_RIGHT,
    MSDOS_PAD_BTN_LEFT_TRIGGER, MSDOS_PAD_BTN_RIGHT_TRIGGER,
    MSDOS_PAD_BTN_COUNT,
    /* a device SDL has no gamepad mapping for (a plain HID adapter): its
     * buttons by index, and hat directions (hat * 8 + dir, dir 0..7 clockwise
     * from Up) */
    MSDOS_PAD_BTN_RAW_BASE = 64, MSDOS_PAD_BTN_RAW_LAST = 127,
    MSDOS_PAD_HAT_BASE = 192, MSDOS_PAD_HAT_LAST = 255
} msdos_pad_button;
#define MSDOS_PAD_HAT_DIRS 8
#define MSDOS_PAD_BTN_IS_NAMED(b) ((b) >= 0 && (b) < MSDOS_PAD_BTN_COUNT)
#define MSDOS_PAD_BTN_IS_RAW(b)   ((b) >= MSDOS_PAD_BTN_RAW_BASE && (b) <= MSDOS_PAD_BTN_RAW_LAST)
#define MSDOS_PAD_BTN_IS_HAT(b)   ((b) >= MSDOS_PAD_HAT_BASE && (b) <= MSDOS_PAD_HAT_LAST)

typedef struct {
    uint32_t key;       /* HID usage, 0 = no key */
    int button;         /* msdos_pad_button, NONE = none */
    int button2;        /* a second pad input for the same target (the triggers), or NONE */
} msdos_binding;

const char *msdos_target_name(int target);          /* "Joystick A: Up" */
const char *msdos_target_short_name(int target);    /* "Up", "Button 1" */
const char *msdos_pad_button_name(int button);      /* "South", "D-pad Left", "Button 5", "Hat 0 Up" */
msdos_binding msdossession_binding_get(msdossession *s, int target);
/* Bind a key / a pad input. What the new holder displaced is described into
 * `stolen` ("Joystick B: Up", comma separated; "" if nothing; may be NULL).
 * key 0 / button NONE unbinds. A pad input is dropped on a target's second
 * slot: rebinding the button replaces both. A usage the PC has no key for is
 * refused. Persists. */
void msdossession_binding_set_key(msdossession *s, int target, uint32_t hid_usage,
                                  char *stolen, int stolensz);
void msdossession_binding_set_button(msdossession *s, int target, int button,
                                     char *stolen, int stolensz);
/* Unbinds a target's key and pad inputs (Clear). Persists. */
void msdossession_binding_clear(msdossession *s, int target);
/* Every target back to its default (Defaults). Persists "". */
void msdossession_bindings_reset(msdossession *s);
/* The Reset Controls action of Preferences: the bindings back to their
 * defaults. (Pad assignments are kept: they belong to the hardware.) */
void msdossession_reset_controls(msdossession *s);
/* The target a key / a pad input of stick `stick`'s pad drives, or -1. */
int  msdossession_target_for_key(msdossession *s, uint32_t hid_usage);
int  msdossession_target_for_button(msdossession *s, int stick, int button);
/* The Controllers window's "Show bindings" setting (controllers_show_bindings). */
int  msdossession_controls_show_bindings(msdossession *s);
void msdossession_set_controls_show_bindings(msdossession *s, int on);

/* Map mode, for gamepads: after begin(), the next gamepad button pressed on any
 * pad is reported by poll() (returns 1 and the button) and swallowed: it does
 * nothing else. While armed every pad button is swallowed. cancel() disarms. */
void msdossession_gamepad_capture_begin(msdossession *s);
void msdossession_gamepad_capture_cancel(msdossession *s);
int  msdossession_gamepad_capture_poll(msdossession *s, int *button);

/* ---- gamepads (hot-plugged; each drives a joystick) ----------------------------- */
int  msdossession_gamepad_count(msdossession *s);
int  msdossession_gamepad_name(msdossession *s, int idx, char *dst, int dstsz);
/* Which joystick a pad drives: 0 = A, 1 = B, -1 = automatic (A for the first
 * pad, B for the next). Remembered across runs by the pad's GUID and name
 * (setting "pad_assign"). B is refused (automatic) when the game port has one
 * stick. */
void msdossession_gamepad_assign(msdossession *s, int idx, int stick);
int  msdossession_gamepad_assignment(msdossession *s, int idx);
/* What it drives now: 0, 1, or -1 (nothing: more pads than sticks, or no game
 * port). */
int  msdossession_gamepad_effective_stick(msdossession *s, int idx);
/* Bumped when a pad is added or removed (rebuild the pad rows). */
unsigned msdossession_gamepad_generation(msdossession *s);
/* "Connected: X (joystick A)" / "Disconnected: X": the latest event. */
int  msdossession_gamepad_last_event(msdossession *s, char *dst, int dstsz);

/* ---- the Controllers window's joysticks (controls.c) ----------------------------
 * Two Kraft sticks side by side, in layout units (1 = one key pitch) from the
 * top left of the picture; the extent is the same in every mode. `port_mode`
 * is msdossession_game_port_mode: it decides which parts are `enabled` (a
 * dimmed part is drawn faded and never hit): Joystick A is live with a game
 * port, B with two sticks, and the buttons 3 and 4 pills only on A with the
 * 4-button port. Items are in PAINT order. */
typedef enum {
    MSDOS_CTL_BODY = 0,     /* a drawn part: label says which ("Base", "Plate", "Well", "Label") */
    MSDOS_CTL_CAPTION,      /* text: "Joystick A", "KRAFT" (label), button numbers */
    MSDOS_CTL_STICK,        /* the steering area: a drag steers (msdossession_stick_from_vector) */
    MSDOS_CTL_BUTTON,       /* button 1 (round, on the knob) and button 2 (the side pad) */
    MSDOS_CTL_PILL,         /* buttons 3 and 4 */
    MSDOS_CTL_TRIM          /* a trim slider, drawn at its centre; display only */
} msdos_control_kind;
typedef struct {
    int kind;               /* msdos_control_kind */
    const char *label;
    const char *legend;     /* the number on a button, or NULL */
    int stick;              /* 0 or 1 */
    int target;             /* MSDOS_TARGET of a BUTTON / PILL, else -1 */
    int enabled;            /* 0 = dimmed */
    float x, y, w, h;
    float cenx, ceny, reach;    /* STICK: the knob's rest centre and its travel (layout units; pass reach to
                               msdossession_stick_from_vector). A BUTTON with reach > 0 is round: centre
                               (cenx, ceny), radius reach (button 1); button 2 is the rectangle x y w h. */
} msdos_control;
#define MSDOS_CONTROLS_W 21.0f
#define MSDOS_CONTROLS_H 10.8f
const msdos_control *msdossession_controls_layout(int port_mode, int *count, float *width,
                                                  float *height);
/* The control at (x, y) in layout units: the topmost enabled BUTTON, PILL or
 * STICK, or -1. Button 1 sits at the knob's rest centre: a press that lands on
 * it holds button 1, a drag that starts anywhere else on the plate steers. */
int  msdossession_controls_hit(int port_mode, float xpos, float ypos);
/* A drag vector (layout units from the knob's rest centre) to axes: dx/reach
 * and dy/reach clamped to -1..1, as -32768..32767. */
void msdossession_stick_from_vector(float delx, float dely, float reach, int *xpos, int *ypos);
/* Held / pressed colour of Kraft parts. */
#define MSDOS_KRAFT_BODY_RGB    0xE8E8E2
#define MSDOS_KRAFT_RIM_RGB     0xBFC2BF
#define MSDOS_KRAFT_PLATE_RGB   0xAEB3B6
#define MSDOS_KRAFT_WELL_RGB    0xB8BCBD
#define MSDOS_KRAFT_SHAFT_RGB   0x8E9396
#define MSDOS_KRAFT_GRIP_RGB    0xE6E6DE
#define MSDOS_KRAFT_CAP_RGB     0x8A9094
#define MSDOS_KRAFT_SIDE_RGB    0xB1B7BB
#define MSDOS_KRAFT_TRIM_RGB    0x9A9FA0
#define MSDOS_KRAFT_LOGO_RGB    0xC0282D

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
