/* The debugger on GLaBIOS booting the FujiNet floppy with FujiNet off:
   attach stops it, a step moves, an INT 13h breakpoint catches the boot
   loader, an OUT breakpoint catches FUJINET.SYS probing COM2's UART, a
   write watchpoint catches the timer tick, Zydis disassembles known bytes,
   a .map file names an address, and the INT trace and console work. */
#include "test_util.h"
#include "msdosdebug.h"

static int wait_stopped(msdosdebug *d, int ms)
{
    for (int t = 0; t < ms; t += 20) {
        if (msdosdebug_stopped(d)) return 1;
        test_sleep_ms(20);
    }
    return 0;
}

static void expect_stop(msdosdebug *d, const char *what, int ms)
{
    char reason[200] = "";
    CHECK(wait_stopped(d, ms), "no stop for %s", what);
    msdosdebug_stop_reason(d, reason, sizeof reason);
    CHECK(strstr(reason, what), "stopped for \"%s\", not %s", reason, what);
}

int main(void)
{
    msdossession *s = test_session_ex("debug", 0, 1);
    msdosdebug *d;
    msdos_cpu_regs r, r2;
    msdos_dis_line l;
    msdos_int_event ev[64];
    char text[4096], dir[600], path[700];
    uint32_t lin;
    uint16_t seg;
    uint32_t off;
    static const uint8_t code[] = { 0xB8, 0x34, 0x12, 0xCD, 0x21 };
    FILE *f;
    CHECK(s, "new");
    test_start(s, 0);
    d = msdossession_debugger(s);
    CHECK(d, "debugger");

    CHECK(msdosdebug_attach(d) == 0, "attach");
    CHECK(wait_stopped(d, 5000), "attach stops it");
    CHECK(msdosdebug_regs(d, &r) == 0 && !r.is386 && strstr(r.cpu, "8088"), "regs: %s", r.cpu);
    CHECK(msdosdebug_step(d) == 0 && wait_stopped(d, 5000), "step");
    msdosdebug_regs(d, &r2);
    CHECK(r2.cs != r.cs || r2.eip != r.eip, "step did not move from %04X:%04X", r.cs, r.eip);

    /* the boot loader reads the floppy through INT 13h */
    CHECK(msdosdebug_bp_add(d, MSDOS_BP_INT, 0x13, 0x13, -1) > 0, "bp INT 13");
    msdosdebug_resume(d);
    expect_stop(d, "INT 13", 30000);
    msdosdebug_bp_clear(d);

    /* FUJINET.SYS probes COM2's UART */
    CHECK(msdosdebug_bp_add(d, MSDOS_BP_OUT, 0x2F8, 0x2FF, -1) > 0, "bp OUT");
    msdossession_set_warp(s, 1);
    msdosdebug_resume(d);
    expect_stop(d, "OUT 02F", 120000);
    msdosdebug_bp_clear(d);

    /* the timer interrupt writes the BIOS tick count, 0040:006C */
    CHECK(msdosdebug_bp_add(d, MSDOS_BP_WRITE, 0x46C, 0x46F, -1) > 0, "bp write");
    msdosdebug_resume(d);
    expect_stop(d, "write to 0046", 5000);
    msdosdebug_bp_clear(d);

    /* memory, Zydis, symbols */
    CHECK(msdosdebug_write(d, 0x600, code, sizeof code) == (int)sizeof code, "write");
    CHECK(msdosdebug_disassemble(d, 0x0060, 0, 1, &l) == 1, "disassemble");
    CHECK(strstr(l.text, "mov ax, 0x1234") && !strcmp(l.bytes, "B8 34 12") && l.linear == 0x600,
          "got %s / %s", l.bytes, l.text);
    test_tmpdir(dir, sizeof dir, "debug-map");
    snprintf(path, sizeof path, "%s/prog.map", dir);
    f = fopen(path, "w");
    CHECK(f, "map");
    fputs(" Start  Stop   Length Name               Class\n"
          " 00000H 0000FH 00010H _TEXT              CODE\n\n"
          "  Address         Publics by Value\n\n"
          " 0000:0000       _start\n"
          " 0000:0003       _dosint\n", f);
    fclose(f);
    CHECK(msdosdebug_load_symbols(d, path, 0x0060, text, sizeof text) == 0, "map: %s", text);
    CHECK(msdosdebug_symbol_at(d, 0x603, text, sizeof text) > 0 && !strcmp(text, "_dosint"),
          "symbol at 0x603: %s", text);
    CHECK(msdosdebug_parse_address(d, "_dosint", &seg, &off, &lin) == 0 && lin == 0x603,
          "parse _dosint");
    CHECK(msdosdebug_parse_address(d, "0060:0003", &seg, &off, &lin) == 0 && lin == 0x603,
          "parse seg:off");

    /* the console */
    CHECK(msdosdebug_command(d, "r", text, sizeof text) == 0 && strstr(text, "CS="), "r: %s", text);
    CHECK(msdosdebug_command(d, "u 0060:0000 l 2", text, sizeof text) == 0 &&
          strstr(text, "int 0x21"), "u: %s", text);

    /* the INT trace sees the timer */
    msdosdebug_int_trace_enable(d, 1);
    msdosdebug_resume(d);
    test_sleep_ms(1500);
    msdosdebug_stop(d);
    CHECK(wait_stopped(d, 5000), "stop");
    CHECK(msdosdebug_int_trace(d, ev, 64) > 0, "INT trace empty");
    msdosdebug_int_trace_enable(d, 0);
    CHECK(msdosdebug_uart(d, text, sizeof text) > 0, "uart");

    msdosdebug_detach(d);
    CHECK(!msdosdebug_stopped(d), "detach lets it run");
    msdossession_stop(s);
    msdossession_free(s);
    puts("debug: ok");
    return 0;
}
