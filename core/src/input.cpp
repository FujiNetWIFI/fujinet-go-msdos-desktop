/*
 * The PC's keyboard, mouse and game port: HID usages to set 1 scan codes,
 * the on-screen keyboards, paste, and the joysticks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "session_impl.h"

extern "C" {
#include <86box/86box.h>
#include <86box/keyboard.h>
#include <86box/mem.h>
#include "msdos_host.h"
#include "screen_text.h"
}

namespace {

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// HID usage -> set 1 scan code (0x100 = E0 prefix). 86Box's own SDL table
// (src/unix/sdl_main.c sdl_to_xt): SDL scancodes are HID usages.
uint16_t ToScan(uint32_t hid)
{
	static uint16_t table[256];
	static bool ready;
	if (!ready) {
		static const uint8_t letters[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
		                                     0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
		                                     0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
		for (int i = 0; i < 26; i++)
			table[0x04 + i] = letters[i];
		for (int i = 0; i < 9; i++)
			table[0x1E + i] = (uint16_t)(0x02 + i);         // 1-9
		table[0x27] = 0x0B;                                 // 0
		table[0x28] = 0x1C; table[0x29] = 0x01; table[0x2A] = 0x0E; table[0x2B] = 0x0F;
		table[0x2C] = 0x39; table[0x2D] = 0x0C; table[0x2E] = 0x0D; table[0x2F] = 0x1A;
		table[0x30] = 0x1B; table[0x31] = 0x2B; table[0x32] = 0x2B; table[0x33] = 0x27;
		table[0x34] = 0x28; table[0x35] = 0x29; table[0x36] = 0x33; table[0x37] = 0x34;
		table[0x38] = 0x35; table[0x39] = 0x3A;
		for (int i = 0; i < 10; i++)
			table[0x3A + i] = (uint16_t)(0x3B + i);         // F1-F10
		table[0x44] = 0x57; table[0x45] = 0x58;             // F11, F12
		table[0x46] = 0x137;                                // Print Screen
		table[0x47] = 0x46;                                 // Scroll Lock
		table[0x48] = 0x145;                                // Pause
		table[0x49] = 0x152; table[0x4A] = 0x147; table[0x4B] = 0x149;
		table[0x4C] = 0x153; table[0x4D] = 0x14F; table[0x4E] = 0x151;
		table[0x4F] = 0x14D; table[0x50] = 0x14B; table[0x51] = 0x150; table[0x52] = 0x148;
		table[0x53] = 0x45;                                 // Num Lock
		table[0x54] = 0x135; table[0x55] = 0x37; table[0x56] = 0x4A; table[0x57] = 0x4E;
		table[0x58] = 0x11C;                                // keypad Enter
		static const uint8_t kp[10] = { 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52 };
		for (int i = 0; i < 10; i++)
			table[0x59 + i] = kp[i];                        // keypad 1-9, 0
		table[0x63] = 0x53;                                 // keypad .
		table[0x64] = 0x56;                                 // the 102nd key
		table[0xE0] = 0x1D; table[0xE1] = 0x2A; table[0xE2] = 0x38; table[0xE3] = 0x15B;
		table[0xE4] = 0x11D; table[0xE5] = 0x36; table[0xE6] = 0x138; table[0xE7] = 0x15C;
		ready = true;
	}
	return hid < 256 ? table[hid] : 0;
}

constexpr uint32_t HID_LSHIFT = 0xE1, HID_LALT = 0xE2, HID_ENTER = 0x28;

// What a character is on a US keyboard.
bool CharKey(uint32_t c, uint32_t& hid, bool& shift)
{
	static const char* const plain = "-=[]\\;'`,./";
	static const char* const shifted = "_+{}|:\"~<>?";
	static const uint32_t punct[] = { 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38 };
	static const char* const digitsShifted = ")!@#$%^&*(";
	shift = false;
	if (c >= 'a' && c <= 'z') { hid = 0x04 + (c - 'a'); return true; }
	if (c >= 'A' && c <= 'Z') { hid = 0x04 + (c - 'A'); shift = true; return true; }
	if (c >= '1' && c <= '9') { hid = 0x1E + (c - '1'); return true; }
	if (c == '0') { hid = 0x27; return true; }
	if (c == ' ') { hid = 0x2C; return true; }
	if (c == '\n' || c == '\r') { hid = HID_ENTER; return true; }
	if (c == '\t') { hid = 0x2B; return true; }
	if (c < 128) {
		if (const char* p = strchr(digitsShifted, (int)c); p && c) {
			int d = (int)(p - digitsShifted);
			hid = d == 0 ? 0x27 : 0x1E + (uint32_t)(d - 1);
			shift = true;
			return true;
		}
		for (int i = 0; plain[i]; i++) {
			if ((char)c == plain[i]) { hid = punct[i]; return true; }
			if ((char)c == shifted[i]) { hid = punct[i]; shift = true; return true; }
		}
	}
	return false;
}

// ---- the on-screen keyboards --------------------------------------------------

struct Row
{
	float y;
	float x;
	struct K { const char* label; const char* shifted; uint32_t hid; float w; float h; float gap; };
	std::vector<K> keys;
};

void Emit(std::vector<msdos_kbd_key>& out, const Row& r)
{
	float x = r.x;
	for (const auto& k : r.keys) {
		x += k.gap;
		if (k.label)
			out.push_back({ k.label, k.shifted, k.hid, x, r.y, k.w, k.h > 0 ? k.h : 1.0f });
		x += k.w;
	}
}

using K = Row::K;
K key(const char* l, uint32_t hid, float w = 1, const char* sh = nullptr, float gap = 0, float h = 0)
{
	return K{ l, sh, hid, w, h, gap };
}

std::vector<msdos_kbd_key> XtLayout()
{
	std::vector<msdos_kbd_key> o;
	// function keys in two columns at the left
	for (int i = 0; i < 10; i++) {
		static const char* const f[10] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10" };
		o.push_back({ f[i], nullptr, (uint32_t)(0x3A + i), (float)(i % 2), (float)(i / 2), 1, 1 });
	}
	Emit(o, { 0, 2.5f, { key("Esc", 0x29), key("1", 0x1E, 1, "!"), key("2", 0x1F, 1, "@"),
	                    key("3", 0x20, 1, "#"), key("4", 0x21, 1, "$"), key("5", 0x22, 1, "%"),
	                    key("6", 0x23, 1, "^"), key("7", 0x24, 1, "&"), key("8", 0x25, 1, "*"),
	                    key("9", 0x26, 1, "("), key("0", 0x27, 1, ")"), key("-", 0x2D, 1, "_"),
	                    key("=", 0x2E, 1, "+"), key("\xe2\x86\x90", 0x2A, 2),
	                    key("Num Lock", 0x53, 2, nullptr, 0.5f), key("Scroll Lock", 0x47, 2) } });
	Emit(o, { 1, 2.5f, { key("Tab", 0x2B, 1.5f), key("Q", 0x14), key("W", 0x1A), key("E", 0x08),
	                    key("R", 0x15), key("T", 0x17), key("Y", 0x1C), key("U", 0x18), key("I", 0x0C),
	                    key("O", 0x12), key("P", 0x13), key("[", 0x2F, 1, "{"), key("]", 0x30, 1, "}"),
	                    key("Enter", 0x28, 1.5f, nullptr, 0, 2),
	                    key("7", 0x5F, 1, "Home", 0.5f), key("8", 0x60, 1, "\xe2\x86\x91"),
	                    key("9", 0x61, 1, "PgUp"), key("-", 0x56) } });
	Emit(o, { 2, 2.5f, { key("Ctrl", 0xE0, 1.5f), key("A", 0x04), key("S", 0x16), key("D", 0x07),
	                    key("F", 0x09), key("G", 0x0A), key("H", 0x0B), key("J", 0x0D), key("K", 0x0E),
	                    key("L", 0x0F), key(";", 0x33, 1, ":"), key("'", 0x34, 1, "\""),
	                    key("`", 0x35, 1, "~"),
	                    key("4", 0x5C, 1, "\xe2\x86\x90", 2.0f), key("5", 0x5D), key("6", 0x5E, 1, "\xe2\x86\x92"),
	                    key("+", 0x57, 1, nullptr, 0, 2) } });
	Emit(o, { 3, 2.5f, { key("Shift", HID_LSHIFT, 1.25f), key("\\", 0x31, 1, "|"), key("Z", 0x1D),
	                    key("X", 0x1B), key("C", 0x06), key("V", 0x19), key("B", 0x05), key("N", 0x11),
	                    key("M", 0x10), key(",", 0x36, 1, "<"), key(".", 0x37, 1, ">"),
	                    key("/", 0x38, 1, "?"), key("Shift", 0xE5, 1.75f), key("PrtSc *", 0x55),
	                    key("1", 0x59, 1, "End", 0.5f), key("2", 0x5A, 1, "\xe2\x86\x93"),
	                    key("3", 0x5B, 1, "PgDn") } });
	Emit(o, { 4, 2.5f, { key("Alt", HID_LALT, 1.5f), key("", 0x2C, 11), key("Caps Lock", 0x39, 2.5f),
	                    key("0", 0x62, 2, "Ins", 0.5f), key(".", 0x63, 1, "Del") } });
	return o;
}

std::vector<msdos_kbd_key> EnhancedLayout()
{
	std::vector<msdos_kbd_key> o;
	Emit(o, { 0, 0, { key("Esc", 0x29), key("F1", 0x3A, 1, nullptr, 1), key("F2", 0x3B), key("F3", 0x3C),
	                  key("F4", 0x3D), key("F5", 0x3E, 1, nullptr, 0.5f), key("F6", 0x3F), key("F7", 0x40),
	                  key("F8", 0x41), key("F9", 0x42, 1, nullptr, 0.5f), key("F10", 0x43), key("F11", 0x44),
	                  key("F12", 0x45), key("PrtSc", 0x46, 1, nullptr, 0.25f), key("Scroll", 0x47),
	                  key("Pause", 0x48) } });
	Emit(o, { 1.5f, 0, { key("`", 0x35, 1, "~"), key("1", 0x1E, 1, "!"), key("2", 0x1F, 1, "@"),
	                     key("3", 0x20, 1, "#"), key("4", 0x21, 1, "$"), key("5", 0x22, 1, "%"),
	                     key("6", 0x23, 1, "^"), key("7", 0x24, 1, "&"), key("8", 0x25, 1, "*"),
	                     key("9", 0x26, 1, "("), key("0", 0x27, 1, ")"), key("-", 0x2D, 1, "_"),
	                     key("=", 0x2E, 1, "+"), key("\xe2\x86\x90", 0x2A, 2),
	                     key("Ins", 0x49, 1, nullptr, 0.25f), key("Home", 0x4A), key("PgUp", 0x4B),
	                     key("Num", 0x53, 1, nullptr, 0.25f), key("/", 0x54), key("*", 0x55), key("-", 0x56) } });
	Emit(o, { 2.5f, 0, { key("Tab", 0x2B, 1.5f), key("Q", 0x14), key("W", 0x1A), key("E", 0x08),
	                     key("R", 0x15), key("T", 0x17), key("Y", 0x1C), key("U", 0x18), key("I", 0x0C),
	                     key("O", 0x12), key("P", 0x13), key("[", 0x2F, 1, "{"), key("]", 0x30, 1, "}"),
	                     key("\\", 0x31, 1.5f, "|"),
	                     key("Del", 0x4C, 1, nullptr, 0.25f), key("End", 0x4D), key("PgDn", 0x4E),
	                     key("7", 0x5F, 1, "Home", 0.25f), key("8", 0x60, 1, "\xe2\x86\x91"),
	                     key("9", 0x61, 1, "PgUp"), key("+", 0x57, 1, nullptr, 0, 2) } });
	Emit(o, { 3.5f, 0, { key("Caps", 0x39, 1.75f), key("A", 0x04), key("S", 0x16), key("D", 0x07),
	                     key("F", 0x09), key("G", 0x0A), key("H", 0x0B), key("J", 0x0D), key("K", 0x0E),
	                     key("L", 0x0F), key(";", 0x33, 1, ":"), key("'", 0x34, 1, "\""),
	                     key("Enter", 0x28, 2.25f),
	                     key("4", 0x5C, 1, "\xe2\x86\x90", 3.5f), key("5", 0x5D), key("6", 0x5E, 1, "\xe2\x86\x92") } });
	Emit(o, { 4.5f, 0, { key("Shift", HID_LSHIFT, 2.25f), key("Z", 0x1D), key("X", 0x1B), key("C", 0x06),
	                     key("V", 0x19), key("B", 0x05), key("N", 0x11), key("M", 0x10),
	                     key(",", 0x36, 1, "<"), key(".", 0x37, 1, ">"), key("/", 0x38, 1, "?"),
	                     key("Shift", 0xE5, 2.75f), key("\xe2\x86\x91", 0x52, 1, nullptr, 1.25f),
	                     key("1", 0x59, 1, "End", 1.25f), key("2", 0x5A, 1, "\xe2\x86\x93"),
	                     key("3", 0x5B, 1, "PgDn"), key("Enter", 0x58, 1, nullptr, 0, 2) } });
	Emit(o, { 5.5f, 0, { key("Ctrl", 0xE0, 1.5f), key("Alt", HID_LALT, 1.5f, nullptr, 1),
	                     key("", 0x2C, 7), key("Alt", 0xE6, 1.5f), key("Ctrl", 0xE4, 1.5f, nullptr, 1),
	                     key("\xe2\x86\x90", 0x50, 1, nullptr, 0.25f), key("\xe2\x86\x93", 0x51),
	                     key("\xe2\x86\x92", 0x4F), key("0", 0x62, 2, "Ins", 0.25f), key(".", 0x63, 1, "Del") } });
	return o;
}

} // namespace

// ---------------------------------------------------------------------------
// the keyboard
// ---------------------------------------------------------------------------

void SessionImpl::Key(uint32_t hid, bool down)
{
	uint16_t sc = ToScan(hid);
	if (!sc)
		return;
	{
		std::lock_guard<std::mutex> lock(keyMtx);
		if (hid < 256)
			held[hid] = down ? 1 : 0;
	}
	host.Key(sc, down);
}

int msdossession_key(msdossession* s, uint32_t hid, int down)
{
	if (!s->running || !ToScan(hid))
		return 0;
	Impl(s)->Key(hid, down != 0);
	return 1;
}

void msdossession_release_keys(msdossession* s)
{
	SessionImpl* im = Impl(s);
	{
		std::lock_guard<std::mutex> lock(im->keyMtx);
		memset(im->held, 0, sizeof im->held);
	}
	if (s->running)
		im->host.ReleaseAllKeys();
}

unsigned msdossession_lock_lights(msdossession* s)
{
	uint8_t caps = 0, num = 0, scroll = 0, kana = 0;
	if (!s->running)
		return 0;
	keyboard_get_states(&caps, &num, &scroll, &kana);
	return (num ? 1u : 0u) | (caps ? 2u : 0u) | (scroll ? 4u : 0u);
}

int msdossession_keyboard_layout(msdossession* s, const msdos_kbd_key** keys)
{
	SessionImpl* im = Impl(s);
	const int m = prefs_machine(s);
	const int layout = (m == MSDOS_M_5150 || m == MSDOS_M_5160 || m == MSDOS_M_PCJR) ? 0 : 1;
	if (layout != im->oskLayout) {
		im->oskKeys = layout == 0 ? XtLayout() : EnhancedLayout();
		im->oskLayout = layout;
	}
	if (keys)
		*keys = im->oskKeys.data();
	return (int)im->oskKeys.size();
}

void msdossession_keyboard_press(msdossession* s, int index, int down)
{
	SessionImpl* im = Impl(s);
	if (index < 0 || index >= (int)im->oskKeys.size() || !s->running)
		return;
	im->Key(im->oskKeys[(size_t)index].hid, down != 0);
	im->oskHeld.store(down ? index : -1);
}

int msdossession_keyboard_held(msdossession* s, int index)
{
	SessionImpl* im = Impl(s);
	if (index < 0 || index >= (int)im->oskKeys.size())
		return 0;
	uint32_t hid = im->oskKeys[(size_t)index].hid;
	std::lock_guard<std::mutex> lock(im->keyMtx);
	return hid < 256 && im->held[hid];
}

// ---------------------------------------------------------------------------
// paste
// ---------------------------------------------------------------------------

namespace {

// Keys queued in the BIOS buffer; above this, paste waits for DOS to read.
constexpr int BufferRoom = 6;
constexpr int StepMs = 12;

int BiosBufferCount()
{
	const uint16_t head = mem_readw_phys(0x41A), tail = mem_readw_phys(0x41C);
	uint16_t start = mem_readw_phys(0x480), end = mem_readw_phys(0x482);
	if (start < 0x1E || end <= start || end > 0x200) {
		start = 0x1E;
		end = 0x3E;
	}
	const int size = end - start;
	return (((int)tail - (int)head) % size + size) % size / 2;
}

} // namespace

int msdossession_paste(msdossession* s, const char* utf8)
{
	SessionImpl* im = Impl(s);
	if (!utf8 || !s->running)
		return -1;
	std::lock_guard<std::mutex> lock(im->pasteMtx);
	const unsigned char* p = (const unsigned char*)utf8;
	while (*p) {
		uint32_t c = *p++;
		if (c >= 0x80) {
			int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
			c &= 0x3F >> extra;
			while (extra-- && (*p & 0xC0) == 0x80)
				c = (c << 6) | (*p++ & 0x3F);
		}
		if (c == '\r' && *p == '\n')
			continue;
		uint32_t hid;
		bool shift;
		if (CharKey(c, hid, shift)) {
			im->paste.push_back({ hid, shift, 0 });
		} else {
			int code = msdos_utf8_to_cp437(c);
			if (code > 0)
				im->paste.push_back({ 0, false, code });
		}
	}
	return 0;
}

void msdossession_paste_cancel(msdossession* s)
{
	SessionImpl* im = Impl(s);
	std::lock_guard<std::mutex> lock(im->pasteMtx);
	im->paste.clear();
}

int msdossession_paste_pending(msdossession* s)
{
	SessionImpl* im = Impl(s);
	std::lock_guard<std::mutex> lock(im->pasteMtx);
	return (int)im->paste.size();
}

// Emulation thread, before every slice: one keystroke at a time, each a
// press and a release a few milliseconds apart, the next only once the
// BIOS buffer has room. A character with no key is typed as Alt+keypad
// digits, the way DOS takes any code page character.
void SessionImpl::PasteStep()
{
	std::lock_guard<std::mutex> lock(pasteMtx);
	if (paste.empty())
		return;
	const int64_t now = NowMs();
	if (now < pasteAt)
		return;
	Stroke& st = paste.front();
	if (pastePhase == 0) {
		if (BiosBufferCount() >= BufferRoom)
			return;
		if (st.altCode) {
			keyboard_input(1, ToScan(HID_LALT));
			char digits[4];
			snprintf(digits, sizeof digits, "%d", st.altCode);
			for (const char* d = digits; *d; d++) {
				uint32_t hid = *d == '0' ? 0x62 : 0x59 + (uint32_t)(*d - '1');
				keyboard_input(1, ToScan(hid));
				keyboard_input(0, ToScan(hid));
			}
			keyboard_input(0, ToScan(HID_LALT));
			paste.pop_front();
			pasteAt = now + StepMs * 2;
			return;
		}
		if (st.shift)
			keyboard_input(1, ToScan(HID_LSHIFT));
		keyboard_input(1, ToScan(st.hid));
		pastePhase = 1;
		pasteAt = now + StepMs;
		return;
	}
	keyboard_input(0, ToScan(st.hid));
	if (st.shift)
		keyboard_input(0, ToScan(HID_LSHIFT));
	pasteAt = now + (st.hid == HID_ENTER ? StepMs * 5 : StepMs);
	pastePhase = 0;
	paste.pop_front();
}

// ---------------------------------------------------------------------------
// the mouse
// ---------------------------------------------------------------------------

int msdossession_mouse_fitted(msdossession* s)
{
	return strcmp(prefs_mouse(s), "none") != 0;
}

void msdossession_mouse_motion(msdossession* s, int dx, int dy)
{
	if (s->running)
		Impl(s)->host.MouseMotion(dx, dy);
}

void msdossession_mouse_buttons(msdossession* s, unsigned buttons)
{
	if (s->running)
		Impl(s)->host.MouseButtons((int)(buttons & 7));
}

void msdossession_mouse_wheel(msdossession* s, int dz)
{
	if (s->running)
		Impl(s)->host.MouseWheel(dz);
}

// ---------------------------------------------------------------------------
// the game port
// ---------------------------------------------------------------------------

int msdossession_joystick_fitted(msdossession* s)
{
	return strcmp(prefs_joystick(s), "none") != 0;
}

void SessionImpl::PushSticks()
{
	std::lock_guard<std::mutex> lock(stickMtx);
	for (int i = 0; i < MSDOS_JOYSTICKS; i++) {
		const int axes[4] = { sticks[i].x, sticks[i].y, 0, 0 };
		msdos_host_joystick_set(i, axes, sticks[i].buttons, -1);
	}
}

void msdossession_joystick(msdossession* s, int stick, int x, int y, unsigned buttons)
{
	SessionImpl* im = Impl(s);
	if (stick < 0 || stick >= MSDOS_JOYSTICKS)
		return;
	{
		std::lock_guard<std::mutex> lock(im->stickMtx);
		if (im->sticks[stick].pad && !buttons && !x && !y)
			return;     // a gamepad has it
		im->sticks[stick] = { x, y, buttons, false };
	}
	im->PushSticks();
}

void session_gamepad_stick(struct msdossession* s, int stick, int x, int y, unsigned buttons,
                           int present)
{
	SessionImpl* im = Impl(s);
	if (stick < 0 || stick >= MSDOS_JOYSTICKS)
		return;
	{
		std::lock_guard<std::mutex> lock(im->stickMtx);
		im->sticks[stick] = { x, y, buttons, present != 0 };
	}
	im->PushSticks();
}

void msdossession_joystick_state(msdossession* s, int stick, int* x, int* y, unsigned* buttons)
{
	SessionImpl* im = Impl(s);
	if (stick < 0 || stick >= MSDOS_JOYSTICKS)
		return;
	std::lock_guard<std::mutex> lock(im->stickMtx);
	if (x) *x = im->sticks[stick].x;
	if (y) *y = im->sticks[stick].y;
	if (buttons) *buttons = im->sticks[stick].buttons;
}
