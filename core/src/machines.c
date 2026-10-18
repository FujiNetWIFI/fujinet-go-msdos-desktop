/*
 * The seven machines FujiNet Go MS-DOS offers, and the CPUs 86Box allows on
 * each (from its own tables, so a speed the board cannot run is never
 * offered).
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
#include "cpu.h"

#include "session_internal.h"

static const msdos_profile profiles[MSDOS_M_COUNT] = {
    [MSDOS_M_5150] = {
        "IBM 5150 PC", "ibmpc82", "m_5150", "8088", 4772728,
        640, 64, 640, 64, 256, "cga", "none", "525_2dd", "none",
        1, 0, 0, 1 },
    [MSDOS_M_5160] = {
        "IBM 5160 PC/XT", "ibmxt86", "m_5160", "8088", 4772728,
        640, 256, 640, 64, 640, "cga", "none", "525_2dd", "none",
        1, 0, 0, 1 },
    [MSDOS_M_PCJR] = {
        "IBM 4860 PCjr", "ibmpcjr", "m_pcjr", "8088", 4772728,
        640, 128, 640, 64, 640, "internal", "none", "525_2dd", "none",
        0, 0, 1, 0 },
    [MSDOS_M_286] = {
        "Typical 286", "ami286", "m_286", "286", 12500000,
        1024, 512, 8192, 128, 8192, "vga", "sbprov2", "525_2hd", "35_2hd",
        0, 1, 0, 0 },
    [MSDOS_M_386] = {
        "Typical 386", "ecs386v", "m_386", "am386dx", 40000000,
        4096, 1024, 65536, 1024, 65536, "tvga8900", "sb16", "35_2hd", "525_2hd",
        0, 1, 0, 0 },
    [MSDOS_M_486] = {
        "Typical 486", "dtk486", "m_486", "i486dx2", 66666666,
        16384, 1024, 131072, 1024, 131072, "tvga8900", "sb16", "35_2hd", "525_2hd",
        0, 1, 0, 0 },
    [MSDOS_M_PENTIUM] = {
        "Typical Pentium", "p54tp4xe", "m_pentium", "pentium_p54c", 133333333,
        32768, 8192, 131072, 8192, 131072, "tvga8900", "sb16", "35_2hd", "525_2hd",
        0, 0, 0, 0 },
};

/* The families a typical machine of each kind had. */
static const char *const families[MSDOS_M_COUNT][5] = {
    [MSDOS_M_5150] = { "8088" },
    [MSDOS_M_5160] = { "8088" },
    [MSDOS_M_PCJR] = { "8088" },
    [MSDOS_M_286] = { "286" },
    [MSDOS_M_386] = { "i386dx", "am386dx" },
    [MSDOS_M_486] = { "i486sx", "i486dx", "i486dx2", "am486dx4" },
    [MSDOS_M_PENTIUM] = { "pentium_p54c" },
};

const msdos_profile *
msdos_profile_get(int m)
{
    if (m < 0 || m >= MSDOS_M_COUNT)
        m = MSDOS_M_5160;
    return &profiles[m];
}

int
msdos_test_glabios(int m)
{
    const char *v = getenv("MSDOS_TEST_GLABIOS");
    return v && *v && *v != '0' && (m == MSDOS_M_5150 || m == MSDOS_M_5160);
}

const char *
msdos_profile_machine86(int m)
{
    return msdos_test_glabios(m) ? "glabios" : msdos_profile_get(m)->internal;
}

const char *
msdos_machine_name(int m)
{
    return (m >= 0 && m < MSDOS_M_COUNT) ? profiles[m].name : "";
}

int
msdos_cpu_choices(int m, msdos_cpu_choice *out, int max)
{
    int mach = machine_get_machine_from_internal_name(msdos_profile_machine86(m));
    int n    = 0;

    if (mach < 0)
        return 0;
    for (int k = 0; k < 5 && families[m][k]; k++) {
        for (int f = 0; cpu_families[f].package != 0; f++) {
            const cpu_family_t *fam = &cpu_families[f];
            if (strcmp(fam->internal_name, families[m][k]) != 0)
                continue;
            if (!cpu_family_is_eligible(fam, mach))
                break;
            for (int c = 0; fam->cpus[c].cpu_type != 0; c++) {
                if (!cpu_is_eligible(fam, c, mach) || n >= max)
                    continue;
                snprintf(out[n].family, sizeof out[n].family, "%s", fam->internal_name);
                out[n].speed = fam->cpus[c].rspeed;
                out[n].multi = fam->cpus[c].multi;
                snprintf(out[n].label, sizeof out[n].label, "%s %s %s MHz",
                         fam->manufacturer, fam->name, fam->cpus[c].name);
                n++;
            }
            break;
        }
    }
    return n;
}
