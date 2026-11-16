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
#include "msdosvid.h"

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
    char condition[128];    /* "" for none: an expression, see msdosdebug_eval */
} msdos_breakpoint;
#define MSDOS_MAX_BREAKPOINTS 128
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

/* Read / write an I/O port as the CPU would (the console's i and o). */
int msdosdebug_io_in(msdosdebug *d, uint16_t port, uint8_t *value);
int msdosdebug_io_out(msdosdebug *d, uint16_t port, uint8_t value);

/* ---- the screen ------------------------------------------------------------------- */
int msdosdebug_video_info(msdosdebug *d, char *dst, int dstsz);

/*
 * A snapshot of the video card for the Video tab (msdosvid.h): registers, palette, VRAM and
 * the beam as the emulation thread stands. It works while the PC runs and while it is
 * stopped, and only copies. Returns 0, or -1 when the PC is off or has no card (s->card is
 * then MSDOS_VID_OFF). s's buffers are reused, so keep one msdos_vid_snap per tab.
 */
int msdosdebug_video_snap(msdosdebug *d, msdos_vid_snap *s);


/* ===========================================================================
 * The newer family debugger: stops in words, conditions on every breakpoint,
 * expressions, bulk registers, memory map, search, symbols, trace and history,
 * the machine. Calls are safe from any thread; inspection is meant for the
 * stopped state and answers with whatever is current while the PC runs.
 * Calls that need no machine (breakpoints, symbols, expressions on constants)
 * work with the PC off.
 * ======================================================================== */

/* ---- more breakpoint calls ---------------------------------------------------------
 * Conditions are expressions (see msdosdebug_eval); inside one, value / addr / old
 * name the access that fired (memory: the data and the linear address; I/O: the data
 * and the port; INT: the number). A memory or I/O breakpoint stops at the instruction
 * AFTER the access; its condition is checked there. */
/* Returns the new id, or -1 with the reason in err (bad condition, too many). */
int  msdosdebug_bp_add_if(msdosdebug *d, int type, uint32_t start, uint32_t end, int ah,
                          const char *condition, char *err, int errsz);
/* Replaces a condition ("" removes it). 1, or 0 with err. */
int  msdosdebug_bp_set_condition(msdosdebug *d, int id, const char *condition, char *err, int errsz);
void msdosdebug_bp_clear_hits(msdosdebug *d);
/* Run for n instructions (the console's t n). */
int  msdosdebug_step_n(msdosdebug *d, int n);
/* Blocks until the PC is really stopped (it is asked to by msdosdebug_stop); 1 if it is. */
int  msdosdebug_wait_stopped(msdosdebug *d, int ms);
/* Blocks until msdosdebug_stop_serial() != before (a new stop), up to ms; 1 on a stop. */
int  msdosdebug_wait_stop(msdosdebug *d, uint64_t before, int ms);

/* ---- the stop, in words ------------------------------------------------------------- */
enum {
    MSDOS_STOP_NONE = 0,
    MSDOS_STOP_PAUSE,
    MSDOS_STOP_STEP,
    MSDOS_STOP_BREAKPOINT,      /* an EXEC entry */
    MSDOS_STOP_WATCH_READ,
    MSDOS_STOP_WATCH_WRITE,
    MSDOS_STOP_WATCH_IN,
    MSDOS_STOP_WATCH_OUT,
    MSDOS_STOP_INT,
    MSDOS_STOP_ONESHOT          /* step over / out / run to */
};
typedef struct {
    int      reason;            /* MSDOS_STOP_* */
    int      bp_id;             /* the entry that fired, 0 if none */
    uint32_t addr;              /* the linear byte or the port touched (watch), else the PC */
    int      width;             /* bytes moved by a watch, else 0 */
    int64_t  value;             /* the data moved, -1 when not a watch */
    int64_t  old_value;         /* what a write replaced, -1 when not known */
    uint32_t pc;                /* linear cs:ip at the stop */
    uint16_t cs;
    uint32_t ip;
    int64_t  from_pc;           /* linear address of the instruction that made the access, -1 */
    uint64_t serial;            /* msdosdebug_stop_serial at the stop */
} msdos_stop_info;
/* "paused at 0060:0003", "step at ...", "breakpoint 2: exec 0060:0003",
 * "wrote 0046C = $41 (was $40) at F000:E0A1", "read ...", "in 03DA = $09 at ...",
 * "out 02F8 = $01 at ...", "INT 21h AH=09h print string at ...". While running it says
 * "running" and *address is -1 (address: the linear PC to show). Returns the length. */
int  msdosdebug_stop_text(msdosdebug *d, char *dst, int dstsz, int64_t *address);
/* 1 while stopped, 0 while running (out then holds the last stop). */
int  msdosdebug_last_stop(msdosdebug *d, msdos_stop_info *out);
/* Bumped by every stop; windows recentre their views when it changes. */
uint64_t msdosdebug_stop_serial(msdosdebug *d);
/* Everything about the current stop read in one critical section, so a poller never
 * sees a stop with the old serial or reason. paused is 1 only once the PC has really
 * stopped. Returns paused. */
typedef struct {
    int      paused;
    uint64_t serial;
    msdos_stop_info stop;
    char     text[200];
    int64_t  address;           /* the linear PC to show, -1 when running */
} msdos_stop_snapshot;
int  msdosdebug_stop_snapshot(msdosdebug *d, msdos_stop_snapshot *out);

/* ---- expressions ----------------------------------------------------------------------
 * One grammar for conditions, print, set, watches and every address the console takes:
 * numbers ($B800 0xB800 B800h 49152 %1010), the registers (al ah ax eax bl .. edi, cs ds
 * es ss fs gs, ip eip, flags eflags, cr0), the flags (cf pf af zf sf tf if df of), pc (the
 * linear cs:ip), symbols, [x] the byte and {x} the word and dword [x] at linear x,
 * seg:off (ds:si, $B800:0 -- the loosest operator), frame (video frames), cycles (the
 * CPU's TSC), and for an access value, addr, old; C's operators. Registers and memory
 * read the PC as it is now; with the PC off they read 0 and memory reads 0xFF.
 * 0 and *out, or -1 and a message. */
int  msdosdebug_eval(msdosdebug *d, const char *text, int64_t *out, char *err, int errsz);
/* Register names the grammar knows, NULL-terminated, for completion. */
extern const char *const msdosdebug_reg_names[];

/* ---- registers, in bulk ----------------------------------------------------------------
 * Writes every register of *regs (general, eip, eflags, segments; cr0, the bases, mode
 * and cpu are read-only). Only while stopped: 1, or 0 with a reason in err (running, a
 * segment change in protected mode, no PC). */
int  msdosdebug_regs_set(msdosdebug *d, const msdos_cpu_regs *regs, char *err, int errsz);
/* One register or flag by the names of the grammar. Same rules. */
int  msdosdebug_register_set(msdosdebug *d, const char *name, int64_t value, char *err, int errsz);

/* ---- memory map and search ------------------------------------------------------------- */
typedef struct {
    uint32_t low, high;     /* inclusive */
    int      readable, writable;
    char     name[64];      /* "Conventional memory (640K)", "Extended memory (A20 on)" */
} msdos_region;
/* Ordered and gap-free from 0 to the top of the address space the CPU can reach. Returns
 * how many (at most max; the PC makes at most 10). */
int  msdosdebug_memory_map(msdosdebug *d, msdos_region *out, int max);
#define MSDOS_SEARCH_NOCASE 1
/* Up to max linear start addresses in from..to (to bounds the START) where
 * (mem & mask) == (pat & mask) for all count bytes; video memory is skipped unless the
 * card is in a text mode. Returns the count, or -1 on an error / no PC. */
int  msdosdebug_search(msdosdebug *d, const uint8_t *pat, int count, uint8_t mask, int flags,
                       uint32_t from, uint32_t to, uint32_t *out, int max);
/* The next match after `from` (before it if backward), wrapping once.
 * 1 and *found, 0 for none, -1 on an error. */
int  msdosdebug_search_next(msdosdebug *d, const uint8_t *pat, int count, uint8_t mask, int flags,
                            uint32_t from, int backward, uint32_t *found);

/* ---- symbols, for a Symbols tab ------------------------------------------------------------ */
int      msdosdebug_symbols_count(msdosdebug *d);
/* Entry index (0 .. count-1) in address order. 1, or 0 past the end. */
int      msdosdebug_symbols_get(msdosdebug *d, int index, uint32_t *linear, char *name, int namesz);
/* Indices of the entries whose name contains text (case-insensitive, empty = all). */
int      msdosdebug_symbols_filter(msdosdebug *d, const char *text, int *out, int max);
/* Bumped when symbols are loaded, labelled or cleared. */
unsigned msdosdebug_symbols_generation(msdosdebug *d);
/* A user label at a linear address (name "" removes the labels there). 0 or -1. */
int      msdosdebug_set_label(msdosdebug *d, uint32_t linear, const char *name);

/* ---- trace and history ------------------------------------------------------------------------
 * One record per executed instruction: the registers are the state BEFORE the instruction
 * at (cs:eip) ran (a stop never records an instruction that did not run). The ring holds
 * 65536, is allocated on the first enable and stores raw registers and 8 code bytes; len
 * is worked out when a record is read. A HALT or REP that is already looping folds into
 * the previous record (repeat counts the extra spins). Trace and History share the ring:
 * it records while either asked for it. Recording needs 86Box's interpreter, so it slows
 * the PC down. */
typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflags;
    uint16_t cs, ds, es, ss, fs, gs;
    uint32_t linear;        /* cs base + eip */
    uint64_t cycles;        /* the CPU's TSC */
    uint32_t repeat;        /* spins folded into this record */
    uint8_t  bytes[8];
    uint8_t  len;           /* 0 in the ring, filled when read */
    uint8_t  is386;
    uint8_t  code32;
} msdos_trace_entry;
#define MSDOS_TRACE_SIZE 65536
void     msdosdebug_trace_enable(msdosdebug *d, int on);
int      msdosdebug_trace_enabled(msdosdebug *d);
int      msdosdebug_trace_count(msdosdebug *d);
/* Records pushed since the last clear: a tab polls this to see new lines. */
uint64_t msdosdebug_trace_total(msdosdebug *d);
/* Up to max of the newest entries, NEWEST first. Returns how many. */
int      msdosdebug_trace_read(msdosdebug *d, msdos_trace_entry *out, int max);
void     msdosdebug_trace_clear(msdosdebug *d);
/* "0060:0003  B8 34 12  mov ax, 0x1234   AX=.... BX=.... CX=.... DX=.... SP=.... ..." */
int      msdosdebug_trace_format(const msdos_trace_entry *rec, char *dst, int dstsz);
enum { MSDOS_TRACE_ALL = 0, MSDOS_TRACE_BRANCHES, MSDOS_TRACE_RANGE };
/* 1 if the entry passes (BRANCHES: call, jump, conditional jump, loop, ret, int, iret;
 * RANGE: linear in low..high). */
int      msdosdebug_trace_match(const msdos_trace_entry *rec, int filter, uint32_t low, uint32_t high);
/* History: what ran before the machine stopped, OLDEST first, from the same ring. */
void     msdosdebug_history_enable(msdosdebug *d, int on);
int      msdosdebug_history_enabled(msdosdebug *d);
void     msdosdebug_history_clear(msdosdebug *d);
uint64_t msdosdebug_history_count(msdosdebug *d);
int      msdosdebug_history_copy(msdosdebug *d, msdos_trace_entry *out, int max);

/* ---- the machine: Machine & I/O ------------------------------------------------------------------
 * CPU model and mode, A20, RAM, the interrupt controller, timer, DMA, serial and parallel
 * ports, drives, the video card, sound cards and the FujiNet UART as text, read-only on
 * the emulation thread. Returns the length written. */
int      msdosdebug_io_text(msdosdebug *d, char *dst, int dstsz);

/* ---- sound -------------------------------------------------------------------------------------------
 * The Sound tab and the console's `sound`: see msdossnd.h. */
int      msdosdebug_sound_text(msdosdebug *d, char *dst, int dstsz);
struct msdos_snd_state;
/* 0 and *out filled, -1 when the PC is off. */
int      msdosdebug_sound_get(msdosdebug *d, struct msdos_snd_state *out);
/* A tab-neutral hook for text the lead adds to the Machine & I/O page. */
void     msdosdebug_set_sound_text(const char *(*fn)(void));

/* ---- the console: completions ------------------------------------------------------------------------- */
/* Completions for the end of a line (commands, registers, symbols), one per line. */
int      msdosdebug_completions(msdosdebug *d, const char *prefix, char *dst, int dstsz);
/* Saves "dis" (the disassembly from CS:IP, 4096 lines), "mem" (the first megabyte),
 * "regs" to a file. Message into msg; 0 or -1. */
int      msdosdebug_save(msdosdebug *d, const char *kind, const char *path, char *msg, int msgsz);

#ifdef __cplusplus
}
#endif

#endif
