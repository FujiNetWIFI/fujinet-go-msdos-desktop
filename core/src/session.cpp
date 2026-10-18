/*
 * msdossession -- the session: 86Box (PcHost), FujiNet, audio, gamepads and
 * the settings that describe the machine.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "session_impl.h"

extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/plat.h>
#include <86box/ui.h>
#include "msdos_host.h"
#include "screen_text.h"
int char_fujinet_connected(void);
}

SessionImpl* Impl(msdossession* s)
{
	return static_cast<SessionImpl*>(s->impl);
}

void session_set_error(struct msdossession* s, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s->last_error, sizeof s->last_error, fmt, ap);
	va_end(ap);
}

static std::string fatal_path(msdossession* s)
{
	return std::string(s->vm_dir) + "/last-fatal.txt";
}

// ---------------------------------------------------------------------------
// SessionImpl
// ---------------------------------------------------------------------------

SessionImpl::SessionImpl(msdossession* sess) : s(sess)
{
	PcHost::Callbacks cb;
	cb.beforeSlice = [this] { BeforeSlice(); };
	cb.onStopped = [this] { session_debug_stopped(s); };
	cb.onMessage = [this](int flags, const char* header, const char* text) {
		std::string msg = std::string(header ? header : "") + ": " + (text ? text : "");
		if (flags & MBX_FATAL) {
			// 86Box exits the process after this: keep it for the next start
			if (FILE* f = fopen(fatal_path(s).c_str(), "w")) {
				fputs(msg.c_str(), f);
				fclose(f);
			}
			fprintf(stderr, "msdos: 86Box fatal error: %s\n", msg.c_str());
			return;
		}
		Notice(msg);
	};
	cb.onActivity = [this](int tag, int active) {
		const int kind = tag & 0xF0, unit = tag & 0x0F;
		std::atomic<unsigned>* bits = kind == SB_FLOPPY ? &activeFloppy : kind == SB_HDD ? &activeHd : nullptr;
		if (!bits || unit > 7)
			return;
		if (active)
			bits->fetch_or(1u << unit);
		else
			bits->fetch_and(~(1u << unit));
	};
	host.SetCallbacks(cb);
}

void SessionImpl::Notice(const std::string& text)
{
	std::lock_guard<std::mutex> lock(noticeMtx);
	notice = text;
}

std::string SessionImpl::Snapshot()
{
	char sig[2048];
	prefs_reset_signature(s, sig, sizeof sig);
	return sig;
}

void SessionImpl::BeforeSlice()
{
	PasteStep();
}

// Emulation thread: 86Box takes the new 86box.cfg the way its own settings
// dialog applies a change -- everything closed, the configuration read, the
// machine built again.
bool SessionImpl::ReloadConfig(std::string& err)
{
	(void)err;
	if (dopause)
		do_pause(0);
	pc_reset_hard_close();
	config_load();
	pc_reset_hard_init();
	MountFloppies(true);
	return true;
}

// Emulation thread: the drives' images as the settings have them.
void SessionImpl::MountFloppies(bool force)
{
	for (int d = 0; d < MSDOS_FLOPPIES; d++) {
		int ro = 0;
		const char* path = media_floppy(s, d, &ro);
		std::string want = std::string(path ? path : "") + (ro ? "|ro" : "");
		if (!force && want == appliedFloppy[d])
			continue;
		appliedFloppy[d] = want;
		if (!strcmp(prefs_fdd_type(s, d), "none"))
			continue;
		if (path && *path) {
			std::string p = path;
			floppy_mount((uint8_t)d, p.data(), (uint8_t)ro);
		} else {
			floppy_eject((uint8_t)d);
		}
	}
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

msdossession* msdossession_new(const msdossession_paths* paths)
{
	msdossession* s = (msdossession*)calloc(1, sizeof *s);
	if (!s)
		return nullptr;
	pthread_mutex_init(&s->pad_event_mtx, nullptr);
	if (paths_init(s, paths ? paths->config_dir : nullptr, paths ? paths->data_dir : nullptr) != 0) {
		pthread_mutex_destroy(&s->pad_event_mtx);
		free(s);
		return nullptr;
	}
	if (paths && paths->fujinet_lib) {
		snprintf(s->fujinet_lib, sizeof s->fujinet_lib, "%s", paths->fujinet_lib);
		s->fujinet_disabled = !*paths->fujinet_lib;
	}
	if (paths && paths->fujinet_runtime_src)
		snprintf(s->fujinet_runtime_src, sizeof s->fujinet_runtime_src, "%s",
		         paths->fujinet_runtime_src);
	settings_init(s);
	s->impl = new SessionImpl(s);

	// what 86Box said as it went down last time
	std::string fp = fatal_path(s);
	if (FILE* f = fopen(fp.c_str(), "r")) {
		char buf[512] = "";
		size_t n = fread(buf, 1, sizeof buf - 1, f);
		buf[n] = 0;
		fclose(f);
		remove(fp.c_str());
		Impl(s)->Notice(std::string("86Box stopped the last session: ") + buf);
	}
	return s;
}

void msdossession_free(msdossession* s)
{
	if (!s)
		return;
	msdossession_stop(s);
	session_fujinet_stop(s);
	session_debug_free(s);
	delete Impl(s);
	msdossession_settings_flush(s);
	settings_free_all(s);
	pthread_mutex_destroy(&s->pad_event_mtx);
	free(s);
}

void msdossession_default_opts(msdossession* s, msdossession_start_opts* o)
{
	o->enable_fujinet = msdossession_get_int(s, "fujinet", 1);
	o->enable_audio = msdossession_get_int(s, "enable_audio", 1);
	o->enable_gamepad = msdossession_get_int(s, "enable_gamepad", 1);
}

int msdossession_is_running(const msdossession* s)
{
	return s->running;
}

const char* msdossession_last_error(const msdossession* s)
{
	return s->last_error;
}

// The selected machine's cards and drives must be ones it can have: a
// choice whose ROM is gone, or a default nobody imported, moves to the
// first that works.
static void settle_choices(msdossession* s)
{
	static const char* const keys[] = { "video", "sound", "midi", "fdd_a", "fdd_b", "hdc",
	                                    "mouse", nullptr };
	for (int k = 0; keys[k]; k++) {
		int i = msdossession_pref_find(keys[k]);
		if (i < 0 || !msdossession_pref_enabled(s, i))
			continue;
		int cur = msdossession_pref_get_choice(s, i);
		if (msdossession_pref_choice_enabled(s, i, cur))
			continue;
		for (int c = 0; msdossession_pref_choice(s, i, c); c++)
			if (msdossession_pref_choice_enabled(s, i, c)) {
				msdossession_pref_set_choice(s, i, c);
				break;
			}
	}
}

int msdossession_start(msdossession* s, const msdossession_start_opts* opts)
{
	SessionImpl* im = Impl(s);
	if (s->running)
		return 0;
	s->last_error[0] = 0;
	if (im->hostStarted) {
		session_set_error(s, "86Box has already run in this process; restart the app");
		return -1;
	}
	if (!msdossession_has_bios(s)) {
		session_set_error(s, "No PC BIOS yet: use Machine > Import BIOS");
		return -1;
	}
	if (!msdossession_machine_available(s, prefs_machine(s))) {
		for (int m = 0; m < MSDOS_M_COUNT; m++)
			if (msdossession_machine_available(s, m)) {
				im->Notice(std::string(msdos_machine_name(prefs_machine(s))) +
				           " has no BIOS imported; starting the " + msdos_machine_name(m));
				msdossession_set_int(s, "machine", m);
				break;
			}
	}
	settle_choices(s);
	if (opts)
		s->opts = *opts;
	else
		msdossession_default_opts(s, &s->opts);

	s->fujinet_wanted = s->opts.enable_fujinet && session_fujinet_available(s);
	if (s->fujinet_wanted && session_fujinet_start(s) == 0)
		session_fujinet_wait_listener(s, 4000);

	std::string cfg = std::string(s->vm_dir) + "/86box.cfg";
	if (cfg86_write(s, cfg.c_str()) != 0) {
		session_fujinet_stop(s);
		return -1;
	}

	PcHost::Config config;
	config.vmDir = s->vm_dir;
	config.romDirs.push_back(s->roms_dir);
	std::string err;
	im->hostStarted = true;
	if (!im->host.Start(config, err)) {
		session_set_error(s, "%s", err.c_str());
		session_fujinet_stop(s);
		return -1;
	}
	s->running = 1;
	im->host.SetVolume(prefs_int(s, "volume"));
	session_debug_trace_install(s);
	if (s->opts.enable_gamepad)
		gamepad_start(s);
	if (s->opts.enable_audio && audio_start(s) != 0)
		fprintf(stderr, "msdos: %s (continuing without sound)\n", s->last_error);
	im->appliedReset = im->Snapshot();
	for (int d = 0; d < MSDOS_FLOPPIES; d++) {
		int ro = 0;
		const char* p = media_floppy(s, d, &ro);
		im->appliedFloppy[d] = std::string(p ? p : "") + (ro ? "|ro" : "");
	}
	im->appliedFujiNet = s->fujinet_wanted;
	return 0;
}

void msdossession_stop(msdossession* s)
{
	SessionImpl* im = Impl(s);
	if (!s->running)
		return;
	audio_stop(s);
	gamepad_stop(s);
	im->host.Stop();
	session_fujinet_stop(s);
	s->running = 0;
}

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------

int msdossession_apply_settings(msdossession* s)
{
	SessionImpl* im = Impl(s);
	int need = MSDOS_APPLY_LIVE;
	msdossession_settings_flush(s);
	if (!s->running)
		return need;

	im->host.SetVolume(prefs_int(s, "volume"));

	const bool fn = msdossession_get_int(s, "fujinet", 1) && s->opts.enable_fujinet
	                && session_fujinet_available(s);
	if (fn != im->appliedFujiNet) {
		need = MSDOS_APPLY_RESTART;
		if (fn) {
			s->fujinet_wanted = 1;
			session_fujinet_start(s);
		} else {
			session_fujinet_stop(s);
			s->fujinet_wanted = 0;
		}
		im->appliedFujiNet = fn;
	}

	settle_choices(s);
	std::string sig = im->Snapshot();
	if (sig != im->appliedReset) {
		need = std::max(need, (int)MSDOS_APPLY_RESET);
		std::string cfg = std::string(s->vm_dir) + "/86box.cfg";
		if (cfg86_write(s, cfg.c_str()) == 0) {
			std::string err;
			im->host.Call([&] { im->ReloadConfig(err); });
		}
		im->appliedReset = sig;
	} else {
		im->host.Call([im] { im->MountFloppies(false); });
	}
	return need;
}

// ---------------------------------------------------------------------------
// the machine's buttons
// ---------------------------------------------------------------------------

int msdossession_reset(msdossession* s)
{
	if (!s->running)
		return -1;
	Impl(s)->host.HardReset();
	return 0;
}

int msdossession_ctrl_alt_del(msdossession* s)
{
	if (!s->running)
		return -1;
	Impl(s)->host.SendCtrlAltDel();
	return 0;
}

void msdossession_set_paused(msdossession* s, int on)
{
	Impl(s)->host.SetPaused(on != 0);
}

int msdossession_paused(msdossession* s)
{
	return Impl(s)->host.Paused() ? 1 : 0;
}

void msdossession_set_warp(msdossession* s, int on)
{
	Impl(s)->host.SetWarp(on != 0);
}

int msdossession_warp(msdossession* s)
{
	return Impl(s)->host.Warp() ? 1 : 0;
}

int msdossession_speed_percent(msdossession* s)
{
	return s->running ? Impl(s)->host.SpeedPercent() : 0;
}

// ---------------------------------------------------------------------------
// video and audio
// ---------------------------------------------------------------------------

int msdossession_copy_frame(msdossession* s, uint32_t* dst, int* width, int* height,
                            int* par_milli, uint64_t* serial_inout)
{
	int w = 0, h = 0;
	if (!Impl(s)->host.CopyFrame(dst, (size_t)MSDOSSESSION_FB_MAX_WIDTH * MSDOSSESSION_FB_MAX_HEIGHT,
	                             w, h, serial_inout))
		return 0;
	if (width)
		*width = w;
	if (height)
		*height = h;
	if (par_milli)
		*par_milli = (prefs_int(s, "aspect43") && w > 0) ? (4000 * h) / (3 * w) : 1000;
	return 1;
}

int msdossession_render_audio(msdossession* s, float* stereo, int nframes)
{
	Impl(s)->host.FillAudio(stereo, (uint32_t)nframes);
	return nframes;
}

void msdossession_set_volume(msdossession* s, int percent)
{
	msdossession_set_int(s, "volume", percent);
	Impl(s)->host.SetVolume(percent);
}

// ---------------------------------------------------------------------------
// FujiNet, paths, status
// ---------------------------------------------------------------------------

int msdossession_fujinet_link(msdossession* s)
{
	return s->running && s->fujinet_wanted && char_fujinet_connected() > 0 ? 1 : 0;
}

void session_fujinet_link_lost(struct msdossession* s)
{
	(void)s;
}

const char* msdossession_config_path(const msdossession* s) { return s->config_dir; }
const char* msdossession_data_path(const msdossession* s) { return s->data_dir; }
const char* msdossession_roms_path(const msdossession* s) { return s->roms_dir; }
const char* msdossession_media_path(const msdossession* s) { return s->media_dir; }
const char* msdossession_sd_path(const msdossession* s) { return s->fujinet_sd; }

int msdossession_status(msdossession* s, char* dst, int dstsz)
{
	SessionImpl* im = Impl(s);
	std::string fn;
	if (!s->fujinet_wanted)
		fn = "FujiNet off";
	else if (msdossession_fujinet_link(s))
		fn = "FujiNet on COM" + std::to_string(prefs_fujinet_com(s));
	else if (msdossession_fujinet_running(s))
		fn = "FujiNet waiting";
	else
		fn = "FujiNet stopped";
	std::string extra;
	if (!s->running)
		extra += " \xc2\xb7 Off";
	else if (im->host.Stopped())
		extra += " \xc2\xb7 Stopped";
	else if (im->host.Paused() || dopause)
		extra += " \xc2\xb7 Paused";
	else if (im->host.Warp())
		extra += " \xc2\xb7 Warp";
	else if (im->host.SpeedPercent() > 0 && im->host.SpeedPercent() < 95)
		extra += " \xc2\xb7 " + std::to_string(im->host.SpeedPercent()) + "%";
	return snprintf(dst, (size_t)dstsz, "%s \xc2\xb7 %s%s", msdos_machine_name(prefs_machine(s)),
	                fn.c_str(), extra.c_str());
}

int msdossession_take_notice(msdossession* s, char* dst, int dstsz)
{
	SessionImpl* im = Impl(s);
	std::lock_guard<std::mutex> lock(im->noticeMtx);
	if (im->notice.empty())
		return 0;
	snprintf(dst, (size_t)dstsz, "%s", im->notice.c_str());
	im->notice.clear();
	return 1;
}

int msdossession_copy_screen_text(msdossession* s, char* dst, int dstsz)
{
	int n = -1;
	if (!s->running || !dst || dstsz <= 0)
		return -1;
	Impl(s)->host.Call([&] { n = msdos_screen_text(dst, dstsz); }, PcHost::Kind::Any);
	return n;
}

// ---- gamepads ------------------------------------------------------------------

void session_gamepad_event(struct msdossession* s, const char* text)
{
	pthread_mutex_lock(&s->pad_event_mtx);
	snprintf(s->pad_event, sizeof s->pad_event, "%s", text ? text : "");
	pthread_mutex_unlock(&s->pad_event_mtx);
}

int msdossession_gamepad_last_event(msdossession* s, char* dst, int dstsz)
{
	if (!dst || dstsz <= 0)
		return 0;
	pthread_mutex_lock(&s->pad_event_mtx);
	const int n = snprintf(dst, (size_t)dstsz, "%s", s->pad_event);
	pthread_mutex_unlock(&s->pad_event_mtx);
	return n;
}
