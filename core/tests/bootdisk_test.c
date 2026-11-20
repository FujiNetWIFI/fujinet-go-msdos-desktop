/* The FujiNet boot floppy: one image per drive size, a FAT12 boot sector,
   and CONFIG.SYS rewritten in place for the COM port FujiNet is on (the
   PCjr's is always its internal port, FUJI_PORT=2). */
#include "test_util.h"

static char *read_all(const char *path, int *n)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)*n + 1);
    if (fread(buf, 1, (size_t)*n, f) != (size_t)*n) *n = 0;
    buf[*n] = 0;
    fclose(f);
    return buf;
}

static const char *find(const char *hay, int n, const char *needle)
{
    size_t k = strlen(needle);
    for (int i = 0; i + (int)k <= n; i++)
        if (!memcmp(hay + i, needle, k)) return hay + i;
    return NULL;
}

static void check_disk(msdossession *s, int bytes, const char *port_line, const char *system_file)
{
    msdos_floppy_info fi;
    char *img;
    int n = 0;
    CHECK(msdossession_floppy_info(s, 0, &fi) == 0 && fi.boot_disk, "A: has the boot disk");
    img = read_all(fi.path, &n);
    CHECK(img && n == bytes, "%s is %d bytes, not %d", fi.path, n, bytes);
    CHECK((unsigned char)img[510] == 0x55 && (unsigned char)img[511] == 0xAA, "boot signature");
    CHECK(((unsigned char)img[19] | ((unsigned char)img[20] << 8)) * 512 == bytes, "BPB sectors");
    CHECK(find(img, n, system_file) && find(img, n, "FUJINET SYS") && find(img, n, "CONFIG  EXE"),
          "directory");
    CHECK(find(img, n, port_line), "no \"%s\" in CONFIG.SYS", port_line);
    free(img);
}

int main(void)
{
    msdossession *s = test_session_ex("bootdisk", 0, 0);
    CHECK(s, "new");

    /* the 5160: a 360K drive, COM2 */
    test_set_pref(s, "machine", MSDOS_M_5160);
    check_disk(s, 368640, "FUJI_PORT=2 FUJI_BPS=115200", "KERNEL  SYS");

    /* COM4, 57600: the same image, rewritten */
    test_set_pref(s, "fujinet_com", 3);
    CHECK(msdossession_fujinet_com(s) == 4, "COM%d", msdossession_fujinet_com(s));
    {
        int i = msdossession_pref_find("fujinet_bps"), c;
        for (c = 0; msdossession_pref_choice(s, i, c); c++)
            if (!strcmp(msdossession_pref_choice(s, i, c), "57600")) break;
        CHECK(msdossession_pref_choice(s, i, c), "no 57600 choice");
        msdossession_pref_set_choice(s, i, c);
    }
    check_disk(s, 368640, "FUJI_PORT=4 FUJI_BPS=57600", "KERNEL  SYS");

    /* the 486's A: is a 1.44M drive, and its port its own */
    test_set_pref(s, "machine", MSDOS_M_486);
    check_disk(s, 1474560, "FUJI_PORT=2 FUJI_BPS=115200", "KERNEL  SYS");

    /* the PCjr: its own serial port, whatever the setting, and PC-DOS 2.10:
       IBMBIO and IBMDOS are the first two root entries, no LASTDRIVE */
    test_set_pref(s, "machine", MSDOS_M_PCJR);
    check_disk(s, 368640, "FUJI_PORT=2", "IBMBIO  COM");
    {
        msdos_floppy_info fi;
        int n = 0;
        char *img;
        CHECK(msdossession_floppy_info(s, 0, &fi) == 0, "info");
        CHECK(strstr(fi.path, "fujinet-boot-pcjr-360k.img"), "path %s", fi.path);
        img = read_all(fi.path, &n);
        CHECK(img && !memcmp(img + 5 * 512, "IBMBIO  COM", 11)
              && !memcmp(img + 5 * 512 + 32, "IBMDOS  COM", 11), "root order");
        CHECK(!find(img, n, "LASTDRIVE"), "DOS 2.x has no LASTDRIVE");
        free(img);
    }

    /* and back to a PC: FreeDOS again */
    test_set_pref(s, "machine", MSDOS_M_5160);
    check_disk(s, 368640, "FUJI_PORT=4 FUJI_BPS=57600", "KERNEL  SYS");

    msdossession_free(s);
    puts("bootdisk: ok");
    return 0;
}
