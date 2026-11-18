#include "../calendar.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define H(x) (1u << (x))
#define M(x) (1ull << (x))
#define D(x) (1u << (x))

static cal_spec_t S(const char *v) {
    cal_spec_t s;
    if (calendar_parse(v, &s) != 0) { fprintf(stderr, "parse failed: %s\n", v); exit(1); }
    return s;
}
static int bad(const char *v) { cal_spec_t s; return calendar_parse(v, &s) == -1; }
static time_t nx(const char *v, time_t after) { cal_spec_t s = S(v); return calendar_next_after(&s, 1, after); }
static time_t rc(const char *v, time_t now) { cal_spec_t s = S(v); return calendar_recent_at_or_before(&s, 1, now); }
static time_t at(int y, int mo, int d, int h, int mi) {
    struct tm tm = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_isdst = -1 };
    return mktime(&tm);
}

int main(void) {
    cal_spec_t s;

    /* every form valid before 4.9 parses to the same schedule */
    s = S("03:15");
    assert(s.hours == H(3) && s.mins == M(15) && s.dow == CAL_ANY_DOW && s.dom == CAL_ANY_DOM);
    s = S("00:00"); assert(s.hours == H(0) && s.mins == M(0));
    s = S("23:59"); assert(s.hours == H(23) && s.mins == M(59));
    s = S("Sun 04:00"); assert(s.dow == 1 && s.dom == CAL_ANY_DOM && s.hours == H(4));
    s = S("Mon 03:15"); assert(s.dow == 2);
    s = S("sat 12:30"); assert(s.dow == 1 << 6 && s.mins == M(30));
    s = S("WED 09:05"); assert(s.dow == 1 << 3);
    s = S("1 03:15"); assert(s.dom == D(1) && s.dow == CAL_ANY_DOW);
    s = S("31 00:10"); assert(s.dom == D(31));
    s = S("15 23:00"); assert(s.dom == D(15) && s.hours == H(23));
    s = S("6,18:00"); assert(s.hours == (H(6) | H(18)) && s.mins == M(0));
    s = S("Mon 6,18:00"); assert(s.hours == (H(6) | H(18)) && s.dow == 2);
    s = S("1 0,12,23:30"); assert(s.hours == (H(0) | H(12) | H(23)) && s.mins == M(30) && s.dom == D(1));
    uint32_t one_to_8 = H(1) | H(2) | H(3) | H(4) | H(5) | H(6) | H(7) | H(8);
    s = S("1,2,3,4,5,6,7,8:00"); assert(s.hours == one_to_8 && s.dow == CAL_ANY_DOW);
    s = S("Mon 1,2,3,4,5,6,7,8:00"); assert(s.hours == one_to_8 && s.dow == 2);
    s = S("15 1,2,3,4,5,6,7,8:00"); assert(s.hours == one_to_8 && s.dom == D(15));
    s = S("0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23:05");
    assert(s.hours == CAL_ANY_HOURS && s.mins == M(5));
    /* full weekday names are accepted now (systemd takes both) */
    s = S("Monday 03:15"); assert(s.dow == 2);

    /* malformed: a typo must never schedule garbage */
    assert(bad("0 03:15") && bad("32 03:15") && bad("Xyz 03:15") && bad("24:00") && bad("03:60"));
    assert(bad("03:15 junk") && bad("Mon 03") && bad("") && bad("garbage"));
    assert(bad("Monday12345678901 03:15") && bad("24,1:00") && bad("6,,18:00") && bad("6,18"));
    assert(bad(",6:00") && bad("6,:00") && bad("6,18:00 junk"));

    /* new forms */
    s = S("Mon..Fri 09:00"); assert(s.dow == 0x3E && s.hours == H(9));
    s = S("Sat,Sun 10:00"); assert(s.dow == (1 << 6 | 1));
    s = S("Mon,Wed..Fri 08:00"); assert(s.dow == (2 | 8 | 16 | 32));
    s = S("Sun 1..7 01:00"); assert(s.dow == 1 && s.dom == (D(1) | D(2) | D(3) | D(4) | D(5) | D(6) | D(7)));
    s = S("1,15 00:00"); assert(s.dom == (D(1) | D(15)) && s.dow == CAL_ANY_DOW);
    s = S("*:00"); assert(s.hours == CAL_ANY_HOURS && s.mins == M(0));
    s = S("*:0/15"); assert(s.mins == (M(0) | M(15) | M(30) | M(45)));
    s = S("8..17:30"); assert(s.hours == 0x3FF00u && s.mins == M(30));
    s = S("*/6:00"); assert(s.hours == (H(0) | H(6) | H(12) | H(18)));
    s = S("10/5:*"); assert(s.hours == (H(10) | H(15) | H(20)) && s.mins == CAL_ANY_MINS);
    s = S("*:*"); assert(s.hours == CAL_ANY_HOURS && s.mins == CAL_ANY_MINS);
    /* a step inside a range stops at the range's end */
    s = S("8..17/2:00"); assert(s.hours == (H(8) | H(10) | H(12) | H(14) | H(16)));
    s = S("5/1:00"); assert(s.hours == (CAL_ANY_HOURS & ~0x1Fu));
    /* weekday ranges count Monday-first, as systemd: the weekend is Sat..Sun */
    s = S("Sat..Sun 02:00"); assert(s.dow == (1 << 6 | 1));
    s = S("Mon..Sun 00:00"); assert(s.dow == CAL_ANY_DOW);
    s = S("Fri..Sun 00:00"); assert(s.dow == (1 << 5 | 1 << 6 | 1));
    assert(bad("Sun..Sat 00:00") && bad("Sun..Mon 00:00"));
    assert(bad("*:*/0") && bad("*/0:00") && bad("Fri..Mon 00:00") && bad("25:00") && bad("00:60"));
    assert(bad("0 00:00") && bad("Mon Tue 00:00") && bad("Mon 1 2 00:00") && bad("5..3:00"));
    assert(bad("Mon..  00:00") && bad("1..32 00:00") && bad("*:60/5"));

    /* walks, UTC. Anchor 2024-01-01 00:00 UTC, a Monday */
    setenv("TZ", "UTC", 1); tzset();
    time_t mon0000 = 1704067200, mon1200 = mon0000 + 12 * 3600, mon0600 = mon0000 + 6 * 3600;
    assert(nx("12:00", mon0000) == mon1200);
    assert(nx("Mon 12:00", mon0000) == mon1200);
    assert(nx("Tue 12:00", mon0000) == mon1200 + 86400);
    assert(nx("1 12:00", mon0000) == mon1200);
    assert(nx("15 12:00", mon0000) == mon1200 + 14 * 86400);
    assert(nx("12:00", mon1200 + 1) == mon1200 + 86400);
    assert(rc("12:00", mon0600) == mon1200 - 86400);
    assert(rc("Mon 12:00", mon0600) == mon1200 - 7 * 86400);

    time_t oct28_0700 = 1793170800;   /* 2026-10-28 07:00 UTC */
    assert(nx("6,18:00", oct28_0700) == oct28_0700 + 11 * 3600);
    assert(nx("6,18:00", oct28_0700 + 12 * 3600) == oct28_0700 + 23 * 3600);
    assert(nx("6,18:00", oct28_0700 - 3600 - 1) == oct28_0700 - 3600);
    assert(rc("6,18:00", oct28_0700) == oct28_0700 - 3600);
    assert(rc("6,18:00", oct28_0700 - 3600) == oct28_0700 - 3600);
    assert(rc("6,18:00", oct28_0700 - 2 * 3600) == oct28_0700 - 13 * 3600);
    assert(rc("6,18:00", oct28_0700 + 12 * 3600) == oct28_0700 + 11 * 3600);

    /* hourly is strictly after: armed at 10:00:30, the next is 11:00 */
    assert(nx("*:00", mon0000 + 10 * 3600 + 30) == mon0000 + 11 * 3600);
    assert(nx("*:0/15", mon0000 + 10 * 3600 + 15 * 60) == mon0000 + 10 * 3600 + 30 * 60);
    /* first Sunday of the month: 2024-01-07, then 2024-02-04 */
    time_t fs = nx("Sun 1..7 01:00", mon0000);
    assert(fs == at(2024, 1, 7, 1, 0) && nx("Sun 1..7 01:00", fs) == at(2024, 2, 4, 1, 0));
    /* a Monday the 31st exists, far off */
    time_t m31 = nx("Mon 31 00:00", mon0000);
    struct tm tm;
    assert(m31 > mon0000);
    gmtime_r(&m31, &tm);
    assert(tm.tm_wday == 1 && tm.tm_mday == 31);
    /* several specs: the earliest next, the latest recent */
    cal_spec_t two[2] = { S("Mon 03:00"), S("Wed 01:00") };
    time_t tue = mon0000 + 86400;
    assert(calendar_next_after(two, 2, tue) == at(2024, 1, 3, 1, 0));
    cal_spec_t mix[2] = { S("Mon 03:00"), S("*:30") };
    assert(calendar_recent_at_or_before(mix, 2, mon0000 + 5 * 3600 + 10 * 60) == mon0000 + 4 * 3600 + 30 * 60);
    /* a spec no day can satisfy has no occurrence: the load refuses it */
    cal_spec_t none = S("00:00");
    none.dom = 1;
    assert(calendar_next_after(&none, 1, mon0000) == 0 && calendar_recent_at_or_before(&none, 1, mon0000) == 0);

    /* DST, Chicago. Fall back 2026-11-01 (a 25 h day) */
    setenv("TZ", "America/Chicago", 1); tzset();
    time_t nov1_0000_cdt = 1793509200, nov1_0600_cst = 1793534400;
    time_t nov1_1800_cst = 1793577600, oct31_1800_cdt = 1793487600;
    assert(nx("6,18:00", nov1_0000_cdt) == nov1_0600_cst);
    assert(nx("6,18:00", nov1_0600_cst) == nov1_1800_cst);
    assert(rc("6,18:00", nov1_0600_cst - 60) == oct31_1800_cdt);
    assert(rc("6,18:00", nov1_1800_cst - 60) == nov1_0600_cst);
    /* the repeated 01:30 fires once that day */
    time_t fb = nx("01:30", at(2024, 11, 3, 0, 0));
    localtime_r(&fb, &tm);
    assert(tm.tm_mday == 3 && tm.tm_hour == 1 && tm.tm_min == 30);
    time_t fb2 = nx("01:30", fb);
    localtime_r(&fb2, &tm);
    assert(tm.tm_mday == 4);
    /* spring forward 2024-03-10: 02:30 does not exist, it fires at 03:30 once */
    time_t sp = nx("02:30", at(2024, 3, 10, 0, 0));
    localtime_r(&sp, &tm);
    assert(tm.tm_mday == 10 && tm.tm_hour == 3 && tm.tm_min == 30);
    time_t sp2 = nx("02:30", sp);
    localtime_r(&sp2, &tm);
    assert(tm.tm_mday == 11 && tm.tm_hour == 2 && tm.tm_min == 30);

    printf("all calendar tests passed\n");
    return 0;
}
