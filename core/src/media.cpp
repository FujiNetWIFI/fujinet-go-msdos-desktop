/*
 * Media: the PC's own floppy drives and hard disks, blank images, FujiNet's
 * SD folder, and Open's routing.
 *
 * What is in each drive lives in the settings ("floppy_a", "hd_c.<machine>"),
 * so a restart finds the same disks. "@boot" is the FujiNet boot floppy.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "session_impl.h"

extern "C" {
#include "zipread.h"
}

namespace {

std::string lower_ext(const char* path)
{
	const char* dot = strrchr(path, '.');
	const char* slash = std::max(strrchr(path, '/'), strrchr(path, '\\'));
	if (!dot || (slash && dot < slash))
		return "";
	std::string e = dot + 1;
	for (auto& c : e)
		c = (char)tolower((unsigned char)c);
	return e;
}

std::string base_name(const char* path)
{
	const char* a = strrchr(path, '/');
	const char* b = strrchr(path, '\\');
	const char* p = std::max(a, b);
	return p ? p + 1 : path;
}

long long file_size(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

bool write_file(const std::string& path, const void* data, size_t n)
{
	FILE* f = fopen(path.c_str(), "wb");
	if (!f)
		return false;
	bool ok = fwrite(data, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

bool copy_file(const char* src, const std::string& dst)
{
	FILE* in = fopen(src, "rb");
	if (!in)
		return false;
	FILE* out = fopen(dst.c_str(), "wb");
	if (!out) {
		fclose(in);
		return false;
	}
	char buf[65536];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof buf, in)) > 0)
		if (fwrite(buf, 1, n, out) != n) {
			ok = false;
			break;
		}
	fclose(in);
	return fclose(out) == 0 && ok;
}

void set_msg(char* msg, int msgsz, const std::string& text)
{
	if (msg && msgsz > 0)
		snprintf(msg, (size_t)msgsz, "%s", text.c_str());
}

// Each machine has its own disks.
std::string floppy_key(msdossession* s, int d)
{
	return std::string(d ? "floppy_b." : "floppy_a.") + msdos_profile_get(prefs_machine(s))->internal;
}

std::string floppy_ro_key(msdossession* s, int d)
{
	return std::string(d ? "floppy_b_ro." : "floppy_a_ro.") + msdos_profile_get(prefs_machine(s))->internal;
}

std::string hd_key(msdossession* s, int d)
{
	return std::string(d ? "hd_d." : "hd_c.") + msdos_profile_get(prefs_machine(s))->internal;
}

const char* fdd_label(const char* type)
{
	static const struct { const char* t; const char* l; } names[] = {
		{ "525_2dd", "5.25\" 360K" }, { "525_2hd", "5.25\" 1.2M" }, { "35_2dd", "3.5\" 720K" },
		{ "35_2hd", "3.5\" 1.44M" }, { "35_2ed", "3.5\" 2.88M" }, { nullptr, nullptr } };
	for (int i = 0; names[i].t; i++)
		if (!strcmp(names[i].t, type))
			return names[i].l;
	return "None";
}

// Image sizes floppies come in.
bool floppy_sized(long long n)
{
	static const long long sizes[] = { 163840, 184320, 327680, 368640, 737280, 1228800,
	                                   1474560, 1720320, 2949120, 0 };
	for (int i = 0; sizes[i]; i++)
		if (n == sizes[i])
			return true;
	return false;
}

// A blank, formatted FAT12 floppy of the drive's capacity.
bool make_blank_floppy(const std::string& path, const char* type)
{
	struct G { const char* t; int sectors, spc, root, media, spf, spt; } geo[] = {
		{ "525_2dd", 720, 2, 112, 0xFD, 2, 9 }, { "525_2hd", 2400, 1, 224, 0xF9, 7, 15 },
		{ "35_2dd", 1440, 2, 112, 0xF9, 3, 9 }, { "35_2hd", 2880, 1, 224, 0xF0, 9, 18 },
		{ "35_2ed", 5760, 2, 240, 0xF0, 9, 36 }, { nullptr, 0, 0, 0, 0, 0, 0 } };
	const G* g = &geo[0];
	for (int i = 0; geo[i].t; i++)
		if (!strcmp(geo[i].t, type))
			g = &geo[i];
	std::vector<uint8_t> img((size_t)g->sectors * 512, 0);
	uint8_t* b = img.data();
	// a boot sector that says it is not a system disk and waits for a key
	static const uint8_t code[] = {
		0xEB, 0x3C, 0x90, 'F', 'U', 'J', 'I', 'N', 'E', 'T', ' ' };
	memcpy(b, code, sizeof code);
	auto w16 = [&](int off, int v) { b[off] = (uint8_t)v; b[off + 1] = (uint8_t)(v >> 8); };
	w16(11, 512);
	b[13] = (uint8_t)g->spc;
	w16(14, 1);
	b[16] = 2;
	w16(17, g->root);
	w16(19, g->sectors);
	b[21] = (uint8_t)g->media;
	w16(22, g->spf);
	w16(24, g->spt);
	w16(26, 2);
	b[38] = 0x29;
	memcpy(b + 43, "NO NAME    FAT12   ", 19);
	// INT 18h-free stub: print nothing, wait for a key, reboot
	static const uint8_t boot[] = { 0xFA, 0x31, 0xC0, 0x8E, 0xD0, 0xBC, 0x00, 0x7C, 0xFB,
	                                0x31, 0xC0, 0xCD, 0x16, 0xCD, 0x19 };
	memcpy(b + 62, boot, sizeof boot);
	b[510] = 0x55;
	b[511] = 0xAA;
	for (int fat = 0; fat < 2; fat++) {
		uint8_t* f = b + 512 + fat * g->spf * 512;
		f[0] = (uint8_t)g->media;
		f[1] = 0xFF;
		f[2] = 0xFF;
	}
	return write_file(path, img.data(), img.size());
}

// A raw or fixed-VHD hard disk image of a geometry.
bool make_blank_hd(const std::string& path, int spt, int heads, int cyls)
{
	const long long bytes = (long long)spt * heads * cyls * 512;
	FILE* f = fopen(path.c_str(), "wb");
	if (!f)
		return false;
	bool ok = true;
	if (bytes > 0) {
		ok = fseek(f, (long)(bytes - 1), SEEK_SET) == 0 && fputc(0, f) != EOF;
	}
	if (ok && lower_ext(path.c_str()) == "vhd") {
		// a fixed VHD: the raw disk and a 512-byte footer
		uint8_t foot[512] = { 0 };
		auto be32 = [&](int off, uint32_t v) {
			foot[off] = (uint8_t)(v >> 24); foot[off + 1] = (uint8_t)(v >> 16);
			foot[off + 2] = (uint8_t)(v >> 8); foot[off + 3] = (uint8_t)v; };
		memcpy(foot, "conectix", 8);
		be32(8, 2);                          // features
		be32(12, 0x00010000);                // version
		be32(16, 0xFFFFFFFF);                // data offset: fixed
		be32(20, 0xFFFFFFFF);
		memcpy(foot + 28, "fjgo", 4);        // creator application
		be32(32, 0x00010000);
		memcpy(foot + 36, "Wi2k", 4);
		be32(40, (uint32_t)(bytes >> 32));   // original size
		be32(44, (uint32_t)bytes);
		be32(48, (uint32_t)(bytes >> 32));   // current size
		be32(52, (uint32_t)bytes);
		foot[56] = (uint8_t)(cyls >> 8);
		foot[57] = (uint8_t)cyls;
		foot[58] = (uint8_t)heads;
		foot[59] = (uint8_t)spt;
		be32(60, 2);                         // disk type: fixed
		for (int i = 0; i < 16; i++)
			foot[68 + i] = (uint8_t)(rand() & 0xFF);
		uint32_t sum = 0;
		for (int i = 0; i < 512; i++)
			sum += foot[i];
		be32(64, ~sum);
		ok = fseek(f, 0, SEEK_END) == 0 && fwrite(foot, 1, 512, f) == 512;
	}
	return fclose(f) == 0 && ok;
}

} // namespace

// ---------------------------------------------------------------------------
// what is in the drives
// ---------------------------------------------------------------------------

const char* media_floppy(struct msdossession* s, int d, int* read_only)
{
	static thread_local char path[MSDOS_PATH_MAX];
	const char* v = msdossession_get_str(s, floppy_key(s, d).c_str(), d == 0 ? "@boot" : "");
	if (read_only)
		*read_only = msdossession_get_int(s, floppy_ro_key(s, d).c_str(), 0);
	if (!strcmp(v, "@boot")) {
		if (bootdisk_path(s, prefs_fdd_type(s, d), path, sizeof path) != 0)
			return "";
		return path;
	}
	snprintf(path, sizeof path, "%s", v);
	return path;
}

const char* media_hd(struct msdossession* s, int d)
{
	static thread_local char path[MSDOS_PATH_MAX];
	snprintf(path, sizeof path, "%s", msdossession_get_str(s, hd_key(s, d).c_str(), ""));
	return path;
}

void media_migrate(struct msdossession* s)
{
	for (int d = 0; d < MSDOS_FLOPPIES; d++) {
		const std::string keys[2][2] = {
			{ d ? "floppy_b" : "floppy_a", floppy_key(s, d) },
			{ d ? "floppy_b_ro" : "floppy_a_ro", floppy_ro_key(s, d) },
		};
		for (const auto& k : keys) {
			const char* old = msdossession_get_str(s, k[0].c_str(), nullptr);
			if (!old)
				continue;
			const std::string v = old;
			if (!msdossession_get_str(s, k[1].c_str(), nullptr))
				msdossession_set_str(s, k[1].c_str(), v.c_str());
			settings_remove(s, k[0].c_str());
		}
	}
}

int msdossession_floppy_info(msdossession* s, int drive, msdos_floppy_info* out)
{
	if (drive < 0 || drive >= MSDOS_FLOPPIES || !out)
		return -1;
	memset(out, 0, sizeof *out);
	const char* type = prefs_fdd_type(s, drive);
	out->present = strcmp(type, "none") != 0;
	snprintf(out->type, sizeof out->type, "%s", fdd_label(type));
	int ro = 0;
	const char* v = msdossession_get_str(s, floppy_key(s, drive).c_str(), drive == 0 ? "@boot" : "");
	out->boot_disk = !strcmp(v, "@boot");
	snprintf(out->path, sizeof out->path, "%s", media_floppy(s, drive, &ro));
	out->read_only = ro;
	out->active = (Impl(s)->activeFloppy.load() >> drive) & 1;
	return 0;
}

static int remount(msdossession* s)
{
	if (s->running)
		Impl(s)->host.Call([s] { Impl(s)->MountFloppies(false); });
	return 0;
}

int msdossession_floppy_insert(msdossession* s, int drive, const char* path, int read_only,
                               char* msg, int msgsz)
{
	if (drive < 0 || drive >= MSDOS_FLOPPIES || !path)
		return -1;
	if (!strcmp(prefs_fdd_type(s, drive), "none")) {
		set_msg(msg, msgsz, std::string("There is no drive ") + (drive ? "B:" : "A:"));
		return -1;
	}
	if (file_size(path) <= 0) {
		set_msg(msg, msgsz, "Cannot read " + base_name(path));
		return -1;
	}
	msdossession_set_str(s, floppy_key(s, drive).c_str(), path);
	msdossession_set_int(s, floppy_ro_key(s, drive).c_str(), read_only ? 1 : 0);
	msdossession_settings_flush(s);
	remount(s);
	set_msg(msg, msgsz, base_name(path) + " is in " + (drive ? "B:" : "A:"));
	return 0;
}

int msdossession_floppy_eject(msdossession* s, int drive)
{
	if (drive < 0 || drive >= MSDOS_FLOPPIES)
		return -1;
	msdossession_set_str(s, floppy_key(s, drive).c_str(), "");
	msdossession_settings_flush(s);
	return remount(s);
}

int msdossession_floppy_set_read_only(msdossession* s, int drive, int ro)
{
	if (drive < 0 || drive >= MSDOS_FLOPPIES)
		return -1;
	msdossession_set_int(s, floppy_ro_key(s, drive).c_str(), ro ? 1 : 0);
	return remount(s);
}

int msdossession_floppy_boot_disk(msdossession* s, int drive)
{
	if (drive < 0 || drive >= MSDOS_FLOPPIES)
		return -1;
	msdossession_set_str(s, floppy_key(s, drive).c_str(), "@boot");
	msdossession_settings_flush(s);
	return remount(s);
}

int msdossession_new_blank_floppy(msdossession* s, int drive, const char* path,
                                  char* msg, int msgsz)
{
	const char* type = prefs_fdd_type(s, drive);
	if (!strcmp(type, "none"))
		type = "525_2dd";
	if (!make_blank_floppy(path, type)) {
		set_msg(msg, msgsz, std::string("Cannot write ") + path);
		return -1;
	}
	set_msg(msg, msgsz, std::string("A blank ") + fdd_label(type) + " disk");
	return 0;
}

int msdossession_hd_info(msdossession* s, int drive, msdos_hd_info* out)
{
	if (drive < 0 || drive >= MSDOS_HARD_DISKS || !out)
		return -1;
	memset(out, 0, sizeof *out);
	out->present = strcmp(prefs_hdc(s), "none") != 0;
	snprintf(out->path, sizeof out->path, "%s", media_hd(s, drive));
	int i = msdossession_pref_find(drive ? "hd_d_type" : "hd_c_type");
	const char* label = msdossession_pref_choice(s, i, msdossession_pref_get_choice(s, i));
	snprintf(out->type, sizeof out->type, "%s", label ? label : "");
	out->active = (Impl(s)->activeHd.load() >> drive) & 1;
	return 0;
}

int msdossession_hd_attach(msdossession* s, int drive, const char* path, char* msg, int msgsz)
{
	if (drive < 0 || drive >= MSDOS_HARD_DISKS || !path)
		return -1;
	if (!strcmp(prefs_hdc(s), "none")) {
		set_msg(msg, msgsz, "This machine has no hard disk controller (Preferences > Storage)");
		return -1;
	}
	msdossession_set_str(s, hd_key(s, drive).c_str(), path);
	msdossession_settings_flush(s);
	set_msg(msg, msgsz, base_name(path) + " is " + (drive ? "D:" : "C:") + " from the next reset");
	return 0;
}

int msdossession_hd_detach(msdossession* s, int drive)
{
	if (drive < 0 || drive >= MSDOS_HARD_DISKS)
		return -1;
	msdossession_set_str(s, hd_key(s, drive).c_str(), "");
	msdossession_settings_flush(s);
	return 0;
}

int msdossession_new_blank_hd(msdossession* s, int drive, const char* path, char* msg, int msgsz)
{
	int spt, heads, cyls;
	const char* bus;
	prefs_hd_type(s, drive, &spt, &heads, &cyls, &bus);
	if (!spt) {
		// "From the image" has none to give: the controller's usual size
		if (!strcmp(bus, "mfm")) { spt = 17; heads = 4; cyls = 615; }
		else { spt = 63; heads = 16; cyls = 1024; }
	}
	if (!make_blank_hd(path, spt, heads, cyls)) {
		set_msg(msg, msgsz, std::string("Cannot write ") + path);
		return -1;
	}
	char size[32];
	snprintf(size, sizeof size, "%lld MB", (long long)spt * heads * cyls * 512 / (1024 * 1024));
	set_msg(msg, msgsz, std::string("A blank ") + size + " disk (" + std::to_string(cyls) + "/" +
	                        std::to_string(heads) + "/" + std::to_string(spt) +
	                        "); FDISK and FORMAT it in DOS");
	return 0;
}

// ---------------------------------------------------------------------------
// FujiNet's SD folder
// ---------------------------------------------------------------------------

int msdossession_import_to_sd(msdossession* s, const char* src, char* dest_out, int dest_sz)
{
	std::string sd = s->fujinet_sd, first;
	paths_mkdir_p(s->fujinet_sd);
	zipread* z = lower_ext(src) == "zip" ? zipread_open(src) : nullptr;
	if (z) {
		for (int i = 0; i < zipread_count(z); i++) {
			std::string name = base_name(zipread_name(z, i));
			uint8_t* d;
			uint32_t n;
			if (name.empty() || zipread_extract(z, i, &d, &n, 64u << 20) != 0)
				continue;
			std::string dst = sd + "/" + name;
			if (write_file(dst, d, n) && first.empty())
				first = dst;
			free(d);
		}
		zipread_close(z);
		if (first.empty())
			return -1;
	} else {
		first = sd + "/" + base_name(src);
		if (!copy_file(src, first))
			return -1;
	}
	if (dest_out && dest_sz > 0)
		snprintf(dest_out, (size_t)dest_sz, "%s", first.c_str());
	return 0;
}

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

int msdossession_media_kind(const char* path)
{
	const std::string e = lower_ext(path);
	const long long n = file_size(path);
	if (e == "86f" || e == "imd" || e == "td0" || e == "fdi" || e == "mfm" || e == "vfd"
	    || e == "flp" || e == "ima" || ((e == "img" || e == "dsk") && floppy_sized(n)))
		return MSDOS_MEDIA_FLOPPY;
	if (e == "vhd" || e == "hdi" || e == "hdx" || ((e == "img" || e == "dsk") && n > 2949120))
		return MSDOS_MEDIA_HARD_DISK;
	if (e == "rom" || e == "bin" || e == "awd" || e == "bio" || e == "vbi" || e == "raw")
		return MSDOS_MEDIA_ROM;
	return MSDOS_MEDIA_UNKNOWN;
}

int msdossession_open(msdossession* s, const char* path, char* msg, int msgsz)
{
	switch (msdossession_media_kind(path)) {
	case MSDOS_MEDIA_FLOPPY:
		return msdossession_floppy_insert(s, 0, path, 0, msg, msgsz);
	case MSDOS_MEDIA_HARD_DISK:
		set_msg(msg, msgsz, "Which drive?");
		return 1;
	case MSDOS_MEDIA_ROM:
		return msdossession_rom_import(s, path, msg, msgsz);
	default:
		break;
	}
	// a zip of ROMs is an import; anything else goes on the SD card
	if (lower_ext(path) == "zip") {
		char rmsg[512];
		if (msdossession_rom_import(s, path, rmsg, sizeof rmsg) == 0) {
			set_msg(msg, msgsz, rmsg);
			return 0;
		}
	}
	char dest[MSDOS_PATH_MAX];
	if (msdossession_import_to_sd(s, path, dest, sizeof dest) != 0) {
		set_msg(msg, msgsz, "Could not copy " + base_name(path) + " to FujiNet's SD card");
		return -1;
	}
	set_msg(msg, msgsz, base_name(dest) + " is on FujiNet's SD card");
	return 0;
}
