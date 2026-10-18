/*
 * debug86.c: the debugger's access to 86Box. Emulation thread only.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_DEBUG86_H
#define MSDOS_DEBUG86_H

#include <stdint.h>

#include "msdosdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

void     d86_regs(msdos_cpu_regs *r);
int      d86_set_reg(const char *name, uint32_t v);
int      d86_code32(void);
int      d86_read(uint32_t linear, uint8_t *buf, int n);
int      d86_write(uint32_t linear, const uint8_t *buf, int n);
uint32_t d86_seg_base(uint16_t seg);
uint32_t d86_cs_ip(uint16_t *seg, uint32_t *off);
uint32_t d86_stack_pointer(void);
void     d86_force_interpreter(int on);
void     d86_int_regs(uint16_t *ax, uint16_t *bx, uint16_t *cx, uint16_t *dx, uint16_t *ds,
                      uint16_t *es);
int      d86_ivt(int vec, uint16_t *seg, uint16_t *off);
int      d86_uart(int com, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif
