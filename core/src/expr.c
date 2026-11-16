/*
 * expr.c -- a recursive-descent evaluator for the grammar in expr.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "expr.h"

typedef struct {
    const char *cur;
    const msdosexpr_ctx *ctx;
    char *err;
    int errsz;
    int failed;
    int depth;
} parser;

#define MAX_DEPTH 64

static void fail(parser *prs, const char *msg)
{
    if (!prs->failed && prs->err && prs->errsz > 0)
        snprintf(prs->err, (size_t)prs->errsz, "%s", msg);
    prs->failed = 1;
}

static void skip(parser *prs)
{
    while (*prs->cur && isspace((unsigned char)*prs->cur))
        prs->cur++;
}

static int accept(parser *prs, const char *tok)
{
    size_t len = strlen(tok);
    skip(prs);
    if (strncmp(prs->cur, tok, len) == 0) {
        prs->cur += len;
        return 1;
    }
    return 0;
}

static int64_t expr_colon(parser *prs);

static int is_name_start(char chr)
{
    return isalpha((unsigned char)chr) || chr == '_' || chr == '.' || chr == '@' || chr == '?';
}

static int is_name_char(char chr)
{
    return isalnum((unsigned char)chr) || chr == '_' || chr == '.' || chr == '@' || chr == '\'' ||
           chr == '?' || chr == '$';
}

static int64_t number(parser *prs)
{
    const char *str = prs->cur;
    char *end;
    int64_t val;

    if (*str == '$') {
        val = (int64_t)strtoull(str + 1, &end, 16);
        if (end == str + 1)
            fail(prs, "bad hex number");
        prs->cur = end;
        return val;
    }
    if (*str == '%') {
        val = (int64_t)strtoull(str + 1, &end, 2);
        if (end == str + 1)
            fail(prs, "bad binary number");
        prs->cur = end;
        return val;
    }
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        val = (int64_t)strtoull(str + 2, &end, 16);
        if (end == str + 2)
            fail(prs, "bad hex number");
        prs->cur = end;
        return val;
    }
    /* digits: hex with an h suffix (B800h must start with a digit, 0B800h),
     * else decimal */
    {
        const char *look = str;
        while (isxdigit((unsigned char)*look))
            look++;
        if ((*look == 'h' || *look == 'H') && !is_name_char(look[1])) {
            val = (int64_t)strtoull(str, NULL, 16);
            prs->cur = look + 1;
            return val;
        }
    }
    val = (int64_t)strtoull(str, &end, 10);
    prs->cur = end;
    return val;
}

static int64_t read_mem(parser *prs, int64_t addr, int count)
{
    int64_t val = 0;
    int idx;
    if (!prs->ctx || !prs->ctx->read_byte)
        return 0;
    for (idx = count - 1; idx >= 0; idx--)
        val = (val << 8) | prs->ctx->read_byte(prs->ctx->user, (uint32_t)(addr + idx));
    return val;
}

/* [..] or {..} after a size word or alone: the memory at the address inside */
static int64_t memory(parser *prs, char open, int count)
{
    int64_t addr;
    const char close = open == '[' ? ']' : '}';
    prs->cur++;
    addr = expr_colon(prs);
    skip(prs);
    if (*prs->cur != close) {
        fail(prs, open == '[' ? "missing ]" : "missing }");
        return 0;
    }
    prs->cur++;
    return read_mem(prs, addr, count);
}

static int64_t primary(parser *prs)
{
    int64_t val;

    skip(prs);
    if (++prs->depth > MAX_DEPTH) {
        fail(prs, "expression too deep");
        prs->depth--;
        return 0;
    }
    if (*prs->cur == '(') {
        prs->cur++;
        val = expr_colon(prs);
        if (!accept(prs, ")"))
            fail(prs, "missing )");
        prs->depth--;
        return val;
    }
    if (*prs->cur == '[') {
        val = memory(prs, '[', 1);
        prs->depth--;
        return val;
    }
    if (*prs->cur == '{') {
        val = memory(prs, '{', 2);
        prs->depth--;
        return val;
    }
    if (isdigit((unsigned char)*prs->cur) || *prs->cur == '$' || *prs->cur == '%') {
        val = number(prs);
        prs->depth--;
        return val;
    }
    if (is_name_start(*prs->cur)) {
        char name[64];
        int len = 0;
        const char *after;
        while (is_name_char(*prs->cur) && len + 1 < (int)sizeof name)
            name[len++] = *prs->cur++;
        name[len] = '\0';
        after = prs->cur;
        while (*after && isspace((unsigned char)*after))
            after++;
        if ((*after == '[' || *after == '{') &&
            (!strcasecmp(name, "byte") || !strcasecmp(name, "word") || !strcasecmp(name, "dword"))) {
            prs->cur = after;
            val = memory(prs, *after, !strcasecmp(name, "byte") ? 1 : !strcasecmp(name, "word") ? 2 : 4);
            prs->depth--;
            return val;
        }
        prs->depth--;
        if (!prs->ctx)
            return 0;
        if (prs->ctx->name && prs->ctx->name(prs->ctx->user, name, &val))
            return val;
        {
            char msg[96];
            snprintf(msg, sizeof msg, "unknown name '%s'", name);
            fail(prs, msg);
        }
        return 0;
    }
    if (*prs->cur)
        fail(prs, "unexpected character");
    else
        fail(prs, "expression expected");
    prs->depth--;
    return 0;
}

static int64_t unary(parser *prs)
{
    int64_t val;
    int op = 0;
    skip(prs);
    if (*prs->cur == '-' || *prs->cur == '!' || *prs->cur == '~' || *prs->cur == '+') {
        op = *prs->cur++;
        if (++prs->depth > MAX_DEPTH) {
            fail(prs, "expression too deep");
            prs->depth--;
            return 0;
        }
        val = unary(prs);
        prs->depth--;
        return op == '-' ? -val : op == '!' ? !val : op == '~' ? ~val : val;
    }
    return primary(prs);
}

static int64_t mul(parser *prs)
{
    int64_t val = unary(prs);
    for (;;) {
        if (accept(prs, "*"))
            val *= unary(prs);
        else if (accept(prs, "/")) {
            int64_t den = unary(prs);
            if (den == 0) {
                fail(prs, "division by zero");
                return 0;
            }
            val = den == -1 ? -val : val / den;
        } else if (accept(prs, "%")) {
            int64_t den = unary(prs);
            if (den == 0) {
                fail(prs, "division by zero");
                return 0;
            }
            val = den == -1 ? 0 : val % den;
        } else
            return val;
    }
}

static int64_t add(parser *prs)
{
    int64_t val = mul(prs);
    for (;;) {
        if (accept(prs, "+"))
            val += mul(prs);
        else if (accept(prs, "-"))
            val -= mul(prs);
        else
            return val;
    }
}

static int64_t shift(parser *prs)
{
    int64_t val = add(prs);
    for (;;) {
        if (accept(prs, "<<")) {
            int64_t count = add(prs);
            val = count < 0 || count > 62 ? 0 : (int64_t)((uint64_t)val << count);
        } else if (accept(prs, ">>")) {
            int64_t count = add(prs);
            val = count < 0 || count > 62 ? 0 : val >> count;
        } else
            return val;
    }
}

static int64_t rel(parser *prs)
{
    int64_t val = shift(prs);
    for (;;) {
        if (accept(prs, "<="))
            val = val <= shift(prs);
        else if (accept(prs, ">="))
            val = val >= shift(prs);
        else if (strncmp((skip(prs), prs->cur), "<<", 2) != 0 && accept(prs, "<"))
            val = val < shift(prs);
        else if (strncmp((skip(prs), prs->cur), ">>", 2) != 0 && accept(prs, ">"))
            val = val > shift(prs);
        else
            return val;
    }
}

static int64_t equality(parser *prs)
{
    int64_t val = rel(prs);
    for (;;) {
        if (accept(prs, "=="))
            val = val == rel(prs);
        else if (accept(prs, "!="))
            val = val != rel(prs);
        else
            return val;
    }
}

static int64_t band(parser *prs)
{
    int64_t val = equality(prs);
    for (;;) {
        skip(prs);
        if (prs->cur[0] == '&' && prs->cur[1] != '&') {
            prs->cur++;
            val &= equality(prs);
        } else
            return val;
    }
}

static int64_t bxor(parser *prs)
{
    int64_t val = band(prs);
    while (accept(prs, "^"))
        val ^= band(prs);
    return val;
}

static int64_t bor(parser *prs)
{
    int64_t val = bxor(prs);
    for (;;) {
        skip(prs);
        if (prs->cur[0] == '|' && prs->cur[1] != '|') {
            prs->cur++;
            val |= bxor(prs);
        } else
            return val;
    }
}

static int64_t land(parser *prs)
{
    int64_t val = bor(prs);
    while (accept(prs, "&&")) {
        int64_t rhs = bor(prs);
        val = val && rhs;
    }
    return val;
}

static int64_t expr_or(parser *prs)
{
    int64_t val = land(prs);
    while (accept(prs, "||")) {
        int64_t rhs = land(prs);
        val = val || rhs;
    }
    return val;
}

/* seg:off, the loosest operator */
static int64_t expr_colon(parser *prs)
{
    int64_t seg = expr_or(prs);
    skip(prs);
    if (*prs->cur == ':') {
        int64_t off;
        prs->cur++;
        off = expr_or(prs);
        if (prs->ctx && prs->ctx->linear)
            return (int64_t)prs->ctx->linear(prs->ctx->user, (uint32_t)seg, (uint32_t)off);
        return (int64_t)(((uint64_t)seg << 4) + (uint64_t)off);
    }
    return seg;
}

int msdosexpr_eval(const char *text, const msdosexpr_ctx *ctx, int64_t *out, char *err, int errsz)
{
    parser prs;
    int64_t val;

    memset(&prs, 0, sizeof prs);
    prs.cur = text ? text : "";
    prs.ctx = ctx;
    prs.err = err;
    prs.errsz = errsz;
    if (err && errsz > 0)
        err[0] = '\0';

    val = expr_colon(&prs);
    skip(&prs);
    if (!prs.failed && *prs.cur)
        fail(&prs, "unexpected text after the expression");
    if (prs.failed)
        return -1;
    if (out)
        *out = val;
    return 0;
}
