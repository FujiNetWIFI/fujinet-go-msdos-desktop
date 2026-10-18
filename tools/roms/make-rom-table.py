#!/usr/bin/env python3
"""Generate core/src/rom_table.c: every ROM file the offered machines and
cards can use, with its size and CRC-32, from an 86Box ROM set.

    make-rom-table.py --roms DIR [--source "86Box/roms v6.0"] [--out FILE]

DIR is the root of an 86Box ROM set (machines/, video/, ...). The paths are
86Box's own (they are where the import puts a file); the CRCs identify a file
whatever it is called. --source is recorded in the output as where the CRCs
came from (core/tests/rom_table_test.py checks it is there).

Copyright (C) 2026 Thomas Cherryhomes
SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import os
import zlib

# (key, name, group, config section for the BIOS choice or None,
#  [(bios internal name or None, [86Box paths under roms/]), ...])
# One complete variant makes the item available; the first complete one is
# the one configured.
ITEMS = [
    ("m_5150", "IBM 5150 PC BIOS (1982)", "Machines", "IBM PC (1982)", [
        ("ibm5150_1501476", ["machines/ibmpc82/BIOS_5150_27OCT82_1501476_U33.BIN"]),
        ("ibm5150_5000024", ["machines/ibmpc82/BIOS_5150_16AUG82_5000024_U33.BIN"]),
    ]),
    ("m_5150_basic", "IBM Cassette BASIC 1.10", "Machines", None, [
        (None, ["machines/ibmpc82/ibm-basic-1.10.rom"]),
        (None, ["machines/ibmpc82/IBM 5150 - Cassette BASIC version C1.10 - U29 - 5000019.bin",
                "machines/ibmpc82/IBM 5150 - Cassette BASIC version C1.10 - U30 - 5000021.bin",
                "machines/ibmpc82/IBM 5150 - Cassette BASIC version C1.10 - U31 - 5000022.bin",
                "machines/ibmpc82/IBM 5150 - Cassette BASIC version C1.10 - U32 - 5000023.bin"]),
    ]),
    ("m_5160", "IBM 5160 PC/XT BIOS (1986)", "Machines", "IBM XT (1986)", [
        ("ibm5160_050986", ["machines/ibmxt86/BIOS_5160_09MAY86_U18_59X7268_62X0890_27256_F800.BIN",
                            "machines/ibmxt86/BIOS_5160_09MAY86_U19_62X0819_68X4370_27256_F000.BIN"]),
        ("ibm5160_011086", ["machines/ibmxt86/BIOS_5160_10JAN86_U18_62X0851_27256_F800.BIN",
                            "machines/ibmxt86/BIOS_5160_10JAN86_U19_62X0854_27256_F000.BIN"]),
    ]),
    ("m_pcjr", "IBM 4860 PCjr BIOS", "Machines", None, [
        (None, ["machines/ibmpcjr/bios.rom"]),
    ]),
    ("m_286", "AMI 286 BIOS (DataExpert NEAT)", "Machines", None, [
        (None, ["machines/ami286/AMIC206.BIN"]),
    ]),
    ("m_386", "ECS Panda 386V BIOS", "Machines", None, [
        (None, ["machines/ecs386v/PANDA_386V.BIN"]),
    ]),
    ("m_486", "DTK PKM-0038S E-2 BIOS", "Machines", None, [
        (None, ["machines/dtk486/4siw005.bin"]),
    ]),
    ("m_pentium", "ASUS P/I-P55TP4XE BIOS", "Machines", "ASUS P/I-P55TP4XE", [
        ("p54tp4xe", ["machines/p54tp4xe/t15i0302.awd"]),
        ("p54tp4xe_mr", ["machines/p54tp4xe/TRITON.BIO"]),
    ]),
    ("v_font", "IBM MDA/CGA character set", "Video", None, [
        (None, ["video/mda/mda.rom"]),
    ]),
    ("v_ega", "IBM EGA BIOS", "Video", None, [
        (None, ["video/ega/ibm_6277356_ega_card_u44_27128.bin"]),
    ]),
    ("v_vga", "IBM VGA BIOS", "Video", None, [
        (None, ["video/vga/ibm_vga.bin"]),
    ]),
    ("v_tvga8900", "Trident TVGA 8900 BIOS", "Video", "Trident TVGA 8900", [
        ("tvga8900dr", ["video/tvga/8900DR.VBI"]),
        ("tvga8900d", ["video/tvga/trident.bin"]),
        ("tvga8900b", ["video/tvga/tvga8900b.vbi"]),
    ]),
    ("h_xebec", "IBM Fixed Disk Adapter (Xebec) BIOS", "Disk controllers", None, [
        (None, ["hdd/st506/ibm_xebec_62x0822_1985.bin"]),
    ]),
    ("h_wd1002a", "Western Digital WD1002A-WX1 BIOS", "Disk controllers", None, [
        (None, ["hdd/st506/wd1002a_wx1-62-000094-032.bin"]),
    ]),
    ("h_dtc5150x", "DTC 5150X BIOS", "Disk controllers", None, [
        (None, ["hdd/st506/dtc_cxd21a.bin"]),
    ]),
    ("h_st11m", "Seagate ST-11M BIOS", "Disk controllers", None, [
        (None, ["hdd/st506/st11_bios_vers_1.7.bin", "hdd/st506/st11_bios_vers_2.0.bin"]),
    ]),
    ("h_xtide", "XTIDE Universal BIOS (XT)", "Disk controllers", None, [
        (None, ["hdd/xtide/ide_xt.bin"]),
    ]),
    ("s_awe32", "Sound Blaster AWE32 wavetable", "Sound", None, [
        (None, ["sound/creative/awe32.raw"]),
    ]),
    ("s_mt32", "Roland MT-32", "Sound", None, [
        (None, ["sound/mt32/MT32_CONTROL.ROM", "sound/mt32/MT32_PCM.ROM"]),
    ]),
    ("s_cm32l", "Roland CM-32L", "Sound", None, [
        (None, ["sound/cm32l/CM32L_CONTROL.ROM", "sound/cm32l/CM32L_PCM.ROM"]),
    ]),
]


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"' if s is not None else "NULL"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roms", required=True)
    ap.add_argument("--source", default="86Box/roms v6.0")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "..",
                                                   "core", "src", "rom_table.c"))
    args = ap.parse_args()

    files = {}
    for _, _, _, _, variants in ITEMS:
        for _, paths in variants:
            for p in paths:
                full = os.path.join(args.roms, p)
                with open(full, "rb") as f:
                    data = f.read()
                files[p] = (len(data), zlib.crc32(data) & 0xFFFFFFFF)

    out = ["/* Generated by tools/roms/make-rom-table.py -- do not edit.",
           f" * CRCs from {args.source}. */",
           "#include \"rom_table.h\"", "",
           f"const char msdos_rom_table_source[] = {c_str(args.source)};", "",
           "const msdos_rom_file msdos_rom_files[] = {"]
    index = {}
    for i, (p, (size, crc)) in enumerate(sorted(files.items())):
        index[p] = i
        out.append(f"    {{ {c_str(p)}, {size}u, 0x{crc:08x}u }},")
    out.append("};")
    out.append(f"const int msdos_rom_file_count = {len(files)};")
    out.append("")
    out.append("const msdos_rom_item_def msdos_rom_items[] = {")
    for key, name, group, section, variants in ITEMS:
        vs = []
        for bios, paths in variants:
            idx = ", ".join(str(index[p]) for p in paths)
            vs.append(f"{{ {c_str(bios)}, {len(paths)}, {{ {idx} }} }}")
        out.append(f"    {{ {c_str(key)}, {c_str(name)}, {c_str(group)}, {c_str(section)}, "
                   f"{len(variants)}, {{ {', '.join(vs)} }} }},")
    out.append("};")
    out.append(f"const int msdos_rom_item_count = {len(ITEMS)};")
    out.append("")
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
