#ifndef SCHEMA_CALENDAR_H
#define SCHEMA_CALENDAR_H

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* on_calendar=[DOW] [DOM] H:M as one mask per field; a full mask is "any". */
typedef struct {
    uint8_t  dow;     /* bit 0 = Sunday */
    uint32_t dom;     /* bits 1..31 */
    uint32_t hours;   /* bits 0..23 */
    uint64_t mins;    /* bits 0..59 */
} cal_spec_t;

#define CAL_ANY_DOW   0x7Fu
#define CAL_ANY_DOM   0xFFFFFFFEu
#define CAL_ANY_HOURS 0x00FFFFFFu
#define CAL_ANY_MINS  0x0FFFFFFFFFFFFFFFull
#define CAL_SEARCH_DAYS 2922   /* 8 years: long enough to find a Monday the 31st */

static inline int cal_num(const char **p, long lo, long hi, long *out) {
    char *end;
    if (!isdigit((unsigned char)**p)) return -1;
    long v = strtol(*p, &end, 10);
    if (v < lo || v > hi) return -1;
    *p = end;
    *out = v;
    return 0;
}

/* a weekday name, 3-letter or full, case-insensitive; advances *p */
static inline int cal_dow_name(const char **p) {
    static const char *const n[7] = { "sunday", "monday", "tuesday", "wednesday",
                                      "thursday", "friday", "saturday" };
    size_t len = 0;
    while (isalpha((unsigned char)(*p)[len])) len++;
    for (int i = 0; i < 7; i++)
        if ((len == 3 || len == strlen(n[i])) && !strncasecmp(*p, n[i], len)) {
            *p += len;
            return i;
        }
    return -1;
}

/* comma list of items, each a single value or a..b (a <= b) */
static inline int cal_dow(const char *p, const char *e, uint8_t *out) {
    uint8_t m = 0;
    for (;;) {
        int a = cal_dow_name(&p), b = a;
        if (a < 0) return -1;
        if (p[0] == '.' && p[1] == '.') {
            p += 2;
            if ((b = cal_dow_name(&p)) < a) return -1;
        }
        for (int i = a; i <= b; i++) m |= (uint8_t)(1u << i);
        if (p == e) break;
        if (*p++ != ',') return -1;
    }
    *out = m;
    return 0;
}

static inline int cal_dom(const char *p, const char *e, uint32_t *out) {
    uint32_t m = 0;
    for (;;) {
        long a, b;
        if (cal_num(&p, 1, 31, &a)) return -1;
        b = a;
        if (p[0] == '.' && p[1] == '.') {
            p += 2;
            if (cal_num(&p, 1, 31, &b) || b < a) return -1;
        }
        for (long i = a; i <= b; i++) m |= 1u << i;
        if (p == e) break;
        if (*p++ != ',') return -1;
    }
    *out = m;
    return 0;
}

/* hour or minute field: *, n, a..b, a/s, * /s, comma lists of those */
static inline int cal_field(const char *p, const char *e, long max, uint64_t *out) {
    uint64_t m = 0;
    for (;;) {
        long a, b, step = 1;
        if (*p == '*') {
            p++;
            a = 0;
            b = max;
        } else {
            if (cal_num(&p, 0, max, &a)) return -1;
            b = a;
            if (p[0] == '.' && p[1] == '.') {
                p += 2;
                if (cal_num(&p, 0, max, &b) || b < a) return -1;
            }
        }
        if (*p == '/') {
            p++;
            if (cal_num(&p, 1, max, &step)) return -1;
            b = max;
        }
        for (long i = a; i <= b; i += step) m |= 1ull << i;
        if (p == e) break;
        if (*p++ != ',') return -1;
    }
    *out = m;
    return 0;
}

static inline int calendar_parse(const char *val, cal_spec_t *out) {
    const char *tok[4], *end[4];
    int n = 0;
    const char *p = val;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (n == 3) return -1;
        tok[n] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        end[n++] = p;
    }
    if (!n) return -1;
    cal_spec_t s = { CAL_ANY_DOW, CAL_ANY_DOM, CAL_ANY_HOURS, CAL_ANY_MINS };
    int k = 0;
    if (n == 3 || (n == 2 && isalpha((unsigned char)*tok[0]))) {
        if (cal_dow(tok[k], end[k], &s.dow)) return -1;
        k++;
    }
    if (k < n - 1) {
        if (cal_dom(tok[k], end[k], &s.dom)) return -1;
        k++;
    }
    const char *colon = memchr(tok[k], ':', (size_t)(end[k] - tok[k]));
    uint64_t h;
    if (!colon || cal_field(tok[k], colon, 23, &h) || cal_field(colon + 1, end[k], 59, &s.mins))
        return -1;
    s.hours = (uint32_t)h;
    *out = s;
    return 0;
}

/* the day `d` days from `base`, as a struct tm with wday/mday filled (noon,
 * so a DST shift can never move it to a neighbouring day) */
static inline int cal_day(time_t base, int d, struct tm *day) {
    struct tm tm;
    localtime_r(&base, &tm);
    tm.tm_mday += d;
    tm.tm_hour = 12; tm.tm_min = 0; tm.tm_sec = 0; tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return -1;
    localtime_r(&t, day);
    return 0;
}

static inline int cal_day_ok(const cal_spec_t *s, const struct tm *day) {
    return (s->dow >> day->tm_wday & 1) && (s->dom >> day->tm_mday & 1);
}

/* H:M on `day`, or -1 when mktime lands on another day */
static inline time_t cal_slot(const struct tm *day, int h, int m) {
    struct tm tm = *day, ck;
    tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = 0; tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return -1;
    localtime_r(&t, &ck);
    return ck.tm_mday == day->tm_mday ? t : (time_t)-1;
}

static inline time_t cal_next_one(const cal_spec_t *s, time_t after) {
    struct tm day;
    for (int d = 0; d <= CAL_SEARCH_DAYS; d++) {
        if (cal_day(after, d, &day) || !cal_day_ok(s, &day)) continue;
        for (int h = 0; h < 24; h++) {
            if (!(s->hours >> h & 1)) continue;
            for (int m = 0; m < 60; m++) {
                if (!(s->mins >> m & 1)) continue;
                time_t t = cal_slot(&day, h, m);
                if (t != (time_t)-1 && t > after) return t;
            }
        }
    }
    return 0;
}

static inline time_t cal_recent_one(const cal_spec_t *s, time_t now) {
    struct tm day;
    for (int d = 0; d <= CAL_SEARCH_DAYS; d++) {
        if (cal_day(now, -d, &day) || !cal_day_ok(s, &day)) continue;
        for (int h = 23; h >= 0; h--) {
            if (!(s->hours >> h & 1)) continue;
            for (int m = 59; m >= 0; m--) {
                if (!(s->mins >> m & 1)) continue;
                time_t t = cal_slot(&day, h, m);
                if (t != (time_t)-1 && t <= now) return t;
            }
        }
    }
    return 0;
}

/* earliest occurrence of any spec strictly after `after`; 0 if none within
 * CAL_SEARCH_DAYS. Recomputed against the live clock at every arm, so DST and
 * NTP steps correct themselves each cycle. */
static inline time_t calendar_next_after(const cal_spec_t *s, int n, time_t after) {
    time_t best = 0;
    for (int i = 0; i < n; i++) {
        time_t t = cal_next_one(&s[i], after);
        if (t && (!best || t < best)) best = t;
    }
    return best;
}

/* latest occurrence of any spec at or before `now`; 0 if none */
static inline time_t calendar_recent_at_or_before(const cal_spec_t *s, int n, time_t now) {
    time_t best = 0;
    for (int i = 0; i < n; i++) {
        time_t t = cal_recent_one(&s[i], now);
        if (t > best) best = t;
    }
    return best;
}

#endif
