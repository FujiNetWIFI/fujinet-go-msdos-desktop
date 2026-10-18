/*
 * 86Box's gdbstub hooks, implemented for the app's own debugger instead of
 * GDB's remote protocol (86Box's src/gdbstub.c is not built).
 *
 * The CPU cores call gdbstub_instruction() after every interpreted
 * instruction, so cs:ip is the next one to run; a nonzero return ends the
 * cpu_exec() slice before it. The first instruction of a slice is checked by
 * the cpu_exec wrapper. While stopped, gdbstub_step is not GDBSTUB_EXEC
 * (pc_run then skips the mouse) and nothing executes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/mem.h>
#include <86box/gdbstub.h>
#include "cpu.h"

#include "msdos_host.h"

int      gdbstub_step             = GDBSTUB_EXEC;
int      gdbstub_next_asap        = 0;
int      gdbstub_interpreter_only = 0;
uint64_t gdbstub_watch_pages[(((uint32_t) -1) >> (MEM_GRANULARITY_BITS + 6)) + 1];
uint64_t gdbstub_watch_io[65536 / 64];

static msdos_host_debug_hooks hooks;
static volatile int           installed;
static int                    in_hook;
static void                 (*cpu_exec_shadow)(int32_t cycs);

static void
host_cpu_exec(int32_t cycs)
{
    if (installed && gdbstub_step == GDBSTUB_EXEC && hooks.instruction && hooks.instruction(hooks.user, 1))
        gdbstub_step = GDBSTUB_BREAK;
    if (gdbstub_step != GDBSTUB_EXEC)
        return;
    cpu_exec_shadow(cycs);
}

void
gdbstub_cpu_init(void)
{
    if (cpu_exec != host_cpu_exec) {
        cpu_exec_shadow = cpu_exec;
        cpu_exec        = host_cpu_exec;
    }
}

int
gdbstub_instruction(void)
{
    if (gdbstub_step != GDBSTUB_EXEC)
        return 1;
    if (!installed || !hooks.instruction)
        return 0;
    if (hooks.instruction(hooks.user, 0)) {
        gdbstub_step = GDBSTUB_BREAK;
        return 1;
    }
    return 0;
}

/* INT 3 runs as on the real machine: DOS debuggers own it. */
int
gdbstub_int3(void)
{
    return 0;
}

void
gdbstub_mem_access(uint32_t *addrs, int access)
{
    if (!installed || in_hook || !hooks.mem_access)
        return;
    in_hook = 1;
    hooks.mem_access(hooks.user, addrs, access);
    in_hook = 0;
}

void
gdbstub_io_access(uint16_t port, int access, int width, uint32_t val)
{
    if (!installed || in_hook || !hooks.io_access)
        return;
    in_hook = 1;
    hooks.io_access(hooks.user, port, access == GDBSTUB_IO_WRITE, width, val);
    in_hook = 0;
}

void gdbstub_poll(void) { }
void gdbstub_init(void) { }
void gdbstub_close(void) { }

void
msdos_host_debug_install(const msdos_host_debug_hooks *h)
{
    installed = 0;
    if (h) {
        hooks     = *h;
        installed = 1;
    } else {
        memset(&hooks, 0, sizeof(hooks));
        msdos_host_debug_watch_clear();
        gdbstub_step = GDBSTUB_EXEC;
    }
}

void
msdos_host_debug_set_stopped(int stopped)
{
    gdbstub_step = stopped ? GDBSTUB_BREAK : GDBSTUB_EXEC;
}

int
msdos_host_debug_stopped(void)
{
    return gdbstub_step != GDBSTUB_EXEC;
}

void
msdos_host_debug_watch_page(uint32_t addr, int on)
{
    uint32_t page = addr >> MEM_GRANULARITY_BITS;
    if (on)
        gdbstub_watch_pages[page >> 6] |= 1ULL << (page & 63);
    else
        gdbstub_watch_pages[page >> 6] &= ~(1ULL << (page & 63));
}

void
msdos_host_debug_watch_io(uint16_t port, int on)
{
    if (on)
        gdbstub_watch_io[port >> 6] |= 1ULL << (port & 63);
    else
        gdbstub_watch_io[port >> 6] &= ~(1ULL << (port & 63));
}

void
msdos_host_debug_watch_clear(void)
{
    memset(gdbstub_watch_pages, 0, sizeof(gdbstub_watch_pages));
    memset(gdbstub_watch_io, 0, sizeof(gdbstub_watch_io));
}
