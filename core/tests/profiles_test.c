/* Each machine is a profile: its ports, recompiler and disks come back when
   it is selected again, and settings saved when they were the app's become
   every machine's own (the disks only the selected one's). */
#include "test_util.h"

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    CHECK(f, "write %s", path);
    fputs(text, f);
    fclose(f);
}

static int read_file(const char *path, char *dst, int dstsz)
{
    FILE *f = fopen(path, "rb");
    int n;
    if (!f) return -1;
    n = (int)fread(dst, 1, (size_t)dstsz - 1, f);
    fclose(f);
    dst[n] = 0;
    return n;
}

static msdossession *reopen(const char *cfg, const char *data)
{
    msdossession_paths p;
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    p.fujinet_lib = "";
    return msdossession_new(&p);
}

int main(void)
{
    char dir[600], xt[800], at[800], msg[1024], key[128], cfg[1024], data[1024], ini[1100];
    char text[4096], seed[2048], again[4096];
    msdos_floppy_info fi;
    msdossession *s = test_session_ex("profiles", 0, 0);
    int m;
    CHECK(s, "new");
    test_tmpdir(dir, sizeof dir, "profiles-files");

    /* the XT: COM1 at 38400, a serial mouse, its own write-protected disk */
    test_set_pref(s, "machine", MSDOS_M_5160);
    test_set_pref(s, "fujinet_com", 0);
    test_set_pref(s, "fujinet_bps", 2);
    test_set_pref(s, "mouse", 1);
    snprintf(xt, sizeof xt, "%s/xt.img", dir);
    CHECK(msdossession_new_blank_floppy(s, 0, xt, msg, sizeof msg) == 0, "blank: %s", msg);
    CHECK(msdossession_floppy_insert(s, 0, xt, 1, msg, sizeof msg) == 0, "insert: %s", msg);
    CHECK(msdossession_pref_key(s, msdossession_pref_find("fujinet_com"), key, sizeof key) == 0
          && !strcmp(key, "fujinet_com.ibmxt86"), "key %s", key);
    CHECK(msdossession_pref_key(s, msdossession_pref_find("volume"), key, sizeof key) == 0
          && !strcmp(key, "volume"), "volume is the app's: %s", key);

    /* the 286 starts from its own defaults */
    test_set_pref(s, "machine", MSDOS_M_286);
    CHECK(msdossession_fujinet_com(s) == 2, "286 on COM%d", msdossession_fujinet_com(s));
    CHECK(test_get_pref(s, "fujinet_bps") == 4, "286 bps %d", test_get_pref(s, "fujinet_bps"));
    CHECK(test_get_pref(s, "mouse") == 0, "286 mouse %d", test_get_pref(s, "mouse"));
    CHECK(msdossession_floppy_info(s, 0, &fi) == 0 && fi.boot_disk && !fi.read_only,
          "286 A: %s ro %d", fi.path, fi.read_only);
    test_set_pref(s, "fujinet_com", 2);
    snprintf(at, sizeof at, "%s/at.img", dir);
    CHECK(msdossession_new_blank_floppy(s, 1, at, msg, sizeof msg) == 0, "blank B: %s", msg);
    CHECK(msdossession_floppy_insert(s, 1, at, 0, msg, sizeof msg) == 0, "insert B: %s", msg);

    /* the recompiler is the 386's and 486's own */
    test_set_pref(s, "machine", MSDOS_M_386);
    test_set_pref(s, "dynarec", 0);
    test_set_pref(s, "machine", MSDOS_M_486);
    CHECK(test_get_pref(s, "dynarec") == 1, "486 recompiler off");
    test_set_pref(s, "machine", MSDOS_M_386);
    CHECK(test_get_pref(s, "dynarec") == 0, "386 recompiler on");

    /* back to the XT: everything it had */
    test_set_pref(s, "machine", MSDOS_M_5160);
    CHECK(msdossession_fujinet_com(s) == 1, "XT on COM%d", msdossession_fujinet_com(s));
    CHECK(test_get_pref(s, "fujinet_bps") == 2, "XT bps %d", test_get_pref(s, "fujinet_bps"));
    CHECK(test_get_pref(s, "mouse") == 1, "XT mouse %d", test_get_pref(s, "mouse"));
    CHECK(msdossession_floppy_info(s, 0, &fi) == 0 && !strcmp(fi.path, xt) && fi.read_only,
          "XT A: %s ro %d", fi.path, fi.read_only);
    test_set_pref(s, "machine", MSDOS_M_286);
    CHECK(msdossession_fujinet_com(s) == 3, "286 on COM%d", msdossession_fujinet_com(s));
    CHECK(msdossession_floppy_info(s, 1, &fi) == 0 && !strcmp(fi.path, at), "286 B: %s", fi.path);

    /* settings from before machines had their own */
    snprintf(cfg, sizeof cfg, "%s", msdossession_config_path(s));
    snprintf(data, sizeof data, "%s", msdossession_data_path(s));
    msdossession_free(s);
    snprintf(ini, sizeof ini, "%s/settings.ini", cfg);
    snprintf(seed, sizeof seed,
             "machine=1\nfujinet_com=1\nmouse=msserial\nfloppy_a=%s\nfloppy_a_ro=1\n", xt);
    write_file(ini, seed);
    s = reopen(cfg, data);
    CHECK(s, "reopen");
    for (m = 0; m < MSDOS_M_COUNT; m++) {
        test_set_pref(s, "machine", m);
        CHECK(test_get_pref(s, "mouse") == 1, "%s: mouse %d", msdos_machine_name(m),
              test_get_pref(s, "mouse"));
        CHECK(msdossession_fujinet_com(s) == (m == MSDOS_M_PCJR ? 2 : 1), "%s: COM%d",
              msdos_machine_name(m), msdossession_fujinet_com(s));
        CHECK(msdossession_floppy_info(s, 0, &fi) == 0
              && (m == MSDOS_M_5160 ? !strcmp(fi.path, xt) && fi.read_only
                                    : fi.boot_disk && !fi.read_only),
              "%s: A: %s ro %d", msdos_machine_name(m), fi.path, fi.read_only);
    }
    CHECK(!msdossession_get_str(s, "fujinet_com", NULL) && !msdossession_get_str(s, "mouse", NULL)
          && !msdossession_get_str(s, "floppy_a", NULL)
          && !msdossession_get_str(s, "floppy_a_ro", NULL), "the old keys are still there");
    test_set_pref(s, "machine", MSDOS_M_5160);
    msdossession_free(s);

    /* once is enough */
    CHECK(read_file(ini, text, sizeof text) > 0, "read %s", ini);
    s = reopen(cfg, data);
    CHECK(s, "reopen again");
    msdossession_free(s);
    CHECK(read_file(ini, again, sizeof again) > 0 && !strcmp(text, again),
          "settings changed:\n%s\n----\n%s", text, again);

    puts("profiles: ok");
    return 0;
}
