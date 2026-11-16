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
#include <atomic>
#include <chrono>
#include <functional>
#include <shared_mutex>
#include <thread>

#include <Zydis.h>

#include "session_impl.h"
#include "msdosdebug.h"
#include "msdossnd.h"

extern "C" {
#include "expr.h"
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
namespace {

// What an expression sees: the registers as they are now (all 0 with no PC), the memory
// through d86_read, and for an access the data and the address.
struct EvalState
{
	msdosdebug* d = nullptr;
	bool machine = false;       // registers and memory are live (emulation thread only)
	bool validate = false;      // names resolve to 0: a syntax and name check
	msdos_cpu_regs r = {};
	bool access = false;
	int64_t value = 0, addr = 0, old = 0;
};

enum class Mode { Run, Step, Out, RunTo };

struct Pending
{
	int bpId = 0;
	int type = 0;               // MSDOS_STOP_WATCH_*
	uint32_t addr = 0;
	int width = 1;
	int64_t value = -1, old = -1;
	int64_t from = -1;
};

} // namespace

struct msdosdebug
{
	msdossession* s = nullptr;

	// ---- the session's life: public calls hold it shared, teardown takes it
	std::shared_mutex life;

	// ---- emulation thread
	bool attached = false;
	bool hooksOn = false;
	int nextId = 1;
	std::unordered_set<uint32_t> execAt;        // enabled single-address execute bps
	bool execRanges = false, intBps = false;
	Mode mode = Mode::Run;
	int stepLeft = 0;
	uint32_t tempBp = NoAddr;
	uint32_t outSp = 0;
	uint8_t prevOp = 0;
	uint32_t skipLinear = NoAddr;
	bool skipPending = false;
	bool needRecord = true;
	std::vector<Pending> pend;
	std::vector<uint32_t> shadowPages;
	bool intTrace = false;

	// ---- breakpoints (any thread; the hooks take the lock only on a candidate hit)
	std::mutex bpMtx;
	std::vector<msdos_breakpoint> bps;

	// ---- the stop (one critical section)
	std::atomic<unsigned> generation { 0 };
	std::mutex stopMtx;
	std::string reason;
	std::string stopText;
	msdos_stop_info stop = {};
	std::atomic<uint64_t> serial { 0 };

	// ---- traces
	std::mutex traceMtx;
	std::deque<msdos_int_event> ints;
	uint64_t intSerial = 0;
	std::deque<msdos_fuji_packet> fuji;
	uint64_t fujiSerial = 0;
	std::vector<uint8_t> slip[2];
	bool slipEsc[2] = { false, false };

	// ---- the instruction ring (Trace and History)
	std::mutex ringMtx;
	std::vector<msdos_trace_entry> ring;
	size_t ringHead = 0, ringCount = 0;
	uint64_t ringTotal = 0;
	std::atomic<bool> traceOn { false }, historyOn { false };
	int lastKind = 0, lastOpLen = 0;      // the newest record is a HLT (1) or a REP string (2)

	// ---- symbols
	std::mutex symMtx;
	std::multimap<uint32_t, std::string> syms;
	std::vector<std::pair<uint32_t, std::string>> symList;
	unsigned symListGen = ~0u;
	std::atomic<unsigned> symGen { 0 };

	// ---- helpers that take the life lock
	bool Up()
	{
		std::shared_lock<std::shared_mutex> lock(life);
		return s && s->running;
	}
	bool Run(const std::function<void()>& fn)
	{
		std::shared_lock<std::shared_mutex> lock(life);
		return s && s->running && Impl(s)->host.Call(fn, PcHost::Kind::Any);
	}
	bool HostStopped()
	{
		std::shared_lock<std::shared_mutex> lock(life);
		return s && s->running && Impl(s)->host.Stopped();
	}
	void HostResume()
	{
		std::shared_lock<std::shared_mutex> lock(life);
		if (s && s->running)
			Impl(s)->host.Resume();
	}
	uint64_t Frames()
	{
		std::shared_lock<std::shared_mutex> lock(life, std::try_to_lock);
		return lock.owns_lock() && s && s->running ? Impl(s)->host.FrameCount() : 0;
	}

	// ---- emulation thread
	int Instruction(bool atSliceStart);
	void MemAccess(const uint32_t* addrs, int access);
	void IoAccess(uint16_t port, bool write, int width, uint32_t val);
	int StopHere(const msdos_stop_info& info, const std::string& why);
	void Stop(const msdos_stop_info& info, const std::string& why);
	bool ResolvePending(uint32_t lin);
	void Rebuild();
	void UpdateHooks();
	void ResumeFrom();
	void Record(uint32_t lin);
	uint8_t OpcodeAt(uint32_t linear, int* prefixes, uint8_t* next);
	int InstructionLength(uint32_t linear, uint32_t off);
	std::string Where(uint32_t linear);
	std::string Position();
	int EvalHere(const char* text, EvalState& st, int64_t* out, char* err, int errsz);
	bool Condition(const std::string& cond, EvalState& st);

	void SetReason(const std::string& r)
	{
		std::lock_guard<std::mutex> lock(stopMtx);
		reason = r;
	}
	void Apply() { Run([this] { Rebuild(); }); }
};

namespace {

msdosdebug* gDebug = nullptr;     // the one the hooks talk to
const char* (*gSoundText)(void) = nullptr;

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

void WatchPort(uint16_t port)
{
	msdos_host_debug_watch_io(port, 1);
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

// ---- expressions

std::string lower(std::string t)
{
	for (auto& c : t)
		c = (char)tolower((unsigned char)c);
	return t;
}

const char* const kRegNames[] = {
	"al", "ah", "ax", "eax", "bl", "bh", "bx", "ebx", "cl", "ch", "cx", "ecx", "dl", "dh", "dx",
	"edx", "si", "esi", "di", "edi", "bp", "ebp", "sp", "esp", "ip", "eip", "cs", "ds", "es", "ss",
	"fs", "gs", "flags", "eflags", "cr0", "pc", "cf", "pf", "af", "zf", "sf", "tf", "if", "df",
	"of", "frame", "cycles", nullptr };

bool access_name(const std::string& n)
{
	return n == "value" || n == "addr" || n == "old" || n == "port";
}

bool reg_name(const std::string& n)
{
	for (int i = 0; kRegNames[i]; i++)
		if (n == kRegNames[i])
			return true;
	return false;
}

int eval_name(void* user, const char* name, int64_t* out)
{
	EvalState& st = *static_cast<EvalState*>(user);
	const std::string n = lower(name);
	if (access_name(n)) {
		if (st.validate) {
			*out = 0;
			return true;
		}
		if (!st.access)
			return false;
		*out = n == "value" ? st.value : n == "old" ? st.old : st.addr;
		return true;
	}
	if (reg_name(n)) {
		if (st.validate || !st.machine) {
			*out = 0;
			return true;
		}
		const msdos_cpu_regs& r = st.r;
		const uint32_t f = r.eflags;
		struct { const char* n; int64_t v; } table[] = {
			{ "al", r.eax & 0xFF }, { "ah", (r.eax >> 8) & 0xFF }, { "ax", r.eax & 0xFFFF },
			{ "eax", r.eax }, { "bl", r.ebx & 0xFF }, { "bh", (r.ebx >> 8) & 0xFF },
			{ "bx", r.ebx & 0xFFFF }, { "ebx", r.ebx }, { "cl", r.ecx & 0xFF },
			{ "ch", (r.ecx >> 8) & 0xFF }, { "cx", r.ecx & 0xFFFF }, { "ecx", r.ecx },
			{ "dl", r.edx & 0xFF }, { "dh", (r.edx >> 8) & 0xFF }, { "dx", r.edx & 0xFFFF },
			{ "edx", r.edx }, { "si", r.esi & 0xFFFF }, { "esi", r.esi }, { "di", r.edi & 0xFFFF },
			{ "edi", r.edi }, { "bp", r.ebp & 0xFFFF }, { "ebp", r.ebp }, { "sp", r.esp & 0xFFFF },
			{ "esp", r.esp }, { "ip", r.eip & 0xFFFF }, { "eip", r.eip }, { "cs", r.cs },
			{ "ds", r.ds }, { "es", r.es }, { "ss", r.ss }, { "fs", r.fs }, { "gs", r.gs },
			{ "flags", f & 0xFFFF }, { "eflags", f }, { "cr0", r.cr0 },
			{ "pc", (int64_t)r.cs_base + r.eip },
			{ "cf", f & 1 }, { "pf", (f >> 2) & 1 }, { "af", (f >> 4) & 1 }, { "zf", (f >> 6) & 1 },
			{ "sf", (f >> 7) & 1 }, { "tf", (f >> 8) & 1 }, { "if", (f >> 9) & 1 },
			{ "df", (f >> 10) & 1 }, { "of", (f >> 11) & 1 },
			{ "cycles", (int64_t)d86_cycles() } };
		if (n == "frame") {
			*out = (int64_t)st.d->Frames();
			return true;
		}
		for (const auto& t : table)
			if (n == t.n) {
				*out = t.v;
				return true;
			}
		return false;
	}
	uint32_t lin;
	if (msdosdebug_symbol_find(st.d, name, &lin) == 0) {
		*out = lin;
		return true;
	}
	return false;
}

uint8_t eval_read(void* user, uint32_t linear)
{
	EvalState& st = *static_cast<EvalState*>(user);
	uint8_t b = 0xFF;
	if (st.machine && !st.validate)
		d86_read(linear, &b, 1);
	return b;
}

uint32_t eval_linear(void* user, uint32_t seg, uint32_t off)
{
	EvalState& st = *static_cast<EvalState*>(user);
	if (st.machine && !st.validate)
		return d86_seg_base((uint16_t)seg) + off;
	return (seg << 4) + off;
}

} // namespace

const char* const msdosdebug_reg_names[] = {
	"al", "ah", "ax", "eax", "bl", "bh", "bx", "ebx", "cl", "ch", "cx", "ecx", "dl", "dh", "dx",
	"edx", "si", "esi", "di", "edi", "bp", "ebp", "sp", "esp", "ip", "eip", "cs", "ds", "es", "ss",
	"fs", "gs", "flags", "eflags", "cr0", "pc", "cf", "pf", "af", "zf", "sf", "tf", "if", "df",
	"of", "frame", "cycles", nullptr };

int msdosdebug::EvalHere(const char* text, EvalState& st, int64_t* out, char* err, int errsz)
{
	st.d = this;
	if (st.machine && !st.validate)
		d86_regs(&st.r);
	msdosexpr_ctx ctx = {};
	ctx.name = eval_name;
	ctx.read_byte = eval_read;
	ctx.linear = eval_linear;
	ctx.user = &st;
	int64_t v = 0;
	const int rc = msdosexpr_eval(text, &ctx, &v, err, errsz);
	if (rc == 0 && out)
		*out = v;
	return rc;
}

bool msdosdebug::Condition(const std::string& cond, EvalState& st)
{
	if (cond.empty())
		return true;
	st.machine = true;
	int64_t v = 0;
	char err[96];
	return EvalHere(cond.c_str(), st, &v, err, sizeof err) == 0 && v != 0;
}

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

std::string msdosdebug::Position()
{
	uint16_t seg;
	uint32_t off;
	d86_cs_ip(&seg, &off);
	return hex(seg, 4) + ":" + hex(off, d86_code32() ? 8 : 4);
}

// Records a stop: the legacy reason string, the words, the info -- all under one lock.
void msdosdebug::Stop(const msdos_stop_info& base, const std::string& why)
{
	mode = Mode::Run;
	tempBp = NoAddr;
	stepLeft = 0;
	pend.clear();
	uint16_t seg = 0;
	uint32_t off = 0;
	const uint32_t lin = d86_cs_ip(&seg, &off);
	msdos_stop_info info = base;
	info.pc = lin;
	info.cs = seg;
	info.ip = off;
	if (info.reason != MSDOS_STOP_WATCH_READ && info.reason != MSDOS_STOP_WATCH_WRITE &&
	    info.reason != MSDOS_STOP_WATCH_IN && info.reason != MSDOS_STOP_WATCH_OUT) {
		info.addr = lin;
		info.value = info.old_value = -1;
		info.from_pc = -1;
	}
	const std::string where = hex(seg, 4) + ":" + hex(off, d86_code32() ? 8 : 4);
	std::string text;
	char b[200];
	switch (info.reason) {
	case MSDOS_STOP_STEP: text = "step at " + where; break;
	case MSDOS_STOP_BREAKPOINT:
		text = "breakpoint " + std::to_string(info.bp_id) + ": exec " + where;
		break;
	case MSDOS_STOP_WATCH_WRITE:
	case MSDOS_STOP_WATCH_READ: {
		const int digits = info.width * 2;
		const bool w = info.reason == MSDOS_STOP_WATCH_WRITE;
		snprintf(b, sizeof b, "%s %s = $%0*llX", w ? "wrote" : "read", hex(info.addr, 5).c_str(), digits,
		         (unsigned long long)info.value);
		text = b;
		if (w && info.old_value >= 0) {
			snprintf(b, sizeof b, " (was $%0*llX)", digits, (unsigned long long)info.old_value);
			text += b;
		}
		text += " at " + where;
		break;
	}
	case MSDOS_STOP_WATCH_IN:
	case MSDOS_STOP_WATCH_OUT:
		snprintf(b, sizeof b, "%s %s = $%0*llX at %s",
		         info.reason == MSDOS_STOP_WATCH_IN ? "in" : "out", hex(info.addr, 4).c_str(),
		         info.width * 2, (unsigned long long)info.value, where.c_str());
		text = b;
		break;
	case MSDOS_STOP_INT:
		text = "breakpoint " + std::to_string(info.bp_id) + ": " + why + " at " + where;
		break;
	case MSDOS_STOP_ONESHOT: text = "stopped at " + where; break;
	default: text = "paused at " + where; break;
	}
	{
		std::lock_guard<std::mutex> lock(stopMtx);
		info.serial = serial.fetch_add(1) + 1;
		stop = info;
		reason = why;
		stopText = text;
	}
	generation.fetch_add(1);
}

int msdosdebug::StopHere(const msdos_stop_info& info, const std::string& why)
{
	Stop(info, why);
	needRecord = true;      // the instruction here has not run, so it is not in the ring yet
	return 1;
}

// A memory or port access recorded by the hooks, checked now that the instruction has
// finished: the data are in memory, and the conditions see the registers after it.
bool msdosdebug::ResolvePending(uint32_t lin)
{
	(void)lin;
	std::vector<Pending> work;
	work.swap(pend);
	for (auto& p : work) {
		std::string cond;
		int type = 0;
		{
			std::lock_guard<std::mutex> lock(bpMtx);
			bool found = false;
			for (const auto& b : bps)
				if (b.id == p.bpId && b.enabled) {
					cond = b.condition;
					type = b.type;
					found = true;
				}
			if (!found)
				continue;
		}
		const bool port = p.type == MSDOS_STOP_WATCH_IN || p.type == MSDOS_STOP_WATCH_OUT;
		if (!port) {
			uint8_t buf[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
			d86_read(p.addr, buf, p.width);
			int64_t v = 0;
			for (int i = p.width - 1; i >= 0; i--)
				v = (v << 8) | buf[i];
			p.value = v;
			if (p.type == MSDOS_STOP_WATCH_READ)
				p.old = v;
		}
		EvalState st;
		st.d = this;
		st.access = true;
		st.value = p.value;
		st.addr = p.addr;
		st.old = p.old < 0 ? p.value : p.old;
		if (!Condition(cond, st))
			continue;
		{
			std::lock_guard<std::mutex> lock(bpMtx);
			for (auto& b : bps)
				if (b.id == p.bpId)
					b.hits++;
		}
		msdos_stop_info info = {};
		info.reason = p.type;
		info.bp_id = p.bpId;
		info.addr = p.addr;
		info.width = p.width;
		info.value = p.value;
		info.old_value = p.type == MSDOS_STOP_WATCH_WRITE ? p.old : -1;
		info.from_pc = p.from;
		(void)type;
		std::string why;
		if (port)
			why = "Breakpoint " + std::to_string(p.bpId) + ": " +
			      (p.type == MSDOS_STOP_WATCH_OUT ? "OUT " : "IN ") + hex(p.addr, 4) +
			      (p.type == MSDOS_STOP_WATCH_OUT ? " = " : " \xe2\x86\x92 ") + hex((uint32_t)p.value, p.width * 2);
		else
			why = "Breakpoint " + std::to_string(p.bpId) + ": " +
			      (p.type == MSDOS_STOP_WATCH_WRITE ? "write to " : "read of ") + hex(p.addr, 5);
		StopHere(info, why);
		return true;
	}
	return false;
}

// One record per instruction about to run, folding a REP or HALT that goes round again.
void msdosdebug::Record(uint32_t lin)
{
	msdos_trace_entry e = {};
	d86_trace_capture(&e);
	d86_read(lin, e.bytes, sizeof e.bytes);
	int i = 0;
	bool rep = false;
	while (i < 4 && (e.bytes[i] == 0x26 || e.bytes[i] == 0x2E || e.bytes[i] == 0x36 || e.bytes[i] == 0x3E
	                 || e.bytes[i] == 0x64 || e.bytes[i] == 0x65 || e.bytes[i] == 0x66
	                 || e.bytes[i] == 0x67 || e.bytes[i] == 0xF0 || e.bytes[i] == 0xF2 || e.bytes[i] == 0xF3)) {
		if (e.bytes[i] == 0xF2 || e.bytes[i] == 0xF3)
			rep = true;
		i++;
	}
	const uint8_t op = e.bytes[i];
	const bool str = (op >= 0xA4 && op <= 0xA7) || (op >= 0xAA && op <= 0xAF) || (op >= 0x6C && op <= 0x6F);
	const bool hlt = op == 0xF4;
	const int kind = hlt ? 1 : (rep && str) ? 2 : 0;
	std::lock_guard<std::mutex> lock(ringMtx);
	if (ring.empty())
		return;
	if (ringCount && lastKind) {
		// 86Box's 8088 core calls the hook once per HLT / REP iteration with cs:ip already past
		// the opcode; the 386 core stays on the instruction. Either way it is one record.
		msdos_trace_entry& last = ring[(ringHead + ring.size() - 1) % ring.size()];
		const uint32_t cx = e.code32 ? e.ecx : (e.ecx & 0xFFFF);
		const bool past = lin == last.linear + (uint32_t)lastOpLen;
		const bool same = lin == last.linear;
		if (lastKind == 1 && (past || same) && (hlt || past)) {
			last.repeat++;
			ringTotal++;
			return;
		}
		if (lastKind == 2 && (same || (past && cx != 0))) {
			last.repeat++;
			ringTotal++;
			return;
		}
	}
	lastKind = kind;
	lastOpLen = i + 1;
	ring[ringHead] = e;
	ringHead = (ringHead + 1) % ring.size();
	if (ringCount < ring.size())
		ringCount++;
	ringTotal++;
}

int msdosdebug::Instruction(bool atSliceStart)
{
	uint16_t seg;
	uint32_t off;
	const uint32_t lin = d86_cs_ip(&seg, &off);

	if (!pend.empty() && ResolvePending(lin))
		return 1;
	const bool skip = skipPending && lin == skipLinear;
	skipPending = false;

	if (!atSliceStart) {
		if (mode == Mode::Step && --stepLeft <= 0) {
			msdos_stop_info info = {};
			info.reason = MSDOS_STOP_STEP;
			return StopHere(info, "Step");
		}
		if (mode == Mode::Out) {
			const bool wasRet = prevOp == 0xC3 || prevOp == 0xC2 || prevOp == 0xCB || prevOp == 0xCA
			                    || prevOp == 0xCF;
			if (wasRet && d86_stack_pointer() > outSp) {
				msdos_stop_info info = {};
				info.reason = MSDOS_STOP_ONESHOT;
				return StopHere(info, "Returned");
			}
		}
	}
	if (!skip) {
		if (mode == Mode::RunTo && lin == tempBp) {
			msdos_stop_info info = {};
			info.reason = MSDOS_STOP_ONESHOT;
			return StopHere(info, "Reached " + Where(lin));
		}
		if (execAt.count(lin) || execRanges) {
			struct Cand { int id; std::string cond; };
			std::vector<Cand> cands;
			{
				std::lock_guard<std::mutex> lock(bpMtx);
				for (const auto& b : bps)
					if (b.enabled && b.type == MSDOS_BP_EXEC && lin >= b.start && lin <= b.end)
						cands.push_back({ b.id, b.condition });
			}
			for (const auto& c : cands) {
				EvalState st;
				st.d = this;
				st.access = true;
				st.value = st.old = lin;
				st.addr = lin;
				if (!Condition(c.cond, st))
					continue;
				{
					std::lock_guard<std::mutex> lock(bpMtx);
					for (auto& b : bps)
						if (b.id == c.id)
							b.hits++;
				}
				msdos_stop_info info = {};
				info.reason = MSDOS_STOP_BREAKPOINT;
				info.bp_id = c.id;
				return StopHere(info, "Breakpoint " + std::to_string(c.id) + " at " + Where(lin));
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
			if (intBps && !skip) {
				struct Cand { int id; std::string cond; };
				std::vector<Cand> cands;
				{
					std::lock_guard<std::mutex> lock(bpMtx);
					for (const auto& b : bps)
						if (b.enabled && b.type == MSDOS_BP_INT && b.start == n
						    && (b.ah < 0 || b.ah == (ax >> 8)))
							cands.push_back({ b.id, b.condition });
				}
				for (const auto& c : cands) {
					EvalState st;
					st.d = this;
					st.access = true;
					st.value = st.old = st.addr = n;
					if (!Condition(c.cond, st))
						continue;
					{
						std::lock_guard<std::mutex> lock(bpMtx);
						for (auto& b : bps)
							if (b.id == c.id)
								b.hits++;
					}
					msdos_stop_info info = {};
					info.reason = MSDOS_STOP_INT;
					info.bp_id = c.id;
					return StopHere(info, int_text(n, ax));
				}
			}
		}
	}
	if ((traceOn || historyOn) && (!atSliceStart || needRecord))
		Record(lin);
	needRecord = false;
	return 0;
}

void msdosdebug::MemAccess(const uint32_t* addrs, int access)
{
	const int width = access & 15;
	const bool write = (access & 16) != 0;
	std::lock_guard<std::mutex> lock(bpMtx);
	for (auto& b : bps) {
		if (!b.enabled)
			continue;
		const bool kind = b.type == MSDOS_BP_ACCESS || (write ? b.type == MSDOS_BP_WRITE
		                                                         : b.type == MSDOS_BP_READ);
		if (!kind)
			continue;
		for (int i = 0; i < width; i++)
			if (addrs[i] >= b.start && addrs[i] <= b.end) {
				if (pend.size() >= 32)
					return;
				Pending p;
				p.bpId = b.id;
				p.type = write ? MSDOS_STOP_WATCH_WRITE : MSDOS_STOP_WATCH_READ;
				p.addr = addrs[0];
				p.width = std::min(width, 4);
				// the hook fires before the store: memory still holds what is replaced
				uint8_t buf[4] = { 0, 0, 0, 0 };
				d86_read(addrs[0], buf, p.width);
				int64_t v = 0;
				for (int k = p.width - 1; k >= 0; k--)
					v = (v << 8) | buf[k];
				p.old = v;
				p.from = (int64_t)d86_old_pc();
				pend.push_back(p);
				break;
			}
	}
}

void msdosdebug::IoAccess(uint16_t port, bool write, int width, uint32_t val)
{
	msdos_snd_io_observe(port, write ? 1 : 0, width, val);
	std::lock_guard<std::mutex> lock(bpMtx);
	for (auto& b : bps) {
		if (!b.enabled || port < b.start || port > b.end)
			continue;
		if (b.type == MSDOS_BP_IO || (write ? b.type == MSDOS_BP_OUT : b.type == MSDOS_BP_IN)) {
			if (pend.size() >= 32)
				return;
			Pending p;
			p.bpId = b.id;
			p.type = write ? MSDOS_STOP_WATCH_OUT : MSDOS_STOP_WATCH_IN;
			p.addr = port;
			p.width = width;
			p.value = p.old = val;
			p.from = (int64_t)d86_old_pc();
			pend.push_back(p);
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
	std::lock_guard<std::mutex> lock(bpMtx);
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
	// the Sound tab's observer (Rebuild just cleared the bitmaps)
	msdos_snd_watch_ports(WatchPort);
}

// Hooks are installed while the debugger is attached or anything records per instruction.
void msdosdebug::UpdateHooks()
{
	const bool want = attached || intTrace || traceOn || historyOn;
	if (want && !hooksOn) {
		msdos_host_debug_hooks h = {};
		h.user = this;
		h.instruction = HookInstruction;
		h.mem_access = HookMem;
		h.io_access = HookIo;
		msdos_host_debug_install(&h);
		d86_force_interpreter(1);
		hooksOn = true;
		needRecord = true;
		Rebuild();
	} else if (!want && hooksOn) {
		msdos_host_debug_install(nullptr);
		d86_force_interpreter(0);
		hooksOn = false;
	}
}

// Leaving a stop: the instruction it stopped at runs before any breakpoint
// there can fire again.
void msdosdebug::ResumeFrom()
{
	skipLinear = d86_cs_ip(nullptr, nullptr);
	skipPending = true;
	pend.clear();
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

// The session is going away. The engine object is kept (a window that still polls it
// gets "the PC is off", not freed memory); it just lets go of the session.
void session_debug_free(struct msdossession* s)
{
	auto* d = static_cast<msdosdebug*>(s->debugger);
	if (!d)
		return;
	if (gDebug == d) {
		char_fujinet_set_trace(nullptr);
		gDebug = nullptr;
	}
	{
		std::unique_lock<std::shared_mutex> lock(d->life);
		if (d->hooksOn) {
			msdos_host_debug_install(nullptr);
			d86_force_interpreter(0);
			d->hooksOn = false;
		}
		d->attached = false;
		d->s = nullptr;
	}
	d->generation.fetch_add(1);
	s->debugger = nullptr;
}

void session_debug_reset(struct msdossession* s)
{
	auto* d = static_cast<msdosdebug*>(s->debugger);
	if (!d)
		return;
	msdos_snd_reset();
	d->pend.clear();
	d->needRecord = true;
	if (d->hooksOn)
		d->Rebuild();
	d->generation.fetch_add(1);
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
	if (!d->Up())
		return -1;
	d->Run([d] {
		if (!d->attached) {
			d->attached = true;
			msdos_snd_reset();
			d->UpdateHooks();
			d->Rebuild();
		}
		msdos_stop_info info = {};
		info.reason = MSDOS_STOP_PAUSE;
		d->Stop(info, "Stopped");
		msdos_host_debug_set_stopped(1);
	});
	msdosdebug_wait_stopped(d, 500);
	return 0;
}

void msdosdebug_detach(msdosdebug* d)
{
	d->Run([d] {
		if (!d->attached)
			return;
		d->attached = false;
		d->mode = Mode::Run;
		d->UpdateHooks();
		if (!d->hooksOn)
			msdos_host_debug_set_stopped(0);
		d->ResumeFrom();
	});
	d->HostResume();
	d->generation.fetch_add(1);
}

int msdosdebug_attached(msdosdebug* d)
{
	int a = 0;
	d->Run([&] { a = d->attached; });
	return a;
}

int msdosdebug_wait_stopped(msdosdebug* d, int ms)
{
	for (int t = 0; t <= ms; t += 2) {
		if (d->HostStopped())
			return 1;
		if (!d->Up())
			return 0;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return d->HostStopped() ? 1 : 0;
}

int msdosdebug_wait_stop(msdosdebug* d, uint64_t before, int ms)
{
	for (int t = 0; t <= ms; t += 2) {
		if (d->serial.load() != before && d->HostStopped())
			return 1;
		if (!d->Up())
			return 0;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return 0;
}

void msdosdebug_stop(msdosdebug* d)
{
	if (d->HostStopped())
		return;
	d->Run([d] {
		msdos_stop_info info = {};
		info.reason = MSDOS_STOP_PAUSE;
		d->Stop(info, "Stopped");
		msdos_host_debug_set_stopped(1);
	});
	// the host parks at the next slice boundary: be stopped when this returns
	msdosdebug_wait_stopped(d, 500);
}

void msdosdebug_resume(msdosdebug* d)
{
	d->Run([d] {
		d->mode = Mode::Run;
		d->ResumeFrom();
	});
	d->HostResume();
	d->generation.fetch_add(1);
}

int msdosdebug_stopped(msdosdebug* d)
{
	return d->HostStopped() ? 1 : 0;
}

int msdosdebug_step_n(msdosdebug* d, int n)
{
	if (!msdosdebug_stopped(d))
		return -1;
	d->Run([d, n] {
		d->mode = Mode::Step;
		d->stepLeft = n < 1 ? 1 : n;
		d->ResumeFrom();
	});
	d->HostResume();
	return 0;
}

int msdosdebug_step(msdosdebug* d)
{
	return msdosdebug_step_n(d, 1);
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
	d->HostResume();
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
	d->HostResume();
	return 0;
}

int msdosdebug_run_to(msdosdebug* d, uint32_t linear)
{
	d->Run([d, linear] {
		d->tempBp = linear;
		d->mode = Mode::RunTo;
		d->ResumeFrom();
	});
	d->HostResume();
	return 0;
}

unsigned msdosdebug_generation(msdosdebug* d)
{
	return d->generation.load();
}

int msdosdebug_stop_reason(msdosdebug* d, char* dst, int dstsz)
{
	std::lock_guard<std::mutex> lock(d->stopMtx);
	return snprintf(dst, (size_t)dstsz, "%s", d->reason.c_str());
}

uint64_t msdosdebug_stop_serial(msdosdebug* d)
{
	return d->serial.load();
}

int msdosdebug_stop_snapshot(msdosdebug* d, msdos_stop_snapshot* out)
{
	const int paused = d->HostStopped() ? 1 : 0;
	std::lock_guard<std::mutex> lock(d->stopMtx);
	memset(out, 0, sizeof *out);
	out->paused = paused;
	out->serial = d->serial.load();
	out->stop = d->stop;
	if (paused) {
		snprintf(out->text, sizeof out->text, "%s", d->stopText.c_str());
		out->address = d->stop.pc;
	} else {
		snprintf(out->text, sizeof out->text, "running");
		out->address = -1;
	}
	return paused;
}

int msdosdebug_stop_text(msdosdebug* d, char* dst, int dstsz, int64_t* address)
{
	msdos_stop_snapshot snap;
	msdosdebug_stop_snapshot(d, &snap);
	if (address)
		*address = snap.address;
	return snprintf(dst, (size_t)dstsz, "%s", snap.text);
}

int msdosdebug_last_stop(msdosdebug* d, msdos_stop_info* out)
{
	msdos_stop_snapshot snap;
	const int paused = msdosdebug_stop_snapshot(d, &snap);
	*out = snap.stop;
	return paused;
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
	std::string n = lower(name);
	int rc = -1;
	d->Run([&] {
		rc = d86_set_reg(n.c_str(), value);
		if (rc == 0)
			d->needRecord = true;
	});
	if (rc == 0) {
		d->generation.fetch_add(1);
		uint16_t seg;
		uint32_t off;
		uint32_t lin = 0;
		d->Run([&] { lin = d86_cs_ip(&seg, &off); });
		std::lock_guard<std::mutex> lock(d->stopMtx);
		d->stop.pc = lin;
	}
	return rc;
}

int msdosdebug_register_set(msdosdebug* d, const char* name, int64_t value, char* err, int errsz)
{
	if (err && errsz > 0)
		err[0] = 0;
	if (!d->Up()) {
		put(err, errsz, "the PC is off");
		return 0;
	}
	if (!msdosdebug_stopped(d)) {
		put(err, errsz, "the PC is running: stop it first");
		return 0;
	}
	const std::string n = lower(name);
	if (!reg_name(n) || n == "pc" || n == "frame" || n == "cycles" || n == "cr0") {
		put(err, errsz, "no register '" + std::string(name) + "' to set");
		return 0;
	}
	if (msdosdebug_set_reg(d, n.c_str(), (uint32_t)value) != 0) {
		const bool seg = n == "cs" || n == "ds" || n == "es" || n == "ss" || n == "fs" || n == "gs";
		put(err, errsz, seg ? "segment registers can only be set in real mode (and fs/gs on a 386)"
		                    : "this CPU has no register '" + n + "'");
		return 0;
	}
	return 1;
}

int msdosdebug_regs_set(msdosdebug* d, const msdos_cpu_regs* regs, char* err, int errsz)
{
	if (err && errsz > 0)
		err[0] = 0;
	if (!d->Up()) {
		put(err, errsz, "the PC is off");
		return 0;
	}
	if (!msdosdebug_stopped(d)) {
		put(err, errsz, "the PC is running: stop it first");
		return 0;
	}
	char msg[200] = "";
	int rc = -1;
	const msdos_cpu_regs copy = *regs;
	d->Run([&] {
		rc = d86_regs_set(&copy, msg, sizeof msg);
		if (rc == 0)
			d->needRecord = true;
	});
	if (rc != 0) {
		put(err, errsz, msg);
		return 0;
	}
	d->generation.fetch_add(1);
	{
		uint16_t seg;
		uint32_t off;
		uint32_t lin = 0;
		d->Run([&] { lin = d86_cs_ip(&seg, &off); });
		std::lock_guard<std::mutex> lock(d->stopMtx);
		d->stop.pc = lin;
	}
	return 1;
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
	d->Run([&] {
		put_ = d86_write(linear, buf, n);
		d->needRecord = true;
	});
	d->generation.fetch_add(1);
	return put_;
}

uint32_t msdosdebug_linear(msdosdebug* d, uint16_t seg, uint32_t off)
{
	uint32_t base = (uint32_t)seg << 4;
	d->Run([&] { base = d86_seg_base(seg); });
	return base + off;
}

int msdosdebug_eval(msdosdebug* d, const char* text, int64_t* out, char* err, int errsz)
{
	EvalState st;
	st.d = d;
	int rc = -1;
	if (!d->Run([&] {
		    st.machine = true;
		    rc = d->EvalHere(text, st, out, err, errsz);
	    })) {
		st.machine = false;
		rc = d->EvalHere(text, st, out, err, errsz);
	}
	return rc;
}

// seg:off pieces: a register, a hex number, a symbol, or an expression
static bool addr_piece(msdosdebug* d, const std::string& t, uint32_t& v)
{
	std::string n = t;
	n.erase(0, n.find_first_not_of(" \t"));
	n.erase(n.find_last_not_of(" \t") + 1);
	if (n.empty())
		return false;
	const std::string l = lower(n);
	int64_t out;
	uint32_t sym;
	if (reg_name(l) && l != "frame" && l != "cycles") {
		char err[64];
		if (msdosdebug_eval(d, l.c_str(), &out, err, sizeof err) == 0) {
			v = (uint32_t)out;
			return true;
		}
		return false;
	}
	if (msdosdebug_symbol_find(d, n.c_str(), &sym) == 0) {
		v = sym;
		return true;
	}
	size_t i = 0;
	while (i < n.size() && isxdigit((unsigned char)n[i]))
		i++;
	if (i == n.size() && n.size() <= 8) {      // a bare hex number
		v = (uint32_t)strtoul(n.c_str(), nullptr, 16);
		return true;
	}
	char err[96];
	if (msdosdebug_eval(d, n.c_str(), &out, err, sizeof err) == 0) {
		v = (uint32_t)out;
		return true;
	}
	return false;
}

int msdosdebug_parse_address(msdosdebug* d, const char* text, uint16_t* seg, uint32_t* off,
                             uint32_t* linear)
{
	std::string t = text;
	t.erase(0, t.find_first_not_of(" \t"));
	t.erase(t.find_last_not_of(" \t") + 1);
	if (t.empty())
		return -1;
	const size_t colon = t.find(':');
	if (colon != std::string::npos) {
		uint32_t sv, ov;
		if (!addr_piece(d, t.substr(0, colon), sv) || !addr_piece(d, t.substr(colon + 1), ov))
			return -1;
		if (seg) *seg = (uint16_t)sv;
		if (off) *off = ov;
		if (linear) *linear = msdosdebug_linear(d, (uint16_t)sv, ov);
		return 0;
	}
	uint32_t lin;
	if (!addr_piece(d, t, lin))
		return -1;
	if (seg) *seg = (uint16_t)(lin >> 4);
	if (off) *off = lin & 0xF;
	if (linear) *linear = lin;
	return 0;
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

// A condition that is not an expression, or names something unknown, is refused.
static bool check_condition(msdosdebug* d, const char* cond, char* err, int errsz)
{
	if (!cond || !*cond)
		return true;
	EvalState st;
	st.d = d;
	st.validate = true;
	int64_t v;
	return d->EvalHere(cond, st, &v, err, errsz) == 0;
}

int msdosdebug_bp_add_if(msdosdebug* d, int type, uint32_t start, uint32_t end, int ah,
                         const char* condition, char* err, int errsz)
{
	if (err && errsz > 0)
		err[0] = 0;
	if (type < 0 || type >= MSDOS_BP_TYPES) {
		put(err, errsz, "bad breakpoint type");
		return -1;
	}
	if (end < start)
		end = start;
	std::string cond = condition ? condition : "";
	while (!cond.empty() && isspace((unsigned char)cond.back()))
		cond.pop_back();
	if (cond.size() >= sizeof(msdos_breakpoint::condition)) {
		put(err, errsz, "condition too long");
		return -1;
	}
	if (!check_condition(d, cond.c_str(), err, errsz))
		return -1;
	if (type >= MSDOS_BP_IN && type <= MSDOS_BP_IO && (start > 0xFFFF || end > 0xFFFF)) {
		put(err, errsz, "ports are 0-FFFF");
		return -1;
	}
	int id = -1;
	{
		std::lock_guard<std::mutex> lock(d->bpMtx);
		if (d->bps.size() >= MSDOS_MAX_BREAKPOINTS) {
			put(err, errsz, "too many breakpoints");
			return -1;
		}
		msdos_breakpoint b = {};
		b.id = d->nextId++;
		b.type = type;
		b.start = start;
		b.end = end;
		b.ah = type == MSDOS_BP_INT ? ah : -1;
		b.enabled = 1;
		snprintf(b.condition, sizeof b.condition, "%s", cond.c_str());
		std::string t = msdosdebug_bp_type_name(type);
		if (type == MSDOS_BP_INT)
			t += " " + hex(start, 2) + "h" + (ah >= 0 ? " AH=" + hex((uint32_t)ah, 2) + "h" : "");
		else if (type >= MSDOS_BP_IN)
			t += " " + hex(start, 4) + (end != start ? "-" + hex(end, 4) : "");
		else
			t += " " + hex(start, 5) + (end != start ? "-" + hex(end, 5) : "");
		snprintf(b.text, sizeof b.text, "%s", t.c_str());
		d->bps.push_back(b);
		id = b.id;
	}
	d->Apply();
	d->generation.fetch_add(1);
	return id;
}

int msdosdebug_bp_add(msdosdebug* d, int type, uint32_t start, uint32_t end, int ah)
{
	return msdosdebug_bp_add_if(d, type, start, end, ah, "", nullptr, 0);
}

int msdosdebug_bp_set_condition(msdosdebug* d, int id, const char* condition, char* err, int errsz)
{
	if (err && errsz > 0)
		err[0] = 0;
	std::string cond = condition ? condition : "";
	while (!cond.empty() && isspace((unsigned char)cond.back()))
		cond.pop_back();
	if (cond.size() >= sizeof(msdos_breakpoint::condition)) {
		put(err, errsz, "condition too long");
		return 0;
	}
	if (!check_condition(d, cond.c_str(), err, errsz))
		return 0;
	int found = 0;
	{
		std::lock_guard<std::mutex> lock(d->bpMtx);
		for (auto& b : d->bps)
			if (b.id == id) {
				snprintf(b.condition, sizeof b.condition, "%s", cond.c_str());
				found = 1;
			}
	}
	if (!found)
		put(err, errsz, "no such breakpoint");
	d->generation.fetch_add(1);
	return found;
}

int msdosdebug_bp_remove(msdosdebug* d, int id)
{
	int rc = -1;
	{
		std::lock_guard<std::mutex> lock(d->bpMtx);
		auto it = std::remove_if(d->bps.begin(), d->bps.end(),
		                         [id](const msdos_breakpoint& b) { return b.id == id; });
		rc = it != d->bps.end() ? 0 : -1;
		d->bps.erase(it, d->bps.end());
	}
	d->Apply();
	d->generation.fetch_add(1);
	return rc;
}

int msdosdebug_bp_enable(msdosdebug* d, int id, int on)
{
	int rc = -1;
	{
		std::lock_guard<std::mutex> lock(d->bpMtx);
		for (auto& b : d->bps)
			if (b.id == id) {
				b.enabled = on ? 1 : 0;
				rc = 0;
			}
	}
	d->Apply();
	d->generation.fetch_add(1);
	return rc;
}

int msdosdebug_bp_list(msdosdebug* d, msdos_breakpoint* out, int max)
{
	std::lock_guard<std::mutex> lock(d->bpMtx);
	int n = 0;
	for (const auto& b : d->bps)
		if (n < max)
			out[n++] = b;
	return n;
}

void msdosdebug_bp_clear(msdosdebug* d)
{
	{
		std::lock_guard<std::mutex> lock(d->bpMtx);
		d->bps.clear();
	}
	d->Apply();
	d->generation.fetch_add(1);
}

void msdosdebug_bp_clear_hits(msdosdebug* d)
{
	{
		std::lock_guard<std::mutex> lock(d->bpMtx);
		for (auto& b : d->bps)
			b.hits = 0;
	}
	d->generation.fetch_add(1);
}

int msdosdebug_bp_at(msdosdebug* d, uint32_t linear)
{
	std::lock_guard<std::mutex> lock(d->bpMtx);
	int id = 0;
	for (const auto& b : d->bps)
		if (b.type == MSDOS_BP_EXEC && b.start == linear && b.end == linear)
			id = b.id;
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
	d->Run([d, on] {
		d->intTrace = on != 0;
		d->UpdateHooks();
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

// ---- the instruction ring

static void ring_enable(msdosdebug* d, std::atomic<bool>* flag, int on)
{
	if (on) {
		std::lock_guard<std::mutex> lock(d->ringMtx);
		if (d->ring.empty())
			d->ring.resize(MSDOS_TRACE_SIZE);
	}
	d->Run([d, flag, on] {
		*flag = on != 0;
		d->needRecord = true;
		d->UpdateHooks();
	});
	if (!d->Up())
		*flag = on != 0;
}

void msdosdebug_trace_enable(msdosdebug* d, int on)
{
	ring_enable(d, &d->traceOn, on);
}

int msdosdebug_trace_enabled(msdosdebug* d)
{
	return d->traceOn ? 1 : 0;
}

void msdosdebug_history_enable(msdosdebug* d, int on)
{
	ring_enable(d, &d->historyOn, on);
}

int msdosdebug_history_enabled(msdosdebug* d)
{
	return d->historyOn ? 1 : 0;
}

int msdosdebug_trace_count(msdosdebug* d)
{
	std::lock_guard<std::mutex> lock(d->ringMtx);
	return (int)d->ringCount;
}

uint64_t msdosdebug_trace_total(msdosdebug* d)
{
	std::lock_guard<std::mutex> lock(d->ringMtx);
	return d->ringTotal;
}

uint64_t msdosdebug_history_count(msdosdebug* d)
{
	std::lock_guard<std::mutex> lock(d->ringMtx);
	return d->ringCount;
}

void msdosdebug_trace_clear(msdosdebug* d)
{
	std::lock_guard<std::mutex> lock(d->ringMtx);
	d->ringHead = d->ringCount = 0;
	d->ringTotal = 0;
	d->lastKind = 0;
}

void msdosdebug_history_clear(msdosdebug* d)
{
	msdosdebug_trace_clear(d);
}

static int trace_length(const msdos_trace_entry& e, ZydisDecodedInstruction* ins, ZydisDecodedOperand* ops)
{
	ZydisDecoder dec;
	ZydisDecoderInit(&dec, e.code32 ? ZYDIS_MACHINE_MODE_LEGACY_32 : ZYDIS_MACHINE_MODE_LEGACY_16,
	                 e.code32 ? ZYDIS_STACK_WIDTH_32 : ZYDIS_STACK_WIDTH_16);
	ZydisDecodedInstruction local;
	ZydisDecodedOperand lops[ZYDIS_MAX_OPERAND_COUNT];
	if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, e.bytes, sizeof e.bytes, ins ? ins : &local,
	                                       ops ? ops : lops)))
		return 0;
	return (ins ? ins : &local)->length;
}

// newest first: the ring's last record is index 0
static int ring_copy(msdosdebug* d, msdos_trace_entry* out, int max, bool newestFirst)
{
	std::lock_guard<std::mutex> lock(d->ringMtx);
	const int n = (int)std::min<size_t>(d->ringCount, (size_t)std::max(0, max));
	for (int i = 0; i < n; i++) {
		// i-th newest
		const size_t idx = (d->ringHead + d->ring.size() - 1 - (size_t)i) % d->ring.size();
		msdos_trace_entry& e = out[newestFirst ? i : n - 1 - i];
		e = d->ring[idx];
		e.len = (uint8_t)trace_length(e, nullptr, nullptr);
		if (!e.len)
			e.len = 1;
	}
	return n;
}

int msdosdebug_trace_read(msdosdebug* d, msdos_trace_entry* out, int max)
{
	return ring_copy(d, out, max, true);
}

int msdosdebug_history_copy(msdosdebug* d, msdos_trace_entry* out, int max)
{
	return ring_copy(d, out, max, false);
}

int msdosdebug_trace_format(const msdos_trace_entry* e, char* dst, int dstsz)
{
	ZydisDecodedInstruction ins;
	ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
	char text[96] = "";
	char bytes[40] = "";
	const int len = trace_length(*e, &ins, ops);
	if (len > 0) {
		ZydisFormatter fmt;
		ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);
		ZydisFormatterSetProperty(&fmt, ZYDIS_FORMATTER_PROP_FORCE_SEGMENT, ZYAN_FALSE);
		ZydisFormatterFormatInstruction(&fmt, &ins, ops, ins.operand_count_visible, text, sizeof text,
		                                e->eip, nullptr);
	} else {
		snprintf(text, sizeof text, "db 0x%02X", e->bytes[0]);
	}
	int p = 0;
	for (int i = 0; i < (len > 0 ? len : 1) && p < (int)sizeof bytes - 4; i++)
		p += snprintf(bytes + p, sizeof bytes - (size_t)p, "%s%02X", i ? " " : "", e->bytes[i]);
	char regs[200];
	if (e->is386)
		snprintf(regs, sizeof regs,
		         "EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X DS=%04X ES=%04X SS=%04X",
		         e->eax, e->ebx, e->ecx, e->edx, e->esi, e->edi, e->ebp, e->esp, e->ds, e->es, e->ss);
	else
		snprintf(regs, sizeof regs,
		         "AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%04X ES=%04X SS=%04X",
		         e->eax & 0xFFFF, e->ebx & 0xFFFF, e->ecx & 0xFFFF, e->edx & 0xFFFF, e->esi & 0xFFFF,
		         e->edi & 0xFFFF, e->ebp & 0xFFFF, e->esp & 0xFFFF, e->ds, e->es, e->ss);
	const uint32_t f = e->eflags;
	char flags[16];
	snprintf(flags, sizeof flags, "%c%c%c%c%c%c%c%c", (f & 0x800) ? 'O' : '.', (f & 0x400) ? 'D' : '.',
	         (f & 0x200) ? 'I' : '.', (f & 0x80) ? 'S' : '.', (f & 0x40) ? 'Z' : '.',
	         (f & 0x10) ? 'A' : '.', (f & 4) ? 'P' : '.', (f & 1) ? 'C' : '.');
	return snprintf(dst, (size_t)dstsz, "%04X:%0*X  %-20s %-28s %s  %s%s%s", e->cs, e->code32 ? 8 : 4,
	                e->eip, bytes, text, regs, flags, e->repeat ? "  x" : "",
	                e->repeat ? std::to_string(e->repeat + 1).c_str() : "");
}

int msdosdebug_trace_match(const msdos_trace_entry* e, int filter, uint32_t low, uint32_t high)
{
	if (filter == MSDOS_TRACE_RANGE)
		return e->linear >= low && e->linear <= high;
	if (filter != MSDOS_TRACE_BRANCHES)
		return 1;
	ZydisDecodedInstruction ins;
	ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
	if (!trace_length(*e, &ins, ops))
		return 0;
	switch (ins.mnemonic) {
	case ZYDIS_MNEMONIC_CALL: case ZYDIS_MNEMONIC_JMP: case ZYDIS_MNEMONIC_RET: case ZYDIS_MNEMONIC_IRET:
	case ZYDIS_MNEMONIC_IRETD: case ZYDIS_MNEMONIC_INT: case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_INTO:
	case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE: case ZYDIS_MNEMONIC_LOOPNE:
	case ZYDIS_MNEMONIC_JCXZ: case ZYDIS_MNEMONIC_JECXZ: case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_JBE:
	case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_JNBE:
	case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_JNP:
	case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_JP:
	case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_JZ:
		return 1;
	default:
		return 0;
	}
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

static void sym_add(msdosdebug* d, uint32_t linear, const std::string& name)
{
	auto range = d->syms.equal_range(linear);
	for (auto it = range.first; it != range.second; ++it)
		if (it->second == name)
			return;
	d->syms.emplace(linear, name);
}

int msdosdebug_load_symbols(msdosdebug* d, const char* path, uint16_t load_seg, char* msg, int msgsz)
{
	FILE* f = fopen(path, "r");
	if (!f) {
		put(msg, msgsz, std::string("Cannot read ") + path);
		return -1;
	}
	char line[512];
	int n = 0;
	std::vector<std::pair<uint32_t, std::string>> found;
	while (fgets(line, sizeof line, f)) {
		unsigned seg, off;
		char name[256];
		// MS LINK " 0000:0010       _main", WLINK "0000:0010+     _main_"
		if (sscanf(line, " %4x:%8x%*[+* ] %255s", &seg, &off, name) == 3
		    || sscanf(line, " %4x:%8x %255s", &seg, &off, name) == 3) {
			if (!isalpha((unsigned char)name[0]) && name[0] != '_' && name[0] != '$' && name[0] != '@')
				continue;
			found.emplace_back((((uint32_t)seg + load_seg) << 4) + off, name);
			n++;
		} else if (!strchr(line, ':') && sscanf(line, " %8x %255s", &off, name) == 2 &&
		           (isalpha((unsigned char)name[0]) || name[0] == '_' || name[0] == '@' ||
		            name[0] == '$')) {
			// "HHHHH NAME": a linear address and a name
			found.emplace_back(off, name);
			n++;
		}
	}
	fclose(f);
	{
		std::lock_guard<std::mutex> lock(d->symMtx);
		for (auto& kv : found)
			sym_add(d, kv.first, kv.second);
	}
	d->symGen.fetch_add(1);
	d->generation.fetch_add(1);
	put(msg, msgsz, std::to_string(n) + " symbols from " + path);
	return n > 0 ? 0 : -1;
}

void msdosdebug_clear_symbols(msdosdebug* d)
{
	{
		std::lock_guard<std::mutex> lock(d->symMtx);
		d->syms.clear();
	}
	d->symGen.fetch_add(1);
	d->generation.fetch_add(1);
}

int msdosdebug_set_label(msdosdebug* d, uint32_t linear, const char* name)
{
	if (!name)
		return -1;
	std::string n = name;
	for (char c : n)
		if (isspace((unsigned char)c))
			return -1;
	{
		std::lock_guard<std::mutex> lock(d->symMtx);
		if (n.empty()) {
			d->syms.erase(linear);
		} else {
			if (!isalpha((unsigned char)n[0]) && n[0] != '_' && n[0] != '@' && n[0] != '$')
				return -1;
			sym_add(d, linear, n);
		}
	}
	d->symGen.fetch_add(1);
	d->generation.fetch_add(1);
	return 0;
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
	const uint32_t key = it->first;
	it = d->syms.lower_bound(key);
	if (linear == key)
		return snprintf(dst, (size_t)dstsz, "%s", it->second.c_str());
	return snprintf(dst, (size_t)dstsz, "%s+%X", it->second.c_str(), linear - key);
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

// the sorted list the Symbols tab walks, rebuilt when the table changed
static void sym_list(msdosdebug* d)
{
	const unsigned gen = d->symGen.load();
	if (d->symListGen == gen)
		return;
	d->symList.assign(d->syms.begin(), d->syms.end());
	std::sort(d->symList.begin(), d->symList.end());
	d->symListGen = gen;
}

int msdosdebug_symbols_count(msdosdebug* d)
{
	std::lock_guard<std::mutex> lock(d->symMtx);
	sym_list(d);
	return (int)d->symList.size();
}

int msdosdebug_symbols_get(msdosdebug* d, int index, uint32_t* linear, char* name, int namesz)
{
	std::lock_guard<std::mutex> lock(d->symMtx);
	sym_list(d);
	if (index < 0 || (size_t)index >= d->symList.size())
		return 0;
	if (linear)
		*linear = d->symList[(size_t)index].first;
	if (name && namesz > 0)
		snprintf(name, (size_t)namesz, "%s", d->symList[(size_t)index].second.c_str());
	return 1;
}

int msdosdebug_symbols_filter(msdosdebug* d, const char* text, int* out, int max)
{
	std::lock_guard<std::mutex> lock(d->symMtx);
	sym_list(d);
	const std::string want = lower(text ? text : "");
	int n = 0;
	for (size_t i = 0; i < d->symList.size() && n < max; i++)
		if (want.empty() || lower(d->symList[i].second).find(want) != std::string::npos)
			out[n++] = (int)i;
	return n;
}

unsigned msdosdebug_symbols_generation(msdosdebug* d)
{
	return d->symGen.load();
}

// ---------------------------------------------------------------------------
// memory map and search
// ---------------------------------------------------------------------------

static void region(msdos_region* out, int& n, int max, uint32_t low, uint32_t high, int r, int w,
                   const std::string& name)
{
	if (n >= max)
		return;
	msdos_region& g = out[n++];
	g.low = low;
	g.high = high;
	g.readable = r;
	g.writable = w;
	snprintf(g.name, sizeof g.name, "%s", name.c_str());
}

int msdosdebug_memory_map(msdosdebug* d, msdos_region* out, int max)
{
	int n = 0;
	msdos_d86_meminfo m = {};
	char vid[64] = "none";
	bool text = false;
	if (!d->Run([&] {
		    d86_mem_info(&m);
		    text = m.text_mode != 0;
		    snprintf(vid, sizeof vid, "%s", m.video);
	    }))
		return 0;
	const uint32_t conv = m.ram_kb >= 640 ? 640 : m.ram_kb;
	const std::string cs = std::to_string(conv) + " KB";
	if (conv)
		region(out, n, max, 0, conv * 1024 - 1, 1, 1, "Conventional memory (" + cs + ")");
	if (conv < 640)
		region(out, n, max, conv * 1024, 0x9FFFF, 0, 0, "Not populated");
	region(out, n, max, 0xA0000, 0xAFFFF, 0, 1, "Video memory: graphics window (not shown)");
	region(out, n, max, 0xB0000, 0xBFFFF, text, 1,
	       std::string("Video memory (") + vid + (text ? ", text mode)" : ", graphics mode: not shown)"));
	region(out, n, max, 0xC0000, 0xEFFFF, 1, 1, "Option ROMs / upper memory blocks");
	region(out, n, max, 0xF0000, 0xFFFFF, 1, 0, "System BIOS (ROM)");
	if (m.bits > 20) {
		const uint64_t top = m.bits >= 32 ? 0xFFFFFFFFull : (1ull << m.bits) - 1;
		uint64_t ext_end = m.ram_kb > 1024 ? (uint64_t)m.ram_kb * 1024 - 1 : 0;
		const std::string a20 = m.a20 ? "A20 on" : "A20 off: bit 20 reads as 0";
		if (ext_end) {
			if (ext_end > top)
				ext_end = top;
			region(out, n, max, 0x100000, (uint32_t)ext_end, 1, 1,
			       "Extended memory (" + std::to_string(m.ram_kb - 1024) + " KB, " + a20 + ")");
		} else {
			ext_end = 0xFFFFF;
		}
		if (ext_end < top)
			region(out, n, max, (uint32_t)ext_end + 1, (uint32_t)top, 0, 0,
			       std::string("Not populated (") + a20 + ")");
	}
	return n;
}

static bool pat_match(const uint8_t* mem, const uint8_t* pat, int count, uint8_t mask, int flags)
{
	for (int i = 0; i < count; i++) {
		uint8_t a = mem[i], b = pat[i];
		if (flags & MSDOS_SEARCH_NOCASE) {
			a = (uint8_t)tolower(a);
			b = (uint8_t)tolower(b);
		}
		if ((a & mask) != (b & mask))
			return false;
	}
	return true;
}

// Scans the readable regions in from..to for matches; fn is called with each start address.
static int scan(msdosdebug* d, const uint8_t* pat, int count, uint8_t mask, int flags, uint64_t from,
                uint64_t to, bool backward, const std::function<bool(uint32_t)>& found)
{
	if (count < 1 || count > 64)
		return -1;
	msdos_region map[12];
	const int regions = msdosdebug_memory_map(d, map, 12);
	if (!regions)
		return -1;
	int rc = 0;
	d->Run([&] {
		std::vector<uint8_t> buf;
		for (int ri = backward ? regions - 1 : 0; ri >= 0 && ri < regions; ri += backward ? -1 : 1) {
			const msdos_region& g = map[ri];
			if (!g.readable)
				continue;
			// window of start addresses inside this region
			uint64_t lo = std::max<uint64_t>(from, g.low), hi = std::min<uint64_t>(to, g.high);
			if (lo > hi)
				continue;
			const uint64_t end = std::min<uint64_t>((uint64_t)g.high + 1, hi + count);   // bytes read
			const size_t len = (size_t)(end - lo);
			buf.assign(len, 0xFF);
			// 4K at a time keeps one bad page from costing the rest
			for (size_t off = 0; off < len; off += 4096)
				d86_read((uint32_t)(lo + off), buf.data() + off, (int)std::min<size_t>(4096, len - off));
			if (len < (size_t)count)
				continue;
			const size_t last = len - (size_t)count;
			if (backward) {
				for (size_t i = last + 1; i-- > 0;) {
					if (lo + i > hi)
						continue;
					if (pat_match(buf.data() + i, pat, count, mask, flags) && !found((uint32_t)(lo + i))) {
						rc = 1;
						return;
					}
				}
			} else {
				for (size_t i = 0; i <= last; i++) {
					if (lo + i > hi)
						break;
					if (pat_match(buf.data() + i, pat, count, mask, flags) && !found((uint32_t)(lo + i))) {
						rc = 1;
						return;
					}
				}
			}
		}
	});
	return 0;
}

int msdosdebug_search(msdosdebug* d, const uint8_t* pat, int count, uint8_t mask, int flags,
                      uint32_t from, uint32_t to, uint32_t* out, int max)
{
	int n = 0;
	if (to < from)
		return 0;
	if (scan(d, pat, count, mask, flags, from, to, false, [&](uint32_t a) {
		    if (n < max)
			    out[n++] = a;
		    return n < max;
	    }) < 0)
		return -1;
	return n;
}

int msdosdebug_search_next(msdosdebug* d, const uint8_t* pat, int count, uint8_t mask, int flags,
                           uint32_t from, int backward, uint32_t* found)
{
	int hit = 0;
	auto take = [&](uint32_t a) {
		*found = a;
		hit = 1;
		return false;
	};
	if (!backward) {
		if (from != 0xFFFFFFFFu && scan(d, pat, count, mask, flags, (uint64_t)from + 1, 0xFFFFFFFFull, false, take) < 0)
			return -1;
		if (!hit && scan(d, pat, count, mask, flags, 0, from, false, take) < 0)
			return -1;
	} else {
		if (from > 0 && scan(d, pat, count, mask, flags, 0, (uint64_t)from - 1, true, take) < 0)
			return -1;
		if (!hit && scan(d, pat, count, mask, flags, from, 0xFFFFFFFFull, true, take) < 0)
			return -1;
	}
	return hit;
}

// ---------------------------------------------------------------------------
// the machine, sound
// ---------------------------------------------------------------------------

int msdosdebug_io_text(msdosdebug* d, char* dst, int dstsz)
{
	std::vector<char> big(8192);
	int n = 0;
	if (!d->Run([&] { n = d86_machine_info(big.data(), (int)big.size()); }))
		return snprintf(dst, (size_t)dstsz, "The PC is off\n");
	std::string t(big.data(), (size_t)n);
	char uart[1024];
	t += "\nFujiNet link\n";
	msdosdebug_uart(d, uart, sizeof uart);
	t += uart;
	if (gSoundText) {
		const char* more = gSoundText();
		if (more && *more)
			t += std::string("\n") + more;
	}
	return snprintf(dst, (size_t)dstsz, "%s", t.c_str());
}

void msdosdebug_set_sound_text(const char* (*fn)(void))
{
	gSoundText = fn;
}

int msdosdebug_sound_text(msdosdebug* d, char* dst, int dstsz)
{
	int n = 0;
	if (!d->Run([&] { n = msdos_snd_text(dst, dstsz); }))
		return snprintf(dst, (size_t)dstsz, "The PC is off\n");
	return n;
}

int msdosdebug_sound_get(msdosdebug* d, struct msdos_snd_state* out)
{
	int rc = -1;
	if (!d->Run([&] { rc = msdos_snd_get(out) ? 0 : -1; }))
		return -1;
	return rc;
}

// ---------------------------------------------------------------------------
// completions and save
// ---------------------------------------------------------------------------

int msdosdebug_save(msdosdebug* d, const char* kind, const char* path, char* msg, int msgsz)
{
	const std::string k = lower(kind ? kind : "");
	std::string text;
	std::vector<uint8_t> mem;
	if (k == "regs") {
		char out[1024];
		msdosdebug_command(d, "r", out, sizeof out);
		text = out;
	} else if (k == "dis") {
		msdos_cpu_regs r;
		if (msdosdebug_regs(d, &r) != 0) {
			put(msg, msgsz, "The PC is off");
			return -1;
		}
		std::vector<msdos_dis_line> lines(4096);
		const int n = msdosdebug_disassemble(d, r.cs, r.eip, 4096, lines.data());
		for (int i = 0; i < n; i++) {
			char row[200];
			snprintf(row, sizeof row, "%04X:%04X  %-20s %s%s%s\n", lines[(size_t)i].seg,
			         lines[(size_t)i].off, lines[(size_t)i].bytes, lines[(size_t)i].text,
			         lines[(size_t)i].symbol[0] ? "   ; " : "", lines[(size_t)i].symbol);
			text += row;
		}
	} else if (k == "mem") {
		mem.resize(0x100000);
		if (!d->Run([&] { d86_read(0, mem.data(), (int)mem.size()); })) {
			put(msg, msgsz, "The PC is off");
			return -1;
		}
	} else {
		put(msg, msgsz, "save <dis|mem|regs> <file>");
		return -1;
	}
	FILE* f = fopen(path, "wb");
	if (!f) {
		put(msg, msgsz, std::string("Cannot write ") + path);
		return -1;
	}
	if (k == "mem")
		fwrite(mem.data(), 1, mem.size(), f);
	else
		fwrite(text.data(), 1, text.size(), f);
	fclose(f);
	put(msg, msgsz, std::string("saved ") + path);
	return 0;
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

int msdosdebug_io_in(msdosdebug* d, uint16_t port, uint8_t* value)
{
	uint8_t v = 0xFF;
	const bool ok = d->Run([&] { v = inb(port); });
	if (value)
		*value = v;
	return ok ? 0 : -1;
}

int msdosdebug_io_out(msdosdebug* d, uint16_t port, uint8_t value)
{
	return d->Run([&] { outb(port, value); }) ? 0 : -1;
}

int msdosdebug_video_snap(msdosdebug* d, msdos_vid_snap* s)
{
	int rc = -1;
	if (!d->Run([&] { rc = d86_video_snap(s); })) {
		s->card = MSDOS_VID_OFF;
		return -1;
	}
	if (rc != 0)
		s->card = MSDOS_VID_OFF;
	return rc;
}

