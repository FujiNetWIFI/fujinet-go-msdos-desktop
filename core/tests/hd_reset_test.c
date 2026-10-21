/* A hard disk attached while the PC runs is there after Reset, with no
   restart, and gone after the next one once detached: the 5160 (GLaBIOS)
   with the IBM Fixed Disk Adapter, whose BIOS counts the drives into
   0040:0075. CI has no controller ROM: set MSDOS_TEST_ROMS to a folder with
   hdd/st506/ibm_xebec_62x0822_1985.bin (an 86Box ROM set), else SKIP (77). */
#include "test_util.h"
#include "msdosdebug.h"

static int fixed_disks(msdossession *s)
{
    uint8_t n = 0xFF;
    msdosdebug_read(msdossession_debugger(s), 0x475, &n, 1);
    return n;
}

int main(void)
{
    const char *roms = getenv("MSDOS_TEST_ROMS");
    char rom[800], dir[600], path[800], msg[1024];
    msdossession *s;
    FILE *f;
    int hdc;
    if (!roms || !*roms) {
        puts("hd_reset: MSDOS_TEST_ROMS is not set; skipped");
        return 77;
    }
    snprintf(rom, sizeof rom, "%s/hdd/st506/ibm_xebec_62x0822_1985.bin", roms);
    if (!(f = fopen(rom, "rb"))) {
        printf("hd_reset: no %s; skipped\n", rom);
        return 77;
    }
    fclose(f);

    s = test_session_ex("hdreset", 0, 1);
    CHECK(s, "new");
    CHECK(msdossession_rom_import(s, rom, msg, sizeof msg) == 0, "import: %s", msg);
    test_set_pref(s, "machine", MSDOS_M_5160);
    test_set_pref(s, "hdc", 1);
    hdc = msdossession_pref_find("hdc");
    CHECK(msdossession_pref_choice_enabled(s, hdc, 1), "the Xebec adapter is not available");
    CHECK(msdossession_floppy_insert(s, 0, MSDOS_TEST_PLAIN_FLOPPY, 1, msg, sizeof msg) == 0,
          "insert: %s", msg);
    test_start(s, 0);
    msdossession_set_warp(s, 1);
    CHECK(test_wait_text(s, "A:\\>", 120000), "the DOS prompt");
    CHECK(fixed_disks(s) == 0, "%d fixed disks before attaching", fixed_disks(s));

    /* attached while it runs: C: from the reset */
    test_tmpdir(dir, sizeof dir, "hdreset-files");
    snprintf(path, sizeof path, "%s/c.img", dir);
    CHECK(msdossession_new_blank_hd(s, 0, path, msg, sizeof msg) == 0, "blank hd: %s", msg);
    CHECK(msdossession_hd_attach(s, 0, path, msg, sizeof msg) == 0, "attach: %s", msg);
    CHECK(msdossession_reset(s) == 0, "reset");
    CHECK(test_wait_text(s, "A:\\>", 120000), "the DOS prompt after attaching");
    CHECK(fixed_disks(s) == 1, "%d fixed disks after attaching and Reset", fixed_disks(s));

    /* detached: gone from the next reset */
    CHECK(msdossession_hd_detach(s, 0) == 0, "detach");
    CHECK(msdossession_reset(s) == 0, "reset");
    CHECK(test_wait_text(s, "A:\\>", 120000), "the DOS prompt after detaching");
    CHECK(fixed_disks(s) == 0, "%d fixed disks after detaching and Reset", fixed_disks(s));

    msdossession_stop(s);
    msdossession_free(s);
    puts("hd_reset: ok");
    return 0;
}
