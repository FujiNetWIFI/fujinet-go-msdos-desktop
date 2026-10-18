/*
 * msdosdebug -- see msdosdebug.h.
 *
 * The engine's state belongs to the emulation thread: the public calls run
 * as host jobs (Kind::Any, so they also run while the machine is stopped),
 * and the hooks 86Box calls per instruction, memory access and I/O access
 * (host_debug.c) read it without locks. Only the trace buffers, filled from
 * the CPU thread and read by windows, have a mutex.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <Zydis.h>

#include "session_impl.h"
#include "msdosdebug.h"

extern "C" {
#include "debug86.h"
#include "msdos_host.h"
#include <86box/86box.h>
#include <86box/io.h>
typedef void (*char_fujinet_trace_t)(int dir, const uint8_t* buf, size_t len);
void char_fujinet_set_trace(char_fujinet_trace_t cb);
}

#include "fuji_names.h"

namespace {

constexpr uint32_t NoAddr = 0xFFFFFFFFu;
constexpr size_t IntTraceMax = 512;
constexpr size_t FujiTraceMax = 512;

enum class Mode { Run, Step, Out, RunTo };

const char* fuji_command_name(int id)
{
	for (const auto& c : fuji_command_names)
		if (c.id == id)
			return c.name;
	return "";
}

std::string hex(uint32_t v, int width)
{
	char b[16];
	snprintf(b, sizeof b, "%0*X", width, v);
	return b;
}

void put(char* out, int outsz, const std::string& s)
{
	if (out && outsz > 0)
		snprintf(out, (size_t)outsz, "%s", s.c_str());
}

// What DOS and the BIOS calls are, for the trace.
std::string int_text(uint8_t n, uint16_t ax)
{
	const uint8_t ah = ax >> 8;
	char b[96];
	const char* what = "";
	if (n == 0x21) {
		static const struct { uint8_t f; const char* s; } dos[] = {
			{ 0x00, "terminate" }, { 0x01, "read key with echo" }, { 0x02, "write character" },
			{ 0x06, "console I/O" }, { 0x07, "read key" }, { 0x08, "read key" },
			{ 0x09, "print string" }, { 0x0A, "buffered input" }, { 0x0B, "key ready?" },
			{ 0x0E, "select drive" }, { 0x19, "current drive" }, { 0x1A, "set DTA" },
			{ 0x25, "set vector" }, { 0x2A, "get date" }, { 0x2C, "get time" },
			{ 0x30, "DOS version" }, { 0x31, "stay resident" }, { 0x35, "get vector" },
			{ 0x36, "free space" }, { 0x39, "make directory" }, { 0x3A, "remove directory" },
			{ 0x3B, "change directory" }, { 0x3C, "create" }, { 0x3D, "open" }, { 0x3E, "close" },
			{ 0x3F, "read" }, { 0x40, "write" }, { 0x41, "delete" }, { 0x42, "seek" },
			{ 0x43, "attributes" }, { 0x44, "IOCTL" }, { 0x47, "current directory" },
			{ 0x48, "allocate memory" }, { 0x49, "free memory" }, { 0x4A, "resize memory" },
			{ 0x4B, "exec" }, { 0x4C, "exit" }, { 0x4E, "find first" }, { 0x4F, "find next" },
			{ 0x56, "rename" }, { 0x62, "get PSP" } };
		for (const auto& d : dos)
			if (d.f == ah)
				what = d.s;
		snprintf(b, sizeof b, "INT 21h AH=%02Xh %s", ah, what);
		return b;
	}
	switch (n) {
	case 0x10: what = "video"; break;
	case 0x13: what = "disk"; break;
	case 0x14: what = "serial"; break;
	case 0x16: what = "keyboard"; break;
	case 0x17: what = "printer"; break;
	case 0x1A: what = "clock"; break;
	case 0x2F: what = "multiplex"; break;
	case 0x33: what = "mouse"; break;
	case 0xF5: what = "FujiNet"; break;
	default: break;
	}
	snprintf(b, sizeof b, "INT %02Xh AH=%02Xh%s%s", n, ah, *what ? " " : "", what);
	return b;
}

} // namespace

struct msdosdebug
{
	msdossession* s = nullptr;

	// ---- emulation thread
	bool attached = false;
	std::vector<msdos_breakpoint> bps;
	int nextId = 1;
	std::unordered_set<uint32_t> execAt;        // enabled single-address execute bps
	bool execRanges = false, intBps = false;
	Mode mode = Mode::Run;
	int stepLeft = 0;
	uint32_t tempBp = NoAddr;
	uint32_t outSp = 0;
	uint8_t prevOp = 0;
	uint32_t skipLinear = NoAddr;
	bool pendingStop = false;
	std::string pendingReason;
	bool intTrace = false;

	// ---- any thread
	std::atomic<unsigned> generation { 0 };
	std::mutex reasonMtx;
	std::string reason;
	std::mutex traceMtx;
	std::deque<msdos_int_event> ints;
	uint64_t intSerial = 0;
	std::deque<msdos_fuji_packet> fuji;
	uint64_t fujiSerial = 0;
	std::vector<uint8_t> slip[2];
	bool slipEsc[2] = { false, false };
	std::mutex symMtx;
	std::map<uint32_t, std::string> syms;

	PcHost& Host() { return Impl(s)->host; }
	bool Run(const std::function<void()>& fn)
	{
		return s->running && Host().Call(fn, PcHost::Kind::Any);
	}
	void SetReason(const std::string& r)
	{
		std::lock_guard<std::mutex> lock(reasonMtx);
		reason = r;
	}

	// emulation thread
	int Instruction(bool atSliceStart);
	void MemAccess(const uint32_t* addrs, int access);
	void IoAccess(uint16_t port, bool write, int width, uint32_t val);
	int Stop(const std::string& why);
	void Rebuild();
	void ResumeFrom();
	uint8_t OpcodeAt(uint32_t linear, int* prefixes, uint8_t* next);
	int InstructionLength(uint32_t linear, uint32_t off);
	std::string Where(uint32_t linear);
};

namespace {

msdosdebug* gDebug = nullptr;     // the one the hooks talk to

int HookInstruction(void* user, int atSliceStart)
{
	return static_cast<msdosdebug*>(user)->Instruction(atSliceStart != 0);
}

void HookMem(void* user, const uint32_t* addrs, int access)
{
	static_cast<msdosdebug*>(user)->MemAccess(addrs, access);
}

void HookIo(void* user, uint16_t port, int write, int width, uint32_t val)
{
	static_cast<msdosdebug*>(user)->IoAccess(port, write != 0, width, val);
}

// FujiNet's bytes, both ways: SLIP frames into FujiBus packets.
void FujiTrace(int dir, const uint8_t* buf, size_t len)
{
	msdosdebug* d = gDebug;
	if (!d)
		return;
	std::lock_guard<std::mutex> lock(d->traceMtx);
	const int to_pc = dir == 0 ? 1 : 0;
	std::vector<uint8_t>& acc = d->slip[to_pc];
	for (size_t i = 0; i < len; i++) {
		const uint8_t b = buf[i];
		if (b == 0xC0) {
			if (!acc.empty()) {
				msdos_fuji_packet p = {};
				p.serial = ++d->fujiSerial;
				p.to_pc = to_pc;
				p.nbytes = (int)acc.size();
				memcpy(p.data, acc.data(), std::min(acc.size(), sizeof p.data));
				p.device = acc.size() >= 6 ? acc[0] : -1;
				p.command = acc.size() >= 6 ? acc[1] : -1;
				p.length = acc.size() >= 6 ? (acc[2] | (acc[3] << 8)) : -1;
				if (p.device >= 0)
					snprintf(p.text, sizeof p.text, "%s %02X %02X %s (%d bytes)",
					         to_pc ? "FujiNet \xe2\x86\x92 PC" : "PC \xe2\x86\x92 FujiNet",
					         p.device, p.command, to_pc ? "" : fuji_command_name(p.command), p.nbytes);
				else
					snprintf(p.text, sizeof p.text, "%s %d bytes",
					         to_pc ? "FujiNet \xe2\x86\x92 PC" : "PC \xe2\x86\x92 FujiNet", p.nbytes);
				d->fuji.push_back(p);
				if (d->fuji.size() > FujiTraceMax)
					d->fuji.pop_front();
				acc.clear();
			}
			d->slipEsc[to_pc] = false;
			continue;
		}
		if (d->slipEsc[to_pc]) {
			acc.push_back(b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b);
			d->slipEsc[to_pc] = false;
		} else if (b == 0xDB) {
			d->slipEsc[to_pc] = true;
		} else if (acc.size() < 4096) {
			acc.push_back(b);
		}
	}
}

} // namespace

// ---------------------------------------------------------------------------
// the hooks (emulation thread)
// ---------------------------------------------------------------------------

uint8_t msdosdebug::OpcodeAt(uint32_t linear, int* prefixes, uint8_t* next)
{
	uint8_t b[8];
	d86_read(linear, b, sizeof b);
	int i = 0;
	while (i < 4 && (b[i] == 0x26 || b[i] == 0x2E || b[i] == 0x36 || b[i] == 0x3E || b[i] == 0x64
	                 || b[i] == 0x65 || b[i] == 0x66 || b[i] == 0x67 || b[i] == 0xF0
	                 || b[i] == 0xF2 || b[i] == 0xF3))
		i++;
	if (prefixes)
		*prefixes = i;
	if (next)
		*next = b[i + 1];
	return b[i];
}

std::string msdosdebug::Where(uint32_t linear)
{
	uint16_t seg;
	uint32_t off;
	uint32_t at = d86_cs_ip(&seg, &off);
	if (at == linear)
		return hex(seg, 4) + ":" + hex(off, d86_code32() ? 8 : 4);
	return hex(linear, 5);
}

int msdosdebug::Stop(const std::string& why)
{
	mode = Mode::Run;
	tempBp = NoAddr;
	stepLeft = 0;
	SetReason(why);
	generation.fetch_add(1);
	return 1;
}

int msdosdebug::Instruction(bool atSliceStart)
{
	uint16_t seg;
	uint32_t off;
	const uint32_t lin = d86_cs_ip(&seg, &off);

	if (pendingStop) {
		pendingStop = false;
		return Stop(pendingReason);
	}
	const bool skip = lin == skipLinear;
	if (skip)
		skipLinear = NoAddr;

	if (!atSliceStart) {
		if (mode == Mode::Step && --stepLeft <= 0)
			return Stop("Step");
		if (mode == Mode::Out) {
			const bool wasRet = prevOp == 0xC3 || prevOp == 0xC2 || prevOp == 0xCB || prevOp == 0xCA
			                    || prevOp == 0xCF;
			if (wasRet && d86_stack_pointer() > outSp)
				return Stop("Returned");
		}
	}
	if (!skip) {
		if (mode == Mode::RunTo && lin == tempBp)
			return Stop("Reached " + Where(lin));
		if (execAt.count(lin) || execRanges) {
			for (auto& b : bps)
				if (b.enabled && b.type == MSDOS_BP_EXEC && lin >= b.start && lin <= b.end) {
					b.hits++;
					return Stop("Breakpoint " + std::to_string(b.id) + " at " + Where(lin));
				}
		}
	}

	if (intBps || intTrace || mode == Mode::Out) {
		int pre;
		uint8_t next;
		const uint8_t op = OpcodeAt(lin, &pre, &next);
		prevOp = op;
		if (op == 0xCD || op == 0xCC || op == 0xCE) {
			const uint8_t n = op == 0xCD ? next : op == 0xCC ? 3 : 4;
			uint16_t ax, bx, cx, dx, ds, es;
			d86_int_regs(&ax, &bx, &cx, &dx, &ds, &es);
			if (intTrace) {
				msdos_int_event e = {};
				e.num = n;
				e.ax = ax; e.bx = bx; e.cx = cx; e.dx = dx; e.ds = ds; e.es = es;
				e.cs = seg;
				e.ip = off;
				snprintf(e.text, sizeof e.text, "%s", int_text(n, ax).c_str());
				std::lock_guard<std::mutex> lock(traceMtx);
				e.serial = ++intSerial;
				ints.push_back(e);
				if (ints.size() > IntTraceMax)
					ints.pop_front();
			}
			if (intBps && !skip)
				for (auto& b : bps)
					if (b.enabled && b.type == MSDOS_BP_INT && b.start == n
					    && (b.ah < 0 || b.ah == (ax >> 8))) {
						b.hits++;
						return Stop("Breakpoint " + std::to_string(b.id) + ": " + int_text(n, ax));
					}
		}
	}
	return 0;
}

void msdosdebug::MemAccess(const uint32_t* addrs, int access)
{
	const int width = access & 15;
	const bool write = (access & 16) != 0;
	for (auto& b : bps) {
		if (!b.enabled)
			continue;
		const bool kind = b.type == MSDOS_BP_ACCESS || (write ? b.type == MSDOS_BP_WRITE
		                                                         : b.type == MSDOS_BP_READ);
		if (!kind)
			continue;
		for (int i = 0; i < width; i++)
			if (addrs[i] >= b.start && addrs[i] <= b.end) {
				b.hits++;
				pendingStop = true;
				pendingReason = "Breakpoint " + std::to_string(b.id) + ": " +
				                (write ? "write to " : "read of ") + hex(addrs[i], 5);
				return;
			}
	}
}

void msdosdebug::IoAccess(uint16_t port, bool write, int width, uint32_t val)
{
	for (auto& b : bps) {
		if (!b.enabled || port < b.start || port > b.end)
			continue;
		if (b.type == MSDOS_BP_IO || (write ? b.type == MSDOS_BP_OUT : b.type == MSDOS_BP_IN)) {
			b.hits++;
			pendingStop = true;
			pendingReason = "Breakpoint " + std::to_string(b.id) + ": " + (write ? "OUT " : "IN ") +
			                hex(port, 4) + (write ? " = " : " \xe2\x86\x92 ") + hex(val, width * 2);
			return;
		}
	}
}

// The watch bitmaps and the fast sets, from the breakpoint list.
void msdosdebug::Rebuild()
{
	msdos_host_debug_watch_clear();
	execAt.clear();
	execRanges = intBps = false;
	for (const auto& b : bps) {
		if (!b.enabled)
			continue;
		switch (b.type) {
		case MSDOS_BP_EXEC:
			if (b.start == b.end)
				execAt.insert(b.start);
			else
				execRanges = true;
			break;
		case MSDOS_BP_READ:
		case MSDOS_BP_WRITE:
		case MSDOS_BP_ACCESS:
			for (uint64_t a = b.start & ~0xFFFull; a <= b.end; a += 0x1000)
				msdos_host_debug_watch_page((uint32_t)a, 1);
			break;
		case MSDOS_BP_IN:
		case MSDOS_BP_OUT:
		case MSDOS_BP_IO:
			for (uint32_t p = b.start; p <= b.end && p < 65536; p++)
				msdos_host_debug_watch_io((uint16_t)p, 1);
			break;
		case MSDOS_BP_INT:
			intBps = true;
			break;
		default:
			break;
		}
	}
}

// Leaving a stop: the instruction it stopped at runs before any breakpoint
// there can fire again.
void msdosdebug::ResumeFrom()
{
	skipLinear = d86_cs_ip(nullptr, nullptr);
	pendingStop = false;
}

int msdosdebug::InstructionLength(uint32_t linear, uint32_t off)
{
	uint8_t b[16];
	d86_read(linear, b, sizeof b);
	ZydisDecoder dec;
	const bool c32 = d86_code32() != 0;
	ZydisDecoderInit(&dec, c32 ? ZYDIS_MACHINE_MODE_LEGACY_32 : ZYDIS_MACHINE_MODE_LEGACY_16,
	                 c32 ? ZYDIS_STACK_WIDTH_32 : ZYDIS_STACK_WIDTH_16);
	ZydisDecodedInstruction ins;
	if (ZYAN_FAILED(ZydisDecoderDecodeInstruction(&dec, nullptr, b, sizeof b, &ins)))
		return 1;
	(void)off;
	return ins.length;
}

// ---------------------------------------------------------------------------
// the session's side
// ---------------------------------------------------------------------------

msdosdebug* msdossession_debugger(msdossession* s)
{
	if (!s->debugger) {
		auto* d = new msdosdebug;
		d->s = s;
		s->debugger = d;
	}
	return static_cast<msdosdebug*>(s->debugger);
}

void session_debug_free(struct msdossession* s)
{
	auto* d = static_cast<msdosdebug*>(s->debugger);
	if (!d)
		return;
	if (gDebug == d) {
		char_fujinet_set_trace(nullptr);
		gDebug = nullptr;
	}
	delete d;
	s->debugger = nullptr;
}

void session_debug_stopped(struct msdossession* s)
{
	if (s->debugger)
		static_cast<msdosdebug*>(s->debugger)->generation.fetch_add(1);
}

// The FujiNet trace is always on: it is cheap, and it is most useful about
// what happened before anyone thought to look.
void session_debug_trace_install(struct msdossession* s)
{
	gDebug = msdossession_debugger(s);
	char_fujinet_set_trace(FujiTrace);
}

// ---------------------------------------------------------------------------
// control
// ---------------------------------------------------------------------------

int msdosdebug_attach(msdosdebug* d)
{
	if (!d->s->running)
		return -1;
	d->Run([d] {
		if (!d->attached) {
			msdos_host_debug_hooks h = {};
			h.user = d;
			h.instruction = HookInstruction;
			h.mem_access = HookMem;
			h.io_access = HookIo;
			msdos_host_debug_install(&h);
			d86_force_interpreter(1);
			d->attached = true;
			d->Rebuild();
		}
		d->Stop("Stopped");
		msdos_host_debug_set_stopped(1);
	});
	return 0;
}

void msdosdebug_detach(msdosdebug* d)
{
	d->Run([d] {
		if (!d->attached)
			return;
		d->attached = false;
		d->mode = Mode::Run;
		msdos_host_debug_install(nullptr);
		d86_force_interpreter(0);
		d->ResumeFrom();
	});
	d->Host().Resume();
	d->generation.fetch_add(1);
}

int msdosdebug_attached(msdosdebug* d)
{
	int a = 0;
	d->Run([&] { a = d->attached; });
	return a;
}

void msdosdebug_stop(msdosdebug* d)
{
	d->Run([d] {
		d->Stop("Stopped");
		msdos_host_debug_set_stopped(1);
	});
}

void msdosdebug_resume(msdosdebug* d)
{
	d->Run([d] {
		d->mode = Mode::Run;
		d->ResumeFrom();
	});
	d->Host().Resume();
	d->generation.fetch_add(1);
}

int msdosdebug_stopped(msdosdebug* d)
{
	return d->s->running && d->Host().Stopped() ? 1 : 0;
}

int msdosdebug_step(msdosdebug* d)
{
	if (!msdosdebug_stopped(d))
		return -1;
	d->Run([d] {
		d->mode = Mode::Step;
		d->stepLeft = 1;
		d->ResumeFrom();
	});
	d->Host().Resume();
	return 0;
}

int msdosdebug_step_over(msdosdebug* d)
{
	if (!msdosdebug_stopped(d))
		return -1;
	bool over = false;
	d->Run([&] {
		uint16_t seg;
		uint32_t off;
		const uint32_t lin = d86_cs_ip(&seg, &off);
		int pre;
		uint8_t next;
		const uint8_t op = d->OpcodeAt(lin, &pre, &next);
		uint8_t b[4];
		d86_read(lin, b, pre > 0 ? 1 : 0);
		const bool rep = pre > 0 && (b[0] == 0xF2 || b[0] == 0xF3);
		const bool call = op == 0xE8 || op == 0x9A || (op == 0xFF && ((next >> 3) & 7) >= 2
		                                                 && ((next >> 3) & 7) <= 3);
		over = call || op == 0xCD || op == 0xCC || op == 0xCE || (op >= 0xE0 && op <= 0xE2) || rep;
		if (over) {
			d->tempBp = lin + (uint32_t)d->InstructionLength(lin, off);
			d->mode = Mode::RunTo;
		} else {
			d->mode = Mode::Step;
			d->stepLeft = 1;
		}
		d->ResumeFrom();
	});
	d->Host().Resume();
	return 0;
}

int msdosdebug_step_out(msdosdebug* d)
{
	if (!msdosdebug_stopped(d))
		return -1;
	d->Run([d] {
		d->mode = Mode::Out;
		d->outSp = d86_stack_pointer();
		d->prevOp = 0;
		d->ResumeFrom();
	});
	d->Host().Resume();
	return 0;
}

int msdosdebug_run_to(msdosdebug* d, uint32_t linear)
{
	d->Run([d, linear] {
		d->tempBp = linear;
		d->mode = Mode::RunTo;
		d->ResumeFrom();
	});
	d->Host().Resume();
	return 0;
}

unsigned msdosdebug_generation(msdosdebug* d)
{
	return d->generation.load();
}

int msdosdebug_stop_reason(msdosdebug* d, char* dst, int dstsz)
{
	std::lock_guard<std::mutex> lock(d->reasonMtx);
	return snprintf(dst, (size_t)dstsz, "%s", d->reason.c_str());
}

// ---------------------------------------------------------------------------
// registers and memory
// ---------------------------------------------------------------------------

int msdosdebug_regs(msdosdebug* d, msdos_cpu_regs* r)
{
	return d->Run([r] { d86_regs(r); }) ? 0 : -1;
}

int msdosdebug_set_reg(msdosdebug* d, const char* name, uint32_t value)
{
	std::string n = name;
	for (auto& c : n)
		c = (char)tolower((unsigned char)c);
	int rc = -1;
	d->Run([&] { rc = d86_set_reg(n.c_str(), value); });
	if (rc == 0)
		d->generation.fetch_add(1);
	return rc;
}

int msdosdebug_read(msdosdebug* d, uint32_t linear, uint8_t* buf, int n)
{
	int got = 0;
	if (!d->Run([&] { got = d86_read(linear, buf, n); }))
		memset(buf, 0xFF, (size_t)n);
	return got;
}

int msdosdebug_write(msdosdebug* d, uint32_t linear, const uint8_t* buf, int n)
{
	int put_ = 0;
	d->Run([&] { put_ = d86_write(linear, buf, n); });
	d->generation.fetch_add(1);
	return put_;
}

uint32_t msdosdebug_linear(msdosdebug* d, uint16_t seg, uint32_t off)
{
	uint32_t base = (uint32_t)seg << 4;
	d->Run([&] { base = d86_seg_base(seg); });
	return base + off;
}

int msdosdebug_parse_address(msdosdebug* d, const char* text, uint16_t* seg, uint32_t* off,
                             uint32_t* linear)
{
	std::string t = text;
	t.erase(0, t.find_first_not_of(" \t"));
	t.erase(t.find_last_not_of(" \t") + 1);
	if (t.empty())
		return -1;
	msdos_cpu_regs r;
	if (msdosdebug_regs(d, &r) != 0)
		return -1;
	auto reg = [&](std::string n, uint32_t& v) -> bool {
		for (auto& c : n)
			c = (char)tolower((unsigned char)c);
		const struct { const char* n; uint32_t v; } regs[] = {
			{ "cs", r.cs }, { "ds", r.ds }, { "es", r.es }, { "ss", r.ss }, { "fs", r.fs },
			{ "gs", r.gs }, { "ip", r.eip & 0xFFFF }, { "eip", r.eip }, { "ax", r.eax & 0xFFFF },
			{ "bx", r.ebx & 0xFFFF }, { "cx", r.ecx & 0xFFFF }, { "dx", r.edx & 0xFFFF },
			{ "si", r.esi & 0xFFFF }, { "di", r.edi & 0xFFFF }, { "bp", r.ebp & 0xFFFF },
			{ "sp", r.esp & 0xFFFF }, { "esi", r.esi }, { "edi", r.edi }, { "ebp", r.ebp },
			{ "esp", r.esp } };
		for (const auto& x : regs)
			if (n == x.n) {
				v = x.v;
				return true;
			}
		char* end;
		v = (uint32_t)strtoul(n.c_str(), &end, 16);
		return !n.empty() && *end == 0;
	};
	size_t colon = t.find(':');
	if (colon != std::string::npos) {
		uint32_t sv, ov;
		if (!reg(t.substr(0, colon), sv) || !reg(t.substr(colon + 1), ov))
			return -1;
		if (seg) *seg = (uint16_t)sv;
		if (off) *off = ov;
		if (linear) *linear = msdosdebug_linear(d, (uint16_t)sv, ov);
		return 0;
	}
	uint32_t lin;
	if (msdosdebug_symbol_find(d, t.c_str(), &lin) == 0 || reg(t, lin)) {
		if (seg) *seg = (uint16_t)(lin >> 4);
		if (off) *off = lin & 0xF;
		if (linear) *linear = lin;
		return 0;
	}
	return -1;
}

// ---------------------------------------------------------------------------
// disassembly
// ---------------------------------------------------------------------------

int msdosdebug_disassemble(msdosdebug* d, uint16_t seg, uint32_t off, int count, msdos_dis_line* out)
{
	int n = 0;
	d->Run([&] {
		uint16_t cseg;
		d86_cs_ip(&cseg, nullptr);
		const bool c32 = seg == cseg && d86_code32();
		const uint32_t base = d86_seg_base(seg);
		ZydisDecoder dec;
		ZydisDecoderInit(&dec, c32 ? ZYDIS_MACHINE_MODE_LEGACY_32 : ZYDIS_MACHINE_MODE_LEGACY_16,
		                 c32 ? ZYDIS_STACK_WIDTH_32 : ZYDIS_STACK_WIDTH_16);
		ZydisFormatter fmt;
		ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);
		ZydisFormatterSetProperty(&fmt, ZYDIS_FORMATTER_PROP_FORCE_SEGMENT, ZYAN_FALSE);
		uint32_t o = off;
		for (; n < count; n++) {
			msdos_dis_line& l = out[n];
			memset(&l, 0, sizeof l);
			l.seg = seg;
			l.off = o;
			l.linear = base + o;
			uint8_t b[16];
			d86_read(l.linear, b, sizeof b);
			ZydisDecodedInstruction ins;
			ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
			if (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, b, sizeof b, &ins, ops))) {
				l.len = ins.length;
				ZydisFormatterFormatInstruction(&fmt, &ins, ops, ins.operand_count_visible, l.text,
				                                sizeof l.text, o, nullptr);
			} else {
				l.len = 1;
				snprintf(l.text, sizeof l.text, "db 0x%02X", b[0]);
			}
			int p = 0;
			for (int i = 0; i < l.len && p < (int)sizeof l.bytes - 4; i++)
				p += snprintf(l.bytes + p, sizeof l.bytes - (size_t)p, "%s%02X", i ? " " : "", b[i]);
			{
				std::lock_guard<std::mutex> lock(d->symMtx);
				auto it = d->syms.find(l.linear);
				if (it != d->syms.end())
					snprintf(l.symbol, sizeof l.symbol, "%s", it->second.c_str());
			}
			o += (uint32_t)l.len;
			if (!c32)
				o &= 0xFFFF;
		}
	});
	return n;
}

// ---------------------------------------------------------------------------
// breakpoints
// ---------------------------------------------------------------------------

const char* msdosdebug_bp_type_name(int type)
{
	static const char* const names[MSDOS_BP_TYPES] = { "Execute", "Read", "Write", "Access",
	                                                   "IN", "OUT", "IN/OUT", "INT" };
	return (type >= 0 && type < MSDOS_BP_TYPES) ? names[type] : "";
}

int msdosdebug_bp_add(msdosdebug* d, int type, uint32_t start, uint32_t end, int ah)
{
	if (type < 0 || type >= MSDOS_BP_TYPES)
		return -1;
	if (end < start)
		end = start;
	int id = -1;
	d->Run([&] {
		msdos_breakpoint b = {};
		b.id = d->nextId++;
		b.type = type;
		b.start = start;
		b.end = end;
		b.ah = type == MSDOS_BP_INT ? ah : -1;
		b.enabled = 1;
		std::string t = msdosdebug_bp_type_name(type);
		if (type == MSDOS_BP_INT)
			t += " " + hex(start, 2) + "h" + (ah >= 0 ? " AH=" + hex((uint32_t)ah, 2) + "h" : "");
		else if (type >= MSDOS_BP_IN)
			t += " " + hex(start, 4) + (end != start ? "-" + hex(end, 4) : "");
		else
			t += " " + hex(start, 5) + (end != start ? "-" + hex(end, 5) : "");
		snprintf(b.text, sizeof b.text, "%s", t.c_str());
		d->bps.push_back(b);
		d->Rebuild();
		id = b.id;
	});
	d->generation.fetch_add(1);
	return id;
}

int msdosdebug_bp_remove(msdosdebug* d, int id)
{
	int rc = -1;
	d->Run([&] {
		auto it = std::remove_if(d->bps.begin(), d->bps.end(),
		                         [id](const msdos_breakpoint& b) { return b.id == id; });
		rc = it != d->bps.end() ? 0 : -1;
		d->bps.erase(it, d->bps.end());
		d->Rebuild();
	});
	d->generation.fetch_add(1);
	return rc;
}

int msdosdebug_bp_enable(msdosdebug* d, int id, int on)
{
	int rc = -1;
	d->Run([&] {
		for (auto& b : d->bps)
			if (b.id == id) {
				b.enabled = on ? 1 : 0;
				rc = 0;
			}
		d->Rebuild();
	});
	d->generation.fetch_add(1);
	return rc;
}

int msdosdebug_bp_list(msdosdebug* d, msdos_breakpoint* out, int max)
{
	int n = 0;
	d->Run([&] {
		for (const auto& b : d->bps)
			if (n < max)
				out[n++] = b;
	});
	return n;
}

void msdosdebug_bp_clear(msdosdebug* d)
{
	d->Run([d] {
		d->bps.clear();
		d->Rebuild();
	});
	d->generation.fetch_add(1);
}

int msdosdebug_bp_at(msdosdebug* d, uint32_t linear)
{
	int id = 0;
	d->Run([&] {
		for (const auto& b : d->bps)
			if (b.type == MSDOS_BP_EXEC && b.start == linear && b.end == linear)
				id = b.id;
	});
	return id;
}

int msdosdebug_bp_toggle(msdosdebug* d, uint32_t linear)
{
	int id = msdosdebug_bp_at(d, linear);
	if (id) {
		msdosdebug_bp_remove(d, id);
		return 0;
	}
	return msdosdebug_bp_add(d, MSDOS_BP_EXEC, linear, linear, -1);
}

// ---------------------------------------------------------------------------
// traces
// ---------------------------------------------------------------------------

void msdosdebug_int_trace_enable(msdosdebug* d, int on)
{
	d->Run([d, on] { d->intTrace = on != 0; });
	// a hook per instruction needs the interpreter
	d->Run([d, on] {
		if (!d->attached) {
			if (on) {
				msdos_host_debug_hooks h = {};
				h.user = d;
				h.instruction = HookInstruction;
				h.mem_access = HookMem;
				h.io_access = HookIo;
				msdos_host_debug_install(&h);
				d86_force_interpreter(1);
				d->Rebuild();
			} else {
				msdos_host_debug_install(nullptr);
				d86_force_interpreter(0);
			}
		}
	});
}

int msdosdebug_int_trace_enabled(msdosdebug* d)
{
	int on = 0;
	d->Run([&] { on = d->intTrace; });
	return on;
}

int msdosdebug_int_trace(msdosdebug* d, msdos_int_event* out, int max)
{
	std::lock_guard<std::mutex> lock(d->traceMtx);
	int n = 0;
	size_t start = d->ints.size() > (size_t)max ? d->ints.size() - (size_t)max : 0;
	for (size_t i = start; i < d->ints.size(); i++)
		out[n++] = d->ints[i];
	return n;
}

void msdosdebug_int_trace_clear(msdosdebug* d)
{
	std::lock_guard<std::mutex> lock(d->traceMtx);
	d->ints.clear();
}

int msdosdebug_ivt(msdosdebug* d, int vec, uint16_t* seg, uint16_t* off)
{
	int rc = -1;
	d->Run([&] { rc = d86_ivt(vec, seg, off); });
	return rc;
}

int msdosdebug_fujinet_trace(msdosdebug* d, msdos_fuji_packet* out, int max)
{
	std::lock_guard<std::mutex> lock(d->traceMtx);
	int n = 0;
	size_t start = d->fuji.size() > (size_t)max ? d->fuji.size() - (size_t)max : 0;
	for (size_t i = start; i < d->fuji.size(); i++)
		out[n++] = d->fuji[i];
	return n;
}

void msdosdebug_fujinet_trace_clear(msdosdebug* d)
{
	std::lock_guard<std::mutex> lock(d->traceMtx);
	d->fuji.clear();
}

int msdosdebug_uart(msdosdebug* d, char* dst, int dstsz)
{
	int n = 0;
	const int com = msdos_profile_get(prefs_machine(d->s))->pcjr ? 1 : prefs_fujinet_com(d->s);
	if (!d->Run([&] { n = d86_uart(com, dst, dstsz); }))
		n = snprintf(dst, (size_t)dstsz, "The PC is off\n");
	return n;
}

// ---------------------------------------------------------------------------
// symbols
// ---------------------------------------------------------------------------

int msdosdebug_load_symbols(msdosdebug* d, const char* path, uint16_t load_seg, char* msg, int msgsz)
{
	FILE* f = fopen(path, "r");
	if (!f) {
		put(msg, msgsz, std::string("Cannot read ") + path);
		return -1;
	}
	char line[512];
	int n = 0;
	std::map<uint32_t, std::string> found;
	while (fgets(line, sizeof line, f)) {
		unsigned seg, off;
		char name[256];
		// MS LINK " 0000:0010       _main", WLINK "0000:0010+     _main_"
		if (sscanf(line, " %4x:%8x%*[+* ] %255s", &seg, &off, name) == 3
		    || sscanf(line, " %4x:%8x %255s", &seg, &off, name) == 3) {
			if (!isalpha((unsigned char)name[0]) && name[0] != '_' && name[0] != '$' && name[0] != '@')
				continue;
			found[(((uint32_t)seg + load_seg) << 4) + off] = name;
			n++;
		}
	}
	fclose(f);
	{
		std::lock_guard<std::mutex> lock(d->symMtx);
		for (auto& kv : found)
			d->syms[kv.first] = kv.second;
	}
	d->generation.fetch_add(1);
	put(msg, msgsz, std::to_string(n) + " symbols from " + path);
	return n > 0 ? 0 : -1;
}

void msdosdebug_clear_symbols(msdosdebug* d)
{
	std::lock_guard<std::mutex> lock(d->symMtx);
	d->syms.clear();
}

int msdosdebug_symbol_at(msdosdebug* d, uint32_t linear, char* dst, int dstsz)
{
	std::lock_guard<std::mutex> lock(d->symMtx);
	auto it = d->syms.upper_bound(linear);
	if (it == d->syms.begin())
		return 0;
	--it;
	if (linear - it->first > 0x10000)
		return 0;
	if (linear == it->first)
		return snprintf(dst, (size_t)dstsz, "%s", it->second.c_str());
	return snprintf(dst, (size_t)dstsz, "%s+%X", it->second.c_str(), linear - it->first);
}

int msdosdebug_symbol_find(msdosdebug* d, const char* name, uint32_t* linear)
{
	std::lock_guard<std::mutex> lock(d->symMtx);
	for (const auto& kv : d->syms)
		if (!strcasecmp(kv.second.c_str(), name)) {
			*linear = kv.first;
			return 0;
		}
	return -1;
}

// ---------------------------------------------------------------------------
// the screen
// ---------------------------------------------------------------------------

int msdosdebug_video_info(msdosdebug* d, char* dst, int dstsz)
{
	uint8_t bda[0x60];
	if (msdosdebug_read(d, 0x400, bda, sizeof bda) <= 0)
		return snprintf(dst, (size_t)dstsz, "The PC is off\n");
	const int mode = bda[0x49], cols = bda[0x4A] | (bda[0x4B] << 8), page = bda[0x62];
	const int cx = bda[0x50 + page * 2], cy = bda[0x51 + page * 2];
	return snprintf(dst, (size_t)dstsz,
	                "BIOS video mode %02Xh, %d columns, page %d, cursor at %d,%d\n", mode, cols,
	                page, cx, cy);
}

// ---------------------------------------------------------------------------
// the console
// ---------------------------------------------------------------------------

namespace {

std::string regs_text(msdosdebug* d)
{
	msdos_cpu_regs r;
	if (msdosdebug_regs(d, &r) != 0)
		return "The PC is off\n";
	char b[512];
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
	msdos_dis_line l;
	msdosdebug_disassemble(d, r.cs, r.eip, 1, &l);
	return std::string(b) + hex(l.seg, 4) + ":" + hex(l.off, r.code32 ? 8 : 4) + " " + l.bytes +
	       std::string(std::max(1, 16 - (int)strlen(l.bytes)), ' ') + l.text + "\n";
}

const char* const help_text =
	"r [reg value]        registers (or set one)\n"
	"d [addr] [l len]     dump memory\n"
	"e addr bytes...      enter bytes\n"
	"u [addr] [l count]   unassemble\n"
	"g [addr]             go (to addr)\n"
	"t [n]                trace n instructions\n"
	"p                    proceed over a call, INT or loop\n"
	"bp addr              breakpoint;  bpr/bpw/bpa addr [len]  on memory\n"
	"bpi/bpo/bpio port [last]   on I/O ports;  bpint n [ah]   on INT n\n"
	"bl, bc n|*           list, clear breakpoints\n"
	"i port, o port byte  read, write an I/O port\n"
	"Addresses: seg:off, a register pair (cs:ip), a hex linear address, a symbol.\n";

} // namespace

static uint16_t gDumpSeg, gUnasmSeg;
static uint32_t gDumpOff, gUnasmOff;
static bool gHaveDump, gHaveUnasm;

int msdosdebug_command(msdosdebug* d, const char* line, char* out, int outsz)
{
	std::vector<std::string> a;
	{
		std::string cur;
		for (const char* p = line; ; p++) {
			if (!*p || isspace((unsigned char)*p)) {
				if (!cur.empty())
					a.push_back(cur);
				cur.clear();
				if (!*p)
					break;
			} else {
				cur += (char)tolower((unsigned char)*p);
			}
		}
	}
	std::string res;
	if (a.empty()) {
		put(out, outsz, "");
		return 0;
	}
	const std::string& c = a[0];
	auto addr = [&](size_t i, uint16_t& seg, uint32_t& off, uint32_t& lin) {
		return i < a.size() && msdosdebug_parse_address(d, a[i].c_str(), &seg, &off, &lin) == 0;
	};
	auto num = [&](size_t i, uint32_t def) -> uint32_t {
		return i < a.size() ? (uint32_t)strtoul(a[i].c_str(), nullptr, 16) : def;
	};
	auto lenArg = [&](size_t i, uint32_t def) -> uint32_t {
		if (i < a.size() && a[i] == "l")
			return num(i + 1, def);
		if (i < a.size() && a[i][0] == 'l')
			return (uint32_t)strtoul(a[i].c_str() + 1, nullptr, 16);
		return def;
	};
	msdos_cpu_regs r;
	const bool up = msdosdebug_regs(d, &r) == 0;
	if (!up) {
		put(out, outsz, "The PC is off\n");
		return -1;
	}

	if (c == "?" || c == "h" || c == "help") {
		res = help_text;
	} else if (c == "r") {
		if (a.size() >= 3) {
			if (msdosdebug_set_reg(d, a[1].c_str(), num(2, 0)) != 0)
				res = "Cannot set " + a[1] + "\n";
		}
		res += regs_text(d);
	} else if (c == "d") {
		uint16_t seg = gHaveDump ? gDumpSeg : r.ds;
		uint32_t off = gHaveDump ? gDumpOff : 0, lin;
		size_t li = 1;
		if (addr(1, seg, off, lin))
			li = 2;
		const uint32_t len = std::min(lenArg(li, 0x80), 0x1000u);
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
			res += std::string(lineb) + "\n";
		}
		gDumpSeg = seg;
		gDumpOff = off + len;
		gHaveDump = true;
	} else if (c == "e") {
		uint16_t seg;
		uint32_t off, lin;
		if (!addr(1, seg, off, lin)) {
			res = "e addr bytes...\n";
		} else {
			std::vector<uint8_t> bytes;
			for (size_t i = 2; i < a.size(); i++)
				bytes.push_back((uint8_t)strtoul(a[i].c_str(), nullptr, 16));
			msdosdebug_write(d, lin, bytes.data(), (int)bytes.size());
		}
	} else if (c == "u") {
		uint16_t seg = gHaveUnasm ? gUnasmSeg : r.cs;
		uint32_t off = gHaveUnasm ? gUnasmOff : r.eip, lin;
		size_t li = 1;
		if (addr(1, seg, off, lin))
			li = 2;
		const int count = (int)std::min(lenArg(li, 16), 64u);
		std::vector<msdos_dis_line> lines((size_t)count);
		const int n = msdosdebug_disassemble(d, seg, off, count, lines.data());
		for (int i = 0; i < n; i++) {
			const auto& l = lines[(size_t)i];
			if (l.symbol[0])
				res += std::string(l.symbol) + ":\n";
			res += hex(l.seg, 4) + ":" + hex(l.off, 4) + " " + l.bytes +
			       std::string(std::max(1, 16 - (int)strlen(l.bytes)), ' ') + l.text + "\n";
		}
		if (n) {
			gUnasmSeg = seg;
			gUnasmOff = lines[(size_t)n - 1].off + (uint32_t)lines[(size_t)n - 1].len;
			gHaveUnasm = true;
		}
	} else if (c == "g") {
		uint16_t seg;
		uint32_t off, lin;
		if (addr(1, seg, off, lin))
			msdosdebug_run_to(d, lin);
		else
			msdosdebug_resume(d);
		res = "Running\n";
	} else if (c == "t") {
		const uint32_t n = std::max(1u, num(1, 1));
		if (!msdosdebug_stopped(d)) {
			res = "Stop first\n";
		} else {
			d->Run([d, n] {
				d->mode = Mode::Step;
				d->stepLeft = (int)n;
				d->ResumeFrom();
			});
			d->Host().Resume();
			res = "";
		}
		gHaveUnasm = false;
	} else if (c == "p") {
		if (msdosdebug_step_over(d) != 0)
			res = "Stop first\n";
		gHaveUnasm = false;
	} else if (c == "bp" || c == "bpr" || c == "bpw" || c == "bpa") {
		uint16_t seg;
		uint32_t off, lin;
		if (!addr(1, seg, off, lin)) {
			res = c + " addr\n";
		} else {
			const int type = c == "bp" ? MSDOS_BP_EXEC : c == "bpr" ? MSDOS_BP_READ
			                 : c == "bpw" ? MSDOS_BP_WRITE : MSDOS_BP_ACCESS;
			const uint32_t len = type == MSDOS_BP_EXEC ? 1 : std::max(1u, num(2, 1));
			res = "Breakpoint " + std::to_string(msdosdebug_bp_add(d, type, lin, lin + len - 1, -1)) + "\n";
		}
	} else if (c == "bpi" || c == "bpo" || c == "bpio") {
		if (a.size() < 2) {
			res = c + " port [last]\n";
		} else {
			const uint32_t p0 = num(1, 0), p1 = num(2, p0);
			const int type = c == "bpi" ? MSDOS_BP_IN : c == "bpo" ? MSDOS_BP_OUT : MSDOS_BP_IO;
			res = "Breakpoint " + std::to_string(msdosdebug_bp_add(d, type, p0, p1, -1)) + "\n";
		}
	} else if (c == "bpint") {
		if (a.size() < 2)
			res = "bpint n [ah]\n";
		else
			res = "Breakpoint " +
			      std::to_string(msdosdebug_bp_add(d, MSDOS_BP_INT, num(1, 0), num(1, 0),
			                                       a.size() > 2 ? (int)num(2, 0) : -1)) + "\n";
	} else if (c == "bl") {
		msdos_breakpoint bl[64];
		const int n = msdosdebug_bp_list(d, bl, 64);
		for (int i = 0; i < n; i++)
			res += std::to_string(bl[i].id) + (bl[i].enabled ? "  " : " (off) ") + bl[i].text +
			       "  hits " + std::to_string(bl[i].hits) + "\n";
		if (!n)
			res = "No breakpoints\n";
	} else if (c == "bc") {
		if (a.size() > 1 && a[1] == "*")
			msdosdebug_bp_clear(d);
		else if (a.size() > 1)
			msdosdebug_bp_remove(d, atoi(a[1].c_str()));
	} else if (c == "i") {
		const uint16_t port = (uint16_t)num(1, 0);
		uint8_t v = 0xFF;
		d->Run([&] { v = inb(port); });
		res = hex(v, 2) + "\n";
	} else if (c == "o") {
		const uint16_t port = (uint16_t)num(1, 0);
		const uint8_t v = (uint8_t)num(2, 0);
		d->Run([&] { outb(port, v); });
	} else {
		res = "Unknown command (h for help)\n";
	}
	put(out, outsz, res);
	return 0;
}
