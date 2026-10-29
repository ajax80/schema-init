#include "../service.h"
#include <assert.h>
#include <stdio.h>

#define H(x) (1u << (x))

static int pc(const char *v, uint32_t *h, int *m, int *dw, int *dm) {
    *h = 0xdeadbeef; *m = *dw = *dm = -99;
    return parse_calendar_fields(v, h, m, dw, dm);
}

int main(void) {
    uint32_t h;
    int m, dw, dm;

    /* daily HH:MM -> no day constraints */
    assert(pc("03:15", &h, &m, &dw, &dm) == 0);
    assert(h == H(3) && m == 15 && dw == -1 && dm == -1);
    assert(pc("00:00", &h, &m, &dw, &dm) == 0 && h == H(0) && m == 0);
    assert(pc("23:59", &h, &m, &dw, &dm) == 0 && h == H(23) && m == 59);

    /* weekday prefix (case-insensitive), Sun=0..Sat=6 */
    assert(pc("Sun 04:00", &h, &m, &dw, &dm) == 0 && dw == 0 && dm == -1 && h == H(4));
    assert(pc("Mon 03:15", &h, &m, &dw, &dm) == 0 && dw == 1);
    assert(pc("sat 12:30", &h, &m, &dw, &dm) == 0 && dw == 6 && m == 30);
    assert(pc("WED 09:05", &h, &m, &dw, &dm) == 0 && dw == 3);

    /* day-of-month prefix */
    assert(pc("1 03:15", &h, &m, &dw, &dm) == 0 && dm == 1 && dw == -1);
    assert(pc("31 00:10", &h, &m, &dw, &dm) == 0 && dm == 31);
    assert(pc("15 23:00", &h, &m, &dw, &dm) == 0 && dm == 15 && h == H(23));

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
    assert(calendar_next_after(H(12), 0, -1, -1, mon0000) == mon1200);
    /* weekly Mon: today is Monday, 12:00 ahead -> same day */
    assert(calendar_next_after(H(12), 0, 1, -1, mon0000) == mon1200);
    /* weekly Tue: next day 2024-01-02 12:00 */
    assert(calendar_next_after(H(12), 0, 2, -1, mon0000) == mon1200 + 86400);
    /* monthly 1st: today is the 1st, 12:00 ahead -> same day */
    assert(calendar_next_after(H(12), 0, -1, 1, mon0000) == mon1200);
    /* monthly 15th: 2024-01-15 12:00 */
    assert(calendar_next_after(H(12), 0, -1, 15, mon0000) == mon1200 + 14 * 86400);
    /* daily, but the slot already passed today -> tomorrow */
    assert(calendar_next_after(H(12), 0, -1, -1, mon1200 + 1) == mon1200 + 86400);

    /* recent_at_or_before — now = 2024-01-01 06:00, before today's 12:00 slot */
    time_t mon0600 = mon0000 + 6 * 3600;
    assert(calendar_recent_at_or_before(H(12), 0, -1, -1, mon0600) == mon1200 - 86400);
    /* weekly Mon at 06:00 -> previous Monday 12:00 (7 days back) */
    assert(calendar_recent_at_or_before(H(12), 0, 1, -1, mon0600) == mon1200 - 7 * 86400);

    /* hour lists: each listed hour sets its bit */
    assert(pc("6,18:00", &h, &m, &dw, &dm) == 0);
    assert(h == (H(6) | H(18)) && m == 0 && dw == -1 && dm == -1);
    assert(pc("Mon 6,18:00", &h, &m, &dw, &dm) == 0 && h == (H(6) | H(18)) && dw == 1);
    assert(pc("1 0,12,23:30", &h, &m, &dw, &dm) == 0);
    assert(h == (H(0) | H(12) | H(23)) && m == 30 && dm == 1);
    assert(pc("24,1:00", &h, &m, &dw, &dm) == -1);   /* hour > 23 in list */
    assert(pc("6,,18:00", &h, &m, &dw, &dm) == -1);  /* empty element */
    assert(pc("6,18", &h, &m, &dw, &dm) == -1);      /* no minute */
    assert(pc(",6:00", &h, &m, &dw, &dm) == -1);
    assert(pc("6,:00", &h, &m, &dw, &dm) == -1);
    assert(pc("6,18:00 junk", &h, &m, &dw, &dm) == -1);

    /* multi-hour walk, UTC. 2026-10-28 07:00 UTC */
    time_t oct28_0700 = 1793170800;
    uint32_t am_pm = H(6) | H(18);
    assert(calendar_next_after(am_pm, 0, -1, -1, oct28_0700) == oct28_0700 + 11 * 3600);
    assert(calendar_next_after(am_pm, 0, -1, -1, oct28_0700 + 12 * 3600)
           == oct28_0700 + 23 * 3600);                   /* 19:00 -> 06:00 next day */
    assert(calendar_next_after(am_pm, 0, -1, -1, oct28_0700 - 3600 - 1)
           == oct28_0700 - 3600);                        /* 05:59:59 -> 06:00 */
    assert(calendar_recent_at_or_before(am_pm, 0, -1, -1, oct28_0700) == oct28_0700 - 3600);
    assert(calendar_recent_at_or_before(am_pm, 0, -1, -1, oct28_0700 - 3600)
           == oct28_0700 - 3600);                        /* at-or-before is inclusive */
    assert(calendar_recent_at_or_before(am_pm, 0, -1, -1, oct28_0700 - 2 * 3600)
           == oct28_0700 - 13 * 3600);                   /* 05:00 -> 18:00 yesterday */
    assert(calendar_recent_at_or_before(am_pm, 0, -1, -1, oct28_0700 + 12 * 3600)
           == oct28_0700 + 11 * 3600);                   /* 19:00 -> 18:00 today */

    /* DST fall-back day: 2026-11-01 in Chicago is 25 hours long */
    setenv("TZ", "America/Chicago", 1); tzset();
    time_t nov1_0000_cdt = 1793509200;
    time_t nov1_0600_cst = 1793534400;   /* 7 real hours after midnight */
    time_t nov1_1800_cst = 1793577600;
    time_t oct31_1800_cdt = 1793487600;
    assert(calendar_next_after(am_pm, 0, -1, -1, nov1_0000_cdt) == nov1_0600_cst);
    assert(calendar_next_after(am_pm, 0, -1, -1, nov1_0600_cst) == nov1_1800_cst);
    assert(calendar_recent_at_or_before(am_pm, 0, -1, -1, nov1_0600_cst - 60)
           == oct31_1800_cdt);
    assert(calendar_recent_at_or_before(am_pm, 0, -1, -1, nov1_1800_cst - 60)
           == nov1_0600_cst);

    printf("all calendar tests passed\n");
    return 0;
}
