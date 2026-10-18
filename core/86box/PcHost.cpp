/*
 * PcHost -- see PcHost.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#endif

#include <SDL3/SDL_timer.h>

#include "PcHost.h"

extern "C" {
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/keyboard.h>
#include <86box/mouse.h>
#include <86box/nvr.h>
#include <86box/plat.h>
#include <86box/video.h>
#include "msdos_host.h"
}

namespace {

int64_t MonoNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// macOS gives secondary threads 512K; leave the CPU cores room.
constexpr size_t EmulationThreadStack = 8u << 20;

// Ctrl+Alt+Del is held this long, as a hand would.
constexpr int64_t CadHoldNs = 120'000'000;

thread_local bool tOnEmulationThread = false;

PcHost* gHost = nullptr;

void CbFrame(void*, const uint32_t* pixels, int stride, int w, int h)
{
	if (gHost)
		gHost->OnFrame(pixels, stride, w, h);
}

void CbAudio(void*, int stream, const void* buf, int frames, int isFloat, int rate)
{
	if (gHost)
		gHost->OnAudio(stream, buf, frames, isFloat != 0, rate);
}

} // namespace

PcHost::PcHost()
	: _frame((size_t)MaxWidth * MaxHeight, 0xff000000u)
{
}

PcHost::~PcHost()
{
	Stop();
}

bool PcHost::OnEmulationThread()
{
	return tOnEmulationThread;
}

bool PcHost::Start(const Config& config, std::string& error)
{
	if (_running.load())
		return true;
	static bool startedOnce = false;
	if (startedOnce) {
		error = "86Box has already run in this process";
		return false;
	}
	if (gHost && gHost != this) {
		error = "another PC is already running in this process";
		return false;
	}
	gHost = this;
	_config = config;
	_quit.store(false);
	_stopped.store(false);
	_startState = 0;
	_startError.clear();

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, EmulationThreadStack);
	_running.store(true);
	if (pthread_create(&_thread, &attr, &PcHost::ThreadMain, this) != 0) {
		pthread_attr_destroy(&attr);
		_running.store(false);
		error = "could not start the emulation thread";
		return false;
	}
	pthread_attr_destroy(&attr);
	_threadStarted = true;
	startedOnce = true;

	std::unique_lock<std::mutex> lock(_startLock);
	_startCv.wait(lock, [this] { return _startState != 0; });
	if (_startState < 0) {
		lock.unlock();
		error = _startError;
		Stop();
		return false;
	}
	return true;
}

void PcHost::Stop()
{
	if (!_threadStarted)
		return;
	_quit.store(true);
	_resume.store(true);
	{
		std::lock_guard<std::mutex> lock(_jobLock);
	}
	_jobCv.notify_all();
	pthread_join(_thread, nullptr);
	_threadStarted = false;
	_running.store(false);
	if (gHost == this)
		gHost = nullptr;
	{
		std::lock_guard<std::mutex> lock(_jobLock);
		_frameJobs.clear();
		_anyJobs.clear();
		_jobsPending.store(false);
	}
	_jobCv.notify_all();
}

void* PcHost::ThreadMain(void* self)
{
	static_cast<PcHost*>(self)->ThreadBody();
	return nullptr;
}

bool PcHost::InitCore(std::string& error)
{
	msdos_host_callbacks hcb = {};
	hcb.user = this;
	hcb.frame = CbFrame;
	hcb.audio = CbAudio;
	hcb.activity = [](void* user, int tag, int active) {
		auto* self = static_cast<PcHost*>(user);
		if (self->_cb.onActivity)
			self->_cb.onActivity(tag, active);
	};
	hcb.message = [](void* user, int flags, const char* header, const char* text) {
		auto* self = static_cast<PcHost*>(user);
		if (self->_cb.onMessage)
			self->_cb.onMessage(flags, header, text);
	};
	msdos_host_set_callbacks(&hcb);

	std::vector<const char*> roms;
	for (const auto& d : _config.romDirs)
		roms.push_back(d.c_str());
	msdos_host_set_dirs(_config.vmDir.c_str(), roms.data(), (int)roms.size());

	// plat_timer_read() is SDL's performance counter.
	timer_freq = SDL_GetPerformanceFrequency();

	std::string vmdir = _config.vmDir;
	char arg0[] = "86Box";
	char argP[] = "-P";
	char argN[] = "-N";
	char* argv[] = { arg0, argP, vmdir.data(), argN, nullptr };
	if (!pc_init(4, argv)) {
		error = "86Box did not start";
		return false;
	}
	if (!pc_init_roms()) {
		error = "No PC BIOS is available: import one first";
		return false;
	}
	pc_init_modules();
	for (int i = 1; i < GFXCARD_MAX; i++)
		gfxcard[i] = 0;
	msdos_host_blit_init();
	pc_reset_hard_init();
	// 86Box comes up paused, for its frontend to let go.
	plat_pause(0);
	return true;
}

void PcHost::ThreadBody()
{
	tOnEmulationThread = true;
	is_cpu_thread = 1;
#if defined(_WIN32)
	timeBeginPeriod(1);
#endif
	std::string err;
	const bool ok = InitCore(err);
	{
		std::lock_guard<std::mutex> lock(_startLock);
		_startState = ok ? 1 : -1;
		_startError = err;
	}
	_startCv.notify_all();
	if (!ok) {
		msdos_host_set_callbacks(nullptr);
		tOnEmulationThread = false;
#if defined(_WIN32)
		timeEndPeriod(1);
#endif
		return;
	}

	// 86Box runs in 1 ms slices (10 ms with force_10ms), paced to the clock
	// the way its own SDL frontend does it; a backlog over 50 ms is dropped
	// rather than raced through.
	const int64_t unitNs = (force_10ms ? 10 : 1) * 1'000'000LL;
	int64_t old = MonoNs();
	int64_t backlog = 0;
	int64_t lastSecond = old;
	int slices = 0, slicesThisSecond = 0;

	while (!_quit.load()) {
		RunJobs(false);
		if (_quit.load())
			break;
		DrainInput();

		if (_stopRequest.exchange(false))
			msdos_host_debug_set_stopped(1);
		if (msdos_host_debug_stopped()) {
			Park();
			old = MonoNs();
			backlog = 0;
			continue;
		}

		const int64_t now = MonoNs();
		backlog += now - old;
		old = now;

		if (now - lastSecond >= 1'000'000'000LL) {
			lastSecond += 1'000'000'000LL;
			if (now - lastSecond > 1'000'000'000LL)
				lastSecond = now;
			if (!_paused.load()) {
				pc_onesec();
				_speed.store(slicesThisSecond * 100 / (force_10ms ? 100 : 1000));
			}
			slicesThisSecond = 0;
			if (_cb.onSecond)
				_cb.onSecond();
		}

		// dopause: 86Box's own pause (an ACPI soft-off, a fatal error).
		if (_paused.load() || dopause) {
			std::unique_lock<std::mutex> lock(_jobLock);
			_jobCv.wait_for(lock, std::chrono::milliseconds(20),
			                [this] { return _jobsPending.load() || _quit.load() || !_paused.load(); });
			backlog = 0;
			old = MonoNs();
			continue;
		}

		const bool warp = _warp.load();
		if (backlog > 0 || warp) {
			backlog -= unitNs;
			if (backlog > 50'000'000LL || warp)
				backlog = 0;
			if (_cb.beforeSlice)
				_cb.beforeSlice();
			pc_run();
			slicesThisSecond++;
			if (++slices >= (force_10ms ? 200 : 2000) && nvr_dosave) {
				nvr_save();
				nvr_dosave = 0;
				slices = 0;
			}
		} else {
			std::unique_lock<std::mutex> lock(_jobLock);
			_jobCv.wait_for(lock, std::chrono::nanoseconds(std::min<int64_t>(-backlog, unitNs)),
			                [this] { return _jobsPending.load() || _quit.load(); });
		}
	}

	// Power off: disks flushed, CMOS kept, everything torn down.
	pc_close(nullptr);
	msdos_host_set_callbacks(nullptr);
	tOnEmulationThread = false;
#if defined(_WIN32)
	timeEndPeriod(1);
#endif
}

// ---- jobs ------------------------------------------------------------------

namespace {
struct Job
{
	std::function<void()> fn;
	bool done = false;
};
}

bool PcHost::Call(const std::function<void()>& fn, Kind kind)
{
	if (tOnEmulationThread) {
		fn();
		return true;
	}
	if (!_running.load())
		return false;
	auto job = std::make_shared<Job>();
	job->fn = fn;
	{
		std::lock_guard<std::mutex> lock(_jobLock);
		// done is set under the lock the caller waits with: set bare, the
		// notify could land between the caller's check and its sleep, and
		// the caller would sleep for good.
		auto wrapped = [this, job] {
			job->fn();
			std::lock_guard<std::mutex> done(_jobLock);
			job->done = true;
		};
		if (kind == Kind::Any)
			_anyJobs.push_back(wrapped);
		else
			_frameJobs.push_back(wrapped);
		_jobsPending.store(true);
	}
	_jobCv.notify_all();

	std::unique_lock<std::mutex> lock(_jobLock);
	_jobCv.wait(lock, [&] { return job->done || !_running.load() || _quit.load(); });
	return job->done;
}

void PcHost::Post(std::function<void()> fn, Kind kind)
{
	if (tOnEmulationThread && kind == Kind::Any) {
		fn();
		return;
	}
	if (!_running.load())
		return;
	{
		std::lock_guard<std::mutex> lock(_jobLock);
		if (kind == Kind::Any)
			_anyJobs.push_back(std::move(fn));
		else
			_frameJobs.push_back(std::move(fn));
		_jobsPending.store(true);
	}
	_jobCv.notify_all();
}

// The machine is always between slices here, so a parked machine can run
// Frame jobs too; `parked` only says not to wait for one.
void PcHost::RunJobs(bool parked)
{
	(void)parked;
	if (!_jobsPending.load())
		return;
	for (;;) {
		std::function<void()> fn;
		{
			std::lock_guard<std::mutex> lock(_jobLock);
			if (!_anyJobs.empty()) {
				fn = std::move(_anyJobs.front());
				_anyJobs.pop_front();
			} else if (!_frameJobs.empty()) {
				fn = std::move(_frameJobs.front());
				_frameJobs.pop_front();
			} else {
				_jobsPending.store(false);
				break;
			}
		}
		fn();
		_jobCv.notify_all();
	}
}

// ---- the debugger's stop ---------------------------------------------------

void PcHost::Park()
{
	_resume.store(false);
	_parkSerial.fetch_add(1);
	_stopped.store(true);
	FlushAudio();
	if (_cb.onStopped)
		_cb.onStopped();
	while (!_resume.load() && !_quit.load() && msdos_host_debug_stopped()) {
		RunJobs(true);
		DrainInput();
		std::unique_lock<std::mutex> lock(_jobLock);
		_jobCv.wait_for(lock, std::chrono::milliseconds(50), [this] {
			return _jobsPending.load() || _resume.load() || _quit.load();
		});
	}
	msdos_host_debug_set_stopped(0);
	{
		std::lock_guard<std::mutex> lock(_jobLock);
		_stopped.store(false);
		_parkSerial.fetch_add(1);
	}
	_jobCv.notify_all();
}

void PcHost::Resume()
{
	const unsigned serial = _parkSerial.load();
	const bool wasStopped = _stopped.load();
	_resume.store(true);
	_jobCv.notify_all();
	if (!wasStopped || tOnEmulationThread)
		return;
	std::unique_lock<std::mutex> lock(_jobLock);
	_jobCv.wait_for(lock, std::chrono::seconds(2), [&] {
		return _parkSerial.load() != serial || !_running.load() || _quit.load();
	});
}

// ---- machine ---------------------------------------------------------------

void PcHost::HardReset()
{
	ReleaseAllKeys();
	Post([] {
		if (dopause)
			do_pause(0);
		pc_reset_hard();
	});
}

void PcHost::SendCtrlAltDel()
{
	std::lock_guard<std::mutex> lock(_inputLock);
	const int64_t release = MonoNs() + CadHoldNs;
	for (uint16_t sc : { (uint16_t)0x1d, (uint16_t)0x38, (uint16_t)0x153 })
		_input.push_back({ InputEvent::Key, sc, 1, 0 });
	for (uint16_t sc : { (uint16_t)0x153, (uint16_t)0x38, (uint16_t)0x1d })
		_input.push_back({ InputEvent::Key, sc, 0, (int32_t)(release / 1'000'000) });
}

void PcHost::SetPaused(bool on)
{
	_paused.store(on);
	if (on)
		FlushAudio();
	_jobCv.notify_all();
}

// ---- input -----------------------------------------------------------------

void PcHost::Key(uint16_t scancode, bool down)
{
	std::lock_guard<std::mutex> lock(_inputLock);
	_input.push_back({ InputEvent::Key, scancode, down ? 1 : 0, 0 });
}

void PcHost::MouseMotion(int dx, int dy)
{
	std::lock_guard<std::mutex> lock(_inputLock);
	_input.push_back({ InputEvent::Motion, 0, dx, dy });
}

void PcHost::MouseButtons(int mask)
{
	std::lock_guard<std::mutex> lock(_inputLock);
	_input.push_back({ InputEvent::Buttons, 0, mask, 0 });
}

void PcHost::MouseWheel(int dz)
{
	std::lock_guard<std::mutex> lock(_inputLock);
	_input.push_back({ InputEvent::Wheel, 0, dz, 0 });
}

void PcHost::ReleaseAllKeys()
{
	Post([this] {
		for (int sc = 0; sc < 512; sc++)
			if (_keysDown[sc]) {
				keyboard_input(0, (uint16_t)sc);
				_keysDown[sc] = 0;
			}
		mouse_set_buttons_ex(0);
	}, Kind::Any);
}

// Events with a due time (b, in ms of MonoNs) wait for it, and so does
// everything queued after them: order is what a key sequence means.
void PcHost::DrainInput()
{
	{
		std::lock_guard<std::mutex> lock(_inputLock);
		if (_input.empty())
			return;
		const int64_t nowMs = MonoNs() / 1'000'000;
		size_t n = 0;
		while (n < _input.size()) {
			const InputEvent& e = _input[n];
			if (e.type == InputEvent::Key && e.b && e.b > nowMs)
				break;
			n++;
		}
		_inputWork.assign(_input.begin(), _input.begin() + (ptrdiff_t)n);
		_input.erase(_input.begin(), _input.begin() + (ptrdiff_t)n);
	}
	for (const InputEvent& e : _inputWork) {
		switch (e.type) {
		case InputEvent::Key: {
			uint16_t code = e.code;
			if (code < 512)
				_keysDown[code] = (uint8_t)(e.a != 0);
			// Print Screen and Pause are key sequences (86Box's Qt does the same)
			if (code == 0x137) {
				if (keyboard_recv_ui(0x38) || keyboard_recv_ui(0x138)) {
					code = 0x54;    // Alt+PrtSc is SysRq
				} else if (e.a) {
					keyboard_input(1, 0x12a);
				} else {
					keyboard_input(0, 0x137);
					code = 0x12a;
				}
			} else if (code == 0x145) {
				if (keyboard_recv_ui(0x1d) || keyboard_recv_ui(0x11d)) {
					code = 0x146;   // Ctrl+Pause is Break
				} else {
					keyboard_input(e.a, 0xe11d);
					code = 0x45;
				}
			}
			keyboard_input(e.a, code);
			break;
		}
		case InputEvent::Motion:
			mouse_scale(e.a, e.b);
			break;
		case InputEvent::Buttons:
			mouse_set_buttons_ex(e.a);
			break;
		case InputEvent::Wheel:
			mouse_set_z(e.a);
			break;
		}
	}
	_inputWork.clear();
}

// ---- video -----------------------------------------------------------------

void PcHost::OnFrame(const uint32_t* pixels, int stride, int w, int h)
{
	w = std::min(w, MaxWidth);
	h = std::min(h, MaxHeight);
	std::lock_guard<std::mutex> lock(_frameLock);
	for (int y = 0; y < h; y++) {
		const uint32_t* src = pixels + (size_t)y * stride;
		uint32_t* dst = _frame.data() + (size_t)y * w;
		for (int x = 0; x < w; x++)
			dst[x] = src[x] | 0xff000000u;
	}
	_frameW = w;
	_frameH = h;
	_frameSerial++;
}

void PcHost::ClearFrame()
{
	std::lock_guard<std::mutex> lock(_frameLock);
	std::fill(_frame.begin(), _frame.begin() + (size_t)_frameW * _frameH, 0xff000000u);
	_frameSerial++;
}

bool PcHost::CopyFrame(uint32_t* dst, size_t maxPixels, int& width, int& height,
                       uint64_t* serialInOut)
{
	std::lock_guard<std::mutex> lock(_frameLock);
	if (serialInOut && *serialInOut == _frameSerial && *serialInOut != 0)
		return false;
	const size_t n = std::min((size_t)_frameW * _frameH, maxPixels);
	if (dst)
		memcpy(dst, _frame.data(), n * sizeof(uint32_t));
	width = _frameW;
	height = _frameH;
	if (serialInOut)
		*serialInOut = _frameSerial;
	return true;
}

// ---- audio -----------------------------------------------------------------

// Each 86Box stream (cards, MT-32, CD, drive noises) arrives at its own rate
// from the CPU thread, timed by emulated time; the device runs on its own
// clock. Every stream is resampled to 48 kHz into a ring of its own, a linear
// resampler stepping up to 0.5% faster or slower to hold the ring near its
// target fill; the device callback sums the rings.
void PcHost::OnAudio(int stream, const void* buf, int frames, bool isFloat, int rate)
{
	if (stream < 0 || stream >= Streams || frames <= 0 || rate <= 0 || _warp.load())
		return;
	Ring& ring = _rings[stream];
	if (!ring.enabled.load(std::memory_order_relaxed))
		return;

	_scratch.resize((size_t)frames * 2);
	if (isFloat) {
		const float* in = static_cast<const float*>(buf);
		std::copy(in, in + (size_t)frames * 2, _scratch.begin());
	} else {
		const int16_t* in = static_cast<const int16_t*>(buf);
		for (size_t i = 0; i < (size_t)frames * 2; i++)
			_scratch[i] = in[i] / 32768.0f;
	}
	const float* in = _scratch.data();

	uint32_t w = ring.write.load(std::memory_order_relaxed);
	const uint32_t r = ring.read.load(std::memory_order_acquire);
	const uint32_t fill = (w + Ring::Frames - r) % Ring::Frames;
	const double err = ((double)fill - (double)Ring::Target) / (double)Ring::Target;
	const double step = (double)rate / AudioRate * (1.0 + std::max(-0.005, std::min(0.005, err * 0.005)));

	auto sample = [&](int index, int ch) -> float {
		return index == 0 ? ring.last[ch] : in[(index - 1) * 2 + ch];
	};
	double pos = ring.pos;
	while (pos < (double)frames) {
		const int i = (int)pos;
		const float frac = (float)(pos - i);
		const uint32_t next = (w + 1) % Ring::Frames;
		if (next != r) {
			for (int ch = 0; ch < 2; ch++) {
				const float a = sample(i, ch);
				ring.data[(size_t)w * 2 + ch] = a + (sample(i + 1, ch) - a) * frac;
			}
			w = next;
		}
		pos += step;
	}
	ring.pos = pos - (double)frames;
	ring.last[0] = in[(frames - 1) * 2];
	ring.last[1] = in[(frames - 1) * 2 + 1];
	ring.write.store(w, std::memory_order_release);
}

void PcHost::FillAudio(float* out, uint32_t frames)
{
	std::fill(out, out + (size_t)frames * 2, 0.0f);
	const float gain = (float)_volume.load(std::memory_order_relaxed) / 100.0f;
	for (Ring& ring : _rings) {
		uint32_t r = ring.read.load(std::memory_order_relaxed);
		const uint32_t w = ring.write.load(std::memory_order_acquire);
		if (ring.flush.exchange(false))
			r = w;
		for (uint32_t i = 0; i < frames && r != w; i++) {
			out[i * 2] += ring.data[(size_t)r * 2] * gain;
			out[i * 2 + 1] += ring.data[(size_t)r * 2 + 1] * gain;
			r = (r + 1) % Ring::Frames;
		}
		ring.read.store(r, std::memory_order_release);
	}
	for (uint32_t i = 0; i < frames * 2; i++)
		out[i] = std::max(-1.0f, std::min(1.0f, out[i]));
}

void PcHost::SetVolume(int percent)
{
	_volume.store(std::max(0, std::min(100, percent)));
}

void PcHost::SetStreamEnabled(int stream, bool on)
{
	if (stream >= 0 && stream < Streams) {
		_rings[stream].enabled.store(on);
		if (!on)
			_rings[stream].flush.store(true);
	}
}

void PcHost::FlushAudio()
{
	for (Ring& ring : _rings)
		ring.flush.store(true);
}
