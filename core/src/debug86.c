/*
 * The debugger's window onto 86Box: registers, memory, the UART, the IVT.
 * Emulation thread only (debug.cpp calls these from host jobs).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* before 86Box's cpu.h: its cs/ds/es/ss macros would rename these fields */
#include "debug86.h"
#include "msdosdebug.h"

#include <86box/86box.h>
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/mem.h>
#include <86box/serial.h>
#include "cpu.h"
#include "x86.h"
#include "x86_flags.h"

#undef cs
#undef ds
#undef es
#undef ss
#undef gs
#undef cr0

static void
rebuild_flags(void)
{
    if (is286 || is386)
        flags_rebuild();
}

void
d86_regs(msdos_cpu_regs *r)
{
    memset(r, 0, sizeof *r);
    rebuild_flags();
    r->eax = EAX;
    r->ebx = EBX;
    r->ecx = ECX;
    r->edx = EDX;
    r->esi = ESI;
    r->edi = EDI;
    r->ebp = EBP;
    r->esp = ESP;
    r->eip = cpu_state.pc;
    r->eflags = cpu_state.flags | ((uint32_t) cpu_state.eflags << 16);
    r->cs = cpu_state.seg_cs.seg;
    r->ds = cpu_state.seg_ds.seg;
    r->es = cpu_state.seg_es.seg;
    r->ss = cpu_state.seg_ss.seg;
    r->fs = cpu_state.seg_fs.seg;
    r->gs = cpu_state.seg_gs.seg;
    r->cs_base = cpu_state.seg_cs.base;
    r->ds_base = cpu_state.seg_ds.base;
    r->es_base = cpu_state.seg_es.base;
    r->ss_base = cpu_state.seg_ss.base;
    r->fs_base = cpu_state.seg_fs.base;
    r->gs_base = cpu_state.seg_gs.base;
    r->cr0 = cpu_state.CR0.l;
    r->is386 = is386;
    r->mode = !(msw & 1) ? 0 : (cpu_state.eflags & 2) ? 2 : 1;
    r->code32 = d86_code32();
    snprintf(r->cpu, sizeof r->cpu, "%s %s", cpu_f ? cpu_f->manufacturer : "",
             cpu_f ? cpu_f->name : "");
}

int
d86_code32(void)
{
    return (msw & 1) && !(cpu_state.eflags & 2) && (cpu_state.seg_cs.ar_high & 0x40);
}

static int
set32(uint32_t *p, const char *name, const char *want, uint32_t v)
{
    if (strcmp(name, want))
        return 0;
    *p = v;
    return 1;
}

int
d86_set_reg(const char *name, uint32_t v)
{
    static const char *const r16[8] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" };
    static const char *const r32[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    static const char *const flagnames[] = { "cf", "", "pf", "", "af", "", "zf", "sf", "tf",
                                             "if", "df", "of", NULL };
    rebuild_flags();
    for (int i = 0; i < 8; i++) {
        if (!strcmp(name, r16[i])) {
            cpu_state.regs[i].w = (uint16_t) v;
            return 0;
        }
        if (is386 && !strcmp(name, r32[i])) {
            cpu_state.regs[i].l = v;
            return 0;
        }
    }
    if (!strcmp(name, "ip")) {
        cpu_state.pc = v & 0xFFFF;
        return 0;
    }
    if (set32(&cpu_state.pc, name, "eip", v))
        return 0;
    if (!strcmp(name, "flags") || !strcmp(name, "fl")) {
        cpu_state.flags = (uint16_t) v;
        return 0;
    }
    for (int i = 0; flagnames[i]; i++)
        if (*flagnames[i] && !strcmp(name, flagnames[i])) {
            if (v)
                cpu_state.flags |= (uint16_t) (1u << i);
            else
                cpu_state.flags &= (uint16_t) ~(1u << i);
            return 0;
        }
    /* segment registers: real mode only (protected mode wants a descriptor load) */
    if (!(msw & 1) || (cpu_state.eflags & 2)) {
        x86seg *seg = !strcmp(name, "cs") ? &cpu_state.seg_cs
                    : !strcmp(name, "ds") ? &cpu_state.seg_ds
                    : !strcmp(name, "es") ? &cpu_state.seg_es
                    : !strcmp(name, "ss") ? &cpu_state.seg_ss
                    : !strcmp(name, "fs") ? &cpu_state.seg_fs
                    : !strcmp(name, "gs") ? &cpu_state.seg_gs : NULL;
        if (seg) {
            seg->seg = (uint16_t) v;
            seg->base = (uint32_t) (v & 0xFFFF) << 4;
            return 0;
        }
    }
    return -1;
}

/* Linear to physical: paging honoured, a miss is -1. */
static int64_t
phys(uint32_t linear)
{
    if (!(cpu_state.CR0.l & 0x80000000))
        return linear & rammask;
    uint64_t p = mmutranslate_noabrt(linear, 0);
    return (p == 0xFFFFFFFFFFFFFFFFULL) ? -1 : (int64_t) p;
}

/* A0000-BFFFF is video memory behind the card: reading it in a planar mode
 * moves the card's latches, so the debugger shows it only in text modes. */
static int
safe_phys(int64_t p)
{
    if (p < 0xA0000 || p > 0xBFFFF)
        return 1;
    uint8_t mode = mem_readb_phys(0x449);
    return (mode <= 3 || mode == 7) && p >= 0xB0000;
}

int
d86_read(uint32_t linear, uint8_t *buf, int n)
{
    int ok = 0;
    for (int i = 0; i < n; i++) {
        int64_t p = phys(linear + (uint32_t) i);
        if (p < 0 || !safe_phys(p)) {
            buf[i] = 0xFF;
            continue;
        }
        buf[i] = mem_readb_phys((uint32_t) p);
        ok++;
    }
    return ok;
}

int
d86_write(uint32_t linear, const uint8_t *buf, int n)
{
    int ok = 0;
    for (int i = 0; i < n; i++) {
        int64_t p = phys(linear + (uint32_t) i);
        if (p < 0)
            continue;
        mem_writeb_phys((uint32_t) p, buf[i]);
        ok++;
    }
    return ok;
}

uint32_t
d86_seg_base(uint16_t seg)
{
    if (!(msw & 1) || (cpu_state.eflags & 2))
        return (uint32_t) seg << 4;
    if (seg == cpu_state.seg_cs.seg) return cpu_state.seg_cs.base;
    if (seg == cpu_state.seg_ds.seg) return cpu_state.seg_ds.base;
    if (seg == cpu_state.seg_es.seg) return cpu_state.seg_es.base;
    if (seg == cpu_state.seg_ss.seg) return cpu_state.seg_ss.base;
    if (seg == cpu_state.seg_fs.seg) return cpu_state.seg_fs.base;
    if (seg == cpu_state.seg_gs.seg) return cpu_state.seg_gs.base;
    return (uint32_t) seg << 4;
}

uint32_t
d86_cs_ip(uint16_t *seg, uint32_t *off)
{
    if (seg)
        *seg = cpu_state.seg_cs.seg;
    if (off)
        *off = cpu_state.pc;
    return cpu_state.seg_cs.base + cpu_state.pc;
}

uint32_t
d86_stack_pointer(void)
{
    return cpu_state.seg_ss.base + (stack32 ? ESP : SP);
}

void
d86_force_interpreter(int on)
{
    cpu_force_interpreter = on;
}

void
d86_int_regs(uint16_t *ax, uint16_t *bx, uint16_t *cx, uint16_t *dx, uint16_t *ds_, uint16_t *es_)
{
    *ax = AX;
    *bx = BX;
    *cx = CX;
    *dx = DX;
    *ds_ = cpu_state.seg_ds.seg;
    *es_ = cpu_state.seg_es.seg;
}

int
d86_ivt(int vec, uint16_t *seg, uint16_t *off)
{
    if (vec < 0 || vec > 255)
        return -1;
    *off = mem_readw_phys((uint32_t) vec * 4);
    *seg = mem_readw_phys((uint32_t) vec * 4 + 2);
    return 0;
}

int
d86_uart(int com, char *dst, int dstsz)
{
    serial_t *u;
    int n;
    if (com < 1 || com > SERIAL_MAX)
        return 0;
    u = com_ports[com - 1].serial;
    if (!u)
        return snprintf(dst, (size_t) dstsz, "COM%d is not fitted\n", com);
    const unsigned div = u->dlab ? u->dlab : 1;
    n = snprintf(dst, (size_t) dstsz,
                 "COM%d at %03Xh IRQ %d, %s\n"
                 "Speed %u bps (divisor %u), %d data bits, LCR %02X\n"
                 "IER %02X  IIR %02X  FCR %02X  MCR %02X\n"
                 "LSR %02X  MSR %02X  SCR %02X\n",
                 com, u->base_address, u->irq, u->fifo_enabled ? "FIFO on" : "FIFO off",
                 115200u / div, div, 5 + (u->lcr & 3), u->lcr, u->ier, u->iir, u->fcr, u->mctrl,
                 u->lsr, u->msr, u->scratch);
    return n;
}
