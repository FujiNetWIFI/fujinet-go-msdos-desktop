/*
 * memtools_test -- the pure helpers in core/src/memtools.c that need no PC:
 * region names, the watch list's storage rules, breakpoint descriptions.
 * (Parsing that goes through the engine is prompt_test's.)
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test_util.h"
#include "msdosdebug.h"
#include "msdosmemtools.h"

static int failures;
#define EXPECT(cond, ...) do { if (!(cond)) { fprintf(stderr, "memtools_test: " __VA_ARGS__); fputc('\n', stderr); failures = 1; } } while (0)

int main(void)
{
    msdossession *s = test_session_ex("memtools", 0, 0);
    msdosdebug *d;
    char text[200];
    msdos_region map[4] = { { 0, 0x9FFFF, 1, 1, "Conventional memory (640 KB)" },
                            { 0xA0000, 0xBFFFF, 1, 1, "Video memory" },
                            { 0xF0000, 0xFFFFF, 1, 0, "System BIOS (ROM)" },
                            { 0x100000, 0x1FFFFF, 1, 1, "Extended memory" } };
    msdos_breakpoint b;
    unsigned gen;
    int i;
    CHECK(s, "session");
    d = msdossession_debugger(s);

    /* region names, edges, and a gap */
    EXPECT(msdosmem_region_name(map, 4, 0, text, sizeof text) > 0 && strstr(text, "Conventional"), "low edge");
    EXPECT(msdosmem_region_name(map, 4, 0x9FFFF, text, sizeof text) > 0 && strstr(text, "Conventional"), "high edge");
    EXPECT(msdosmem_region_name(map, 4, 0xA0000, text, sizeof text) > 0 && strstr(text, "Video"), "next region");
    EXPECT(msdosmem_region_name(map, 4, 0xC0000, text, sizeof text) == 0 && !text[0], "a gap names nothing");
    EXPECT(msdosmem_region_name(map, 4, 0xFFFFF, text, sizeof text) > 0 && strstr(text, "BIOS"), "BIOS");
    EXPECT(msdosmem_region_name(NULL, 0, 0, text, sizeof text) == 0, "no map");

    /* the watch list: bounds, trimming, generation */
    msdoswatch_clear();
    gen = msdoswatch_generation();
    EXPECT(msdoswatch_add("") == -1 && msdoswatch_add("   ") == -1 && msdoswatch_add(NULL) == -1, "empty refused");
    EXPECT(msdoswatch_add("  B800:0  ") == 0, "added");
    EXPECT(msdoswatch_text(0, text, sizeof text) == 6 && !strcmp(text, "B800:0"), "trimmed: %s", text);
    EXPECT(msdoswatch_generation() != gen, "generation moved");
    for (i = 1; i < MSDOSWATCH_MAX; i++)
        EXPECT(msdoswatch_add("1000") == i, "fill %d", i);
    EXPECT(msdoswatch_add("1000") == -1, "full");
    msdoswatch_remove(0);
    EXPECT(msdoswatch_count() == MSDOSWATCH_MAX - 1, "remove");
    msdoswatch_remove(500);
    msdoswatch_remove(-1);
    EXPECT(msdoswatch_count() == MSDOSWATCH_MAX - 1, "bad remove");
    EXPECT(msdoswatch_text(1000, text, sizeof text) == 0 && !text[0], "past the end");
    msdoswatch_clear();
    EXPECT(msdoswatch_count() == 0, "clear");

    /* format with no PC: stopped() is false, so the value is "-" */
    msdoswatch_add("1000");
    EXPECT(msdoswatch_format(d, 0, text, sizeof text) > 0 && strstr(text, "1000") && strstr(text, " -"), "format running: '%s'", text);
    msdoswatch_add("nope_not_a_symbol");
    EXPECT(msdoswatch_format(d, 1, text, sizeof text) > 0 && strstr(text, "?"), "format unresolved: '%s'", text);
    EXPECT(msdoswatch_format(d, 9, text, sizeof text) == 0, "format past the end");
    msdoswatch_clear();

    /* breakpoint descriptions */
    msdosdebug_set_label(d, 0x500, "entry");
    memset(&b, 0, sizeof b);
    b.type = MSDOS_BP_EXEC;
    b.start = b.end = 0x500;
    snprintf(b.text, sizeof b.text, "Execute 00500");
    EXPECT(msdosbp_describe(d, &b, text, sizeof text) > 0 && strstr(text, "Execute") && strstr(text, "(entry)"), "describe: %s", text);
    snprintf(b.condition, sizeof b.condition, "ax == 1");
    EXPECT(msdosbp_describe(d, &b, text, sizeof text) > 0 && strstr(text, "if ax == 1"), "condition: %s", text);
    b.type = MSDOS_BP_OUT;
    snprintf(b.text, sizeof b.text, "OUT 02F8");
    b.start = b.end = 0x500;
    EXPECT(msdosbp_describe(d, &b, text, sizeof text) > 0 && !strstr(text, "entry"), "a port gets no symbol: %s", text);
    EXPECT(msdosbp_describe(d, &b, text, 5) == 4, "truncation");

    msdossession_free(s);
    if (failures) return 1;
    puts("memtools: ok");
    return 0;
}
