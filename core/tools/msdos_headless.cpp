/*
 * msdos_headless -- 86Box with no window, for tests and bring-up.
 *
 *   msdos_headless --vm DIR [--rom-dir DIR]... [--set "Section/key=value"]...
 *                  [--floppy IMG] [--seconds N] [--expect TEXT] [--type TEXT]
 *                  [--screen-text] [--ppm FILE]
 *
 * Writes DIR/86box.cfg from the --set lines (and --floppy), boots, and runs
 * for N seconds of wall time -- or until the text screen shows --expect
 * (exit 0; exit 1 if it never does). --type types after the expected text
 * appears (or at once), \n as Enter.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "PcHost.h"
#include "../src/compat.h"
#include "../src/dynlib.h"

extern "C" {
#include "../src/screen_text.h"
}

namespace {

void Usage()
{
	fprintf(stderr,
	        "usage: msdos_headless --vm DIR [--rom-dir DIR]... [--set Section/key=value]...\n"
	        "                      [--floppy IMG] [--seconds N] [--expect TEXT] [--type TEXT]\n"
	        "                      [--screen-text] [--ppm FILE]\n"
	        "                      [--fujinet LIB --fujinet-root DIR]\n");
}

void MakeDir(const std::string& p)
{
#if defined(_WIN32)
	_mkdir(p.c_str());
#else
	mkdir(p.c_str(), 0755);
#endif
}

// US-layout set-1 codes for typing ASCII.
struct KeyStroke { uint16_t sc; bool shift; };
bool AsciiKey(char c, KeyStroke& k)
{
	static const char* row1 = "1234567890-=";
	static const char* row1s = "!@#$%^&*()_+";
	static const char* q = "qwertyuiop[]";
	static const char* qs = "QWERTYUIOP{}";
	static const char* a = "asdfghjkl;'`";
	static const char* as = "ASDFGHJKL:\"~";
	static const char* z = "zxcvbnm,./";
	static const char* zs = "ZXCVBNM<>?";
	const char* p;
	k.shift = false;
	if (c == '\n') { k.sc = 0x1c; return true; }
	if (c == ' ') { k.sc = 0x39; return true; }
	if (c == '\\') { k.sc = 0x2b; return true; }
	if (c == '|') { k.sc = 0x2b; k.shift = true; return true; }
	if ((p = strchr(row1, c)) && *p) { k.sc = (uint16_t)(0x02 + (p - row1)); return true; }
	if ((p = strchr(row1s, c)) && *p) { k.sc = (uint16_t)(0x02 + (p - row1s)); k.shift = true; return true; }
	if ((p = strchr(q, c)) && *p) { k.sc = (uint16_t)(0x10 + (p - q)); return true; }
	if ((p = strchr(qs, c)) && *p) { k.sc = (uint16_t)(0x10 + (p - qs)); k.shift = true; return true; }
	if ((p = strchr(a, c)) && *p) { k.sc = (uint16_t)(0x1e + (p - a)); if (c == '`') k.sc = 0x29; return true; }
	if ((p = strchr(as, c)) && *p) { k.sc = (uint16_t)(0x1e + (p - as)); if (c == '~') k.sc = 0x29; k.shift = true; return true; }
	if ((p = strchr(z, c)) && *p) { k.sc = (uint16_t)(0x2c + (p - z)); return true; }
	if ((p = strchr(zs, c)) && *p) { k.sc = (uint16_t)(0x2c + (p - zs)); k.shift = true; return true; }
	return false;
}

void TypeText(PcHost& host, const std::string& text)
{
	for (size_t i = 0; i < text.size(); i++) {
		char c = text[i];
		if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
			c = '\n';
			i++;
		}
		KeyStroke k;
		if (!AsciiKey(c, k))
			continue;
		if (k.shift)
			host.Key(0x2a, true);
		host.Key(k.sc, true);
		std::this_thread::sleep_for(std::chrono::milliseconds(60));
		host.Key(k.sc, false);
		if (k.shift)
			host.Key(0x2a, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(60));
	}
}

std::string ScreenText(PcHost& host)
{
	std::string out;
	host.Call([&] {
		std::vector<char> buf(16384);
		if (msdos_screen_text(buf.data(), (int)buf.size()) >= 0)
			out = buf.data();
	}, PcHost::Kind::Any);
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	std::string vm, floppy, expect, type, ppm, fnLib, fnRoot;
	void (*fnStop)(void) = nullptr;
	std::vector<std::string> romDirs;
	std::map<std::string, std::map<std::string, std::string>> cfg;
	double seconds = 10;
	bool screenText = false;

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc) { Usage(); exit(2); }
			return argv[++i];
		};
		if (a == "--vm") vm = next();
		else if (a == "--rom-dir") romDirs.push_back(next());
		else if (a == "--floppy") floppy = next();
		else if (a == "--seconds") seconds = atof(next().c_str());
		else if (a == "--expect") expect = next();
		else if (a == "--type") type = next();
		else if (a == "--screen-text") screenText = true;
		else if (a == "--ppm") ppm = next();
		else if (a == "--fujinet") fnLib = next();
		else if (a == "--fujinet-root") fnRoot = next();
		else if (a == "--set") {
			std::string s = next();
			size_t slash = s.find('/'), eq = s.find('=');
			if (slash == std::string::npos || eq == std::string::npos || eq < slash) { Usage(); return 2; }
			cfg[s.substr(0, slash)][s.substr(slash + 1, eq - slash - 1)] = s.substr(eq + 1);
		}
		else { Usage(); return 2; }
	}
	if (vm.empty()) { Usage(); return 2; }

	MakeDir(vm);
	if (!floppy.empty())
		cfg["Floppy and CD-ROM drives"]["fdd_01_fn"] = floppy;
	if (FILE* f = fopen((vm + "/86box.cfg").c_str(), "w")) {
		for (const auto& sec : cfg) {
			fprintf(f, "[%s]\n", sec.first.c_str());
			for (const auto& kv : sec.second)
				fprintf(f, "%s = %s\n", kv.first.c_str(), kv.second.c_str());
			fprintf(f, "\n");
		}
		fclose(f);
	}

	// FujiNet first: it listens, 86Box's COM device connects.
	if (!fnLib.empty()) {
		char err[256];
		msdos_dynlib lib = msdos_dynlib_open(fnLib.c_str());
		if (!lib) {
			fprintf(stderr, "fujinet: %s\n", msdos_dynlib_error(err, sizeof err));
			return 3;
		}
		using start_fn = int (*)(const char*, const char*, const char*, const char*, int);
		auto start = (start_fn)msdos_dynlib_sym(lib, "fujinet_desktop_start_runtime");
		std::string cfgPath = fnRoot + "/fnconfig.ini";
		msdos_setenv("FUJINET_RUNTIME_ROOT", fnRoot.c_str());
		msdos_setenv("FUJINET_WEBUI_BIND", "127.0.0.1:11599");
		if (!start || !start(fnRoot.c_str(), cfgPath.c_str(), (fnRoot + "/SD").c_str(),
		                     (fnRoot + "/data").c_str(), 0)) {
			fprintf(stderr, "fujinet did not start\n");
			return 3;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		fnStop = (void (*)(void))msdos_dynlib_sym(lib, "fujinet_desktop_stop_runtime");
	}

	PcHost host;
	PcHost::Callbacks cb;
	cb.onMessage = [](int flags, const char* header, const char* text) {
		fprintf(stderr, "86Box message (%d) %s: %s\n", flags, header, text);
	};
	host.SetCallbacks(cb);
	PcHost::Config config;
	config.vmDir = vm;
	config.romDirs = romDirs;
	std::string err;
	if (!host.Start(config, err)) {
		fprintf(stderr, "start failed: %s\n", err.c_str());
		return 3;
	}

	const auto t0 = std::chrono::steady_clock::now();
	bool found = expect.empty();
	bool typed = false;
	if (!type.empty() && expect.empty()) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		TypeText(host, type);
		typed = true;
	}
	while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		if (!expect.empty() && !found && ScreenText(host).find(expect) != std::string::npos) {
			found = true;
			if (type.empty())
				break;
		}
		if (found && !typed && !type.empty()) {
			TypeText(host, type);
			typed = true;
			std::this_thread::sleep_for(std::chrono::seconds(2));
			break;
		}
	}

	if (screenText)
		printf("%s", ScreenText(host).c_str());
	fprintf(stderr, "speed %d%%\n", host.SpeedPercent());
	if (!ppm.empty()) {
		std::vector<uint32_t> px((size_t)PcHost::MaxWidth * PcHost::MaxHeight);
		int w = 0, h = 0;
		host.CopyFrame(px.data(), px.size(), w, h, nullptr);
		if (FILE* f = fopen(ppm.c_str(), "wb")) {
			fprintf(f, "P6\n%d %d\n255\n", w, h);
			for (int i = 0; i < w * h; i++) {
				unsigned char rgb[3] = { (unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i] };
				fwrite(rgb, 1, 3, f);
			}
			fclose(f);
		}
	}
	host.Stop();
	if (fnStop)
		fnStop();
	return found ? 0 : 1;
}
