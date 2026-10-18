/*
 * Preferences: the table every frontend builds its dialog from, and what
 * each setting resolves to for 86Box.
 *
 * Machine-specific settings (CPU, RAM, cards, drives) are kept per machine
 * ("video.ibmxt86"), so switching machines and back keeps each one's own.
 * Choices are stored by 86Box internal name, not by position.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <86box/86box.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/video.h>
#include <86box/sound.h>
#include <86box/hdc.h>
#include <86box/mouse.h>

#include "session_internal.h"

/* ---- the choice lists ------------------------------------------------- */

typedef struct {
    const char *value;      /* stored / 86Box internal name */
    const char *label;
    const char *rom_item;   /* needs this imported, or NULL */
} choice;

static const choice video_choices[] = {
    { "mda", "MDA (monochrome)", "v_font" },
    { "cga", "CGA", "v_font" },
    { "ega", "EGA", "v_ega" },
    { "vga", "VGA", "v_vga" },
    { "tvga8900", "SVGA (Trident TVGA 8900)", "v_tvga8900" },
    { NULL, NULL, NULL }
};
static const choice video_pcjr[] = {
    { "internal", "PCjr built-in", "v_font" }, { NULL, NULL, NULL }
};

static const choice sound_choices[] = {
    { "none", "None (PC speaker only)", NULL },
    { "adlib", "AdLib", NULL },
    { "sb", "Sound Blaster 1.0", NULL },
    { "sb2.0", "Sound Blaster 2.0", NULL },
    { "sbprov2", "Sound Blaster Pro 2", NULL },
    { "sb16", "Sound Blaster 16", NULL },
    { "sbawe32", "Sound Blaster AWE32", "s_awe32" },
    { "gus", "Gravis UltraSound", NULL },
    { NULL, NULL, NULL }
};
static const choice sound_pcjr[] = {
    { "none", "PCjr built-in (SN76489)", NULL }, { NULL, NULL, NULL }
};

static const choice midi_choices[] = {
    { "none", "None", NULL },
    { "mt32", "Roland MT-32", "s_mt32" },
    { "cm32l", "Roland CM-32L", "s_cm32l" },
    { NULL, NULL, NULL }
};

static const choice fdd_choices[] = {
    { "none", "None", NULL },
    { "525_2dd", "5.25\" 360K", NULL },
    { "525_2hd", "5.25\" 1.2M", NULL },
    { "35_2dd", "3.5\" 720K", NULL },
    { "35_2hd", "3.5\" 1.44M", NULL },
    { "35_2ed", "3.5\" 2.88M", NULL },
    { NULL, NULL, NULL }
};

static const choice hdc_xt[] = {
    { "none", "None", NULL },
    { "st506_xt", "IBM Fixed Disk Adapter (Xebec)", "h_xebec" },
    { "st506_xt_wd1002a_wx1", "Western Digital WD1002A-WX1", "h_wd1002a" },
    { "st506_xt_dtc5150x", "DTC 5150X", "h_dtc5150x" },
    { "st506_xt_st11_m", "Seagate ST-11M", "h_st11m" },
    { "xtide", "XT-IDE", "h_xtide" },
    { NULL, NULL, NULL }
};
static const choice hdc_at[] = {
    { "builtin", "Built-in IDE", NULL }, { NULL, NULL, NULL }
};
static const choice hdc_pcjr[] = {
    { "none", "None", NULL }, { NULL, NULL, NULL }
};

typedef struct {
    const char *value;
    const char *label;
    int spt, heads, cyls;
} disk_type;

static const disk_type mfm_types[] = {
    { "image", "From the image", 0, 0, 0 },
    { "st412", "10 MB (Seagate ST-412)", 17, 4, 306 },
    { "st225", "20 MB (Seagate ST-225)", 17, 4, 615 },
    { "st4038", "30 MB (Seagate ST-4038)", 17, 5, 733 },
    { "st251", "40 MB (Seagate ST-251)", 17, 6, 820 },
    { NULL, NULL, 0, 0, 0 }
};
static const disk_type ide_types[] = {
    { "image", "From the image", 0, 0, 0 },
    { "ide40", "40 MB", 17, 5, 977 },
    { "ide120", "120 MB", 32, 8, 919 },
    { "ide250", "250 MB", 63, 8, 1015 },
    { "ide504", "504 MB", 63, 16, 1024 },
    { "ide2g", "2 GB", 63, 16, 4092 },
    { NULL, NULL, 0, 0, 0 }
};

static const choice com_choices[] = {
    { "1", "COM1 (3F8h, IRQ 4)", NULL },
    { "2", "COM2 (2F8h, IRQ 3)", NULL },
    { "3", "COM3 (3E8h, IRQ 4)", NULL },
    { "4", "COM4 (2E8h, IRQ 3)", NULL },
    { NULL, NULL, NULL }
};
static const choice com_pcjr[] = {
    { "2", "Built-in serial port (2F8h)", NULL }, { NULL, NULL, NULL }
};

static const choice bps_choices[] = {
    { "9600", "9600", NULL }, { "19200", "19200", NULL }, { "38400", "38400", NULL },
    { "57600", "57600", NULL }, { "115200", "115200", NULL }, { NULL, NULL, NULL }
};

static const choice mouse_choices[] = {
    { "none", "None", NULL },
    { "msserial", "Microsoft serial mouse", NULL },
    { "ps2", "PS/2 mouse", NULL },
    { "msbus", "Microsoft InPort bus mouse", NULL },
    { NULL, NULL, NULL }
};

static const choice joystick_choices[] = {
    { "none", "None", NULL },
    { "2axis_2button", "Standard (2 buttons)", NULL },
    { "2axis_4button", "Standard (4 buttons)", NULL },
    { NULL, NULL, NULL }
};

/* ---- the table -------------------------------------------------------- */

enum {
    P_MACHINE = 0, P_CPU, P_RAM, P_FPU, P_DYNAREC,
    P_VIDEO, P_ASPECT, P_INTSCALE, P_SMOOTH,
    P_SOUND, P_MIDI, P_VOLUME,
    P_FDD_A, P_FDD_B, P_HDC, P_HD_C, P_HD_D,
    P_COM, P_BPS, P_MOUSE, P_JOYSTICK,
    P_FUJINET,
    P_COUNT
};

static const msdos_pref table[P_COUNT] = {
    [P_MACHINE] = { "machine", "Machine", "Machine", "Machine", NULL,
                    MSDOS_PREF_CHOICE, MSDOS_M_5160, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_CPU] = { "cpu", "Machine", "Machine", "Processor", NULL,
                MSDOS_PREF_CHOICE, 0, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_RAM] = { "ram", "Machine", "Machine", "Memory (KB)", NULL,
                MSDOS_PREF_INT, 640, 64, 640, 64, MSDOS_APPLY_RESET },
    [P_FPU] = { "fpu", "Machine", "Machine", "Math coprocessor", NULL,
                MSDOS_PREF_BOOL, 0, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_DYNAREC] = { "dynarec", "Machine", "Speed", "Recompiler",
                    "Runs 386 and later processors much faster",
                    MSDOS_PREF_BOOL, 1, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_VIDEO] = { "video", "Display", "Video card", "Video card", NULL,
                  MSDOS_PREF_CHOICE, 0, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_ASPECT] = { "aspect43", "Display", "Picture", "4:3 picture",
                   "Shape the picture like a PC monitor's",
                   MSDOS_PREF_BOOL, 1, 0, 0, 0, MSDOS_APPLY_LIVE },
    [P_INTSCALE] = { "integer_scale", "Display", "Picture", "Whole-number scaling", NULL,
                     MSDOS_PREF_BOOL, 0, 0, 0, 0, MSDOS_APPLY_LIVE },
    [P_SMOOTH] = { "smooth", "Display", "Picture", "Smooth scaling", NULL,
                   MSDOS_PREF_BOOL, 1, 0, 0, 0, MSDOS_APPLY_LIVE },
    [P_SOUND] = { "sound", "Sound", "Sound card", "Sound card", NULL,
                  MSDOS_PREF_CHOICE, 0, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_MIDI] = { "midi", "Sound", "Sound card", "MIDI synthesizer",
                 "On an MPU-401 (the Sound Blaster 16's own, or a separate one)",
                 MSDOS_PREF_CHOICE, 0, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_VOLUME] = { "volume", "Sound", "Output", "Volume", NULL,
                   MSDOS_PREF_INT, 80, 0, 100, 5, MSDOS_APPLY_LIVE },
    [P_FDD_A] = { "fdd_a", "Storage", "Floppy drives", "Drive A:", NULL,
                  MSDOS_PREF_CHOICE, 0, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_FDD_B] = { "fdd_b", "Storage", "Floppy drives", "Drive B:", NULL,
                  MSDOS_PREF_CHOICE, 0, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_HDC] = { "hdc", "Storage", "Hard disks", "Controller", NULL,
                MSDOS_PREF_CHOICE, 0, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_HD_C] = { "hd_c_type", "Storage", "Hard disks", "Drive C: type",
                 "Used for a new blank disk; an image keeps its own",
                 MSDOS_PREF_CHOICE, 0, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_HD_D] = { "hd_d_type", "Storage", "Hard disks", "Drive D: type", NULL,
                 MSDOS_PREF_CHOICE, 0, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_COM] = { "fujinet_com", "Ports", "FujiNet", "FujiNet port",
                "FUJINET.SYS on the boot floppy follows it",
                MSDOS_PREF_CHOICE, 1, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_BPS] = { "fujinet_bps", "Ports", "FujiNet", "FujiNet speed", NULL,
                MSDOS_PREF_CHOICE, 4, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_MOUSE] = { "mouse", "Ports", "Mouse and joystick", "Mouse",
                  "A serial mouse goes on the first COM port FujiNet is not using",
                  MSDOS_PREF_CHOICE, 0, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_JOYSTICK] = { "joystick", "Ports", "Mouse and joystick", "Game port", NULL,
                     MSDOS_PREF_CHOICE, 1, 0, 0, 0, MSDOS_APPLY_RESET },
    [P_FUJINET] = { "fujinet", "FujiNet", "FujiNet", "FujiNet",
                    "The FujiNet runtime inside the app",
                    MSDOS_PREF_BOOL, 1, 0, 0, 0, MSDOS_APPLY_RESTART },
};

int
msdossession_pref_count(void)
{
    return P_COUNT;
}

const msdos_pref *
msdossession_pref(int i)
{
    return (i >= 0 && i < P_COUNT) ? &table[i] : NULL;
}

int
msdossession_pref_find(const char *key)
{
    for (int i = 0; i < P_COUNT; i++)
        if (strcmp(table[i].key, key) == 0)
            return i;
    return -1;
}

/* ---- per-machine storage ---------------------------------------------- */

int
prefs_machine(struct msdossession *s)
{
    int m = msdossession_get_int(s, "machine", MSDOS_M_5160);
    return (m >= 0 && m < MSDOS_M_COUNT) ? m : MSDOS_M_5160;
}

int
msdossession_machine(msdossession *s)
{
    return prefs_machine(s);
}

static int
per_machine(int i)
{
    return i == P_CPU || i == P_RAM || i == P_FPU || i == P_VIDEO || i == P_SOUND
        || i == P_FDD_A || i == P_FDD_B || i == P_HDC || i == P_HD_C || i == P_HD_D;
}

static void
store_key(struct msdossession *s, int i, char *out, size_t outsz)
{
    if (per_machine(i))
        snprintf(out, outsz, "%s.%s", table[i].key, msdos_profile_get(prefs_machine(s))->internal);
    else
        snprintf(out, outsz, "%s", table[i].key);
}

static const choice *
choices_for(struct msdossession *s, int i)
{
    const msdos_profile *p = msdos_profile_get(prefs_machine(s));
    switch (i) {
        case P_VIDEO: return p->pcjr ? video_pcjr : video_choices;
        case P_SOUND: return p->pcjr ? sound_pcjr : sound_choices;
        case P_MIDI: return midi_choices;
        case P_FDD_A:
        case P_FDD_B: return fdd_choices;
        case P_HDC: return p->pcjr ? hdc_pcjr : p->xt_hdc ? hdc_xt : hdc_at;
        case P_COM: return p->pcjr ? com_pcjr : com_choices;
        case P_BPS: return bps_choices;
        case P_MOUSE: return mouse_choices;
        case P_JOYSTICK: return joystick_choices;
        default: return NULL;
    }
}

static const char *
choice_default(struct msdossession *s, int i)
{
    const msdos_profile *p = msdos_profile_get(prefs_machine(s));
    switch (i) {
        case P_VIDEO: return p->video;
        case P_SOUND: return p->sound;
        case P_FDD_A: return p->fdd_a;
        case P_FDD_B: return p->fdd_b;
        case P_HDC: return p->xt_hdc ? "st506_xt" : p->pcjr ? "none" : "builtin";
        case P_HD_C:
        case P_HD_D: return "image";
        case P_MIDI: return "none";
        case P_COM: return "2";
        case P_BPS: return "115200";
        case P_MOUSE: return "none";
        case P_JOYSTICK: return "2axis_2button";
        default: return "";
    }
}

/* The stored value, or the machine's default. */
static const char *
raw_value(struct msdossession *s, int i)
{
    char key[96];
    store_key(s, i, key, sizeof key);
    const char *v = msdossession_get_str(s, key, NULL);
    return (v && *v) ? v : choice_default(s, i);
}

static const disk_type *
disk_types(struct msdossession *s)
{
    const char *hdc = prefs_hdc(s);
    return (strncmp(hdc, "st506", 5) == 0) ? mfm_types : ide_types;
}

/* ---- availability ------------------------------------------------------ */

static int
rom_ok(struct msdossession *s, const char *item)
{
    if (!item)
        return 1;
    int i = roms_item_index(item);
    return i < 0 || roms_item_complete(s, i, NULL);
}

static int
machine_index(struct msdossession *s)
{
    return machine_get_machine_from_internal_name(msdos_profile_machine86(prefs_machine(s)));
}

static int
device_ok(struct msdossession *s, int i, const char *value)
{
    int mach = machine_index(s);
    const device_t *d = NULL;
    if (mach < 0)
        return 1;
    switch (i) {
        case P_VIDEO:
            if (!strcmp(value, "internal"))
                return 1;
            d = video_card_getdevice(video_get_video_from_internal_name((char *) value));
            break;
        case P_SOUND:
            if (!strcmp(value, "none"))
                return 1;
            d = sound_card_getdevice(sound_card_get_from_internal_name((char *) value));
            break;
        case P_MOUSE:
            if (!strcmp(value, "none") || !strcmp(value, "msserial"))
                return 1;
            if (!strcmp(value, "ps2"))
                return machine_has_bus(mach, MACHINE_BUS_PS2_PORTS);
            d = mouse_get_device(mouse_get_from_internal_name((char *) value));
            break;
        default:
            return 1;
    }
    return d ? device_is_valid(d, mach) : 1;
}

int
msdossession_pref_enabled(msdossession *s, int i)
{
    const msdos_profile *p = msdos_profile_get(prefs_machine(s));
    switch (i) {
        case P_CPU: {
            msdos_cpu_choice c[64];
            return msdos_cpu_choices(prefs_machine(s), c, 64) > 1;
        }
        case P_FPU:
            return strcmp(prefs_cpu_family_fpu(s), "") != 0;
        case P_DYNAREC:
            return prefs_machine(s) >= MSDOS_M_386;
        case P_VIDEO:
        case P_SOUND:
        case P_COM:
            return !p->pcjr;
        case P_MIDI:
            return !p->pcjr;
        case P_FDD_A:
        case P_FDD_B:
            return !p->pcjr;
        case P_HDC:
            return p->xt_hdc;
        case P_HD_C:
        case P_HD_D:
            return strcmp(prefs_hdc(s), "none") != 0;
        case P_FUJINET:
            return !s->fujinet_disabled;
        default:
            return 1;
    }
}

int
msdossession_pref_choice_enabled(msdossession *s, int i, int c)
{
    const msdos_profile *p = msdos_profile_get(prefs_machine(s));
    if (i == P_MACHINE)
        return msdossession_machine_available(s, c);
    if (i == P_CPU || i == P_HD_C || i == P_HD_D)
        return 1;
    const choice *cs = choices_for(s, i);
    if (!cs)
        return 1;
    for (int k = 0; k < c; k++)
        if (!cs[k].value)
            return 0;
    if (!cs[c].value)
        return 0;
    if (!rom_ok(s, cs[c].rom_item))
        return 0;
    if (i == P_FDD_A && !strcmp(cs[c].value, "none"))
        return 0;
    if ((i == P_FDD_A || i == P_FDD_B) && p->eight_bit
        && (!strcmp(cs[c].value, "525_2hd") || !strcmp(cs[c].value, "35_2hd")
            || !strcmp(cs[c].value, "35_2ed")))
        return 0;       /* the XT's floppy controller is double density only */
    if (i == P_COM) {
        /* a serial mouse needs a port of its own */
        return 1;
    }
    return device_ok(s, i, cs[c].value);
}

/* ---- choices ------------------------------------------------------------ */

const char *
msdossession_pref_choice(msdossession *s, int i, int c)
{
    static __thread char label[96];
    if (c < 0)
        return NULL;
    if (i == P_MACHINE)
        return c < MSDOS_M_COUNT ? msdos_machine_name(c) : NULL;
    if (i == P_CPU) {
        msdos_cpu_choice cc[64];
        int n = msdos_cpu_choices(prefs_machine(s), cc, 64);
        if (c >= n)
            return NULL;
        snprintf(label, sizeof label, "%s", cc[c].label);
        return label;
    }
    if (i == P_HD_C || i == P_HD_D) {
        const disk_type *t = disk_types(s);
        for (int k = 0; k <= c; k++)
            if (!t[k].value)
                return NULL;
        return t[c].label;
    }
    const choice *cs = choices_for(s, i);
    if (!cs)
        return NULL;
    for (int k = 0; k <= c; k++)
        if (!cs[k].value)
            return NULL;
    return cs[c].label;
}

int
msdossession_pref_get_choice(msdossession *s, int i)
{
    if (i == P_MACHINE)
        return prefs_machine(s);
    if (i == P_CPU) {
        msdos_cpu_choice cc[64], cur;
        int n = msdos_cpu_choices(prefs_machine(s), cc, 64);
        prefs_cpu(s, &cur);
        for (int k = 0; k < n; k++)
            if (!strcmp(cc[k].family, cur.family) && cc[k].speed == cur.speed)
                return k;
        return 0;
    }
    if (i == P_HD_C || i == P_HD_D) {
        const disk_type *t = disk_types(s);
        const char *v = raw_value(s, i);
        for (int k = 0; t[k].value; k++)
            if (!strcmp(t[k].value, v))
                return k;
        return 0;
    }
    const choice *cs = choices_for(s, i);
    if (!cs)
        return msdossession_get_int(s, table[i].key, table[i].def);
    const char *v = raw_value(s, i);
    for (int k = 0; cs[k].value; k++)
        if (!strcmp(cs[k].value, v))
            return k;
    return 0;
}

void
msdossession_pref_set_choice(msdossession *s, int i, int c)
{
    char key[96];
    if (i == P_MACHINE) {
        if (c >= 0 && c < MSDOS_M_COUNT)
            msdossession_set_int(s, "machine", c);
        return;
    }
    store_key(s, i, key, sizeof key);
    if (i == P_CPU) {
        msdos_cpu_choice cc[64];
        int n = msdos_cpu_choices(prefs_machine(s), cc, 64);
        if (c >= 0 && c < n) {
            char v[64];
            snprintf(v, sizeof v, "%s:%u", cc[c].family, (unsigned) cc[c].speed);
            msdossession_set_str(s, key, v);
        }
        return;
    }
    if (i == P_HD_C || i == P_HD_D) {
        const disk_type *t = disk_types(s);
        for (int k = 0; k <= c; k++)
            if (!t[k].value)
                return;
        msdossession_set_str(s, key, t[c].value);
        return;
    }
    const choice *cs = choices_for(s, i);
    if (!cs)
        return;
    for (int k = 0; k <= c; k++)
        if (!cs[k].value)
            return;
    msdossession_set_str(s, key, cs[c].value);
}

int
msdossession_pref_get_int(msdossession *s, int i)
{
    char key[96];
    if (i < 0 || i >= P_COUNT)
        return 0;
    if (i == P_RAM)
        return prefs_ram_kb(s);
    store_key(s, i, key, sizeof key);
    return msdossession_get_int(s, key, table[i].def);
}

void
msdossession_pref_set_int(msdossession *s, int i, int v)
{
    char key[96];
    if (i < 0 || i >= P_COUNT)
        return;
    store_key(s, i, key, sizeof key);
    msdossession_set_int(s, key, v);
}

void
msdossession_pref_int_range(msdossession *s, int i, int *min, int *max, int *step)
{
    const msdos_profile *p = msdos_profile_get(prefs_machine(s));
    int lo = table[i].min, hi = table[i].max, st = table[i].step;
    if (i == P_RAM) {
        lo = p->ram_min;
        hi = p->ram_max;
        st = p->ram_step;
    }
    if (min) *min = lo;
    if (max) *max = hi;
    if (step) *step = st;
}

/* ---- resolved values ------------------------------------------------------ */

int
prefs_cpu(struct msdossession *s, msdos_cpu_choice *out)
{
    const msdos_profile *p = msdos_profile_get(prefs_machine(s));
    msdos_cpu_choice cc[64];
    int n = msdos_cpu_choices(prefs_machine(s), cc, 64);
    const char *v = raw_value(s, P_CPU);
    char fam[32] = "";
    unsigned speed = 0;
    if (v && sscanf(v, "%31[^:]:%u", fam, &speed) == 2) {
        for (int k = 0; k < n; k++)
            if (!strcmp(cc[k].family, fam) && cc[k].speed == speed) {
                *out = cc[k];
                return 0;
            }
    }
    for (int k = 0; k < n; k++)
        if (!strcmp(cc[k].family, p->cpu_family) && cc[k].speed == p->cpu_speed) {
            *out = cc[k];
            return 0;
        }
    if (n > 0) {
        *out = cc[n - 1];
        return 0;
    }
    snprintf(out->family, sizeof out->family, "%s", p->cpu_family);
    out->speed = p->cpu_speed;
    snprintf(out->label, sizeof out->label, "%s", p->cpu_family);
    return -1;
}

/* The FPU a socketed coprocessor would be for the CPU, "" when it has none
 * to add (built in, or not offered). */
const char *
prefs_cpu_family_fpu(struct msdossession *s)
{
    msdos_cpu_choice c;
    prefs_cpu(s, &c);
    if (!strcmp(c.family, "8088")) return "8087";
    if (!strcmp(c.family, "286")) return "287";
    if (!strcmp(c.family, "i386dx") || !strcmp(c.family, "am386dx")) return "387";
    return "";
}

int
prefs_ram_kb(struct msdossession *s)
{
    const msdos_profile *p = msdos_profile_get(prefs_machine(s));
    char key[96];
    store_key(s, P_RAM, key, sizeof key);
    int kb = msdossession_get_int(s, key, p->ram_kb);
    if (kb < p->ram_min) kb = p->ram_min;
    if (kb > p->ram_max) kb = p->ram_max;
    kb -= (kb - p->ram_min) % p->ram_step;
    return kb;
}

int
prefs_fpu(struct msdossession *s)
{
    char key[96];
    store_key(s, P_FPU, key, sizeof key);
    return *prefs_cpu_family_fpu(s) && msdossession_get_int(s, key, 0);
}

static const char *
resolved_choice(struct msdossession *s, int i)
{
    const choice *cs = choices_for(s, i);
    const char *v = raw_value(s, i);
    for (int k = 0; cs && cs[k].value; k++)
        if (!strcmp(cs[k].value, v))
            return cs[k].value;
    return cs ? cs[0].value : v;
}

const char *prefs_video(struct msdossession *s) { return resolved_choice(s, P_VIDEO); }
const char *prefs_midi(struct msdossession *s) { return resolved_choice(s, P_MIDI); }
const char *prefs_mouse(struct msdossession *s) { return resolved_choice(s, P_MOUSE); }
const char *prefs_joystick(struct msdossession *s) { return resolved_choice(s, P_JOYSTICK); }

const char *
prefs_sound(struct msdossession *s)
{
    return msdos_profile_get(prefs_machine(s))->pcjr ? "none" : resolved_choice(s, P_SOUND);
}

const char *
prefs_fdd_type(struct msdossession *s, int drive)
{
    const msdos_profile *p = msdos_profile_get(prefs_machine(s));
    if (p->pcjr)
        return drive == 0 ? "525_2dd" : "none";
    const char *v = resolved_choice(s, drive == 0 ? P_FDD_A : P_FDD_B);
    if (drive == 0 && !strcmp(v, "none"))
        v = p->fdd_a;
    return v;
}

const char *
prefs_hdc(struct msdossession *s)
{
    const msdos_profile *p = msdos_profile_get(prefs_machine(s));
    if (p->pcjr)
        return "none";
    if (!p->xt_hdc)
        return p->at_ide ? "ide_isa" : "internal";
    return resolved_choice(s, P_HDC);
}

int
prefs_hd_type(struct msdossession *s, int drive, int *spt, int *heads, int *cyls,
              const char **bus)
{
    const disk_type *t = disk_types(s);
    const char *v = raw_value(s, drive == 0 ? P_HD_C : P_HD_D);
    const char *hdc = prefs_hdc(s);
    if (bus)
        *bus = (strncmp(hdc, "st506", 5) == 0) ? "mfm" : "ide";
    for (int k = 0; t[k].value; k++)
        if (!strcmp(t[k].value, v)) {
            *spt = t[k].spt;
            *heads = t[k].heads;
            *cyls = t[k].cyls;
            return k;
        }
    *spt = *heads = *cyls = 0;
    return 0;
}

int
prefs_fujinet_com(struct msdossession *s)
{
    if (msdos_profile_get(prefs_machine(s))->pcjr)
        return 2;
    int c = atoi(resolved_choice(s, P_COM));
    return (c >= 1 && c <= 4) ? c : 2;
}

int
msdossession_fujinet_com(msdossession *s)
{
    return prefs_fujinet_com(s);
}

int
prefs_fujinet_bps(struct msdossession *s)
{
    int b = atoi(resolved_choice(s, P_BPS));
    return b > 0 ? b : 115200;
}

int
prefs_int(struct msdossession *s, const char *key)
{
    int i = msdossession_pref_find(key);
    return msdossession_get_int(s, key, i >= 0 ? table[i].def : 0);
}

int
msdossession_integer_scale(msdossession *s)
{
    return prefs_int(s, "integer_scale");
}

int
msdossession_smooth(msdossession *s)
{
    return prefs_int(s, "smooth");
}

/* Everything a hard reset takes in: what apply_settings compares. */
void
prefs_reset_signature(struct msdossession *s, char *out, int outsz)
{
    msdos_cpu_choice c;
    int spt, heads, cyls;
    char hd[2][80];
    prefs_cpu(s, &c);
    for (int d = 0; d < 2; d++) {
        prefs_hd_type(s, d, &spt, &heads, &cyls, NULL);
        snprintf(hd[d], sizeof hd[d], "%d/%d/%d:%s", spt, heads, cyls, media_hd(s, d));
    }
    snprintf(out, (size_t) outsz,
             "%d|%s:%u|%d|%d|%d|%s|%s|%s|%s|%s|%s|%s|%s|%d|%d|%s|%s",
             prefs_machine(s), c.family, (unsigned) c.speed, prefs_ram_kb(s), prefs_fpu(s),
             prefs_int(s, "dynarec"), prefs_video(s), prefs_sound(s), prefs_midi(s),
             prefs_fdd_type(s, 0), prefs_fdd_type(s, 1), prefs_hdc(s), hd[0], hd[1],
             prefs_fujinet_com(s), prefs_fujinet_bps(s), prefs_mouse(s), prefs_joystick(s));
}
