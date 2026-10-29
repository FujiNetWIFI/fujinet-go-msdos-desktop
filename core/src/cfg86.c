/*
 * 86box.cfg from the settings and the drives: the whole machine, every
 * time. 86Box reads it at start and again on every apply (session.cpp:
 * config_load, then a hard reset), so device sections such as the FujiNet
 * port or a card's BIOS take effect the same way as the rest.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rom_table.h"
#include "session_internal.h"

/* Paths in 86box.cfg are taken as written: spaces are fine, but a path with
 * a leading or trailing blank would not survive the ini reader. */
static void
put_path(FILE *f, const char *key, const char *path)
{
    if (path && *path)
        fprintf(f, "%s = %s\n", key, path);
}

/* The BIOS choice of an item: its first complete variant. */
static const char *
bios_choice(struct msdossession *s, const char *item, const char **section)
{
    int i = roms_item_index(item);
    int v = 0;
    *section = NULL;
    if (i < 0 || !roms_item_complete(s, i, &v))
        return NULL;
    *section = msdos_rom_items[i].section;
    return msdos_rom_items[i].variants[v].bios;
}

/* The geometry of an existing image: a VHD's footer, else the size over a
 * standard track layout for the bus. */
static int
image_geometry(const char *path, const char *bus, int *spt, int *heads, int *cyls)
{
    FILE *f = fopen(path, "rb");
    unsigned char foot[512];
    long long size;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    if (size >= 512) {
        fseek(f, -512, SEEK_END);
        if (fread(foot, 1, 512, f) == 512 && !memcmp(foot, "conectix", 8)) {
            *cyls  = (foot[56] << 8) | foot[57];
            *heads = foot[58];
            *spt   = foot[59];
            fclose(f);
            return (*cyls && *heads && *spt) ? 0 : -1;
        }
    }
    fclose(f);
    if (size <= 0 || size % 512)
        return -1;
    long long sectors = size / 512;
    static const int mfm_heads[] = { 4, 6, 5, 8, 2, 0 };
    if (!strcmp(bus, "mfm")) {
        for (int k = 0; mfm_heads[k]; k++)
            if (sectors % (17 * mfm_heads[k]) == 0 && sectors / (17 * mfm_heads[k]) <= 1024) {
                *spt = 17;
                *heads = mfm_heads[k];
                *cyls = (int) (sectors / (17 * mfm_heads[k]));
                return 0;
            }
        return -1;
    }
    *spt = 63;
    *heads = 16;
    *cyls = (int) (sectors / (63 * 16));
    return *cyls > 0 ? 0 : -1;
}

int
cfg86_write(struct msdossession *s, const char *path)
{
    const int m = prefs_machine(s);
    const msdos_profile *p = msdos_profile_get(m);
    msdos_cpu_choice cpu;
    const char *section;
    const char *bios;
    char tmp[MSDOS_PATH_MAX + 8];
    FILE *f;

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) {
        session_set_error(s, "cannot write %s", path);
        return -1;
    }
    prefs_cpu(s, &cpu);

    fprintf(f, "[General]\n");
    fprintf(f, "vid_renderer = default\n");
    fprintf(f, "\n");

    fprintf(f, "[Machine]\n");
    fprintf(f, "machine = %s\n", msdos_profile_machine86(m));
    fprintf(f, "cpu_family = %s\n", cpu.family);
    fprintf(f, "cpu_speed = %u\n", (unsigned) cpu.speed);
    fprintf(f, "cpu_multi = %g\n", cpu.multi);
    if (*prefs_cpu_family_fpu(s))
        fprintf(f, "fpu_type = %s\n", prefs_fpu(s) ? prefs_cpu_family_fpu(s) : "none");
    else
        fprintf(f, "fpu_type = internal\n");
    {
        int kb = prefs_ram_kb(s);
        fprintf(f, "mem_size = %d\n", kb < p->board_ram_max ? kb : p->board_ram_max);
    }
    fprintf(f, "cpu_use_dynarec = %d\n", (m >= MSDOS_M_386 && prefs_int(s, "dynarec")) ? 1 : 0);
    fprintf(f, "time_sync = local\n");
    fprintf(f, "\n");

    /* the board's BIOS revision, where it has a choice */
    bios = msdos_test_glabios(m) ? NULL : bios_choice(s, p->rom_item, &section);
    if (bios && section)
        fprintf(f, "[%s]\nbios = %s\n\n", section, bios);

    fprintf(f, "[Video]\n");
    fprintf(f, "gfxcard = %s\n", prefs_video(s));
    fprintf(f, "\n");
    if (!strcmp(prefs_video(s), "tvga8900")) {
        bios = bios_choice(s, "v_tvga8900", &section);
        for (int k = 0; k < 2; k++) {
            fprintf(f, k ? "[Trident TVGA 8900 #1]\n" : "[Trident TVGA 8900]\n");
            if (bios)
                fprintf(f, "bios = %s\n", bios);
            fprintf(f, "memory = 1024\n\n");
        }
    }

    fprintf(f, "[Input devices]\n");
    fprintf(f, "mouse_type = %s\n", prefs_mouse(s));
    fprintf(f, "joystick_type = %s\n", prefs_joystick(s));
    fprintf(f, "\n");

    fprintf(f, "[Sound]\n");
    fprintf(f, "sndcard = %s\n", p->pcjr ? "internal" : prefs_sound(s));
    {
        const char *midi = p->pcjr ? "none" : prefs_midi(s);
        const char *snd = prefs_sound(s);
        fprintf(f, "midi_device = %s\n", midi);
        /* the SB16 and AWE32 have an MPU-401 of their own */
        fprintf(f, "mpu401_standalone = %d\n",
                strcmp(midi, "none") && strcmp(snd, "sb16") && strcmp(snd, "sbawe32"));
    }
    fprintf(f, "\n");

    /* COM ports: FujiNet's, and the first other one for a serial mouse */
    {
        const int fn = prefs_fujinet_com(s);
        int mouse_com = 0;
        if (!strcmp(prefs_mouse(s), "msserial"))
            for (int c = 1; c <= 4 && !mouse_com; c++)
                if (c != fn)
                    mouse_com = c;
        fprintf(f, "[Ports (COM & LPT)]\n");
        if (p->pcjr) {
            /* the PCjr's own 8250 (2F8h) is its first COM device */
            fprintf(f, "serial1_device = fujinet\n");
        } else {
            for (int c = 1; c <= 4; c++) {
                const int on = (c <= 2) || c == fn || c == mouse_com;
                fprintf(f, "serial%d_enabled = %d\n", c, on);
                if (c == fn)
                    fprintf(f, "serial%d_device = fujinet\n", c);
            }
        }
        fprintf(f, "\n");
        {
            const int slot = p->pcjr ? 1 : fn;
            char boip_host[64];
            int boip_port;
            prefs_boip_target(s, boip_host, sizeof boip_host, &boip_port);
            fprintf(f, "[FujiNet (BoIP) #%d]\nhost = %s\nport = %d\n\n", slot, boip_host,
                    boip_port);
        }
        if (mouse_com)
            fprintf(f, "[Microsoft Serial Mouse]\nport = %d\n\n", mouse_com - 1);
    }

    /* An XT disk controller with no drive on it fails POST (1701): it is
     * fitted only with a disk. */
    {
        const char *hdc = prefs_hdc(s);
        const int disks = *media_hd(s, 0) || *media_hd(s, 1);
        fprintf(f, "[Storage controllers]\n");
        fprintf(f, "fdc = internal\n");
        fprintf(f, "hdc_1 = %s\n", (p->xt_hdc && !disks) ? "none" : hdc);
        fprintf(f, "\n");
    }

    /* hard disks */
    {
        int any = 0;
        const char *hdc = prefs_hdc(s);
        for (int d = 0; d < 2 && strcmp(hdc, "none"); d++) {
            const char *img = media_hd(s, d);
            const char *bus;
            int spt, heads, cyls;
            if (!img || !*img)
                continue;
            prefs_hd_type(s, d, &spt, &heads, &cyls, &bus);
            if ((!spt && image_geometry(img, bus, &spt, &heads, &cyls) != 0) || !spt)
                continue;
            if (!any++)
                fprintf(f, "[Hard disks]\n");
            fprintf(f, "hdd_%02d_parameters = %d, %d, %d, 0, %s\n", d + 1, spt, heads, cyls, bus);
            put_path(f, d ? "hdd_02_fn" : "hdd_01_fn", img);
            if (!strcmp(bus, "mfm"))
                fprintf(f, "hdd_%02d_mfm_channel = %d\n", d + 1, d);
            else
                fprintf(f, "hdd_%02d_ide_channel = 0:%d\n", d + 1, d);
        }
        if (any)
            fprintf(f, "\n");
    }

    fprintf(f, "[Floppy and CD-ROM drives]\n");
    for (int d = 0; d < 2; d++) {
        const char *type = prefs_fdd_type(s, d);
        int ro = 0;
        const char *img = media_floppy(s, d, &ro);
        fprintf(f, "fdd_%02d_type = %s\n", d + 1, type);
        if (strcmp(type, "none") && img && *img) {
            char key[16];
            snprintf(key, sizeof key, "fdd_%02d_fn", d + 1);
            put_path(f, key, img);
            if (ro)
                fprintf(f, "fdd_%02d_writeprot = 1\n", d + 1);
        }
    }
    fprintf(f, "\n");

    /* RAM past the 5150 board's 256K is on an ISA card */
    {
        int kb = prefs_ram_kb(s);
        if (kb > p->board_ram_max && !p->pcjr) {
            fprintf(f, "[Other peripherals]\nisamem0_type = genericxt\n\n");
            fprintf(f, "[Generic PC/XT Memory Expansion #1]\nstart = %d\nsize = %d\n\n",
                    p->board_ram_max, kb - p->board_ram_max);
        }
    }

    if (fclose(f) != 0) {
        remove(tmp);
        session_set_error(s, "cannot write %s", path);
        return -1;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        session_set_error(s, "cannot replace %s", path);
        return -1;
    }
    return 0;
}
