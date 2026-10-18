/*
 * PcHost -- 86Box on a thread of its own, with the family's contracts around
 * it: a serial-stamped XRGB frame slot, a mixed and resampled stereo output
 * the SDL device pulls, queued keyboard and mouse input, and a job queue for
 * everything that must touch the emulator.
 *
 * 86Box is a single global instance, set up once per process (pc_init is not
 * re-entrant). Every 86Box call runs on the emulation thread, in a job or a
 * callback. Jobs come in two kinds:
 *   Frame   between pc_run() slices only (machine, media, power)
 *   Any     also while the debugger holds the machine (inspection)
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_PC_HOST_H
#define MSDOS_PC_HOST_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>

class PcHost
{
public:
	enum class Kind { Frame, Any };

	struct Callbacks
	{
		// emulation thread, before each pc_run() slice
		std::function<void()> beforeSlice;
		// emulation thread, once a second (after pc_onesec)
		std::function<void()> onSecond;
		// emulation thread: the machine stopped in the debugger
		std::function<void()> onStopped;
		// any thread: a message 86Box wanted shown
		std::function<void(int flags, const char* header, const char* text)> onMessage;
		// any thread: drive activity (86Box ui_sb tags)
		std::function<void(int tag, int active)> onActivity;
	};

	struct Config
	{
		std::string vmDir;                 // 86box.cfg, nvr/
		std::vector<std::string> romDirs;  // searched in order
	};

	static constexpr int MaxWidth = 2048;
	static constexpr int MaxHeight = 1536;
	static constexpr int AudioRate = 48000;

	PcHost();
	~PcHost();
	PcHost(const PcHost&) = delete;
	PcHost& operator=(const PcHost&) = delete;

	void SetCallbacks(Callbacks cb) { _cb = std::move(cb); }

	// pc_init .. pc_reset_hard_init on the new thread; once per process.
	bool Start(const Config& config, std::string& error);
	// Powers off and tears 86Box down; it cannot start again in this process.
	void Stop();
	bool IsRunning() const { return _running.load(); }
	static bool OnEmulationThread();

	// ---- jobs
	bool Call(const std::function<void()>& fn, Kind kind = Kind::Frame);
	void Post(std::function<void()> fn, Kind kind = Kind::Frame);

	// ---- machine
	void HardReset();                  // the reset button
	void SendCtrlAltDel();             // the three-finger salute
	void SetPaused(bool on);
	bool Paused() const { return _paused.load(); }
	void SetWarp(bool on) { _warp.store(on); }
	bool Warp() const { return _warp.load(); }
	int SpeedPercent() const { return _speed.load(); }

	// ---- input (any thread; applied on the emulation thread in order)
	void Key(uint16_t scancode, bool down);   // set 1, 0x100 = E0 prefix
	void MouseMotion(int dx, int dy);
	void MouseButtons(int mask);              // bit 0 left, 1 right, 2 middle
	void MouseWheel(int dz);
	void ReleaseAllKeys();

	// ---- video
	bool CopyFrame(uint32_t* dst, size_t maxPixels, int& width, int& height,
	               uint64_t* serialInOut);
	void ClearFrame();

	// ---- audio (the SDL callback)
	void FillAudio(float* stereo, uint32_t frames);
	void SetVolume(int percent);
	void SetStreamEnabled(int stream, bool on);
	void FlushAudio();

	// ---- the debugger's stop
	bool Stopped() const { return _stopped.load(); }
	void Resume();
	unsigned ParkSerial() const { return _parkSerial.load(); }
	// emulation thread, from the instruction hook: stop before cs:ip
	void RequestStop() { _stopRequest.store(true); }

	// bridge callbacks (msdos_host.h)
	void OnFrame(const uint32_t* pixels, int stride, int w, int h);
	void OnAudio(int stream, const void* buf, int frames, bool isFloat, int rate);

private:
	static void* ThreadMain(void* self);
	void ThreadBody();
	bool InitCore(std::string& error);
	void RunJobs(bool parked);
	void DrainInput();
	void Park();

	Callbacks _cb;
	Config _config;
	pthread_t _thread {};
	bool _threadStarted = false;
	std::atomic<bool> _running { false };
	std::atomic<bool> _quit { false };
	std::atomic<bool> _paused { false };
	std::atomic<bool> _warp { false };
	std::atomic<int> _speed { 100 };
	std::atomic<bool> _stopped { false };
	std::atomic<bool> _stopRequest { false };
	std::atomic<bool> _resume { false };
	std::atomic<unsigned> _parkSerial { 0 };

	std::mutex _startLock;
	std::condition_variable _startCv;
	int _startState = 0;               // 0 starting, 1 running, -1 failed
	std::string _startError;

	// frame slot
	mutable std::mutex _frameLock;
	std::vector<uint32_t> _frame;
	int _frameW = 640, _frameH = 480;
	uint64_t _frameSerial = 0;

	// audio: one SPSC ring of 48 kHz float stereo per 86Box stream
	struct Ring
	{
		static constexpr uint32_t Frames = 16384;
		static constexpr uint32_t Target = 2400;     // 50 ms
		std::vector<float> data = std::vector<float>(Frames * 2, 0.0f);
		std::atomic<uint32_t> read { 0 };
		std::atomic<uint32_t> write { 0 };
		std::atomic<bool> flush { false };
		std::atomic<bool> enabled { true };
		double pos = 0.0;
		float last[2] = { 0.0f, 0.0f };
	};
	static constexpr int Streams = 6;
	Ring _rings[Streams];
	std::atomic<int> _volume { 100 };
	std::vector<float> _scratch;

	// input
	struct InputEvent
	{
		enum Type : uint8_t { Key, Motion, Buttons, Wheel } type;
		uint16_t code;
		int32_t a, b;
	};
	std::mutex _inputLock;
	std::vector<InputEvent> _input;
	std::vector<InputEvent> _inputWork;
	uint8_t _keysDown[512] = {};

	// jobs
	std::mutex _jobLock;
	std::condition_variable _jobCv;
	std::deque<std::function<void()>> _frameJobs;
	std::deque<std::function<void()>> _anyJobs;
	std::atomic<bool> _jobsPending { false };
};

#endif
