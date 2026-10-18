/*
 * msdosdebug -- the debugger over the session's PC: execution control,
 * registers, memory, breakpoints (code, memory, I/O ports, software
 * interrupts), a DOS/BIOS call trace, the FujiNet serial traffic, symbols
 * from linker maps, and a DEBUG.COM-style console.
 *
 * Attaching stops the machine and runs 86Box's interpreter (the recompiler
 * has no per-instruction hook); detaching lets it go and restores the
 * recompiler. Everything here is safe from the UI thread.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOSDEBUG_H
#define MSDOSDEBUG_H

#include <stdint.h>

#include "msdossession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- control ---------------------------------------------------------------- */
int  msdosdebug_attach(msdosdebug *d);          /* stops the machine */
void msdosdebug_detach(msdosdebug *d);          /* lets it go */
int  msdosdebug_attached(msdosdebug *d);
void msdosdebug_stop(msdosdebug *d);
void msdosdebug_resume(msdosdebug *d);
int  msdosdebug_stopped(msdosdebug *d);
int  msdosdebug_step(msdosdebug *d);            /* one instruction */
int  msdosdebug_step_over(msdosdebug *d);       /* over CALL, INT, LOOP, REP */
int  msdosdebug_step_out(msdosdebug *d);        /* until the routine returns */
int  msdosdebug_run_to(msdosdebug *d, uint32_t linear);
/* Moves on every stop, resume and change a window should redraw for. */
unsigned msdosdebug_generation(msdosdebug *d);
/* Why it last stopped: "Breakpoint 2 at 1234:0010", "Step", ... */
int  msdosdebug_stop_reason(msdosdebug *d, char *dst, int dstsz);

/* ---- the CPU ---------------------------------------------------------------- */
typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflags;
    uint16_t cs, ds, es, ss, fs, gs;
    uint32_t cs_base, ds_base, es_base, ss_base, fs_base, gs_base;
    uint32_t cr0;
    int mode;           /* 0 real, 1 protected, 2 virtual 8086 */
    int is386;          /* 32-bit registers to show */
    int code32;         /* the code segment is 32-bit */
    char cpu[48];       /* "Intel 8088" */
} msdos_cpu_regs;
int msdosdebug_regs(msdosdebug *d, msdos_cpu_regs *r);
/* "ax", "eax", "cs", "ip", "flags", "cf" ... Returns 0 if it took it. */
int msdosdebug_set_reg(msdosdebug *d, const char *name, uint32_t value);

/* ---- memory (linear addresses; paging is honoured) ----------------------------- */
int      msdosdebug_read(msdosdebug *d, uint32_t linear, uint8_t *buf, int n);
int      msdosdebug_write(msdosdebug *d, uint32_t linear, const uint8_t *buf, int n);
/* seg:off through the segment's base when seg is one in use, else seg*16. */
uint32_t msdosdebug_linear(msdosdebug *d, uint16_t seg, uint32_t off);
/* "1234:0010", "cs:ip", "ds:si", "f000:fff0", "12345" (linear, hex), a
 * symbol. Returns 0 and fills seg/off/linear when it parsed. */
int      msdosdebug_parse_address(msdosdebug *d, const char *text, uint16_t *seg,
                                  uint32_t *off, uint32_t *linear);

/* ---- disassembly -------------------------------------------------------------- */
typedef struct {
    uint16_t seg;
    uint32_t off;
    uint32_t linear;
    int      len;
    char     bytes[40];     /* "B8 34 12" */
    char     text[96];      /* "mov ax, 0x1234" */
    char     symbol[48];    /* the label at this address, or "" */
} msdos_dis_line;
/* count instructions from seg:off (code32 from the CPU when seg is CS). */
int msdosdebug_disassemble(msdosdebug *d, uint16_t seg, uint32_t off, int count,
                           msdos_dis_line *out);

/* ---- breakpoints -------------------------------------------------------------- */
typedef enum {
    MSDOS_BP_EXEC = 0,      /* start..end linear */
    MSDOS_BP_READ,          /* memory */
    MSDOS_BP_WRITE,
    MSDOS_BP_ACCESS,
    MSDOS_BP_IN,            /* I/O ports start..end */
    MSDOS_BP_OUT,
    MSDOS_BP_IO,
    MSDOS_BP_INT,           /* INT start; ah >= 0 matches AH too */
    MSDOS_BP_TYPES
} msdos_bp_type;
typedef struct {
    int id;
    int type;
    uint32_t start, end;
    int ah;                 /* MSDOS_BP_INT: -1 any */
    int enabled;
    unsigned hits;
    char text[80];          /* "Execute 1234:0010", "OUT 02F8-02FF" */
} msdos_breakpoint;
int  msdosdebug_bp_add(msdosdebug *d, int type, uint32_t start, uint32_t end, int ah);
int  msdosdebug_bp_remove(msdosdebug *d, int id);
int  msdosdebug_bp_enable(msdosdebug *d, int id, int on);
int  msdosdebug_bp_list(msdosdebug *d, msdos_breakpoint *out, int max);
void msdosdebug_bp_clear(msdosdebug *d);
/* The disassembly gutter's: an execute breakpoint at one address. */
int  msdosdebug_bp_toggle(msdosdebug *d, uint32_t linear);
int  msdosdebug_bp_at(msdosdebug *d, uint32_t linear);
const char *msdosdebug_bp_type_name(int type);

/* ---- interrupts ---------------------------------------------------------------- */
typedef struct {
    uint64_t serial;
    uint8_t  num;
    uint16_t ax, bx, cx, dx, ds, es;
    uint16_t cs;
    uint32_t ip;
    char     text[80];      /* "INT 21h AH=09h print string" */
} msdos_int_event;
/* Record every INT the program executes (costs speed while on). */
void msdosdebug_int_trace_enable(msdosdebug *d, int on);
int  msdosdebug_int_trace_enabled(msdosdebug *d);
int  msdosdebug_int_trace(msdosdebug *d, msdos_int_event *out, int max);  /* oldest first */
void msdosdebug_int_trace_clear(msdosdebug *d);
int  msdosdebug_ivt(msdosdebug *d, int vec, uint16_t *seg, uint16_t *off);

/* ---- FujiNet: the bytes on its COM port, as FujiBus packets ---------------------- */
typedef struct {
    uint64_t serial;
    int      to_pc;         /* 1: FujiNet -> PC, 0: PC -> FujiNet */
    int      device, command, length;   /* from the FujiBus header, -1 if short */
    int      nbytes;
    uint8_t  data[64];      /* the decoded packet's first bytes */
    char     text[96];      /* "PC -> 70 F2 READ DEVICE SLOTS (6 bytes)" */
} msdos_fuji_packet;
int  msdosdebug_fujinet_trace(msdosdebug *d, msdos_fuji_packet *out, int max);
void msdosdebug_fujinet_trace_clear(msdosdebug *d);
/* FujiNet's UART: registers, speed, FIFO, as text lines. */
int  msdosdebug_uart(msdosdebug *d, char *dst, int dstsz);

/* ---- symbols (MS LINK and Watcom WLINK .map files) --------------------------------- */
int msdosdebug_load_symbols(msdosdebug *d, const char *path, uint16_t load_seg,
                            char *msg, int msgsz);
void msdosdebug_clear_symbols(msdosdebug *d);
int msdosdebug_symbol_at(msdosdebug *d, uint32_t linear, char *dst, int dstsz);
int msdosdebug_symbol_find(msdosdebug *d, const char *name, uint32_t *linear);

/* ---- the console: DEBUG.COM's commands (r d e u g t p) plus bp/bc/bl, i/o --------- */
int msdosdebug_command(msdosdebug *d, const char *line, char *out, int outsz);

/* ---- the screen ------------------------------------------------------------------- */
int msdosdebug_video_info(msdosdebug *d, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif
