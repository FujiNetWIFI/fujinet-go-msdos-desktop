"""FAT12 floppy images: read the files out of one, and build a bootable one.

Enough of FAT12 for 360K-1.44M floppies with files in the root directory
only, which is all the FujiNet boot floppy needs. Pure Python so it runs
wherever the build does (flatpak SDK, MSYS2, CI runners).

Copyright (C) 2026 Thomas Cherryhomes
SPDX-License-Identifier: GPL-3.0-or-later
"""

import struct

# size in KB -> (sectors, sectors/cluster, root entries, media, sectors/FAT,
#                sectors/track, heads)
GEOMETRY = {
    360:  (720, 2, 112, 0xFD, 2, 9, 2),
    720:  (1440, 2, 112, 0xF9, 3, 9, 2),
    1200: (2400, 1, 224, 0xF9, 7, 15, 2),
    1440: (2880, 1, 224, 0xF0, 9, 18, 2),
}
SECTOR = 512


def _fat12_get(fat, n):
    off = n * 3 // 2
    v = fat[off] | (fat[off + 1] << 8)
    return (v >> 4) if n & 1 else (v & 0xFFF)


def _fat12_set(fat, n, val):
    off = n * 3 // 2
    if n & 1:
        fat[off] = (fat[off] & 0x0F) | ((val << 4) & 0xF0)
        fat[off + 1] = (val >> 4) & 0xFF
    else:
        fat[off] = val & 0xFF
        fat[off + 1] = (fat[off + 1] & 0xF0) | ((val >> 8) & 0x0F)


def read_files(image):
    """{NAME.EXT: bytes} for every file in the root directory."""
    bps, spc, reserved, nfats, nroot, total, media, spf = struct.unpack_from(
        "<HBHBHHBH", image, 11)
    root_start = (reserved + nfats * spf) * bps
    data_start = root_start + ((nroot * 32 + bps - 1) // bps) * bps
    fat = image[reserved * bps:(reserved + spf) * bps]
    out = {}
    for i in range(nroot):
        e = image[root_start + i * 32:root_start + i * 32 + 32]
        if e[0] == 0:
            break
        if e[0] == 0xE5 or e[11] & 0x18:      # deleted, volume label, directory
            continue
        name = e[0:8].decode("ascii", "replace").rstrip()
        ext = e[8:11].decode("ascii", "replace").rstrip()
        full = (name + "." + ext if ext else name).upper()
        cluster, size = struct.unpack_from("<HI", e, 26)
        data = bytearray()
        while 2 <= cluster < 0xFF8 and len(data) < size:
            off = data_start + (cluster - 2) * spc * bps
            data += image[off:off + spc * bps]
            cluster = _fat12_get(fat, cluster)
        out[full] = bytes(data[:size])
    return out


def _dos_datetime(y, mo, d, h=0, mi=0, s=0):
    return ((h << 11) | (mi << 5) | (s // 2), ((y - 1980) << 9) | (mo << 5) | d)


def build(size_kb, bootsect, files, label="FUJINET", serial=0x46554A49,
          stamp=(2026, 1, 1)):
    """A formatted image carrying files (list of (NAME.EXT, bytes), in order,
    each contiguous) and the offsets of each one's data: {NAME: (offset, size)}."""
    sectors, spc, nroot, media, spf, spt, heads = GEOMETRY[size_kb]
    img = bytearray(sectors * SECTOR)

    bs = bytearray(bootsect[:SECTOR])
    struct.pack_into("<HBHBHHBHHHII", bs, 11,
                     SECTOR, spc, 1, 2, nroot, sectors, media, spf, spt, heads, 0, 0)
    bs[36] = 0          # drive number
    bs[37] = 0
    bs[38] = 0x29       # extended boot signature
    struct.pack_into("<I", bs, 39, serial)
    bs[43:54] = label.upper().ljust(11)[:11].encode("ascii")
    bs[54:62] = b"FAT12   "
    bs[510:512] = b"\x55\xAA"
    img[0:SECTOR] = bs

    fat = bytearray(spf * SECTOR)
    _fat12_set(fat, 0, 0xF00 | media)
    _fat12_set(fat, 1, 0xFFF)

    root_start = (1 + 2 * spf) * SECTOR
    root_sectors = (nroot * 32 + SECTOR - 1) // SECTOR
    data_start = root_start + root_sectors * SECTOR
    clusters = (sectors - data_start // SECTOR) // spc
    csize = spc * SECTOR

    t, d = _dos_datetime(*stamp)
    root = bytearray(nroot * 32)
    # the volume label first, as FORMAT writes it
    root[0:11] = label.upper().ljust(11)[:11].encode("ascii")
    root[11] = 0x08
    struct.pack_into("<HH", root, 22, t, d)

    where = {}
    next_cluster = 2
    for idx, (fullname, data) in enumerate(files, start=1):
        name, _, ext = fullname.upper().partition(".")
        n = max(1, (len(data) + csize - 1) // csize) if data else 0
        first = next_cluster if n else 0
        if next_cluster + n - 2 > clusters:
            raise ValueError(f"{fullname} does not fit on a {size_kb}K floppy")
        for k in range(n):
            c = next_cluster + k
            _fat12_set(fat, c, 0xFFF if k == n - 1 else c + 1)
        if n:
            off = data_start + (first - 2) * csize
            img[off:off + len(data)] = data
            where[fullname.upper()] = (off, len(data))
        next_cluster += n
        e = bytearray(32)
        e[0:8] = name.ljust(8)[:8].encode("ascii")
        e[8:11] = ext.ljust(3)[:3].encode("ascii")
        e[11] = 0x20
        struct.pack_into("<HHHI", e, 22, t, d, first, len(data))
        root[idx * 32:idx * 32 + 32] = e

    img[SECTOR:SECTOR + len(fat)] = fat
    img[SECTOR + len(fat):SECTOR + 2 * len(fat)] = fat
    img[root_start:root_start + len(root)] = root
    return bytes(img), where
