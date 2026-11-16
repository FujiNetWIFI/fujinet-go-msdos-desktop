/*
 * iostate.c: the Machine & I/O page as text. What 86Box exposes cheaply and
 * read-only: the CPU and its mode, A20, RAM, the interrupt controllers, the
 * timer, DMA, serial and parallel ports, the video card and the disks.
 * Emulation thread only (debug.cpp calls it from a host job).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
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
#include <86box/pic.h>
#include <86box/pit.h>
#include <86box/dma.h>
#include <86box/serial.h>
#include <86box/lpt.h>
#include <86box/hdd.h>
#include <86box/machine.h>
#include <86box/video.h>
#include "cpu.h"

#undef cs
#undef ds
#undef es
#undef ss
#undef gs
#undef cr0
#undef cycles

typedef struct {
    char *dst;
    int   cap, len;
} sink;

static void
put(sink *k, const char *fmt, ...)
{
    va_list ap;
    int     n;
    if (k->len >= k->cap - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(k->dst + k->len, (size_t) (k->cap - k->len), fmt, ap);
    va_end(ap);
    if (n > 0)
        k->len += n < k->cap - k->len ? n : k->cap - k->len - 1;
}

static const char *
bits8(uint8_t v)
{
    static char b[4][9];
    static int  n;
    char       *p = b[n++ & 3];
    for (int i = 0; i < 8; i++)
        p[i] = (char) ('0' + ((v >> (7 - i)) & 1));
    p[8] = 0;
    return p;
}

static void
pic_text(sink *k, const char *name, const pic_t *p, int base)
{
    put(k, "%s: IRR %02X  ISR %02X  IMR %02X  (%s)\n", name, p->irr, p->isr, p->imr, bits8(p->imr));
    put(k, "     masked:");
    for (int i = 0; i < 8; i++)
        if (p->imr & (1 << i))
            put(k, " IRQ%d", base + i);
    if (!p->imr)
        put(k, " none");
    put(k, "\n");
}

static void
pit_text(sink *k)
{
    static const char *const use[3] = { "system timer, IRQ 0", "DRAM refresh", "PC speaker" };
    if (pit_devs[0].data && pit_devs[0].get_count == pit_classic_intf.get_count) {
        const pit_t *pit = (const pit_t *) pit_devs[0].data;
        for (int i = 0; i < 3; i++) {
            const ctr_t *c = &pit->counters[i];
            const uint32_t reload = c->l ? c->l : 65536;
            put(k, "PIT %d: mode %d  reload %5u (%.2f Hz)  count %5u  gate %d  out %d  %s\n", i,
                c->m >= 6 ? c->m - 4 : c->m, (unsigned) reload, 1193182.0 / (double) reload,
                (unsigned) (c->count < 0 ? 0 : c->count), c->gate, c->out, use[i]);
        }
    } else {
        put(k, "PIT: not the classic 8253/8254\n");
    }
}

static void
dma_text(sink *k)
{
    for (int i = 0; i < 4; i++) {
        const dma_t *c = &dma[i];
        put(k, "DMA %d: addr %05X  count %04X  page %02X  mode %02X%s\n", i, (unsigned) c->ab,
            (unsigned) (c->cb & 0xFFFF), c->page, c->mode, (dma_m & (1 << i)) ? "  masked" : "");
    }
}

static void
serial_text(sink *k)
{
    for (int i = 0; i < SERIAL_MAX; i++) {
        const serial_t *u = com_ports[i].serial;
        if (!u)
            continue;
        put(k, "COM%d: %03Xh IRQ %d  %u bps  LCR %02X  IER %02X  MCR %02X  LSR %02X  MSR %02X%s\n",
            i + 1, u->base_address, u->irq, 115200u / (u->dlab ? u->dlab : 1), u->lcr, u->ier,
            u->mctrl, u->lsr, u->msr, u->fifo_enabled ? "  FIFO" : "");
    }
}

static void
disks_text(sink *k)
{
    for (int i = 0; i < HDD_NUM; i++)
        if (hdd[i].bus_type && hdd[i].fn[0])
            put(k, "Hard disk %d: %s  (%u cylinders, %u heads, %u sectors)\n", i, hdd[i].fn,
                (unsigned) hdd[i].tracks, (unsigned) hdd[i].hpc, (unsigned) hdd[i].spt);
}

int
d86_machine_info(char *dst, int dstsz)
{
    sink              k = { dst, dstsz, 0 };
    msdos_d86_meminfo m;
    msdos_cpu_regs    r;
    const char       *vid = video_get_internal_name(gfxcard[0]);
    static const char *const modes[3] = { "real mode", "protected mode", "virtual 8086 mode" };

    if (!dst || dstsz <= 0)
        return 0;
    dst[0] = 0;
    d86_mem_info(&m);
    d86_regs(&r);
    put(&k, "Machine: %s\n", machine_get_internal_name());
    put(&k, "CPU: %s  %s  %s%s\n", r.cpu, modes[m.mode],
        r.is386 ? "32-bit registers" : "16-bit registers", r.code32 ? ", 32-bit code" : "");
    if (cpu_s)
        put(&k, "Clock: %.2f MHz  address lines: %d  CR0 %08X\n", cpu_s->rspeed / 1000000.0, m.bits,
            r.cr0);
    put(&k, "RAM: %u KB (%u KB conventional + %u KB extended)\n", (unsigned) m.ram_kb,
        m.ram_kb > 640 ? 640u : (unsigned) m.ram_kb, m.ram_kb > 1024 ? (unsigned) m.ram_kb - 1024 : 0u);
    put(&k, "A20 gate: %s%s\n", m.bits <= 20 ? "not present (8086 wraps at 1 MB)" : m.a20 ? "on" : "off",
        m.bits > 20 && !m.a20 ? " (address bit 20 forced to 0)" : "");
    put(&k, "Video: %s\n\n", vid ? vid : "none");
    pic_text(&k, "PIC 1", &pic, 0);
    if (pic2.imr || pic2.irr || pic2.isr)
        pic_text(&k, "PIC 2", &pic2, 8);
    put(&k, "\n");
    pit_text(&k);
    put(&k, "\n");
    dma_text(&k);
    put(&k, "\n");
    serial_text(&k);
    for (int i = 0; i < PARALLEL_MAX; i++)
        if (lpt_ports[i].enabled)
            put(&k, "LPT%d: enabled\n", i + 1);
    put(&k, "\n");
    disks_text(&k);
    return k.len;
}
