#include "../service.h"
#include <assert.h>
#include <stdio.h>

static int pc(const char *v, int *h, int *m, int *dw, int *dm) {
    *h = *m = *dw = *dm = -99;
    return parse_calendar_fields(v, h, m, dw, dm);
}

int main(void) {
    int h, m, dw, dm;

    /* daily HH:MM -> no day constraints */
    assert(pc("03:15", &h, &m, &dw, &dm) == 0);
    assert(h == 3 && m == 15 && dw == -1 && dm == -1);
    assert(pc("00:00", &h, &m, &dw, &dm) == 0 && h == 0 && m == 0);
    assert(pc("23:59", &h, &m, &dw, &dm) == 0 && h == 23 && m == 59);

    /* weekday prefix (case-insensitive), Sun=0..Sat=6 */
    assert(pc("Sun 04:00", &h, &m, &dw, &dm) == 0 && dw == 0 && dm == -1 && h == 4);
    assert(pc("Mon 03:15", &h, &m, &dw, &dm) == 0 && dw == 1);
    assert(pc("sat 12:30", &h, &m, &dw, &dm) == 0 && dw == 6 && m == 30);
    assert(pc("WED 09:05", &h, &m, &dw, &dm) == 0 && dw == 3);

    /* day-of-month prefix */
    assert(pc("1 03:15", &h, &m, &dw, &dm) == 0 && dm == 1 && dw == -1);
    assert(pc("31 00:10", &h, &m, &dw, &dm) == 0 && dm == 31);
    assert(pc("15 23:00", &h, &m, &dw, &dm) == 0 && dm == 15 && h == 23);

    /* malformed -> -1 (a typo must never schedule garbage) */
    assert(pc("0 03:15", &h, &m, &dw, &dm) == -1);    /* dom < 1  */
    assert(pc("32 03:15", &h, &m, &dw, &dm) == -1);   /* dom > 31 */
    assert(pc("Xyz 03:15", &h, &m, &dw, &dm) == -1);  /* bad weekday */
    assert(pc("Monday 03:15", &h, &m, &dw, &dm) == -1); /* only 3-letter names */
    assert(pc("24:00", &h, &m, &dw, &dm) == -1);      /* hour > 23 */
    assert(pc("03:60", &h, &m, &dw, &dm) == -1);      /* min > 59  */
    assert(pc("03:15 junk", &h, &m, &dw, &dm) == -1); /* trailing garbage */
    assert(pc("Mon 03", &h, &m, &dw, &dm) == -1);     /* no minute */
    assert(pc("", &h, &m, &dw, &dm) == -1);
    assert(pc("garbage", &h, &m, &dw, &dm) == -1);

    /* calendar_day_matches */
    assert(calendar_day_matches(-1, -1, 3, 17) == 1);  /* unconstrained */
    assert(calendar_day_matches(1, -1, 1, 17) == 1);   /* dow hit  */
    assert(calendar_day_matches(1, -1, 2, 17) == 0);   /* dow miss */
    assert(calendar_day_matches(-1, 15, 3, 15) == 1);  /* dom hit  */
    assert(calendar_day_matches(-1, 15, 3, 14) == 0);  /* dom miss */

    /* arm walk — deterministic under fixed TZ. Anchor: 2024-01-01 00:00 UTC,
     * which is a Monday (tm_wday==1). */
    setenv("TZ", "UTC", 1); tzset();
    time_t mon0000 = 1704067200;          /* 2024-01-01 00:00:00 UTC (Mon) */
    time_t mon1200 = mon0000 + 12 * 3600; /* 2024-01-01 12:00 */

    /* next_after — daily: same day 12:00 (still ahead of 00:00) */
    assert(calendar_next_after(12, 0, -1, -1, mon0000) == mon1200);
    /* weekly Mon: today is Monday, 12:00 ahead -> same day */
    assert(calendar_next_after(12, 0, 1, -1, mon0000) == mon1200);
    /* weekly Tue: next day 2024-01-02 12:00 */
    assert(calendar_next_after(12, 0, 2, -1, mon0000) == mon1200 + 86400);
    /* monthly 1st: today is the 1st, 12:00 ahead -> same day */
    assert(calendar_next_after(12, 0, -1, 1, mon0000) == mon1200);
    /* monthly 15th: 2024-01-15 12:00 */
    assert(calendar_next_after(12, 0, -1, 15, mon0000) == mon1200 + 14 * 86400);
    /* daily, but the slot already passed today -> tomorrow */
    assert(calendar_next_after(12, 0, -1, -1, mon1200 + 1) == mon1200 + 86400);

    /* recent_at_or_before — now = 2024-01-01 06:00, before today's 12:00 slot */
    time_t mon0600 = mon0000 + 6 * 3600;
    assert(calendar_recent_at_or_before(12, 0, -1, -1, mon0600) == mon1200 - 86400);
    /* weekly Mon at 06:00 -> previous Monday 12:00 (7 days back) */
    assert(calendar_recent_at_or_before(12, 0, 1, -1, mon0600) == mon1200 - 7 * 86400);

    printf("all calendar tests passed\n");
    return 0;
}
