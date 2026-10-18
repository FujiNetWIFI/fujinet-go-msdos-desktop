/* Floppies and hard disks: blank images of each drive's size, insert and
   eject, attach a blank hard disk, Open's routing, Import to SD. */
#include "test_util.h"

int main(void)
{
    char dir[600], path[800], msg[1024], dest[1024];
    msdos_floppy_info fi;
    msdos_hd_info hi;
    msdossession *s = test_session_ex("media", 0, 0);
    FILE *f;
    CHECK(s, "new");
    test_tmpdir(dir, sizeof dir, "media-files");
    test_set_pref(s, "machine", MSDOS_M_5160);

    /* a blank 360K floppy for the XT's A: */
    snprintf(path, sizeof path, "%s/blank.img", dir);
    CHECK(msdossession_new_blank_floppy(s, 0, path, msg, sizeof msg) == 0, "blank: %s", msg);
    CHECK(test_file_size(path) == 368640, "blank is %d bytes", test_file_size(path));
    CHECK(msdossession_media_kind(path) == MSDOS_MEDIA_FLOPPY, "kind of a floppy");
    CHECK(msdossession_floppy_insert(s, 0, path, 0, msg, sizeof msg) == 0, "insert: %s", msg);
    CHECK(msdossession_floppy_info(s, 0, &fi) == 0 && !strcmp(fi.path, path) && !fi.boot_disk,
          "A: has %s", fi.path);
    CHECK(msdossession_floppy_eject(s, 0) == 0, "eject");
    CHECK(msdossession_floppy_info(s, 0, &fi) == 0 && fi.path[0] == 0, "A: empty");
    CHECK(msdossession_floppy_boot_disk(s, 0) == 0, "boot disk");
    CHECK(msdossession_floppy_info(s, 0, &fi) == 0 && fi.boot_disk, "A: boot disk");
    /* the XT has no B: drive by default */
    CHECK(msdossession_floppy_insert(s, 1, path, 0, msg, sizeof msg) != 0, "B: took a disk");

    /* the 486's A: is 1.44M */
    test_set_pref(s, "machine", MSDOS_M_486);
    snprintf(path, sizeof path, "%s/blank144.img", dir);
    CHECK(msdossession_new_blank_floppy(s, 0, path, msg, sizeof msg) == 0, "blank: %s", msg);
    CHECK(test_file_size(path) == 1474560, "1.44M blank is %d bytes", test_file_size(path));

    /* a hard disk for the XT: the type in Preferences sizes it */
    test_set_pref(s, "machine", MSDOS_M_5160);
    test_set_pref(s, "hdc", 1);
    snprintf(path, sizeof path, "%s/c.img", dir);
    CHECK(msdossession_new_blank_hd(s, 0, path, msg, sizeof msg) == 0, "blank hd: %s", msg);
    CHECK(test_file_size(path) > 10 * 1024 * 1024, "hd is %d bytes", test_file_size(path));
    CHECK(msdossession_media_kind(path) == MSDOS_MEDIA_HARD_DISK, "kind of a hard disk");
    CHECK(msdossession_hd_attach(s, 0, path, msg, sizeof msg) == 0, "attach: %s", msg);
    CHECK(msdossession_hd_info(s, 0, &hi) == 0 && !strcmp(hi.path, path) && hi.present, "C:");
    CHECK(msdossession_hd_detach(s, 0) == 0, "detach");
    CHECK(msdossession_hd_info(s, 0, &hi) == 0 && hi.path[0] == 0, "C: gone");

    /* anything else goes on FujiNet's SD card */
    snprintf(path, sizeof path, "%s/README.TXT", dir);
    f = fopen(path, "wb");
    CHECK(f, "readme");
    fputs("hello\n", f);
    fclose(f);
    CHECK(msdossession_media_kind(path) == MSDOS_MEDIA_UNKNOWN, "kind of a text file");
    CHECK(msdossession_import_to_sd(s, path, dest, sizeof dest) == 0, "import to SD");
    CHECK(test_file_size(dest) == 6, "SD copy %s", dest);
    CHECK(strncmp(dest, msdossession_sd_path(s), strlen(msdossession_sd_path(s))) == 0,
          "%s is not under %s", dest, msdossession_sd_path(s));

    msdossession_free(s);
    puts("media: ok");
    return 0;
}
