#!/usr/bin/env python3
"""Assert that a frontend binary carries no PC ROM.

This is the test that makes the COMPLIANCE.md claim checkable rather than
aspirational: it takes a distinctive slice out of each ROM image under the
ROM directory (any layout -- 86Box's roms tree works -- and .zip files are
looked inside) and greps the built binary for it. Anything found is a ROM
that got compiled in, which would make the artifact non-redistributable.

IBM's, AMI's and Award's BIOSes, the video, disk-controller and MIDI ROMs
are copyrighted firmware the user imports; no build embeds any.

usage: no_embedded_roms.py <binary> <rom-dir>
"""

import os
import sys
import zipfile
from pathlib import Path

PROBE = 64
SKIP_SUFFIXES = {".py", ".pyc", ".md", ".txt", ".cfg", ".json", ".sh", ".gitignore"}


def probe_bytes(data):
    """A slice distinctive enough that finding it means something.

    Reject any chunk dominated by a single byte value and require real
    variety, so a run of 0x00 or 0xFF padding -- which occurs in almost any
    binary of a reasonable size -- can never be mistaken for a match. Scan the
    whole file rather than a few fixed offsets.
    """
    max_run = PROBE // 2      # no single byte value may cover half the slice
    min_distinct = 12

    for start in range(0, max(1, len(data) - PROBE), PROBE):
        chunk = data[start:start + PROBE]
        if len(chunk) != PROBE:
            continue
        if max(chunk.count(b) for b in set(chunk)) > max_run:
            continue
        if len(set(chunk)) < min_distinct:
            continue
        return chunk
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    binary = Path(sys.argv[1]).read_bytes()
    romdir = Path(sys.argv[2])

    checked = 0
    leaked = []
    unprobeable = []

    images = []
    files = []
    if romdir.is_dir():
        # os.walk: a developer's set is often a symlink to an 86Box roms tree
        for top, dirs, names in os.walk(romdir, followlinks=True):
            dirs[:] = [d for d in dirs if d != "__pycache__"]
            files += [Path(top) / n for n in names]
    for f in sorted(files):
        if not f.is_file() or f.suffix.lower() in SKIP_SUFFIXES:
            continue
        if f.suffix.lower() == ".zip":
            try:
                with zipfile.ZipFile(f) as z:
                    for name in z.namelist():
                        if not name.endswith("/"):
                            images.append((f.name + ":" + name, z.read(name)))
            except zipfile.BadZipFile:
                pass
        else:
            images.append((str(f.relative_to(romdir)), f.read_bytes()))

    for name, data in images:
        if len(data) < 1024:
            continue
        chunk = probe_bytes(data)
        if chunk is None:
            # Nothing distinctive enough to search for -- report it rather
            # than quietly treating it as checked.
            unprobeable.append(name)
            continue
        checked += 1
        if chunk in binary:
            leaked.append(name)

    if checked == 0:
        # No real ROMs to check against -- true of every CI and release build,
        # since the copyrighted ROMs (see COMPLIANCE.md) are gitignored and
        # never present on a checkout that isn't a developer's own machine.
        # SKIP (ctest exit code 77), not fail: there is nothing wrong with the
        # binary, just nothing here to probe it with.
        print("no_embedded_roms: SKIP: no ROM images found in "
              f"{romdir} -- nothing to check against")
        return 77

    for name in leaked:
        print(f"FAIL: {name} is embedded in the binary")

    if leaked:
        return 1
    if unprobeable:
        print(f"no_embedded_roms: {len(unprobeable)} images NOT checked "
              "(no distinctive slice)")
    print(f"no_embedded_roms: {checked} ROM images checked; none embedded, "
          "as intended")
    return 0


if __name__ == "__main__":
    sys.exit(main())
