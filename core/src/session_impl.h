/*
 * SessionImpl -- the C++ half of msdossession, shared by session.cpp,
 * input.cpp, media.cpp and debug.cpp.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_SESSION_IMPL_H
#define MSDOS_SESSION_IMPL_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "PcHost.h"
#include "session_internal.h"

struct SessionImpl
{
	explicit SessionImpl(msdossession* s);

	msdossession* s;
	PcHost host;
	bool hostStarted = false;           // 86Box has run in this process

	// ---- what the running machine was built from
	std::string appliedReset;           // prefs_reset_signature
	std::string appliedFloppy[MSDOS_FLOPPIES];
	bool appliedFujiNet = false;
	std::string Snapshot();

	// ---- emulation thread
	void BeforeSlice();
	bool ReloadConfig(std::string& err);   // config_load + hard reset
	void MountFloppies(bool force);

	// ---- the keyboard
	std::mutex keyMtx;
	uint8_t held[256] = {};             // HID usages down
	void Key(uint32_t hid, bool down);

	// ---- paste: queued keystrokes, paced by the BIOS key buffer
	struct Stroke { uint32_t hid; bool shift; int altCode; };
	std::mutex pasteMtx;
	std::deque<Stroke> paste;
	int pastePhase = 0;                 // 0 idle, 1 down, 2 released
	int64_t pasteAt = 0;                // ms: when the phase may move on
	void PasteStep();

	// ---- the on-screen keyboard
	std::vector<msdos_kbd_key> oskKeys;
	int oskLayout = -1;
	std::atomic<int> oskHeld { -1 };

	// ---- joysticks: the Controllers window's and the gamepads'
	struct Stick { int x = 0, y = 0; unsigned buttons = 0; bool pad = false; };
	std::mutex stickMtx;
	Stick sticks[MSDOS_JOYSTICKS];
	void PushSticks();

	// ---- drive lights (86Box's ui_sb tags)
	std::atomic<unsigned> activeFloppy { 0 };
	std::atomic<unsigned> activeHd { 0 };

	// ---- notices
	std::mutex noticeMtx;
	std::string notice;
	void Notice(const std::string& text);
};

SessionImpl* Impl(msdossession* s);

extern "C" {
/* debug.cpp */
void session_debug_stopped(struct msdossession* s);
void session_debug_free(struct msdossession* s);
void session_debug_trace_install(struct msdossession* s);
}

#endif
