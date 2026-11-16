/*
 * memtools -- see core/include/msdosmemtools.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "msdosmemtools.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- range parsing ------------------------------------------------------------ */

static int eval_addr(msdosdebug *dbg, const char *text, size_t len, uint32_t *out)
{
    char buf[128];
    uint32_t lin;

    while (len && isspace((unsigned char)text[0])) { text++; len--; }
    while (len && isspace((unsigned char)text[len - 1])) len--;
    if (!len || len >= sizeof buf) return 0;
    memcpy(buf, text, len);
    buf[len] = '\0';
    if (msdosdebug_parse_address(dbg, buf, NULL, NULL, &lin) != 0) return 0;
    *out = lin;
    return 1;
}

/* No operator characters: a number, a symbol or a seg:off. */
static int plain(const char *text, size_t len)
{
    size_t idx;
    for (idx = 0; idx < len; idx++)
        if (strchr("+-*/%&|^<>=!~()[]{}", text[idx])) return 0;
    return 1;
}

int msdossym_parse_range(msdosdebug *dbg, const char *text, uint32_t *low, uint32_t *high)
{
    const char *dots, *ptr, *lenp;
    size_t len;
    uint32_t first, last;

    if (!dbg || !text) return 0;
    while (isspace((unsigned char)*text)) text++;
    len = strlen(text);
    if (!len) return 0;

    dots = strstr(text, "..");
    lenp = NULL;
    for (ptr = text + 1; *ptr; ptr++)
        if ((ptr[0] == 'l' || ptr[0] == 'L') && isspace((unsigned char)ptr[-1]) &&
            (isspace((unsigned char)ptr[1]) || isxdigit((unsigned char)ptr[1]) || ptr[1] == '$')) {
            lenp = ptr;
            break;
        }
    if (dots) {
        if (!eval_addr(dbg, text, (size_t)(dots - text), &first) ||
            !eval_addr(dbg, dots + 2, strlen(dots + 2), &last))
            return 0;
    } else if (lenp) {
        uint32_t count;
        if (!eval_addr(dbg, text, (size_t)(lenp - text), &first) ||
            !eval_addr(dbg, lenp + 1, strlen(lenp + 1), &count) || count == 0)
            return 0;
        last = first + count - 1;
    } else {
        int split = 0;
        for (ptr = text + 1; *ptr; ptr++) {
            if (*ptr != '-') continue;
            if (plain(text, (size_t)(ptr - text)) && plain(ptr + 1, strlen(ptr + 1)) &&
                eval_addr(dbg, text, (size_t)(ptr - text), &first) &&
                eval_addr(dbg, ptr + 1, strlen(ptr + 1), &last)) {
                split = 1;
                break;
            }
        }
        if (!split) {
            if (!eval_addr(dbg, text, len, &first)) return 0;
            last = first;
        }
    }
    if (first > last) { uint32_t swap = first; first = last; last = swap; }
    *low = first;
    *high = last;
    return 1;
}

/* ---- watches ------------------------------------------------------------------ */

static struct {
    pthread_mutex_t mtx;
    char text[MSDOSWATCH_MAX][96];
    int count;
    unsigned generation;
} g_watch = { .mtx = PTHREAD_MUTEX_INITIALIZER };

int msdoswatch_count(void)
{
    int count;
    pthread_mutex_lock(&g_watch.mtx);
    count = g_watch.count;
    pthread_mutex_unlock(&g_watch.mtx);
    return count;
}

int msdoswatch_add(const char *text)
{
    int slot = -1;

    if (!text) return -1;
    while (isspace((unsigned char)*text)) text++;
    if (!*text) return -1;
    pthread_mutex_lock(&g_watch.mtx);
    if (g_watch.count < MSDOSWATCH_MAX) {
        size_t len;
        slot = g_watch.count++;
        snprintf(g_watch.text[slot], sizeof g_watch.text[slot], "%s", text);
        len = strlen(g_watch.text[slot]);
        while (len && isspace((unsigned char)g_watch.text[slot][len - 1]))
            g_watch.text[slot][--len] = '\0';
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
    return slot;
}

void msdoswatch_remove(int index)
{
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) {
        memmove(g_watch.text[index], g_watch.text[index + 1],
                (size_t)(g_watch.count - index - 1) * sizeof g_watch.text[0]);
        g_watch.count--;
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
}

void msdoswatch_clear(void)
{
    pthread_mutex_lock(&g_watch.mtx);
    g_watch.count = 0;
    g_watch.generation++;
    pthread_mutex_unlock(&g_watch.mtx);
}

int msdoswatch_text(int index, char *dst, int dstsz)
{
    int len = 0;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count)
        len = snprintf(dst, (size_t)dstsz, "%s", g_watch.text[index]);
    pthread_mutex_unlock(&g_watch.mtx);
    return len;
}

unsigned msdoswatch_generation(void)
{
    unsigned gen;
    pthread_mutex_lock(&g_watch.mtx);
    gen = g_watch.generation;
    pthread_mutex_unlock(&g_watch.mtx);
    return gen;
}

int msdoswatch_range(msdosdebug *dbg, int index, uint32_t *low, uint32_t *high)
{
    char text[96];
    if (!msdoswatch_text(index, text, sizeof text)) return 0;
    return msdossym_parse_range(dbg, text, low, high);
}

int msdoswatch_format(msdosdebug *dbg, int index, char *dst, int dstsz)
{
    char text[96];
    uint32_t low, high;
    int len, idx;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!msdoswatch_text(index, text, sizeof text)) return 0;
    if (!msdossym_parse_range(dbg, text, &low, &high))
        return snprintf(dst, (size_t)dstsz, "%-14s ?", text);
    len = snprintf(dst, (size_t)dstsz, "%-14s %05X", text, low);
    if (len >= dstsz) return dstsz - 1;
    if (!msdosdebug_stopped(dbg))
        return len + snprintf(dst + len, (size_t)(dstsz - len), "  -");
    if (low == high) {
        uint8_t byte = 0;
        msdosdebug_read(dbg, low, &byte, 1);
        return len + snprintf(dst + len, (size_t)(dstsz - len), "  $%02X (%u)", byte, byte);
    }
    len += snprintf(dst + len, (size_t)(dstsz - len), "-%05X ", high);
    {
        uint8_t bytes[8] = { 0 };
        const uint32_t span = high - low + 1;
        const int want = span < 8 ? (int)span : 8;
        msdosdebug_read(dbg, low, bytes, want);
        for (idx = 0; idx < want && len < dstsz - 4; idx++)
            len += snprintf(dst + len, (size_t)(dstsz - len), " %02X", bytes[idx]);
    }
    if (high - low >= 8 && len < dstsz - 4)
        len += snprintf(dst + len, (size_t)(dstsz - len), " ...");
    return len < dstsz ? len : dstsz - 1;
}

/* ---- search ------------------------------------------------------------------- */

static int hex_value(char chr)
{
    if (chr >= '0' && chr <= '9') return chr - '0';
    if (chr >= 'a' && chr <= 'f') return chr - 'a' + 10;
    if (chr >= 'A' && chr <= 'F') return chr - 'A' + 10;
    return -1;
}

int msdossearch_parse(int kind, const char *text, uint8_t *pat, int *count, uint8_t *mask, int *flags)
{
    int found = 0;

    if (!text || !pat || !count || !mask || !flags) return 0;
    *mask = 0xFF;
    *flags = 0;
    if (kind == MSDOSSEARCH_TEXT) {
        const char *ptr;
        for (ptr = text; *ptr && found < MSDOSSEARCH_MAX; ptr++)
            pat[found++] = (uint8_t)*ptr;
        *flags = MSDOS_SEARCH_NOCASE;
    } else {
        const char *ptr = text;
        while (*ptr) {
            int high, low;
            if (isspace((unsigned char)*ptr) || *ptr == ',') { ptr++; continue; }
            if (*ptr == '$') { ptr++; continue; }
            if (ptr[0] == '0' && (ptr[1] == 'x' || ptr[1] == 'X')) { ptr += 2; continue; }
            high = hex_value(*ptr);
            if (high < 0) return 0;
            low = hex_value(ptr[1]);
            if (low < 0) { low = high; high = 0; ptr += 1; }   /* a lone digit is a byte */
            else ptr += 2;
            if (found >= MSDOSSEARCH_MAX) break;
            pat[found++] = (uint8_t)(high << 4 | low);
        }
    }
    *count = found;
    return found > 0;
}

/* ---- breakpoint description ----------------------------------------------------- */

int msdosbp_describe(msdosdebug *dbg, const msdos_breakpoint *bpt, char *dst, int dstsz)
{
    char label[64] = "";
    int len;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!bpt) return 0;
    if (bpt->type <= MSDOS_BP_ACCESS && msdosdebug_symbol_at(dbg, bpt->start, label, sizeof label) <= 0)
        label[0] = '\0';
    len = snprintf(dst, (size_t)dstsz, "%s", bpt->text);
    if (len < dstsz && label[0])
        len += snprintf(dst + len, (size_t)(dstsz - len), " (%s)", label);
    if (len < dstsz && bpt->condition[0])
        len += snprintf(dst + len, (size_t)(dstsz - len), "  if %s", bpt->condition);
    return len < dstsz ? len : dstsz - 1;
}

/* ---- memory map labels ---------------------------------------------------------- */

int msdosmem_region_name(const msdos_region *map, int count, uint32_t linear, char *dst, int dstsz)
{
    int idx;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    for (idx = 0; map && idx < count; idx++)
        if (linear >= map[idx].low && linear <= map[idx].high)
            return snprintf(dst, (size_t)dstsz, "%s", map[idx].name);
    return 0;
}
