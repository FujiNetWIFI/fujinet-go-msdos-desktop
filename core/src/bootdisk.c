/*
 * The FujiNet boot floppy: the embedded image for the drive's size, written
 * to <data>/disks, with CONFIG.SYS rewritten in place to match the FujiNet
 * COM port and speed.
 *
 * A floppy the user has written to stays theirs: a newer embedded image
 * replaces it only while it still matches what was last written (the
 * CRC in the .stamp beside it).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootdisk_data.h"
#include "session_internal.h"

static const msdos_bootdisk_image *
image_for(const char *fdd_type)
{
    int kb = 360;
    if (!strcmp(fdd_type, "35_2dd"))
        kb = 720;
    else if (!strcmp(fdd_type, "35_2hd") || !strcmp(fdd_type, "35_2ed"))
        kb = 1440;
    for (int i = 0; i < msdos_bootdisk_image_count; i++)
        if (msdos_bootdisk_images[i].size_kb == kb)
            return &msdos_bootdisk_images[i];
    return msdos_bootdisk_image_count ? &msdos_bootdisk_images[0] : NULL;
}

static uint32_t
embedded_id(const msdos_bootdisk_image *img)
{
    uint32_t crc = 0;
    for (int r = 0; r < img->nruns; r++) {
        crc = roms_crc32((const uint8_t *) &img->runs[r].offset, sizeof img->runs[r].offset, crc);
        crc = roms_crc32(img->runs[r].data, img->runs[r].len, crc);
    }
    return crc;
}

static uint32_t
file_crc(const char *path, int *ok)
{
    FILE *f = fopen(path, "rb");
    uint8_t buf[65536];
    size_t n;
    uint32_t crc = 0;
    *ok = 0;
    if (!f)
        return 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        crc = roms_crc32(buf, n, crc);
    fclose(f);
    *ok = 1;
    return crc;
}

static int
write_image(const msdos_bootdisk_image *img, const char *path)
{
    FILE *f = fopen(path, "wb");
    uint8_t zero[512] = { 0 };
    if (!f)
        return -1;
    for (uint32_t off = 0; off < img->bytes; off += sizeof zero)
        fwrite(zero, 1, sizeof zero, f);
    for (int r = 0; r < img->nruns; r++) {
        fseek(f, (long) img->runs[r].offset, SEEK_SET);
        fwrite(img->runs[r].data, 1, img->runs[r].len, f);
    }
    return fclose(f);
}

/* The same text tools/disks/make-boot-floppy.py writes, padded with REM
 * lines to the fixed size. */
static void
config_sys(char *out, uint32_t size, int port, int bps)
{
    int n = snprintf(out, size + 1,
                     "DEVICE=FUJINET.SYS FUJI_PORT=%d FUJI_BPS=%d\r\n"
                     "DEVICE=FUJIPRN.SYS\r\n"
                     "LASTDRIVE=Z\r\n"
                     "FILES=20\r\n"
                     "BUFFERS=20\r\n", port, bps);
    while ((uint32_t) n < size) {
        uint32_t room = size - (uint32_t) n;
        if (room < 5) {
            memset(out + n, ' ', room);
            n += (int) room;
            break;
        }
        uint32_t line = (room < 64 ? room : 64) - 2;
        memset(out + n, ' ', line);
        memcpy(out + n, "REM", 3);
        n += (int) line;
        out[n++] = '\r';
        out[n++] = '\n';
    }
}

static int
update_config_sys(const msdos_bootdisk_image *img, const char *path, int port, int bps)
{
    char want[1024], have[1024];
    FILE *f;
    if (img->config_size >= sizeof want)
        return -1;
    config_sys(want, img->config_size, port, bps);
    f = fopen(path, "r+b");
    if (!f)
        return -1;
    fseek(f, (long) img->config_offset, SEEK_SET);
    if (fread(have, 1, img->config_size, f) != img->config_size
        || memcmp(have, want, img->config_size) != 0) {
        fseek(f, (long) img->config_offset, SEEK_SET);
        fwrite(want, 1, img->config_size, f);
    }
    return fclose(f);
}

int
bootdisk_path(struct msdossession *s, const char *fdd_type, char *out, int outsz)
{
    const msdos_bootdisk_image *img = image_for(fdd_type);
    char path[MSDOS_PATH_MAX + 32], stamp[MSDOS_PATH_MAX + 40];
    unsigned id = 0, written = 0;
    int have, ok;
    uint32_t crc;
    FILE *f;

    if (!img)
        return -1;
    snprintf(path, sizeof path, "%s/fujinet-boot-%dk.img", s->disks_dir, img->size_kb);
    snprintf(stamp, sizeof stamp, "%s.stamp", path);

    f = fopen(stamp, "r");
    have = f && fscanf(f, "%x %x", &id, &written) == 2;
    if (f)
        fclose(f);
    crc = file_crc(path, &ok);

    if (!ok || (have && id != embedded_id(img) && crc == written)) {
        if (write_image(img, path) != 0) {
            session_set_error(s, "cannot write the boot floppy %s", path);
            return -1;
        }
    }
    update_config_sys(img, path, prefs_fujinet_com(s), prefs_fujinet_bps(s));

    crc = file_crc(path, &ok);
    f = fopen(stamp, "w");
    if (f) {
        fprintf(f, "%08x %08x\n", (unsigned) embedded_id(img), (unsigned) crc);
        fclose(f);
    }
    snprintf(out, (size_t) outsz, "%s", path);
    return 0;
}
