/*
 * Where the embedded 86Box keeps things: the app's own directories only,
 * never 86Box's per-user or system-wide ones.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/mem.h>
#include <86box/path.h>
#include <86box/plat.h>
#include <86box/rom.h>

#include "msdos_host.h"

#define MAX_ROM_DIRS 8

static char data_dir[1024];
static char rom_dirs[MAX_ROM_DIRS][1024];
static int  nrom;

void
msdos_host_set_dirs(const char *data, const char *const *roms, int n)
{
    snprintf(data_dir, sizeof(data_dir), "%s", data ? data : "");
    nrom = 0;
    for (int i = 0; i < n && nrom < MAX_ROM_DIRS; i++)
        if (roms[i] && roms[i][0])
            snprintf(rom_dirs[nrom++], sizeof(rom_dirs[0]), "%s", roms[i]);
}

void
msdos_host_data_dir(char *out, size_t len)
{
    if (!len)
        return;
    snprintf(out, len, "%s", data_dir);
    if (out[0])
        path_slash(out);
}

void
msdos_host_add_rom_paths(void)
{
    for (int i = 0; i < nrom; i++) {
        char p[1024];
        snprintf(p, sizeof(p), "%s", rom_dirs[i]);
        path_slash(p);
        rom_add_path(p);
    }
}

void
msdos_host_add_asset_paths(void)
{
    /* usr_path/assets is already on the list (pc_init). */
}
