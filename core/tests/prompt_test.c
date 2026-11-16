/*
 * prompt_test -- the debugger console (core/src/prompt.cpp) on an engine with no
 * PC behind it: the commands that only touch the engine's own state
 * (expressions, breakpoints with conditions, the watch list, symbols, help,
 * completion) and what is refused. Anything that needs the machine is
 * debug_test's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test_util.h"
#include "msdosdebug.h"
#include "msdosmemtools.h"

static int failures;
static msdosdebug *g_dbg;

#define EXPECT(cond, ...) do { if (!(cond)) { fprintf(stderr, "prompt_test: " __VA_ARGS__); fputc('\n', stderr); failures = 1; } } while (0)

static const char *run(const char *line)
{
    static char out[16384];
    msdosdebug_command(g_dbg, line, out, sizeof out);
    return out;
}

static int has(const char *line, const char *want)
{
    const char *got = run(line);
    if (!strstr(got, want)) {
        fprintf(stderr, "prompt_test: \"%s\" printed \"%s\", want \"%s\"\n", line, got, want);
        failures = 1;
        return 0;
    }
    return 1;
}

int main(void)
{
    msdossession *s = test_session_ex("prompt", 0, 0);
    msdos_breakpoint list[32];
    char text[1024], err[160];
    uint32_t lin;
    int count, id;

    CHECK(s, "session");
    g_dbg = msdossession_debugger(s);
    CHECK(g_dbg, "debugger");

    /* print: every notation, with the binary form */
    has("print $B800 + 16", "$B810  47120  %1011100000010000");
    has("print 0x10 * 3 - 1", "$002F  47");
    has("print %1010 | 1", "$000B  11");
    has("print (1+2)*3", "$0009  9");
    has("print $100000 + 1", "$00100001  1048577");
    has("print nosuchsymbol_zz", "error: unknown name 'nosuchsymbol_zz'");
    has("print 1 +", "error:");
    has("print 1/0", "division by zero");
    /* with no PC the registers read 0 and memory is open bus */
    has("print ax + 5", "$0005  5");
    has("print [$B8000]", "$00FF  255");
    has("print ds:si", "$0000  0");

    /* labels */
    has("label B8000 SCREEN", "B8000 = SCREEN");
    has("label 0060:0100 mainloop", "00700 = mainloop");
    has("print SCREEN + 1", "$000B8001  753665");
    has("print mainloop", "$0700");
    has("symbols screen", "B8000  SCREEN");
    has("symbols zzzzqqqq", "no symbols match");
    has("label B8000", "usage");
    CHECK(msdosdebug_symbol_find(g_dbg, "screen", &lin) == 0 && lin == 0xB8000, "label stored");
    CHECK(msdosdebug_symbols_count(g_dbg) == 2, "two symbols");

    /* breakpoints: every kind, ranges, conditions */
    has("break 0060:0100", "Breakpoint 1");
    has("bpw B8000-B8FFF value != old", "Breakpoint 2");
    has("bpr SCREEN", "Breakpoint 3");
    has("bpa 10000 value == 5 && addr == $10000", "Breakpoint 4");
    has("bpin 3DA", "Breakpoint 5");
    has("bpout 2F8-2FF value >= $80", "Breakpoint 6");
    has("break 600 ax == 1", "Breakpoint 7");
    has("break 610 ax == 1 && dword [ds:si] > $6000", "Breakpoint 8");
    has("bpint 21 4c", "Breakpoint 9");
    has("bpio 60", "Breakpoint 10");
    count = msdosdebug_bp_list(g_dbg, list, 32);
    EXPECT(count == 10, "%d breakpoints, want 10", count);
    EXPECT(list[0].type == MSDOS_BP_EXEC && list[0].start == 0x700, "exec at 0060:0100 is linear 700");
    EXPECT(list[1].type == MSDOS_BP_WRITE && list[1].end == 0xB8FFF, "the write range");
    EXPECT(!strcmp(list[1].condition, "value != old"), "the condition is kept as typed: \"%s\"", list[1].condition);
    EXPECT(list[3].type == MSDOS_BP_ACCESS && list[3].start == 0x10000, "the access entry");
    EXPECT(list[4].type == MSDOS_BP_IN && list[4].start == 0x3DA, "the IN entry");
    EXPECT(list[5].type == MSDOS_BP_OUT && list[5].end == 0x2FF, "the OUT range");
    EXPECT(list[8].type == MSDOS_BP_INT && list[8].start == 0x21 && list[8].ah == 0x4C, "INT 21h AH=4Ch");
    has("breaks", "4  Access");
    has("breaks", "if value == 5 && addr == $10000");
    has("break 640 nosuch == 1", "error:");
    has("break 640 {bad", "error:");
    has("break 640 ax ==", "error:");
    has("bpin 10000", "error: ports are 0-FFFF");
    has("bpa", "usage");
    count = msdosdebug_bp_list(g_dbg, list, 32);
    EXPECT(count == 10, "a refused breakpoint was added: %d", count);

    /* DEBUG-style lengths */
    has("bpw 20000 l 4", "Breakpoint 11");
    has("bpr 20100 10", "Breakpoint 12");
    count = msdosdebug_bp_list(g_dbg, list, 32);
    EXPECT(list[10].start == 0x20000 && list[10].end == 0x20003, "bpw l 4: %X-%X", list[10].start, list[10].end);
    EXPECT(list[11].start == 0x20100 && list[11].end == 0x2010F, "bpr 10 (hex): %X-%X", list[11].start, list[11].end);

    has("disable 2", "disabled 2");
    msdosdebug_bp_list(g_dbg, list, 32);
    EXPECT(!list[1].enabled, "disable");
    has("breaks", "(off)");
    has("enable #2", "enabled 2");
    has("hits clear", "hit counts cleared");
    has("delete 700", "deleted 1");
    has("delete 3", "deleted 3");
    has("delete 99", "no such breakpoint");
    has("bc 4", "deleted 4");
    count = msdosdebug_bp_list(g_dbg, list, 32);
    EXPECT(count == 9, "delete left %d", count);

    /* the API: add with a condition, set one, refuse one */
    id = msdosdebug_bp_add_if(g_dbg, MSDOS_BP_WRITE, 0x400, 0x4FF, -1, "value == 3", err, sizeof err);
    EXPECT(id > 0, "bp_add_if: %s", err);
    EXPECT(msdosdebug_bp_set_condition(g_dbg, id, "old == 1 || value", err, sizeof err) == 1, "set_condition: %s", err);
    EXPECT(msdosdebug_bp_set_condition(g_dbg, id, "zork", err, sizeof err) == 0 && strstr(err, "unknown name"), "bad condition: %s", err);
    EXPECT(msdosdebug_bp_set_condition(g_dbg, id, "", err, sizeof err) == 1, "remove a condition");
    EXPECT(msdosdebug_bp_set_condition(g_dbg, 999, "1", err, sizeof err) == 0, "no such breakpoint");
    count = msdosdebug_bp_list(g_dbg, list, 32);
    EXPECT(list[count - 1].condition[0] == 0, "condition removed");
    EXPECT(msdosbp_describe(g_dbg, &list[count - 1], text, sizeof text) > 0 && strstr(text, "Write"), "describe: %s", text);
    has("clear", "all breakpoints cleared");
    EXPECT(msdosdebug_bp_list(g_dbg, list, 32) == 0, "clear");

    /* the watch list (process-wide) */
    has("watch SCREEN", " 0  SCREEN");
    has("watch B8000-B8007", " 1  B8000-B8007");
    has("watch nosuch_symbol_q", "?");
    EXPECT(msdoswatch_count() == 3, "three watches");
    {
        uint32_t lo, hi;
        EXPECT(msdoswatch_range(g_dbg, 1, &lo, &hi) && lo == 0xB8000 && hi == 0xB8007, "watch range");
        EXPECT(!msdoswatch_range(g_dbg, 2, &lo, &hi), "an unresolved watch");
    }
    has("unwatch 2", "");
    EXPECT(msdoswatch_count() == 2, "unwatch");
    has("unwatch all", "");
    EXPECT(msdoswatch_count() == 0, "unwatch all");

    /* what needs the PC is refused, and says so */
    has("r", "The PC is off");
    has("d 0:0", "The PC is off");
    has("step", "The PC is off");
    has("find 41 42", "");
    has("zork", "Unknown command");
    has("help", "DEBUG.COM verbs");
    has("?", "bpint");
    has("map", "The PC is off");
    has("io", "The PC is off");
    has("sound", "The PC is off");
    CHECK(msdosdebug_command(g_dbg, "r", text, sizeof text) == -1, "r with no PC returns -1");
    CHECK(msdosdebug_command(g_dbg, "print 1", text, sizeof text) == 0, "print returns 0");
    EXPECT(msdosdebug_regs_set(g_dbg, &(msdos_cpu_regs){ 0 }, err, sizeof err) == 0 && strstr(err, "off"), "regs_set: %s", err);
    EXPECT(msdosdebug_register_set(g_dbg, "ax", 1, err, sizeof err) == 0, "register_set with no PC");
    {
        msdos_stop_snapshot snap;
        EXPECT(msdosdebug_stop_snapshot(g_dbg, &snap) == 0 && !strcmp(snap.text, "running") && snap.address == -1, "stop snapshot of nothing");
    }

    /* completion */
    EXPECT(msdosdebug_completions(g_dbg, "bpw", text, sizeof text) == 1 && !strcmp(text, "bpw\n"), "complete bpw: %s", text);
    EXPECT(msdosdebug_completions(g_dbg, "bp", text, sizeof text) >= 6 && strstr(text, "bpint"), "complete bp: %s", text);
    EXPECT(msdosdebug_completions(g_dbg, "print al", text, sizeof text) >= 1 && strstr(text, "al\n"), "complete a register: %s", text);
    EXPECT(msdosdebug_completions(g_dbg, "print scr", text, sizeof text) == 1 && strstr(text, "SCREEN"), "complete a symbol: %s", text);
    EXPECT(msdosdebug_completions(g_dbg, "save ", text, sizeof text) == 3, "save lists its kinds: %s", text);
    EXPECT(msdosdebug_completions(g_dbg, "save m", text, sizeof text) == 1 && !strcmp(text, "mem\n"), "complete save kind: %s", text);

    /* symbols from a file: MS LINK, WLINK and a plain list */
    {
        char dir[600], path[700];
        FILE *f;
        char msg[200];
        int picks[16];
        uint32_t at;
        char name[64];
        test_tmpdir(dir, sizeof dir, "prompt-sym");
        snprintf(path, sizeof path, "%s/prog.map", dir);
        f = fopen(path, "w");
        CHECK(f, "map");
        fputs(" 0000:0010       _main\n 0000:0020       _helper\n0000:0030+     wlink_sym_\n"
              "00500 plain_sym\n 0000:0040       _helper_alias\n", f);
        fclose(f);
        msdosdebug_clear_symbols(g_dbg);
        EXPECT(msdosdebug_symbols_count(g_dbg) == 0, "cleared");
        EXPECT(msdosdebug_load_symbols(g_dbg, path, 0x0070, msg, sizeof msg) == 0, "load: %s", msg);
        EXPECT(msdosdebug_symbols_count(g_dbg) == 5, "symbols loaded: %d", msdosdebug_symbols_count(g_dbg));
        count = msdosdebug_symbols_filter(g_dbg, "HELPER", picks, 16);
        EXPECT(count == 2, "filter helper: %d", count);
        EXPECT(msdosdebug_symbols_get(g_dbg, picks[0], &at, name, sizeof name) && at == 0x720 && !strcmp(name, "_helper"), "get: %X %s", at, name);
        EXPECT(msdosdebug_symbols_get(g_dbg, 99, &at, name, sizeof name) == 0, "past the end");
        EXPECT(msdosdebug_symbol_at(g_dbg, 0x725, text, sizeof text) > 0 && !strcmp(text, "_helper+5"), "symbol_at: %s", text);
        EXPECT(msdosdebug_symbol_find(g_dbg, "plain_sym", &at) == 0 && at == 0x500, "plain list");
        EXPECT(msdosdebug_symbols_generation(g_dbg) > 0, "generation");
        has("print _main", "$0710");
        has("labels /nonexistent/zz.map", "Cannot read");
        msdosdebug_clear_symbols(g_dbg);
    }

    /* msdossym_parse_range */
    {
        uint32_t lo, hi;
        msdosdebug_set_label(g_dbg, 0x1000, "tbl");
        EXPECT(msdossym_parse_range(g_dbg, "B800", &lo, &hi) && lo == 0xB800 && hi == 0xB800, "bare hex");
        EXPECT(msdossym_parse_range(g_dbg, "tbl", &lo, &hi) && lo == 0x1000, "a symbol");
        EXPECT(msdossym_parse_range(g_dbg, "tbl+8", &lo, &hi) && lo == 0x1008, "symbol + n");
        EXPECT(msdossym_parse_range(g_dbg, "1000..100F", &lo, &hi) && lo == 0x1000 && hi == 0x100F, "dots");
        EXPECT(msdossym_parse_range(g_dbg, "1000-100F", &lo, &hi) && hi == 0x100F, "dash");
        EXPECT(msdossym_parse_range(g_dbg, "100F-1000", &lo, &hi) && lo == 0x1000 && hi == 0x100F, "reversed");
        EXPECT(msdossym_parse_range(g_dbg, "0060:0100 l 20", &lo, &hi) && lo == 0x700 && hi == 0x71F, "DEBUG length");
        EXPECT(msdossym_parse_range(g_dbg, "B800:0", &lo, &hi) && lo == 0xB8000, "seg:off");
        EXPECT(!msdossym_parse_range(g_dbg, "", &lo, &hi), "empty");
        EXPECT(!msdossym_parse_range(g_dbg, "zork_nothing", &lo, &hi), "nothing");
    }

    /* search parsing */
    {
        uint8_t pat[MSDOSSEARCH_MAX], mask;
        int n, flags;
        EXPECT(msdossearch_parse(MSDOSSEARCH_HEX, "CD 21 $B8 0x34 5", pat, &n, &mask, &flags) && n == 5 && pat[0] == 0xCD && pat[3] == 0x34 && pat[4] == 5, "hex bytes");
        EXPECT(msdossearch_parse(MSDOSSEARCH_TEXT, "Abc", pat, &n, &mask, &flags) && n == 3 && (flags & MSDOS_SEARCH_NOCASE), "text");
        EXPECT(!msdossearch_parse(MSDOSSEARCH_HEX, "zz", pat, &n, &mask, &flags), "bad hex");
        EXPECT(!msdossearch_parse(MSDOSSEARCH_HEX, "", pat, &n, &mask, &flags), "empty");
    }

    msdossession_free(s);
    if (failures) return 1;
    puts("prompt: ok");
    return 0;
}
