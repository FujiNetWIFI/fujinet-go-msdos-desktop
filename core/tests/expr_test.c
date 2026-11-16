/*
 * expr_test -- the debugger's expression evaluator on a fake machine: numbers
 * in every notation, registers and names, memory, seg:off, operators and their
 * precedence, errors. No machine behind it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "expr.h"

static int failures;

static uint8_t mem[0x100000];

static int names(void *user, const char *name, int64_t *out)
{
    (void)user;
    if (!strcasecmp(name, "ax")) { *out = 0x1234; return 1; }
    if (!strcasecmp(name, "ds")) { *out = 0x0100; return 1; }
    if (!strcasecmp(name, "si")) { *out = 0x20; return 1; }
    if (!strcasecmp(name, "eax")) { *out = 0xDEADBEEFLL; return 1; }
    if (!strcasecmp(name, "start")) { *out = 0x600; return 1; }
    if (!strcasecmp(name, "Mixed_Case.1")) { *out = 7; return 1; }
    return 0;
}

static uint8_t rd(void *user, uint32_t linear)
{
    (void)user;
    return mem[linear & 0xFFFFF];
}

static uint32_t lin(void *user, uint32_t seg, uint32_t off)
{
    (void)user;
    return (seg << 4) + off;
}

static const msdosexpr_ctx ctx = { names, rd, lin, NULL };

static void expect(const char *text, int64_t want)
{
    int64_t got = 0;
    char err[96];
    if (msdosexpr_eval(text, &ctx, &got, err, sizeof err) != 0) {
        fprintf(stderr, "expr_test: \"%s\" failed: %s\n", text, err);
        failures = 1;
    } else if (got != want) {
        fprintf(stderr, "expr_test: \"%s\" = %lld, want %lld\n", text, (long long)got, (long long)want);
        failures = 1;
    }
}

static void expect_error(const char *text, const char *needle)
{
    int64_t got;
    char err[96];
    if (msdosexpr_eval(text, &ctx, &got, err, sizeof err) == 0) {
        fprintf(stderr, "expr_test: \"%s\" should fail\n", text);
        failures = 1;
    } else if (needle && !strstr(err, needle)) {
        fprintf(stderr, "expr_test: \"%s\" error \"%s\", want \"%s\"\n", text, err, needle);
        failures = 1;
    }
}

int main(void)
{
    char big[5000];
    int64_t v;

    mem[0x1020] = 0x41;
    mem[0x1021] = 0x1E;
    mem[0x1022] = 0x55;
    mem[0x1023] = 0x66;
    mem[0xB8000] = 0x42;

    /* numbers */
    expect("49152", 49152);
    expect("$B800", 0xB800);
    expect("0xB800", 0xB800);
    expect("0B800h", 0xB800);
    expect("1234h", 0x1234);
    expect("%1010", 10);
    expect("$FFFFFFFF", 0xFFFFFFFFLL);
    expect("0x100000", 0x100000);

    /* names, any case */
    expect("ax", 0x1234);
    expect("AX", 0x1234);
    expect("eax", 0xDEADBEEFLL);
    expect("start + 3", 0x603);
    expect("Mixed_Case.1", 7);
    expect("ax & $FF", 0x34);

    /* memory: byte, word, dword, seg:off */
    expect("[$1020]", 0x41);
    expect("{$1020}", 0x1E41);
    expect("dword [$1020]", 0x66551E41);
    expect("word {$1020}", 0x1E41);
    expect("byte [$1021]", 0x1E);
    expect("[ds:si]", 0x41);
    expect("{ds:si}", 0x1E41);
    expect("[ds:si+1]", 0x1E);
    expect("$100:$20", 0x1020);
    expect("$B800:0", 0xB8000);
    expect("[$B800:0]", 0x42);
    expect("[[ds:si]]", mem[0x41]);

    /* operators and precedence */
    expect("1 + 2 * 3", 7);
    expect("(1 + 2) * 3", 9);
    expect("10 - 3 - 2", 5);
    expect("7 / 2", 3);
    expect("7 % 4", 3);
    expect("1 << 4", 16);
    expect("256 >> 4", 16);
    expect("5 > 3", 1);
    expect("5 <= 3", 0);
    expect("3 == 3 && 4 != 5", 1);
    expect("0 || 0", 0);
    expect("$F0 | $0F", 0xFF);
    expect("$F0 & $3C", 0x30);
    expect("$F0 ^ $FF", 0x0F);
    expect("~0 & $FF", 0xFF);
    expect("!5", 0);
    expect("-3 + 5", 2);
    expect("1 + 2 << 1", 6);
    expect("1 | 2 == 2", 1);
    expect("  7  ", 7);

    /* the 64-bit path: a Windows long is 32 bits */
    expect("$100000000 + 1", 0x100000001LL);

    /* errors */
    expect_error("", "expression expected");
    expect_error("1 +", "expression expected");
    expect_error("nosuchname", "unknown name 'nosuchname'");
    expect_error("(1 + 2", "missing )");
    expect_error("[1", "missing ]");
    expect_error("{1", "missing }");
    expect_error("1 2", "unexpected text");
    expect_error("1 / 0", "division by zero");
    expect_error("5 % 0", "division by zero");
    expect_error("$", "bad hex");
    expect_error("%", "bad binary");
    expect_error("1 # 2", "unexpected text");

    /* a deep expression does not overflow the stack */
    memset(big, '(', 2000);
    big[2000] = '1';
    memset(big + 2001, ')', 2000);
    big[4001] = 0;
    expect_error(big, "too deep");
    memset(big, '-', 4000);
    big[4000] = '1';
    big[4001] = 0;
    expect_error(big, "too deep");

    /* no context: names are 0, memory is 0, a syntax check */
    if (msdosexpr_eval("ax + [5] + 1", NULL, &v, NULL, 0) != 0 || v != 1) {
        fprintf(stderr, "expr_test: no-context check\n");
        failures = 1;
    }
    if (msdosexpr_eval("1 +", NULL, &v, NULL, 0) == 0) {
        fprintf(stderr, "expr_test: no-context syntax error not caught\n");
        failures = 1;
    }

    if (failures) return 1;
    puts("expr: ok");
    return 0;
}
