/*
 * prompt -- the debugger's console (msdosdebug_command) and its completions.
 * DEBUG.COM's verbs (r d e u g t p, bp/bc/bl, i o) work as they always did; the
 * family's verbs (step over out run stop runto break bpr bpw bpa bpin bpout
 * bpio bpint delete enable disable clear breaks hits print regs set mem poke
 * disasm find trace history watch unwatch symbols label labels map io sound
 * video save) sit beside them. Every command is a thin layer over the public
 * API, so a window's buttons and the console can never disagree.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

#include "msdosdebug.h"
#include "msdosmemtools.h"

namespace {

constexpr int WaitMs = 1500;

struct Out
{
	std::string text;
	void emit(const char* fmt, ...)
	{
		char b[1024];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(b, sizeof b, fmt, ap);
		va_end(ap);
		text += b;
	}
	void add(const std::string& s) { text += s; }
};

const char* const kCommands[] = {
	"help", "r", "d", "e", "u", "g", "t", "p", "bp", "bc", "bl", "i", "o", "step", "over", "out", "run",
	"stop", "runto", "break", "bpr", "bpw", "bpa", "bpi", "bpo", "bpin", "bpout", "bpio", "bpint",
	"delete", "enable", "disable", "clear", "breaks", "hits", "print", "regs", "set", "mem", "poke",
	"disasm", "find", "trace", "history", "watch", "unwatch", "symbols", "label", "labels", "map", "io",
	"machine", "sound", "video", "save", nullptr };

const char* const kHelp =
	"DEBUG.COM verbs (numbers are hex):\n"
	"r [reg value]        registers (or set one)\n"
	"d [addr] [l len]     dump memory           e addr bytes...   enter bytes\n"
	"u [addr] [l count]   unassemble            g [addr]          go (to addr)\n"
	"t [n]                trace n instructions  p                 proceed over a call, INT or loop\n"
	"bp addr [cond]       breakpoint;  bpr/bpw/bpa addr [len|cond]  on memory\n"
	"bpi/bpo/bpio port [last]   on I/O ports;  bpint n [ah]   on INT n\n"
	"bl, bc n|*           list, clear breakpoints     i port, o port byte   read, write a port\n"
	"The family's verbs (expressions are decimal unless $ 0x h):\n"
	"step [n] (F7), over (F8), out (Shift+F8), run (F5), stop, runto <addr>\n"
	"break <addr> [cond], bpr|bpw|bpa <addr>[-<end>] [cond], bpin|bpout|bpio <port>[-<end>] [cond]\n"
	"bpint <n> [ah] [cond], delete <id|addr>, enable|disable <id>, clear, breaks, hits [clear]\n"
	"print <expr>, regs, set <reg> <expr>, mem <addr> [len], poke <addr> <val...>, disasm [addr] [n]\n"
	"find [hex] <bytes> | find text <string>, trace [on|off|clear|n], history [n|on|off|clear]\n"
	"watch [expr|lo-hi], unwatch <n|all>, symbols [text], label <addr> <name>, labels <file>\n"
	"map, io (Machine & I/O), sound, video [section], save <dis|mem|regs> <file>\n"
	"Addresses: seg:off, a register (cs:ip, ds:si), a hex linear address, a symbol.\n"
	"Expressions: $B800 0xB800 B800h 49152 %1010; al ax eax ... cs ds es ss fs gs ip eip flags\n"
	"cf zf sf of df if pf af tf pc; [x] byte, {x} word, dword [x]; ds:si; symbols; frame cycles;\n"
	"in a condition value, old and addr; C operators. Tab completes.\n";

char* next_tok(char** cursor)
{
	char* str = *cursor;
	while (*str && isspace((unsigned char)*str)) str++;
	if (!*str) {
		*cursor = str;
		return nullptr;
	}
	char* tok = str;
	while (*str && !isspace((unsigned char)*str)) str++;
	if (*str) *str++ = '\0';
	*cursor = str;
	return tok;
}

// The next token without consuming or modifying anything.
std::string peek_tok(const char* cursor)
{
	while (*cursor && isspace((unsigned char)*cursor)) cursor++;
	const char* end = cursor;
	while (*end && !isspace((unsigned char)*end)) end++;
	return std::string(cursor, (size_t)(end - cursor));
}

char* rest_of(char* cursor)
{
	while (*cursor && isspace((unsigned char)*cursor)) cursor++;
	return cursor;
}

bool is_hex(const char* t)
{
	if (!*t) return false;
	size_t len = strlen(t);
	if (len > 8) return false;
	for (; *t; t++)
		if (!isxdigit((unsigned char)*t)) return false;
	return true;
}

int eval_text(msdosdebug* d, Out& out, const char* text, int64_t* val)
{
	char err[96];
	if (msdosdebug_eval(d, text, val, err, sizeof err) != 0) {
		out.emit("error: %s\n", err);
		return -1;
	}
	return 0;
}

// An address argument: seg:off, a register, a symbol, bare hex, or an expression
int eval_addr(msdosdebug* d, Out& out, const char* text, uint32_t* lin, uint16_t* seg = nullptr,
              uint32_t* off = nullptr)
{
	if (msdosdebug_parse_address(d, text, seg, off, lin) != 0) {
		out.emit("error: bad address '%s'\n", text);
		return -1;
	}
	return 0;
}

void show_regs(msdosdebug* d, Out& out)
{
	msdos_cpu_regs r;
	if (msdosdebug_regs(d, &r) != 0) {
		out.add("The PC is off\n");
		return;
	}
	char b[640];
	const uint32_t f = r.eflags;
	auto fl = [&](int bit, const char* on, const char* off) { return (f >> bit) & 1 ? on : off; };
	if (r.is386)
		snprintf(b, sizeof b,
		         "EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESP=%08X EBP=%08X ESI=%08X EDI=%08X\n"
		         "DS=%04X ES=%04X SS=%04X CS=%04X FS=%04X GS=%04X EIP=%08X  %s %s %s %s %s %s %s %s\n",
		         r.eax, r.ebx, r.ecx, r.edx, r.esp, r.ebp, r.esi, r.edi, r.ds, r.es, r.ss, r.cs, r.fs,
		         r.gs, r.eip, fl(11, "OV", "NV"), fl(10, "DN", "UP"), fl(9, "EI", "DI"),
		         fl(7, "NG", "PL"), fl(6, "ZR", "NZ"), fl(4, "AC", "NA"), fl(2, "PE", "PO"),
		         fl(0, "CY", "NC"));
	else
		snprintf(b, sizeof b,
		         "AX=%04X  BX=%04X  CX=%04X  DX=%04X  SP=%04X  BP=%04X  SI=%04X  DI=%04X\n"
		         "DS=%04X  ES=%04X  SS=%04X  CS=%04X  IP=%04X   %s %s %s %s %s %s %s %s\n",
		         r.eax & 0xFFFF, r.ebx & 0xFFFF, r.ecx & 0xFFFF, r.edx & 0xFFFF, r.esp & 0xFFFF,
		         r.ebp & 0xFFFF, r.esi & 0xFFFF, r.edi & 0xFFFF, r.ds, r.es, r.ss, r.cs, r.eip & 0xFFFF,
		         fl(11, "OV", "NV"), fl(10, "DN", "UP"), fl(9, "EI", "DI"), fl(7, "NG", "PL"),
		         fl(6, "ZR", "NZ"), fl(4, "AC", "NA"), fl(2, "PE", "PO"), fl(0, "CY", "NC"));
	out.add(b);
	msdos_dis_line l;
	msdosdebug_disassemble(d, r.cs, r.eip, 1, &l);
	char row[200];
	snprintf(row, sizeof row, "%04X:%0*X %s%*s%s\n", l.seg, r.code32 ? 8 : 4, l.off, l.bytes,
	         std::max(1, 16 - (int)strlen(l.bytes)), "", l.text);
	out.add(row);
}

void where_text(msdosdebug* d, Out& out)
{
	msdos_cpu_regs r;
	if (msdosdebug_regs(d, &r) != 0)
		return;
	msdos_dis_line l;
	if (msdosdebug_disassemble(d, r.cs, r.eip, 1, &l) < 1)
		return;
	out.emit("%04X:%0*X  %-20s %s%s%s\n", l.seg, r.code32 ? 8 : 4, l.off, l.bytes, l.text,
	         l.symbol[0] ? "   ; " : "", l.symbol);
}

// After a command that lets the PC go: wait a moment for the stop it heads for.
void report_stop(msdosdebug* d, Out& out, uint64_t before, const char* running)
{
	if (msdosdebug_wait_stop(d, before, WaitMs)) {
		char text[200];
		msdosdebug_stop_text(d, text, sizeof text, nullptr);
		out.emit("%s\n", text);
		where_text(d, out);
	} else {
		out.emit("%s\n", running);
	}
}

void dump_rows(msdosdebug* d, Out& out, uint16_t seg, uint32_t off, uint32_t len)
{
	for (uint32_t row = 0; row < len; row += 16) {
		uint8_t b[16];
		msdosdebug_read(d, msdosdebug_linear(d, seg, off + row), b, 16);
		char lineb[128];
		int p = snprintf(lineb, sizeof lineb, "%04X:%04X  ", seg, (off + row) & 0xFFFF);
		for (int i = 0; i < 16; i++)
			p += snprintf(lineb + p, sizeof lineb - (size_t)p, "%02X%s", b[i], i == 7 ? "-" : " ");
		p += snprintf(lineb + p, sizeof lineb - (size_t)p, " ");
		for (int i = 0; i < 16; i++)
			lineb[p++] = (b[i] >= 0x20 && b[i] < 0x7F) ? (char)b[i] : '.';
		lineb[p] = 0;
		out.add(std::string(lineb) + "\n");
	}
}

void list_breaks(msdosdebug* d, Out& out)
{
	msdos_breakpoint list[MSDOS_MAX_BREAKPOINTS];
	const int count = msdosdebug_bp_list(d, list, MSDOS_MAX_BREAKPOINTS);
	if (!count) {
		out.add("No breakpoints\n");
		return;
	}
	for (int i = 0; i < count; i++) {
		char desc[256];
		msdosbp_describe(d, &list[i], desc, sizeof desc);
		// the condition goes last, after the hits
		std::string text = list[i].text;
		out.emit("%d %s%s  hits %u%s%s\n", list[i].id, list[i].enabled ? " " : "(off) ", text.c_str(),
		         list[i].hits, list[i].condition[0] ? "  if " : "", list[i].condition);
	}
}

// bp-like: <addr>[-<end>|len] [cond]
void add_bp(msdosdebug* d, Out& out, int type, char* args)
{
	char* tok = next_tok(&args);
	char err[160];
	uint32_t first, last;
	const bool port = type >= MSDOS_BP_IN && type <= MSDOS_BP_IO;
	if (!tok) {
		out.add("usage: <addr>[-<end>] [condition]\n");
		return;
	}
	char* dash = type == MSDOS_BP_EXEC ? nullptr : strchr(tok + 1, '-');
	if (dash && !(port ? is_hex(dash + 1) : true))
		dash = nullptr;
	int64_t v;
	if (port) {
		char* tail = dash ? (*dash = '\0', dash + 1) : nullptr;
		if (!is_hex(tok) && eval_text(d, out, tok, &v) != 0)
			return;
		first = is_hex(tok) ? (uint32_t)strtoul(tok, nullptr, 16) : (uint32_t)v;
		last = first;
		if (tail) {
			last = (uint32_t)strtoul(tail, nullptr, 16);
		} else {
			const std::string second = peek_tok(args);
			if (is_hex(second.c_str())) {
				last = (uint32_t)strtoul(second.c_str(), nullptr, 16);
				next_tok(&args);
			}
		}
		if (first > 0xFFFF || last > 0xFFFF) {
			out.add("error: ports are 0-FFFF\n");
			return;
		}
	} else {
		if (dash)
			*dash = '\0';
		if (eval_addr(d, out, tok, &first) != 0)
			return;
		last = first;
		if (dash) {
			if (eval_addr(d, out, dash + 1, &last) != 0)
				return;
		} else if (type != MSDOS_BP_EXEC) {
			// DEBUG style: a length in hex, "l 20" or "l20" or a bare number
			const std::string second = peek_tok(args);
			if (!second.empty() && (second[0] == 'l' || second[0] == 'L') &&
			    (second.size() == 1 || is_hex(second.c_str() + 1))) {
				next_tok(&args);
				uint32_t len = 1;
				if (second.size() > 1) {
					len = (uint32_t)strtoul(second.c_str() + 1, nullptr, 16);
				} else {
					char* lenText = next_tok(&args);
					len = lenText ? (uint32_t)strtoul(lenText, nullptr, 16) : 1;
				}
				last = first + (len ? len : 1) - 1;
			} else if (is_hex(second.c_str())) {
				const char* look = args;
				while (*look && isspace((unsigned char)*look)) look++;
				while (*look && !isspace((unsigned char)*look)) look++;
				while (*look && isspace((unsigned char)*look)) look++;
				if (!*look) {      // a lone number is a length; more text is a condition
					next_tok(&args);
					last = first + (uint32_t)strtoul(second.c_str(), nullptr, 16) - 1;
					if (last < first) last = first;
				}
			}
		}
	}
	const int id = msdosdebug_bp_add_if(d, type, first, last, -1, rest_of(args), err, sizeof err);
	if (id < 0)
		out.emit("error: %s\n", err);
	else
		out.emit("Breakpoint %d\n", id);
}

} // namespace

int msdosdebug_command(msdosdebug* d, const char* line, char* dst, int dstsz)
{
	static uint16_t dumpSeg, unasmSeg;
	static uint32_t dumpOff, unasmOff;
	static bool haveDump, haveUnasm;

	Out out;
	int rc = 0;
	if (!dst || dstsz <= 0)
		return 0;
	dst[0] = 0;
	if (!d || !line)
		return 0;
	std::string copy = line;
	char* cursor = &copy[0];
	char* cmd = next_tok(&cursor);
	if (!cmd)
		return 0;
	for (char* c = cmd; *c; c++)
		*c = (char)tolower((unsigned char)*c);

	int64_t val = 0, aux = 0;
	msdos_cpu_regs r;
	const bool up = msdosdebug_regs(d, &r) == 0;
	auto machine = [&]() {
		if (up)
			return true;
		out.add("The PC is off\n");
		rc = -1;
		return false;
	};
	auto hexnum = [&](const char* t, uint32_t def) -> uint32_t {
		return t ? (uint32_t)strtoul(t, nullptr, 16) : def;
	};
	auto lenArg = [&](char** cur, uint32_t def) -> uint32_t {
		const std::string t = peek_tok(*cur);
		if (t.empty()) return def;
		if (t == "l" || t == "L") {
			next_tok(cur);
			char* n = next_tok(cur);
			return hexnum(n, def);
		}
		if ((t[0] == 'l' || t[0] == 'L') && t.size() > 1 && is_hex(t.c_str() + 1)) {
			next_tok(cur);
			return hexnum(t.c_str() + 1, def);
		}
		return def;
	};

	if (!strcmp(cmd, "?") || !strcmp(cmd, "h") || !strcmp(cmd, "help")) {
		out.add(kHelp);
	} else if (!strcmp(cmd, "r")) {
		if (!machine()) goto done;
		char* name = next_tok(&cursor);
		char* value = next_tok(&cursor);
		if (name && value) {
			if (msdosdebug_set_reg(d, name, hexnum(value, 0)) != 0)
				out.emit("Cannot set %s\n", name);
		}
		show_regs(d, out);
	} else if (!strcmp(cmd, "regs")) {
		show_regs(d, out);
	} else if (!strcmp(cmd, "d")) {
		if (!machine()) goto done;
		uint16_t seg = haveDump ? dumpSeg : r.ds;
		uint32_t off = haveDump ? dumpOff : 0, lin;
		const std::string tok = peek_tok(cursor);
		if (!tok.empty() && tok != "l" && tok != "L" && !((tok[0] == 'l' || tok[0] == 'L') && is_hex(tok.c_str() + 1)) &&
		    msdosdebug_parse_address(d, tok.c_str(), &seg, &off, &lin) == 0) {
			if (tok.find(':') == std::string::npos) {      // a linear address dumps as seg:0
				seg = (uint16_t)(lin >> 4);
				off = lin & 0xF;
			}
			next_tok(&cursor);
		}
		const uint32_t len = std::min(lenArg(&cursor, 0x80), 0x1000u);
		dump_rows(d, out, seg, off, len);
		dumpSeg = seg;
		dumpOff = off + len;
		haveDump = true;
	} else if (!strcmp(cmd, "mem") || !strcmp(cmd, "m")) {
		char* addr_tok = next_tok(&cursor);
		char* len_tok = next_tok(&cursor);
		if (!addr_tok) {
			out.add("usage: mem <addr> [len]\n");
			goto done;
		}
		if (!machine()) goto done;
		uint32_t lin;
		uint16_t seg;
		uint32_t off;
		if (eval_addr(d, out, addr_tok, &lin, &seg, &off) != 0) goto done;
		aux = 64;
		if (len_tok && eval_text(d, out, len_tok, &aux) != 0) goto done;
		aux = std::max<int64_t>(1, std::min<int64_t>(aux, 512));
		dump_rows(d, out, strchr(addr_tok, ':') ? seg : (uint16_t)(lin >> 4),
		          strchr(addr_tok, ':') ? off : (lin & 0xF), (uint32_t)aux);
	} else if (!strcmp(cmd, "e")) {
		if (!machine()) goto done;
		uint32_t lin;
		char* tok = next_tok(&cursor);
		if (!tok || eval_addr(d, out, tok, &lin) != 0) {
			if (!tok) out.add("e addr bytes...\n");
			goto done;
		}
		std::vector<uint8_t> bytes;
		while ((tok = next_tok(&cursor)) != nullptr)
			bytes.push_back((uint8_t)strtoul(tok, nullptr, 16));
		msdosdebug_write(d, lin, bytes.data(), (int)bytes.size());
	} else if (!strcmp(cmd, "poke")) {
		if (!machine()) goto done;
		char* tok = next_tok(&cursor);
		uint32_t lin;
		int count = 0;
		if (!tok) {
			out.add("usage: poke <addr> <val...>\n");
			goto done;
		}
		if (eval_addr(d, out, tok, &lin) != 0) goto done;
		char* byte_tok;
		while ((byte_tok = next_tok(&cursor)) != nullptr) {
			if (eval_text(d, out, byte_tok, &aux) != 0) goto done;
			const uint8_t byte = (uint8_t)aux;
			msdosdebug_write(d, lin + (uint32_t)count, &byte, 1);
			count++;
		}
		out.emit("%d byte%s at %05X\n", count, count == 1 ? "" : "s", lin);
	} else if (!strcmp(cmd, "u")) {
		if (!machine()) goto done;
		uint16_t seg = haveUnasm ? unasmSeg : r.cs;
		uint32_t off = haveUnasm ? unasmOff : r.eip, lin;
		const std::string tok = peek_tok(cursor);
		if (!tok.empty() && tok != "l" && tok != "L" && !((tok[0] == 'l' || tok[0] == 'L') && is_hex(tok.c_str() + 1)) &&
		    msdosdebug_parse_address(d, tok.c_str(), &seg, &off, &lin) == 0) {
			if (tok.find(':') == std::string::npos) {
				seg = r.cs;
				off = lin - msdosdebug_linear(d, r.cs, 0);
			}
			next_tok(&cursor);
		}
		const int count = (int)std::min(lenArg(&cursor, 16), 64u);
		std::vector<msdos_dis_line> lines((size_t)count);
		const int n = msdosdebug_disassemble(d, seg, off, count, lines.data());
		for (int i = 0; i < n; i++) {
			const auto& l = lines[(size_t)i];
			if (l.symbol[0])
				out.add(std::string(l.symbol) + ":\n");
			out.emit("%04X:%04X %-16s %s\n", l.seg, l.off & 0xFFFF, l.bytes, l.text);
		}
		if (n) {
			unasmSeg = seg;
			unasmOff = lines[(size_t)n - 1].off + (uint32_t)lines[(size_t)n - 1].len;
			haveUnasm = true;
		}
	} else if (!strcmp(cmd, "disasm") || !strcmp(cmd, "dis")) {
		if (!machine()) goto done;
		char* addr_tok = next_tok(&cursor);
		char* count_tok = next_tok(&cursor);
		uint16_t seg = r.cs;
		uint32_t off = r.eip, lin;
		aux = 8;
		if (addr_tok) {
			if (eval_addr(d, out, addr_tok, &lin, &seg, &off) != 0) goto done;
			if (!strchr(addr_tok, ':')) {
				seg = r.cs;
				off = lin - msdosdebug_linear(d, r.cs, 0);
			}
		}
		if (count_tok && eval_text(d, out, count_tok, &aux) != 0) goto done;
		aux = std::max<int64_t>(1, std::min<int64_t>(aux, 64));
		std::vector<msdos_dis_line> lines((size_t)aux);
		const int n = msdosdebug_disassemble(d, seg, off, (int)aux, lines.data());
		for (int i = 0; i < n; i++) {
			const auto& l = lines[(size_t)i];
			out.emit("%c%04X:%04X  %-20s %s%s%s\n", l.linear == msdosdebug_linear(d, r.cs, r.eip) ? '>' : ' ',
			         l.seg, l.off & 0xFFFF, l.bytes, l.text, l.symbol[0] ? "   ; " : "", l.symbol);
		}
	} else if (!strcmp(cmd, "g") || !strcmp(cmd, "run") || !strcmp(cmd, "go") || !strcmp(cmd, "c")) {
		if (!machine()) goto done;
		uint32_t lin;
		char* tok = next_tok(&cursor);
		if (tok && msdosdebug_parse_address(d, tok, nullptr, nullptr, &lin) == 0)
			msdosdebug_run_to(d, lin);
		else
			msdosdebug_resume(d);
		out.add("Running\n");
	} else if (!strcmp(cmd, "t") || !strcmp(cmd, "step") || !strcmp(cmd, "s")) {
		if (!machine()) goto done;
		char* tok = next_tok(&cursor);
		int64_t count = 1;
		if (tok) {
			if (!strcmp(cmd, "t")) count = (int64_t)strtoul(tok, nullptr, 16);
			else if (eval_text(d, out, tok, &count) != 0) goto done;
		}
		count = std::max<int64_t>(1, std::min<int64_t>(count, 100000));
		if (!msdosdebug_stopped(d)) {
			out.add("Stop first\n");
		} else {
			const uint64_t before = msdosdebug_stop_serial(d);
			msdosdebug_step_n(d, (int)count);
			haveUnasm = false;
			if (!strcmp(cmd, "t") && msdosdebug_wait_stop(d, before, WaitMs))
				show_regs(d, out);
			else
				report_stop(d, out, before, "stepping...");
		}
	} else if (!strcmp(cmd, "p") || !strcmp(cmd, "over") || !strcmp(cmd, "n")) {
		if (!machine()) goto done;
		if (!msdosdebug_stopped(d)) {
			out.add("Stop first\n");
		} else {
			const uint64_t before = msdosdebug_stop_serial(d);
			msdosdebug_step_over(d);
			haveUnasm = false;
			if (!strcmp(cmd, "p") && msdosdebug_wait_stop(d, before, WaitMs))
				show_regs(d, out);
			else
				report_stop(d, out, before, "stepping over...");
		}
	} else if (!strcmp(cmd, "out")) {
		if (!machine()) goto done;
		if (!msdosdebug_stopped(d)) {
			out.add("Stop first\n");
		} else {
			const uint64_t before = msdosdebug_stop_serial(d);
			msdosdebug_step_out(d);
			report_stop(d, out, before, "stepping out...");
		}
	} else if (!strcmp(cmd, "stop")) {
		if (!machine()) goto done;
		const uint64_t before = msdosdebug_stop_serial(d);
		if (msdosdebug_stopped(d)) {
			where_text(d, out);
		} else {
			msdosdebug_stop(d);
			report_stop(d, out, before, "stopping...");
		}
	} else if (!strcmp(cmd, "runto")) {
		if (!machine()) goto done;
		char* tok = next_tok(&cursor);
		uint32_t lin;
		if (!tok) {
			out.add("usage: runto <addr>\n");
			goto done;
		}
		if (eval_addr(d, out, tok, &lin) != 0) goto done;
		const uint64_t before = msdosdebug_stop_serial(d);
		msdosdebug_run_to(d, lin);
		out.emit("running to %05X...\n", lin);
		if (msdosdebug_wait_stop(d, before, WaitMs)) where_text(d, out);
	} else if (!strcmp(cmd, "bp") || !strcmp(cmd, "break")) {
		add_bp(d, out, MSDOS_BP_EXEC, cursor);
	} else if (!strcmp(cmd, "bpr")) {
		add_bp(d, out, MSDOS_BP_READ, cursor);
	} else if (!strcmp(cmd, "bpw")) {
		add_bp(d, out, MSDOS_BP_WRITE, cursor);
	} else if (!strcmp(cmd, "bpa")) {
		add_bp(d, out, MSDOS_BP_ACCESS, cursor);
	} else if (!strcmp(cmd, "bpi") || !strcmp(cmd, "bpin")) {
		add_bp(d, out, MSDOS_BP_IN, cursor);
	} else if (!strcmp(cmd, "bpo") || !strcmp(cmd, "bpout")) {
		add_bp(d, out, MSDOS_BP_OUT, cursor);
	} else if (!strcmp(cmd, "bpio")) {
		add_bp(d, out, MSDOS_BP_IO, cursor);
	} else if (!strcmp(cmd, "bpint")) {
		char* n = next_tok(&cursor);
		if (!n) {
			out.add("usage: bpint n [ah] [cond]\n");
			goto done;
		}
		int ah = -1;
		const std::string second = peek_tok(cursor);
		if (is_hex(second.c_str()) && second.size() <= 2) {
			ah = (int)strtoul(second.c_str(), nullptr, 16);
			next_tok(&cursor);
		}
		char err[160];
		const int id = msdosdebug_bp_add_if(d, MSDOS_BP_INT, hexnum(n, 0), hexnum(n, 0), ah,
		                                     rest_of(cursor), err, sizeof err);
		if (id < 0) out.emit("error: %s\n", err);
		else out.emit("Breakpoint %d\n", id);
	} else if (!strcmp(cmd, "bl") || !strcmp(cmd, "breaks") || !strcmp(cmd, "hits")) {
		char* tok = next_tok(&cursor);
		if (!strcmp(cmd, "hits") && tok && !strcmp(tok, "clear")) {
			msdosdebug_bp_clear_hits(d);
			out.add("hit counts cleared\n");
		} else {
			list_breaks(d, out);
		}
	} else if (!strcmp(cmd, "bc") || !strcmp(cmd, "delete")) {
		char* tok = next_tok(&cursor);
		if (!tok) {
			out.emit("usage: %s <id|addr|*>\n", cmd);
			goto done;
		}
		if (!strcmp(tok, "*")) {
			msdosdebug_bp_clear(d);
			goto done;
		}
		if (tok[0] == '#') tok++;
		msdos_breakpoint list[MSDOS_MAX_BREAKPOINTS];
		const int count = msdosdebug_bp_list(d, list, MSDOS_MAX_BREAKPOINTS);
		int target = 0;
		if (!strcmp(cmd, "bc")) {
			target = atoi(tok);
		} else {
			if (eval_text(d, out, tok, &val) != 0) goto done;
			for (int i = 0; i < count && !target; i++)
				if (list[i].id == val) target = list[i].id;
			uint32_t at;
			if (!target && msdosdebug_parse_address(d, tok, nullptr, nullptr, &at) == 0)
				for (int i = 0; i < count && !target; i++)
					if (list[i].type == MSDOS_BP_EXEC && list[i].start == at) target = list[i].id;
		}
		if (target && msdosdebug_bp_remove(d, target) == 0)
			out.emit("deleted %d\n", target);
		else
			out.add("no such breakpoint\n");
	} else if (!strcmp(cmd, "enable") || !strcmp(cmd, "disable")) {
		char* tok = next_tok(&cursor);
		if (!tok) {
			out.emit("usage: %s <id>\n", cmd);
			goto done;
		}
		if (tok[0] == '#') tok++;
		const int id = atoi(tok);
		if (msdosdebug_bp_enable(d, id, !strcmp(cmd, "enable")) == 0)
			out.emit("%sd %d\n", cmd, id);
		else
			out.add("no such breakpoint\n");
	} else if (!strcmp(cmd, "clear")) {
		msdosdebug_bp_clear(d);
		out.add("all breakpoints cleared\n");
	} else if (!strcmp(cmd, "print")) {
		if (eval_text(d, out, rest_of(cursor), &val) == 0) {
			const bool wide = val > 0xFFFF || val < 0;
			char bin[65];
			const int bits = wide ? 32 : 16;
			for (int bit = 0; bit < bits; bit++)
				bin[bit] = (char)('0' + ((val >> (bits - 1 - bit)) & 1));
			bin[bits] = '\0';
			out.emit(wide ? "$%08llX  %lld  %%%s\n" : "$%04llX  %lld  %%%s\n",
			         (unsigned long long)val & (wide ? 0xFFFFFFFFull : 0xFFFFull), (long long)val, bin);
		}
	} else if (!strcmp(cmd, "set")) {
		char* name = next_tok(&cursor);
		char err[160];
		if (!name || !*rest_of(cursor)) {
			out.add("usage: set <reg> <expr>\n");
		} else if (eval_text(d, out, rest_of(cursor), &val) == 0) {
			if (!msdosdebug_register_set(d, name, val, err, sizeof err))
				out.emit("error: %s\n", err);
			else
				show_regs(d, out);
		}
	} else if (!strcmp(cmd, "i")) {
		if (!machine()) goto done;
		char* tok = next_tok(&cursor);
		uint8_t v = 0xFF;
		msdosdebug_io_in(d, (uint16_t)hexnum(tok, 0), &v);
		out.emit("%02X\n", v);
	} else if (!strcmp(cmd, "o")) {
		if (!machine()) goto done;
		char* port = next_tok(&cursor);
		char* value = next_tok(&cursor);
		msdosdebug_io_out(d, (uint16_t)hexnum(port, 0), (uint8_t)hexnum(value, 0));
	} else if (!strcmp(cmd, "label")) {
		char* addr_tok = next_tok(&cursor);
		char* name = next_tok(&cursor);
		uint32_t lin;
		if (!addr_tok || !name) {
			out.add("usage: label <addr> <name>\n");
			goto done;
		}
		if (eval_addr(d, out, addr_tok, &lin) != 0) goto done;
		if (msdosdebug_set_label(d, lin, name) == 0)
			out.emit("%05X = %s\n", lin, name);
		else
			out.add("bad label\n");
	} else if (!strcmp(cmd, "labels")) {
		char msg[256];
		char* file = rest_of(cursor);
		if (*file) {
			msdosdebug_load_symbols(d, file, 0, msg, sizeof msg);
			out.emit("%s\n", msg);
		} else {
			out.add("usage: labels <file>\n");
		}
	} else if (!strcmp(cmd, "symbols")) {
		int picks[40];
		const int count = msdosdebug_symbols_filter(d, rest_of(cursor), picks, 40);
		if (!count) out.add("no symbols match\n");
		for (int i = 0; i < count; i++) {
			uint32_t lin;
			char name[96];
			if (msdosdebug_symbols_get(d, picks[i], &lin, name, sizeof name))
				out.emit("%05X  %s\n", lin, name);
		}
	} else if (!strcmp(cmd, "trace") || !strcmp(cmd, "history")) {
		const bool hist = !strcmp(cmd, "history");
		char* tok = next_tok(&cursor);
		if (tok && !strcmp(tok, "on")) {
			if (hist) msdosdebug_history_enable(d, 1); else msdosdebug_trace_enable(d, 1);
			out.emit("%s on (the PC runs on the interpreter: slower)\n", cmd);
		} else if (tok && !strcmp(tok, "off")) {
			if (hist) msdosdebug_history_enable(d, 0); else msdosdebug_trace_enable(d, 0);
			out.emit("%s off\n", cmd);
		} else if (tok && !strcmp(tok, "clear")) {
			msdosdebug_trace_clear(d);
		} else {
			std::vector<msdos_trace_entry> rows(64);
			aux = 16;
			if (tok && eval_text(d, out, tok, &aux) != 0) goto done;
			aux = std::max<int64_t>(1, std::min<int64_t>(aux, 64));
			const int count = hist ? msdosdebug_history_copy(d, rows.data(), (int)aux)
			                       : msdosdebug_trace_read(d, rows.data(), (int)aux);
			if (!count)
				out.emit("trace is %s and empty\n",
				         msdosdebug_trace_enabled(d) || msdosdebug_history_enabled(d) ? "on" : "off");
			// trace: newest first as read; shown oldest first like a listing
			for (int i = hist ? 0 : count - 1; hist ? i < count : i >= 0; hist ? i++ : i--) {
				char text[400];
				msdosdebug_trace_format(&rows[(size_t)i], text, sizeof text);
				out.emit("%s\n", text);
			}
		}
	} else if (!strcmp(cmd, "watch")) {
		char* text = rest_of(cursor);
		if (*text && msdoswatch_add(text) < 0) out.add("no room for another watch\n");
		if (!msdoswatch_count()) out.add("no watches (watch <addr|symbol|lo-hi>)\n");
		for (int i = 0; i < msdoswatch_count(); i++) {
			char row[200];
			msdoswatch_format(d, i, row, sizeof row);
			out.emit("%2d  %s\n", i, row);
		}
	} else if (!strcmp(cmd, "unwatch")) {
		char* tok = next_tok(&cursor);
		if (!tok) out.add("unwatch <n|all>\n");
		else if (!strcmp(tok, "all")) msdoswatch_clear();
		else if (eval_text(d, out, tok, &val) == 0) msdoswatch_remove((int)val);
	} else if (!strcmp(cmd, "find") || !strcmp(cmd, "f")) {
		char* tok = next_tok(&cursor);
		char joined[300];
		int kind = MSDOSSEARCH_HEX, count = 0, flags = 0;
		uint8_t pat[MSDOSSEARCH_MAX], mask;
		uint32_t hits[32];
		if (tok && !strcmp(tok, "text")) {
			kind = MSDOSSEARCH_TEXT;
			tok = rest_of(cursor);
		} else if (tok && !strcmp(tok, "hex")) {
			tok = rest_of(cursor);
		} else if (tok) {
			snprintf(joined, sizeof joined, "%s %s", tok, rest_of(cursor));
			tok = joined;
		}
		if (!tok || !*tok || !msdossearch_parse(kind, tok, pat, &count, &mask, &flags)) {
			out.add("find [hex] <bytes> | find text <string>\n");
			goto done;
		}
		if (!machine()) goto done;
		const int found = msdosdebug_search(d, pat, count, mask, flags, 0, 0xFFFFFFFFu, hits, 32);
		if (found < 0) out.add("search failed\n");
		else if (!found) out.add("not found\n");
		for (int i = 0; i < found; i++) {
			char sym[64] = "";
			msdosdebug_symbol_at(d, hits[i], sym, sizeof sym);
			out.emit("%05X%s%s\n", hits[i], sym[0] ? "  " : "", sym);
		}
	} else if (!strcmp(cmd, "map")) {
		msdos_region map[12];
		const int count = msdosdebug_memory_map(d, map, 12);
		if (!count) out.add("The PC is off\n");
		for (int i = 0; i < count; i++)
			out.emit("%06X-%06X  %s%s%s\n", map[i].low, map[i].high, map[i].name,
			         map[i].readable ? "" : " (no read)", map[i].writable ? "" : " (read only)");
	} else if (!strcmp(cmd, "io") || !strcmp(cmd, "machine")) {
		char text[10000];
		msdosdebug_io_text(d, text, sizeof text);
		out.add(text);
	} else if (!strcmp(cmd, "sound")) {
		char text[8000];
		msdosdebug_sound_text(d, text, sizeof text);
		out.add(text);
	} else if (!strcmp(cmd, "save")) {
		char* kind = next_tok(&cursor);
		char* file = rest_of(cursor);
		char msg[300];
		if (!kind || !*file) {
			out.add("usage: save <dis|mem|regs> <file>\n");
		} else {
			msdosdebug_save(d, kind, file, msg, sizeof msg);
			out.emit("%s\n", msg);
		}
	} else if (!strcmp(cmd, "video")) {
		if (!machine()) goto done;
		char* section = next_tok(&cursor);
		msdos_vid_snap snap;
		memset(&snap, 0, sizeof snap);
		if (msdosdebug_video_snap(d, &snap) != 0) {
			out.add("The PC is off\n");
		} else {
			std::vector<char> buf(16384);
			msdosvid_text(&snap, section ? section : "", buf.data(), (int)buf.size());
			out.add(buf.data());
		}
		msdos_vid_snap_free(&snap);
	} else {
		out.add("Unknown command (h for help)\n");
	}
done:
	snprintf(dst, (size_t)dstsz, "%s", out.text.c_str());
	return rc;
}

int msdosdebug_completions(msdosdebug* d, const char* prefix, char* dst, int dstsz)
{
	std::string out;
	int count = 0;
	if (!dst || dstsz <= 0)
		return 0;
	dst[0] = 0;
	if (!d || !prefix)
		return 0;
	const char* word = strrchr(prefix, ' ');
	word = word ? word + 1 : prefix;
	const size_t len = strlen(word);
	auto add = [&](const std::string& s) {
		out += s + "\n";
		count++;
	};
	if (word == prefix) {
		for (int i = 0; kCommands[i]; i++)
			if (!strncmp(kCommands[i], word, len))
				add(kCommands[i]);
	}
	if (!strncmp(prefix, "save ", 5) && !strchr(prefix + 5, ' ')) {
		for (const char* k : { "dis", "mem", "regs" })
			if (!strncmp(k, word, len))
				add(k);
	} else if (len) {
		if (word != prefix)
			for (int i = 0; msdosdebug_reg_names[i]; i++)
				if (!strncmp(msdosdebug_reg_names[i], word, len))
					add(msdosdebug_reg_names[i]);
		const int total = msdosdebug_symbols_count(d);
		for (int i = 0; i < total && count < 40; i++) {
			char name[96];
			uint32_t lin;
			if (msdosdebug_symbols_get(d, i, &lin, name, sizeof name) && !strncasecmp(name, word, len))
				add(name);
		}
	}
	snprintf(dst, (size_t)dstsz, "%s", out.c_str());
	return count;
}
