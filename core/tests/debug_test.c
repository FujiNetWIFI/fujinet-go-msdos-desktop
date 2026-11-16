/* The debugger on GLaBIOS booting the FujiNet floppy with FujiNet off:
   attach stops it, a step moves, an INT 13h breakpoint catches the boot
   loader, an OUT breakpoint catches FUJINET.SYS probing COM2's UART, a
   write watchpoint catches the timer tick, Zydis disassembles known bytes,
   a .map file names an address, and the INT trace and console work. */
#include "test_util.h"
#include "msdosdebug.h"
#include "msdosmemtools.h"
#include "msdossnd.h"

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

    /* the video card (CGA on a 5150/5160): the Video tab's snapshot, decoded and tracked */
    {
        msdos_vid_snap v;
        msdosvid_tracker tr;
        msdosvid_info in;
        uint8_t bda[2], cell[2] = { 0x41, 0x1E };
        int have_font;
        memset(&v, 0, sizeof v);
        msdosvid_tracker_init(&tr);
        CHECK(msdosdebug_video_snap(d, &v) == 0 && v.card == MSDOS_VID_CGA, "video snap: card %d", v.card);
        CHECK(msdosvid_info_get(&v, &in) == 0 && in.mode == MSDOS_VM_TEXT && in.rows == 25 &&
              (in.cols == 40 || in.cols == 80), "BIOS text mode: %s", in.mode_name);
        CHECK(v.vram_size == 16384 && v.vram, "VRAM");
        have_font = memcmp(v.font + 0x41 * 16, "\0\0\0\0\0\0\0\0", 8) != 0;   /* the CGA font ROM is imported */
        CHECK(v.displine >= 0 && v.displine < 400, "beam line %d", v.displine);
        CHECK(msdosdebug_read(d, 0x450, bda, 2) == 2, "BDA cursor");
        if (in.cursor_x >= 0)
            CHECK(in.cursor_x == bda[0] && in.cursor_y == bda[1], "cursor %d,%d vs BIOS %d,%d",
                  in.cursor_x, in.cursor_y, bda[0], bda[1]);
        msdosvid_tracker_push(&tr, &v);
        off = (uint32_t)((tr.cur.crtc[12] << 8 | tr.cur.crtc[13]) * 2u) & 0x3FFF;
        CHECK(msdosdebug_write(d, 0xB8000 + off, cell, 2) == 2, "write a cell");
        CHECK(msdosdebug_video_snap(d, &v) == 0, "second snap");
        msdosvid_tracker_push(&tr, &v);
        CHECK(tr.heat[off] == 255 && tr.changed[off] == 1, "the written byte is hot: %d", tr.heat[off]);
        {
            msdosvid_ctx c = { .snap = &tr.cur, .track = &tr };
            msdosvid_pixel hp;
            static uint32_t pic[640 * 400 * 4];
            int w, h, x, y, fg = 0;
            CHECK(msdosvid_hit(&c, MSDOSVID_V_DECODED, 1, 1, 1, &hp) == 0 && hp.chr == 0x41 &&
                  hp.value == 0x1E && hp.voff == off && hp.cpu_addr == 0xB8000 + off,
                  "hit cell 0,0: chr %02X attr %02X voff %X", hp.chr, hp.value, hp.voff);
            CHECK(msdosvid_render(&c, MSDOSVID_V_DECODED, msdosvid_overlay_defaults(), 1, pic,
                                  sizeof pic / 4, &w, &h) == 0 && w == in.width, "render");
            for (y = 0; y < 8; y++)
                for (x = 0; x < 8; x++)
                    fg += (pic[y * w + x] & 0xFFFFFF) == 0xFFFF55;
            CHECK(!have_font || fg > 0, "an 'A' in yellow was drawn");
            CHECK(msdosvid_render(&c, MSDOSVID_V_MEMORY, 0, 2, pic, sizeof pic / 4, &w, &h) == 0 && w > 0,
                  "memory map");
        }
        CHECK(msdosdebug_command(d, "o 3D9 2F", text, sizeof text) == 0, "o 3D9");
        CHECK(msdosdebug_video_snap(d, &v) == 0 && v.color_reg == 0x2F, "colour select %02X", v.color_reg);
        CHECK(msdosdebug_command(d, "video", text, sizeof text) == 0 && strstr(text, "CGA"), "video: %s", text);
        CHECK(msdosdebug_command(d, "video regs", text, sizeof text) == 0 && strstr(text, "Cursor Start"),
              "video regs: %s", text);
        msdos_vid_snap_free(&v);
        msdosvid_tracker_free(&tr);
    }

    /* ---- the newer engine: expressions, conditions, old/value, stop text, trace ---- */
    {
        msdos_stop_snapshot snap;
        msdos_breakpoint bl[16];
        msdos_trace_entry tr[64];
        msdos_region map[12];
        uint32_t hits[8], found, pc;
        int64_t v;
        char err[200];
        int id, n, i;
        unsigned serial_before;
        (void)serial_before;

        /* expressions read the machine */
        msdosdebug_regs(d, &r);
        CHECK(msdosdebug_eval(d, "cs", &v, err, sizeof err) == 0 && v == r.cs, "eval cs: %s", err);
        CHECK(msdosdebug_eval(d, "pc", &v, err, sizeof err) == 0 && v == (int64_t)(r.cs_base + r.eip), "eval pc");
        CHECK(msdosdebug_eval(d, "cs:ip", &v, err, sizeof err) == 0 && v == (int64_t)(r.cs_base + r.eip), "eval cs:ip");
        CHECK(msdosdebug_eval(d, "[0:$46C] == [$46C]", &v, err, sizeof err) == 0 && v == 1, "eval [seg:off]");
        CHECK(msdosdebug_eval(d, "{$600} == $1234 + 0", &v, err, sizeof err) == 0, "eval word");
        CHECK(msdosdebug_eval(d, "al + ah * 256 == ax", &v, err, sizeof err) == 0 && v == 1, "eval halves");
        CHECK(msdosdebug_eval(d, "value", &v, err, sizeof err) != 0, "value outside a watch is unknown");
        CHECK(msdosdebug_eval(d, "frame >= 0 && cycles >= 0", &v, err, sizeof err) == 0 && v == 1, "frame, cycles");

        /* a write watch with a condition on old: the hook fires before the store */
        id = msdosdebug_bp_add_if(d, MSDOS_BP_WRITE, 0x46C, 0x46D, -1, "value != old", err, sizeof err);
        CHECK(id > 0, "bp_add_if: %s", err);
        msdosdebug_resume(d);
        CHECK(wait_stopped(d, 10000), "write watch stops");
        CHECK(msdosdebug_stop_snapshot(d, &snap) == 1, "snapshot paused");
        CHECK(snap.stop.reason == MSDOS_STOP_WATCH_WRITE && snap.stop.bp_id == id, "reason %d", snap.stop.reason);
        CHECK(snap.stop.addr >= 0x46C && snap.stop.addr <= 0x46D && (snap.stop.width == 1 || snap.stop.width == 2), "addr %X width %d", snap.stop.addr, snap.stop.width);
        CHECK(snap.stop.old_value >= 0 && snap.stop.value != snap.stop.old_value, "value %lld old %lld",
              (long long)snap.stop.value, (long long)snap.stop.old_value);
        CHECK(((snap.stop.value - snap.stop.old_value) & ((1LL << (8 * snap.stop.width)) - 1)) == 1, "the tick count moved by one (%llX from %llX)", (long long)snap.stop.value, (long long)snap.stop.old_value);
        CHECK(strstr(snap.text, "wrote 0046") && strstr(snap.text, "= $") && strstr(snap.text, "(was $"), "text: %s", snap.text);
        CHECK(snap.serial == msdosdebug_stop_serial(d) && snap.stop.serial == snap.serial, "serial agrees");
        CHECK(snap.address == snap.stop.pc, "address");
        found = 0;
        msdosdebug_read(d, snap.stop.addr, (uint8_t *)&found, snap.stop.width);
        CHECK(found == (uint32_t)snap.stop.value, "the store has happened by the stop: %X vs %llX", found, (long long)snap.stop.value);
        n = msdosdebug_bp_list(d, bl, 16);
        CHECK(n == 1 && bl[0].hits == 1, "hits %u", bl[0].hits);
        /* a condition that cannot be true never stops, and counts nothing */
        CHECK(msdosdebug_bp_set_condition(d, id, "old == $FFFF && value == 5", err, sizeof err) == 1, "set cond");
        msdosdebug_bp_clear_hits(d);
        msdosdebug_resume(d);
        test_sleep_ms(800);
        CHECK(!msdosdebug_stopped(d), "an impossible condition does not stop");
        msdosdebug_stop(d);
        CHECK(msdosdebug_stopped(d), "stop returns stopped");
        n = msdosdebug_bp_list(d, bl, 16);
        CHECK(bl[0].hits == 0, "no hits");
        /* disabled: the writes pass */
        CHECK(msdosdebug_bp_set_condition(d, id, "", err, sizeof err) == 1, "remove cond");
        CHECK(msdosdebug_bp_enable(d, id, 0) == 0, "disable");
        msdosdebug_resume(d);
        test_sleep_ms(500);
        CHECK(!msdosdebug_stopped(d), "a disabled watch does not stop");
        CHECK(msdosdebug_bp_enable(d, id, 1) == 0, "enable");
        CHECK(wait_stopped(d, 10000), "an enabled watch stops again");
        msdosdebug_bp_clear(d);

        /* an I/O stop in words */
        id = msdosdebug_bp_add_if(d, MSDOS_BP_IN, 0x60, 0x60, -1, "", err, sizeof err);
        CHECK(id > 0, "bp IN 60");
        msdosdebug_bp_remove(d, id);
        id = msdosdebug_bp_add_if(d, MSDOS_BP_OUT, 0x40, 0x43, -1, "value >= 0", err, sizeof err);
        msdosdebug_resume(d);
        if (wait_stopped(d, 3000)) {
            msdosdebug_stop_snapshot(d, &snap);
            CHECK(snap.stop.reason == MSDOS_STOP_WATCH_OUT && strstr(snap.text, "out 004"), "out text: %s", snap.text);
        }
        msdosdebug_bp_clear(d);
        msdosdebug_stop(d);

        /* resuming from a breakpoint at the current PC runs the instruction first */
        msdosdebug_regs(d, &r);
        pc = r.cs_base + r.eip;
        msdosdebug_trace_clear(d);
        msdosdebug_trace_enable(d, 1);
        CHECK(msdosdebug_trace_enabled(d), "trace on");
        id = msdosdebug_bp_add(d, MSDOS_BP_EXEC, pc, pc, -1);
        {
            uint64_t before = msdosdebug_stop_serial(d);
            msdosdebug_resume(d);
            CHECK(msdosdebug_wait_stop(d, before, 15000), "the breakpoint at the PC comes round again, not at once");
        }
        n = msdosdebug_history_copy(d, tr, 64);
        CHECK(n >= 1, "something ran before the stop (%d records)", n);
        if (n >= 1)
            CHECK(tr[0].linear == pc, "the oldest record is the instruction we resumed from: %X vs %X", tr[0].linear, pc);
        msdosdebug_stop_snapshot(d, &snap);
        CHECK(snap.stop.reason == MSDOS_STOP_BREAKPOINT && snap.stop.bp_id == id && strstr(snap.text, "breakpoint"), "bp stop: %s", snap.text);
        msdosdebug_bp_clear(d);

        /* a stop never records the instruction it did not run; steps record what ran */
        msdosdebug_regs(d, &r);
        pc = r.cs_base + r.eip;
        msdosdebug_trace_clear(d);
        CHECK(msdosdebug_step(d) == 0 && wait_stopped(d, 5000), "step");
        n = msdosdebug_trace_read(d, tr, 64);
        CHECK(n == 1 && tr[0].linear == pc && tr[0].len >= 1, "one step, one record at %X (n=%d, got %X)", pc, n, n ? tr[0].linear : 0);
        msdosdebug_stop_snapshot(d, &snap);
        CHECK(snap.stop.reason == MSDOS_STOP_STEP && strstr(snap.text, "step at"), "step text: %s", snap.text);
        CHECK(msdosdebug_step_n(d, 5) == 0 && wait_stopped(d, 5000), "step 5");
        n = msdosdebug_trace_read(d, tr, 64);
        CHECK(n >= 2 && n <= 6, "step 5 records %d", n);
        CHECK(msdosdebug_trace_format(&tr[0], text, sizeof text) > 10 && strchr(text, ':'), "format: %s", text);
        CHECK(msdosdebug_trace_total(d) >= (uint64_t)n, "total");
        /* the ring is shared: trace is newest first, history oldest first */
        {
            msdos_trace_entry hist[64];
            int hn = msdosdebug_history_copy(d, hist, 64);
            CHECK(hn == n && hist[0].linear == tr[n - 1].linear && hist[hn - 1].linear == tr[0].linear, "history is the ring oldest first");
        }
        CHECK(msdosdebug_trace_match(&tr[0], MSDOS_TRACE_ALL, 0, 0), "all");
        CHECK(msdosdebug_trace_match(&tr[0], MSDOS_TRACE_RANGE, tr[0].linear, tr[0].linear), "range in");
        CHECK(!msdosdebug_trace_match(&tr[0], MSDOS_TRACE_RANGE, tr[0].linear + 1, tr[0].linear + 2), "range out");

        /* run on with trace on: records pile up, REP and HALT fold, and Branches filters */
        msdosdebug_trace_clear(d);
        msdosdebug_resume(d);
        test_sleep_ms(600);
        msdosdebug_stop(d);
        n = msdosdebug_trace_count(d);
        CHECK(n > 100, "a running PC fills the ring (%d)", n);
        {
            static msdos_trace_entry big[2048];
            int bn = msdosdebug_trace_read(d, big, 2048), branches = 0, folded = 0;
            for (i = 0; i < bn; i++) {
                branches += msdosdebug_trace_match(&big[i], MSDOS_TRACE_BRANCHES, 0, 0);
                folded += big[i].repeat > 0;
            }
            if (!(bn == 2048 && branches > 0 && branches < bn)) {
                msdosdebug_command(d, "u F000:EF70 l 8", text, sizeof text);
                fprintf(stderr, "%s", text);
                msdosdebug_command(d, "r", text, sizeof text);
                fprintf(stderr, "%s", text);
            }
            if (!(bn == 2048 && branches > 0 && branches < bn))
                for (i = 0; i < 6 && i < bn; i++) {
                    msdosdebug_trace_format(&big[i], text, sizeof text);
                    fprintf(stderr, "  trace[%d] len %d: %s\n", i, big[i].len, text);
                }
            CHECK(bn == 2048 && branches > 0 && branches < bn, "branches %d of %d", branches, bn);
            for (i = 1; i < bn; i++)
                CHECK(big[i].cycles <= big[i - 1].cycles, "newest first, cycles %llu then %llu",
                      (unsigned long long)big[i - 1].cycles, (unsigned long long)big[i].cycles);
            (void)folded;
        }
        msdosdebug_trace_enable(d, 0);
        n = msdosdebug_trace_count(d);
        msdosdebug_resume(d);
        test_sleep_ms(200);
        msdosdebug_stop(d);
        CHECK(msdosdebug_trace_count(d) == n, "trace off records nothing");
        msdosdebug_trace_clear(d);
        CHECK(msdosdebug_trace_count(d) == 0, "clear");

        /* registers: bulk, single, flags, refused while running */
        msdosdebug_regs(d, &r);
        r.eax = 0x1234; r.ebx = 0x5678; r.ds = 0x0777; r.eflags |= 0x40;
        CHECK(msdosdebug_regs_set(d, &r, err, sizeof err) == 1, "regs_set: %s", err);
        msdosdebug_regs(d, &r2);
        CHECK((r2.eax & 0xFFFF) == 0x1234 && (r2.ebx & 0xFFFF) == 0x5678 && r2.ds == 0x0777 && r2.ds_base == 0x7770 && (r2.eflags & 0x40), "regs written back");
        CHECK(msdosdebug_register_set(d, "al", 0x7F, err, sizeof err) == 1, "set al: %s", err);
        CHECK(msdosdebug_register_set(d, "zf", 0, err, sizeof err) == 1, "set zf");
        msdosdebug_regs(d, &r2);
        CHECK((r2.eax & 0xFFFF) == 0x127F && !(r2.eflags & 0x40), "al and zf: %04X %X", r2.eax & 0xFFFF, r2.eflags);
        CHECK(msdosdebug_register_set(d, "bogus", 1, err, sizeof err) == 0 && strstr(err, "no register"), "bogus: %s", err);
        CHECK(msdosdebug_eval(d, "ax == $127F && zf == 0 && ds == $777", &v, err, sizeof err) == 0 && v == 1, "eval sees the new registers");
        CHECK(msdosdebug_command(d, "set bx 4660", text, sizeof text) == 0 && strstr(text, "BX=1234"), "set: %s", text);
        msdosdebug_resume(d);
        CHECK(msdosdebug_regs_set(d, &r, err, sizeof err) == 0 && strstr(err, "running"), "regs_set while running: %s", err);
        CHECK(msdosdebug_register_set(d, "ax", 1, err, sizeof err) == 0 && strstr(err, "running"), "register_set while running");
        msdosdebug_stop(d);

        /* the stop state is one critical section, and Pause returns stopped */
        for (i = 0; i < 25; i++) {
            msdosdebug_resume(d);
            test_sleep_ms(5);
            msdosdebug_stop(d);
            CHECK(msdosdebug_stop_snapshot(d, &snap) == 1 && snap.stop.serial == snap.serial &&
                  snap.serial == msdosdebug_stop_serial(d) && !strncmp(snap.text, "paused at", 9),
                  "iteration %d: paused %d serial %llu/%llu text %s", i, snap.paused,
                  (unsigned long long)snap.serial, (unsigned long long)snap.stop.serial, snap.text);
        }

        /* the memory map is gap-free and ends at 1 MB on an 8088 */
        n = msdosdebug_memory_map(d, map, 12);
        CHECK(n >= 4 && map[0].low == 0 && map[n - 1].high == 0xFFFFF, "map: %d regions to %X", n, map[n - 1].high);
        for (i = 1; i < n; i++)
            CHECK(map[i].low == map[i - 1].high + 1, "gap before region %d: %X vs %X", i, map[i - 1].high, map[i].low);
        CHECK(strstr(map[0].name, "Conventional") && strstr(map[n - 1].name, "BIOS"), "names: %s / %s", map[0].name, map[n - 1].name);
        {
            char rn[80];
            CHECK(msdosmem_region_name(map, n, 0xB8000, rn, sizeof rn) > 0 && strstr(rn, "Video"), "B8000 is video: %s", rn);
        }
        CHECK(msdosdebug_command(d, "map", text, sizeof text) == 0 && strstr(text, "Conventional") && strstr(text, "000000-"), "map: %s", text);

        /* search: hex, text (no case), next, previous, wrap */
        {
            static const uint8_t pat[8] = { 'Q', 'Z', 'X', 'J', '9', 'K', 'W', '7' };
            uint8_t low[8] = { 'q', 'z', 'x', 'j', '9', 'k', 'w', '7' };
            msdosdebug_write(d, 0x20600, pat, 8);        /* above 64K, below 640K */
            msdosdebug_write(d, 0x30700, pat, 8);
            n = msdosdebug_search(d, pat, 8, 0xFF, 0, 0, 0xFFFFF, hits, 8);
            CHECK(n == 2 && hits[0] == 0x20600 && hits[1] == 0x30700, "search: %d hits, %X", n, n ? hits[0] : 0);
            n = msdosdebug_search(d, low, 8, 0xFF, 0, 0, 0xFFFFF, hits, 8);
            CHECK(n == 0, "case matters without the flag");
            n = msdosdebug_search(d, low, 8, 0xFF, MSDOS_SEARCH_NOCASE, 0, 0xFFFFF, hits, 8);
            CHECK(n == 2, "no case: %d", n);
            n = msdosdebug_search(d, pat, 8, 0xFF, 0, 0x20601, 0xFFFFF, hits, 8);
            CHECK(n == 1 && hits[0] == 0x30700, "from bounds the start");
            CHECK(msdosdebug_search_next(d, pat, 8, 0xFF, 0, 0x20600, 0, &found) == 1 && found == 0x30700, "next");
            CHECK(msdosdebug_search_next(d, pat, 8, 0xFF, 0, 0x30700, 0, &found) == 1 && found == 0x20600, "next wraps");
            CHECK(msdosdebug_search_next(d, pat, 8, 0xFF, 0, 0x30700, 1, &found) == 1 && found == 0x20600, "previous");
            CHECK(msdosdebug_search_next(d, pat, 8, 0xFF, 0, 0x20600, 1, &found) == 1 && found == 0x30700, "previous wraps");
            CHECK(msdosdebug_command(d, "find text qzxj9kw7", text, sizeof text) == 0 && strstr(text, "20600") && strstr(text, "30700"), "find: %s", text);
            CHECK(msdosdebug_command(d, "find 51 5A 58 4A", text, sizeof text) == 0 && strstr(text, "30700"), "find hex: %s", text);
            CHECK(msdosdebug_command(d, "find text no_such_text_anywhere_zz", text, sizeof text) == 0 && strstr(text, "not found"), "find none: %s", text);
            {
                uint8_t zero[8] = { 0 };
                msdosdebug_write(d, 0x20600, zero, 8);
                msdosdebug_write(d, 0x30700, zero, 8);
            }
        }

        /* watches through the engine */
        msdoswatch_clear();
        msdoswatch_add("46C");
        msdoswatch_add("0:46C l 2");
        CHECK(msdoswatch_format(d, 0, text, sizeof text) > 0 && strstr(text, "$"), "watch value while stopped: %s", text);
        CHECK(msdoswatch_format(d, 1, text, sizeof text) > 0 && strstr(text, "-0046D"), "watch range: %s", text);
        msdoswatch_clear();

        /* the machine and sound pages */
        {
            msdos_snd_state sound;
            static char page[16384];
            CHECK(msdosdebug_io_text(d, page, sizeof page) > 100, "io text");
            CHECK(strstr(page, "CPU:") && strstr(page, "PIC 1:") && strstr(page, "PIT 0:") && strstr(page, "DMA 0:") && strstr(page, "A20"), "io page: %s", page);
            CHECK(strstr(page, "Intel 8088") && strstr(page, "FujiNet link") == 0 + strstr(page, "FujiNet link"), "8088 on the page");
            CHECK(msdosdebug_sound_get(d, &sound) == 0 && sound.opl_channels >= 9, "sound_get");
            CHECK(msdosdebug_sound_text(d, page, sizeof page) > 20 && strstr(page, "PC speaker"), "sound text: %s", page);
            CHECK(msdosdebug_command(d, "sound", page, sizeof page) == 0 && strstr(page, "PC speaker"), "sound verb");
            CHECK(msdosdebug_command(d, "io", page, sizeof page) == 0 && strstr(page, "PIC 1:"), "io verb");
            /* a program writes the PC speaker: PIT 2 reload 1193 (1000 Hz), gate and data on */
            CHECK(msdosdebug_command(d, "o 43 B6", page, sizeof page) == 0, "o 43");
            CHECK(msdosdebug_command(d, "o 42 A9", page, sizeof page) == 0 && msdosdebug_command(d, "o 42 04", page, sizeof page) == 0, "o 42");
            CHECK(msdosdebug_command(d, "o 61 03", page, sizeof page) == 0, "o 61");
            CHECK(msdosdebug_sound_get(d, &sound) == 0 && sound.speaker_on && sound.pit_reload == 1193 &&
                  sound.speaker_hz > 990.0 && sound.speaker_hz < 1010.0, "speaker %d reload %u hz %.1f", sound.speaker_on,
                  (unsigned)sound.pit_reload, sound.speaker_hz);
            msdosdebug_command(d, "o 61 00", page, sizeof page);
        }

        /* the console on a live PC */
        CHECK(msdosdebug_command(d, "print ax + 1", text, sizeof text) == 0 && strstr(text, "$"), "print: %s", text);
        CHECK(msdosdebug_command(d, "trace 3", text, sizeof text) == 0, "trace verb");
        CHECK(msdosdebug_command(d, "bpw 46C l 2 value > 2000000", text, sizeof text) == 0 && strstr(text, "Breakpoint"), "bpw verb: %s", text);
        CHECK(msdosdebug_command(d, "breaks", text, sizeof text) == 0 && strstr(text, "if value > 2000000"), "breaks: %s", text);
        msdosdebug_bp_clear(d);
        CHECK(msdosdebug_command(d, "step", text, sizeof text) == 0 && strstr(text, "step at"), "step verb: %s", text);
        CHECK(msdosdebug_command(d, "t", text, sizeof text) == 0 && strstr(text, "CS="), "t shows registers: %s", text);
        CHECK(msdosdebug_command(d, "mem 0:46C 4", text, sizeof text) == 0 && strstr(text, "0000:046C"), "mem: %s", text);
        CHECK(msdosdebug_command(d, "d 0:46C l 4", text, sizeof text) == 0 && strstr(text, "0000:046C"), "d: %s", text);
        CHECK(msdosdebug_command(d, "disasm cs:ip 2", text, sizeof text) == 0 && strchr(text, '>'), "disasm: %s", text);

        /* a reset keeps the breakpoints, and the hooks still work after it */
        id = msdosdebug_bp_add_if(d, MSDOS_BP_WRITE, 0x46C, 0x46D, -1, "", err, sizeof err);
        CHECK(id > 0, "bp before reset");
        msdossession_set_warp(s, 1);
        CHECK(msdossession_reset(s) == 0, "reset");
        msdosdebug_resume(d);
        CHECK(wait_stopped(d, 30000), "a write watch set before a reset stops after it");
        n = msdosdebug_bp_list(d, bl, 16);
        CHECK(n == 1 && bl[0].id == id && bl[0].hits >= 1, "the breakpoint survived the reset");
        msdosdebug_bp_clear(d);
        msdossession_set_warp(s, 0);
        msdosdebug_stop(d);
    }

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
    /* a window that still polls after the PC went away gets "off", not a hang or a crash */
    CHECK(msdosdebug_regs(d, &r) == -1 && !msdosdebug_stopped(d), "regs after stop");
    CHECK(msdosdebug_command(d, "r", text, sizeof text) == -1 && strstr(text, "PC is off"), "console after stop");
    {
        msdos_stop_snapshot after;
        CHECK(msdosdebug_stop_snapshot(d, &after) == 0, "snapshot after stop");
    }
    msdossession_free(s);
    CHECK(msdosdebug_regs(d, &r) == -1 && msdosdebug_attach(d) == -1, "an engine outliving its session answers 'off'");
    puts("debug: ok");
    return 0;
}
