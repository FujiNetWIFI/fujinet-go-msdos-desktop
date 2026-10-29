/* The external-FujiNet address field: "host:port", a bare port, empty text
   for the default, and everything that must be refused; and the pref table's
   text row that stores it. */
#include "test_util.h"

static int failures;

static void ok(const char *text, const char *want_host, int want_port)
{
    char host[64] = "";
    int port = 0;
    int rc = msdossession_parse_hostport(text, host, sizeof host, &port);
    if (rc != 0 || strcmp(host, want_host) || port != want_port) {
        fprintf(stderr, "FAIL \"%s\": rc %d, got %s:%d, want %s:%d\n", text ? text : "(null)",
                rc, host, port, want_host, want_port);
        failures++;
    }
}

static void bad(const char *text)
{
    char host[64] = "keep";
    int port = 7;
    if (msdossession_parse_hostport(text, host, sizeof host, &port) == 0 ||
        strcmp(host, "keep") || port != 7) {
        fprintf(stderr, "FAIL \"%s\" should be refused untouched\n", text);
        failures++;
    }
}

int main(void)
{
    ok("", "127.0.0.1", 11516);
    ok("   ", "127.0.0.1", 11516);
    ok(NULL, "127.0.0.1", 11516);
    ok("11600", "127.0.0.1", 11600);
    ok("0.0.0.0:11516", "0.0.0.0", 11516);
    ok("fujinet.local:11516", "fujinet.local", 11516);
    ok("192.168.1.20:9985", "192.168.1.20", 9985);
    ok("  127.0.0.1:1  ", "127.0.0.1", 1);
    ok("localhost:65535", "localhost", 65535);

    bad("0");
    bad("65536");
    bad("-5");
    bad("abc");
    bad("0.0.0.0:");
    bad(":11516");
    bad("0.0.0.0:abc");
    bad("bad host:11516");
    bad(".local:11516");
    bad("a_b:11516");
    bad("1.2.3.4:11516x");

    {
        char text[256];
        msdossession *s = test_session_ex("hostport", 0, 0);
        int ext, addr;

        CHECK(s, "session");
        ext = msdossession_pref_find("fujinet_external");
        addr = msdossession_pref_find("fujinet_external_addr");
        CHECK(ext >= 0 && addr >= 0, "the external FujiNet prefs exist");
        CHECK(msdossession_pref(addr)->type == MSDOS_PREF_TEXT, "the address is a text pref");
        CHECK(!msdossession_pref_enabled(s, addr), "the address is greyed while off");
        msdossession_pref_set_int(s, ext, 1);
        CHECK(msdossession_pref_enabled(s, addr), "the address is live while on");
        msdossession_pref_set_text(s, addr, "  192.168.1.20:9985 ");
        msdossession_pref_get_text(s, addr, text, sizeof text);
        CHECK(!strcmp(text, "192.168.1.20:9985"), "the text is kept trimmed: \"%s\"", text);
        CHECK(msdossession_pref_text_valid(addr, "10.0.0.5:11516"), "valid text");
        CHECK(!msdossession_pref_text_valid(addr, "nonsense here"), "invalid text");
        msdossession_free(s);
    }
    return failures ? 1 : 0;
}
