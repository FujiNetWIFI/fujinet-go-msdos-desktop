/* Shared helpers for the session tests. */
#ifndef MSDOS_TEST_UTIL_H
#define MSDOS_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <io.h>
#define _exit(c) _exit(c)
#else
#include <unistd.h>
#endif

#include "msdossession.h"

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#include <windows.h>
#define test_mkdir(p) _mkdir(p)
#define test_getpid() _getpid()
static inline void test_sleep_ms(int ms) { Sleep((DWORD)ms); }
static inline int test_setenv(const char *k, const char *v) { return _putenv_s(k, v); }
#else
#include <sys/stat.h>
#define test_mkdir(p) mkdir(p, 0755)
#define test_getpid() getpid()
static inline void test_sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
static inline int test_setenv(const char *k, const char *v) { return setenv(k, v, 1); }
#endif

/* FujiNet's runtime captures stdout and stderr into its log: give its log
   thread a moment to pass the message on before exiting. */
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); \
    test_sleep_ms(500); _exit(1); } } while (0)

/* A fresh, private tree for a test, so it never reads or writes the
   developer's real settings. */
static inline void test_tmpdir(char *dst, size_t dstsz, const char *tag)
{
    const char *base = getenv("TMPDIR");
#if defined(_WIN32)
    if (!base || !*base) base = getenv("TEMP");
#endif
    if (!base || !*base) base = "/tmp";
    snprintf(dst, dstsz, "%s/msdos-%s-%ld-%ld", base, tag,
             (long)test_getpid(), (long)time(NULL));
    test_mkdir(dst);
}

static inline int test_copy(const char *from, const char *to)
{
    char buf[8192];
    size_t n;
    FILE *in = fopen(from, "rb"), *out;
    if (!in) return -1;
    out = fopen(to, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return 0;
}

/* A session in a fresh tree. With FujiNet, its own fnconfig.ini: the SD
   card and no network hosts (tests never touch real servers). With
   glabios, the 5150/5160 run GLaBIOS (GPL; core/tests/fixtures), so CI
   boots with no IBM ROM. */
static inline msdossession *test_session_ex(const char *tag, int fujinet, int glabios)
{
    char base[512], cfg[600], data[600], path[800];
    msdossession_paths p;
    FILE *fp;
    test_tmpdir(base, sizeof base, tag);
    snprintf(cfg, sizeof cfg, "%s/config", base);
    snprintf(data, sizeof data, "%s/data", base);
    test_mkdir(data);
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    if (!fujinet)
        p.fujinet_lib = "";
    else {
        snprintf(path, sizeof path, "%s/fujinet", data);
        test_mkdir(path);
        snprintf(path, sizeof path, "%s/fujinet/fnconfig.ini", data);
        if ((fp = fopen(path, "w")) != NULL) {
            fputs("[General]\ndevicename=fujinet-test\nconfig_enabled=1\nboot_mode=0\n"
                  "[Host1]\ntype=SD\nname=SD\n", fp);
            fclose(fp);
        }
    }
    if (glabios) {
        static const char *const dirs[] = { "roms", "roms/machines", "roms/machines/glabios" };
        test_setenv("MSDOS_TEST_GLABIOS", "1");
        for (int i = 0; i < 3; i++) {
            snprintf(path, sizeof path, "%s/%s", data, dirs[i]);
            test_mkdir(path);
        }
        snprintf(path, sizeof path, "%s/roms/machines/glabios/GLABIOS_0.4.0_8X.ROM", data);
        CHECK(test_copy(MSDOS_TEST_FIXTURES "/roms/machines/glabios/GLABIOS_0.4.0_8X.ROM",
                        path) == 0, "copy the GLaBIOS fixture");
    }
    return msdossession_new(&p);
}

static inline void test_start(msdossession *s, int fujinet)
{
    msdossession_start_opts o = {fujinet, 0, 0};
    int rc = msdossession_start(s, &o);
    CHECK(rc == 0, "start: %s", msdossession_last_error(s));
}

/* Waits up to ms for text on the text screen. */
static inline int test_wait_text(msdossession *s, const char *text, int ms)
{
    static char buf[8192];
    int t;
    buf[0] = 0;
    for (t = 0; t < ms; t += 100) {
        if (msdossession_copy_screen_text(s, buf, sizeof buf) > 0 && strstr(buf, text))
            return 1;
        test_sleep_ms(100);
    }
    fprintf(stderr, "screen was:\n%s\n", buf);
    return 0;
}

/* Waits up to ms for text in FujiNet's console log. */
static inline int test_wait_fujinet_log(msdossession *s, const char *text, int ms)
{
    static char buf[1 << 20];
    int t;
    for (t = 0; t < ms; t += 100) {
        msdossession_fujinet_copy_log(s, buf, sizeof buf);
        if (strstr(buf, text))
            return 1;
        test_sleep_ms(100);
    }
    return 0;
}

static inline void test_set_pref(msdossession *s, const char *key, int value)
{
    int i = msdossession_pref_find(key);
    CHECK(i >= 0, "no pref %s", key);
    if (msdossession_pref(i)->type == MSDOS_PREF_CHOICE)
        msdossession_pref_set_choice(s, i, value);
    else
        msdossession_pref_set_int(s, i, value);
}

static inline int test_get_pref(msdossession *s, const char *key)
{
    int i = msdossession_pref_find(key);
    CHECK(i >= 0, "no pref %s", key);
    if (msdossession_pref(i)->type == MSDOS_PREF_CHOICE)
        return msdossession_pref_get_choice(s, i);
    return msdossession_pref_get_int(s, i);
}

static inline int test_file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return (int)n;
}

#endif
