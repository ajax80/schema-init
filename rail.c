#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rail.h"
#include "rail_glyphs.h"

#define RST "\033[0m"

typedef struct { char *p; size_t sz, n; } wbuf_t;

static void put(wbuf_t *w, const char *fmt, ...) {
    va_list ap;
    int k;
    if (w->n >= w->sz) return;
    va_start(ap, fmt);
    k = vsnprintf(w->p + w->n, w->sz - w->n, fmt, ap);
    va_end(ap);
    if (k > 0) w->n = w->n + (size_t)k < w->sz ? w->n + (size_t)k : w->sz - 1;
}

/* one code point from s, its length in *len; invalid UTF-8 is one byte, -1 */
static long utf8_cp(const unsigned char *s, int *len) {
    long c;
    int n, i;
    if (s[0] < 0x80) { *len = 1; return s[0]; }
    if ((s[0] & 0xe0) == 0xc0) { c = s[0] & 0x1f; n = 2; }
    else if ((s[0] & 0xf0) == 0xe0) { c = s[0] & 0x0f; n = 3; }
    else if ((s[0] & 0xf8) == 0xf0) { c = s[0] & 0x07; n = 4; }
    else { *len = 1; return -1; }
    for (i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) { *len = 1; return -1; }
        c = (c << 6) | (s[i] & 0x3f);
    }
    *len = n;
    return c;
}

/* Names, reasons and hostnames come from outside: every code point is shown
 * as itself only if the active font can draw it, else '?'. maxcols < 0 = no
 * limit; pad fills to maxcols. Returns columns written. */
static int clean(wbuf_t *w, const char *in, int fontmode, int maxcols, int pad) {
    const struct rail_alphabet *ab = fontmode ? &rail_font_ab : &rail_ascii_ab;
    const unsigned char *s = (const unsigned char *)in;
    int cols = 0, total = 0, len;

    if (maxcols >= 0)
        for (const unsigned char *q = s; *q; q += len, total++) utf8_cp(q, &len);
    for (; *s; s += len) {
        long cp = utf8_cp(s, &len);
        if (maxcols >= 0 && total > maxcols && cols == maxcols - 1) {
            put(w, "%s", ab->trunc);
            return ++cols;
        }
        if (cp >= 0x20 && cp < 0x7f) put(w, "%c", (int)cp);
        else if (fontmode && cp >= 0xa0 && schema_font_has((uint32_t)cp)) put(w, "%.*s", len, (const char *)s);
        else if (!fontmode && cp == 0xb7) put(w, "-");
        else if (!fontmode && cp == 0x2026) put(w, "~");
        else put(w, "?");
        cols++;
    }
    while (pad && maxcols >= 0 && cols < maxcols) { put(w, " "); cols++; }
    return cols;
}

int rail_name_cols(char *out, size_t sz, const char *name, int fontmode) {
    wbuf_t w = { out, sz, 0 };
    out[0] = '\0';
    clean(&w, name, fontmode, RAIL_NAME_COLS, 1);
    return (int)w.n;
}

static const struct { const char *label, *fg, *bg; int glyph; } CH[] = {
    [CHIP_NONE]    = { "",        "90",   "30;100", 0 },
    [CHIP_ONLINE]  = { "ONLINE",  "32",   "30;42",  0 },
    [CHIP_PERFECT] = { "PERFECT", "35",   "30;45",  0 },
    [CHIP_DONE]    = { "DONE",    "32",   "30;42",  0 },
    [CHIP_SPAWN]   = { "SPAWN",   "2;32", "2;30;42", 1 },
    [CHIP_WAITING] = { "WAITING", "2;32", "2;30;42", 1 },
    [CHIP_RETRY]   = { "RETRY",   "33",   "30;43",  2 },
    [CHIP_DORMANT] = { "DORMANT", "34",   "30;44",  4 },
    [CHIP_CUT]     = { "CUT",     "31",   "30;41",  3 },
    [CHIP_STOPPED] = { "STOPPED", "34",   "30;44",  4 },
    [CHIP_EXITED]  = { "EXITED",  "90",   "30;100", 4 },
    [CHIP_KILLED]  = { "KILLED",  "31",   "30;41",  3 },
    [CHIP_STUCK]   = { "STUCK",   "31",   "30;41",  3 },
    [CHIP_TIMER]   = { "TIMER",   "36",   "30;46",  0 },
};

size_t rail_fmt_line(char *out, size_t sz, const rail_line_t *l, int fontmode) {
    const struct rail_alphabet *ab = fontmode ? &rail_font_ab : &rail_ascii_ab;
    const char *glyphs[] = { ab->g_on, ab->g_start, ab->g_warn, ab->g_cut, ab->g_stop };
    wbuf_t w = { out, sz, 0 };
    char chip[24];

    out[0] = '\0';
    if (l->chip == CHIP_RETRY && l->retry_n > 0) snprintf(chip, sizeof chip, "RETRY %d", l->retry_n);
    else snprintf(chip, sizeof chip, "%s", CH[l->chip].label);
    put(&w, "\033[%sm%s%s" RST " \033[1;37m", CH[l->chip].fg, l->runtime ? ab->branch_rt : ab->branch,
        glyphs[CH[l->chip].glyph]);
    clean(&w, l->name ? l->name : "", fontmode, RAIL_NAME_COLS, 1);
    put(&w, RST " \033[%sm %-8s" RST "  ", CH[l->chip].bg, chip);
    if (l->reason[0]) {
        put(&w, "\033[%sm", CH[l->chip].fg);
        clean(&w, l->reason, fontmode, -1, 0);
    } else if (l->runtime) {
        time_t t = (time_t)(l->t_ms / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        put(&w, "\033[90m%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else
        put(&w, "\033[90m+%lld.%02llds", (long long)(l->t_ms / 1000), (long long)(l->t_ms % 1000 / 10));
    put(&w, RST "\n");
    return w.n;
}

size_t rail_fmt_banner(char *out, size_t sz, const char *word, const char *version, const char *host, int nsvc,
                       int fontmode) {
    const struct rail_alphabet *ab = fontmode ? &rail_font_ab : &rail_ascii_ab;
    wbuf_t w = { out, sz, 0 };
    int halt = !strcmp(word, "HALT");

    out[0] = '\0';
    put(&w, "\033[35m%s" RST " \033[95mS C H E M A\033[36m//\033[%sm", ab->banner_l, halt ? "91" : "92");
    for (const char *c = word; *c; c++) put(&w, c[1] ? "%c " : "%c", *c);
    put(&w, RST " \033[35m%s" RST, ab->banner_r);
    if (version) {
        put(&w, "  \033[90m%s", version[0] == 'v' ? "" : "v");
        clean(&w, version, fontmode, -1, 0);
        put(&w, " %s ", ab->sep);
        clean(&w, host ? host : "?", fontmode, -1, 0);
        put(&w, " %s %d services" RST, ab->sep, nsvc);
    }
    put(&w, "\n");
    if (!halt) put(&w, "\033[35m%s" RST "\n", ab->track_v);
    return w.n;
}

size_t rail_fmt_finish(char *out, size_t sz, const rail_counts_t *c, int64_t settled_ms, int timed_out, int fontmode) {
    const struct rail_alphabet *ab = fontmode ? &rail_font_ab : &rail_ascii_ab;
    const struct { int n; const char *label, *bg; } b[] = {
        { c->online, "ONLINE", "30;42" }, { c->waiting, "WAITING", "30;100" }, { c->friction, "FRICTION", "30;43" },
        { c->cut, "CUT", "30;41" }, { c->starting, "STARTING", "30;43" },
    };
    wbuf_t w = { out, sz, 0 };

    out[0] = '\0';
    put(&w, "\033[35m%s" RST, ab->finish);
    for (size_t i = 0; i < sizeof b / sizeof *b; i++)
        if (b[i].n) put(&w, " \033[%sm %d %s " RST, b[i].bg, b[i].n, b[i].label);
    if (timed_out) put(&w, "  \033[33mrail still starting after %llds" RST "\n", (long long)(settled_ms / 1000));
    else put(&w, "  \033[36mrail settled in %lld.%02llds" RST "\n", (long long)(settled_ms / 1000),
             (long long)(settled_ms % 1000 / 10));
    return w.n;
}

size_t rail_fmt_shutdown_end(char *out, size_t sz, int64_t ms, int reboot, int fontmode) {
    const struct rail_alphabet *ab = fontmode ? &rail_font_ab : &rail_ascii_ab;
    wbuf_t w = { out, sz, 0 };
    out[0] = '\0';
    put(&w, "\033[35m%s" RST " \033[36mall down in %lld.%llds %s %s" RST "\n", ab->finish, (long long)(ms / 1000),
        (long long)(ms % 1000 / 100), ab->sep, reboot ? "rebooting" : "powering off");
    return w.n;
}

size_t rail_fmt_note(char *out, size_t sz, const char *text, int fontmode) {
    const struct rail_alphabet *ab = fontmode ? &rail_font_ab : &rail_ascii_ab;
    wbuf_t w = { out, sz, 0 };
    out[0] = '\0';
    put(&w, "\033[90m%s ", ab->branch);
    clean(&w, text, fontmode, -1, 0);
    put(&w, RST "\n");
    return w.n;
}
