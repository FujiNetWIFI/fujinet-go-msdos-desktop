/*
 * expr -- the debugger's expression evaluator: breakpoint conditions,
 * print, set, watches, and every address argument the console takes.
 *
 *   numbers     $B800  0xB800  B800h  49152  %1010
 *   names       whatever the context resolves: x86 registers (al ax eax cs ip
 *               eip esp ...), flags (cf zf ...), symbols, frame, cycles, and
 *               value/addr/old for a memory or port access
 *   memory      [expr] the byte at that linear address, {expr} the 16-bit word,
 *               dword [expr] the 32-bit dword (also byte [..] and word {..});
 *               little-endian, as the CPU sees them now, side-effect free
 *   seg:off     a:b is the linear address of offset b in segment a, so
 *               [ds:si], [es:di+2], {ss:bp+4}, $B800:0. It binds loosest.
 *   operators   C's, with C's precedence: unary - ! ~, * / %, + -, << >>,
 *               < <= > >=, == !=, &, ^, |, &&, ||
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_EXPR_H
#define MSDOS_EXPR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* A name to a value; return 1 if known. */
    int (*name)(void *user, const char *name, int64_t *out);
    /* A byte of memory at a linear address, without side effects. */
    uint8_t (*read_byte)(void *user, uint32_t linear);
    /* seg:off to linear (NULL: seg * 16 + off). */
    uint32_t (*linear)(void *user, uint32_t seg, uint32_t off);
    void *user;
} msdosexpr_ctx;

/* Evaluate; 0 and *out on success, -1 with a message otherwise. With a NULL
 * context, names evaluate to 0 and memory reads to 0 -- a syntax check. */
int msdosexpr_eval(const char *text, const msdosexpr_ctx *ctx, int64_t *out, char *err, int errsz);

#ifdef __cplusplus
}
#endif

#endif /* MSDOS_EXPR_H */
