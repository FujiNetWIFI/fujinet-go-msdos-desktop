/* The ROM table and import, with no BIOS: nothing is available, a start
   says why, and files that match nothing import nothing. */
#include "test_util.h"

extern const char msdos_rom_table_source[];

int main(void)
{
    char msg[2048], dir[600], path[700];
    msdossession *s = test_session_ex("roms", 0, 0);
    msdossession_start_opts o = {0, 0, 0};
    int i, n = msdossession_rom_item_count();
    FILE *f;

    CHECK(s, "new");
    CHECK(strstr(msdos_rom_table_source, "86Box/roms") != NULL, "table cites its source: %s",
          msdos_rom_table_source);
    CHECK(n > 10, "%d ROM items", n);
    for (i = 0; i < n; i++) {
        const msdos_rom_item *it = msdossession_rom_item(i);
        CHECK(it && it->name && *it->name && it->group && *it->group, "item %d", i);
        CHECK(!msdossession_rom_item_available(s, i), "%s available with no ROMs", it->name);
    }
    for (i = 0; i < MSDOS_M_COUNT; i++)
        CHECK(!msdossession_machine_available(s, i), "machine %d available", i);
    CHECK(!msdossession_has_bios(s), "has_bios");
    CHECK(msdossession_start(s, &o) != 0, "started with no BIOS");
    CHECK(*msdossession_last_error(s), "no reason given");

    /* junk, and a BIOS no machine here uses: nothing comes in */
    test_tmpdir(dir, sizeof dir, "roms-junk");
    snprintf(path, sizeof path, "%s/junk.bin", dir);
    f = fopen(path, "wb");
    CHECK(f, "junk");
    for (i = 0; i < 8192; i++) fputc((i * 7) & 0xFF, f);
    fclose(f);
    snprintf(path, sizeof path, "%s/GLABIOS.ROM", dir);
    CHECK(test_copy(MSDOS_TEST_FIXTURES "/roms/machines/glabios/GLABIOS_0.4.0_8X.ROM", path) == 0,
          "fixture");
    msg[0] = 0;
    msdossession_rom_import(s, dir, msg, sizeof msg);
    CHECK(!msdossession_has_bios(s), "imported something from junk: %s", msg);

    msdossession_free(s);
    puts("roms: ok");
    return 0;
}
