/*
 * Imported ROMs: an 86Box ROM tree under <data>/roms that 86Box searches
 * first. Files come in from a single image, a zip or a folder (an 86Box ROM
 * set works) and are recognised by size and CRC-32 (rom_table.c), whatever
 * they are called.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#endif

#include "rom_table.h"
#include "session_internal.h"
#include "zipread.h"

/* ---- CRC-32 (the zip/PNG polynomial) ------------------------------------ */

uint32_t
roms_crc32(const uint8_t *p, size_t n, uint32_t crc)
{
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- availability --------------------------------------------------------- */

int
roms_item_index(const char *key)
{
    for (int i = 0; i < msdos_rom_item_count; i++)
        if (!strcmp(msdos_rom_items[i].key, key))
            return i;
    return -1;
}

static int
file_present(struct msdossession *s, int file)
{
    char path[MSDOS_PATH_MAX * 2];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", s->roms_dir, msdos_rom_files[file].path);
    return stat(path, &st) == 0 && (uint32_t) st.st_size == msdos_rom_files[file].size;
}

int
roms_item_complete(struct msdossession *s, int item, int *variant)
{
    const msdos_rom_item_def *it;
    if (item < 0 || item >= msdos_rom_item_count)
        return 0;
    it = &msdos_rom_items[item];
    for (int v = 0; v < it->nvariants; v++) {
        int ok = 1;
        for (int k = 0; k < it->variants[v].nfiles && ok; k++)
            ok = file_present(s, it->variants[v].files[k]);
        if (ok) {
            if (variant)
                *variant = v;
            return 1;
        }
    }
    return 0;
}

int
msdossession_rom_item_count(void)
{
    return msdos_rom_item_count;
}

const msdos_rom_item *
msdossession_rom_item(int i)
{
    static msdos_rom_item items[64];
    if (i < 0 || i >= msdos_rom_item_count || i >= 64)
        return NULL;
    items[i].name = msdos_rom_items[i].name;
    items[i].group = msdos_rom_items[i].group;
    return &items[i];
}

int
msdossession_rom_item_available(msdossession *s, int i)
{
    return roms_item_complete(s, i, NULL);
}

int
msdossession_machine_available(msdossession *s, int m)
{
    if (m < 0 || m >= MSDOS_M_COUNT)
        return 0;
    if (msdos_test_glabios(m)) {
        char path[MSDOS_PATH_MAX];
        FILE *f;
        snprintf(path, sizeof path, "%s/machines/glabios/GLABIOS_0.4.0_8X.ROM", s->roms_dir);
        if ((f = fopen(path, "rb")) == NULL)
            return 0;
        fclose(f);
        return 1;
    }
    return roms_item_complete(s, roms_item_index(msdos_profile_get(m)->rom_item), NULL);
}

int
msdossession_has_bios(msdossession *s)
{
    for (int m = 0; m < MSDOS_M_COUNT; m++)
        if (msdossession_machine_available(s, m))
            return 1;
    return 0;
}

/* ---- import ------------------------------------------------------------------- */

typedef struct {
    struct msdossession *s;
    int imported;           /* files written */
    int seen;               /* candidate files looked at */
    uint8_t got[256];       /* which table files this import supplied */
} import_ctx;

static int
size_wanted(uint32_t size)
{
    for (int i = 0; i < msdos_rom_file_count; i++)
        if (msdos_rom_files[i].size == size)
            return 1;
    return 0;
}

static void
take(import_ctx *ctx, const uint8_t *data, uint32_t size)
{
    uint32_t crc = roms_crc32(data, size, 0);
    ctx->seen++;
    for (int i = 0; i < msdos_rom_file_count; i++) {
        const msdos_rom_file *rf = &msdos_rom_files[i];
        char path[MSDOS_PATH_MAX * 2], dir[MSDOS_PATH_MAX * 2];
        FILE *f;
        char *slash;
        if (rf->size != size || rf->crc32 != crc)
            continue;
        snprintf(path, sizeof path, "%s/%s", ctx->s->roms_dir, rf->path);
        snprintf(dir, sizeof dir, "%s", path);
        slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            paths_mkdir_p(dir);
        }
        f = fopen(path, "wb");
        if (!f)
            continue;
        if (fwrite(data, 1, size, f) == size) {
            if (i < 256 && !ctx->got[i]) {
                ctx->got[i] = 1;
                ctx->imported++;
            }
        }
        fclose(f);
    }
}

static int
read_whole(const char *path, uint8_t **data, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (64L << 20)) {
        fclose(f);
        return -1;
    }
    *data = malloc((size_t) n + 1);
    if (!*data || fread(*data, 1, (size_t) n, f) != (size_t) n) {
        free(*data);
        fclose(f);
        return -1;
    }
    fclose(f);
    *size = (uint32_t) n;
    return 0;
}

static void import_path(import_ctx *ctx, const char *path, int depth);

static void
import_zip(import_ctx *ctx, zipread *z)
{
    for (int i = 0; i < zipread_count(z); i++) {
        uint8_t *data = NULL;
        uint32_t size = 0;
        if (!size_wanted(zipread_size(z, i)))
            continue;
        if (zipread_extract(z, i, &data, &size, 64u << 20) == 0) {
            take(ctx, data, size);
            free(data);
        }
    }
}

static void
import_dir(import_ctx *ctx, const char *dir, int depth)
{
    char path[MSDOS_PATH_MAX * 2];
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(path, sizeof path, "%s\\*", dir);
    h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, fd.cFileName);
        import_path(ctx, path, depth + 1);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        import_path(ctx, path, depth + 1);
    }
    closedir(d);
#endif
}

static void
import_path(import_ctx *ctx, const char *path, int depth)
{
    struct stat st;
    zipread *z;
    if (depth > 8 || stat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        import_dir(ctx, path, depth);
        return;
    }
    z = zipread_open(path);
    if (z) {
        import_zip(ctx, z);
        zipread_close(z);
        return;
    }
    if (size_wanted((uint32_t) st.st_size)) {
        uint8_t *data;
        uint32_t size;
        if (read_whole(path, &data, &size) == 0) {
            take(ctx, data, size);
            free(data);
        }
    }
}

int
msdossession_rom_import(msdossession *s, const char *path, char *msg, int msgsz)
{
    import_ctx ctx;
    int before[64] = { 0 };
    int len = 0;

    memset(&ctx, 0, sizeof ctx);
    ctx.s = s;
    for (int i = 0; i < msdos_rom_item_count && i < 64; i++)
        before[i] = roms_item_complete(s, i, NULL);
    import_path(&ctx, path, 0);

    if (msg && msgsz > 0) {
        msg[0] = 0;
        for (int i = 0; i < msdos_rom_item_count && i < 64; i++) {
            if (!before[i] && roms_item_complete(s, i, NULL))
                len += snprintf(msg + len, (size_t) (msgsz > len ? msgsz - len : 0), "%s%s",
                                len ? ", " : "Now available: ", msdos_rom_items[i].name);
        }
        if (!len) {
            if (ctx.imported)
                snprintf(msg, (size_t) msgsz,
                         "Imported %d file%s; nothing new is complete yet", ctx.imported,
                         ctx.imported == 1 ? "" : "s");
            else
                snprintf(msg, (size_t) msgsz, "No ROM the machines here use was found in %s",
                         path);
        }
    }
    return ctx.imported > 0 ? 0 : -1;
}
