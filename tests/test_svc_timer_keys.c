#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[] = "/tmp/schema-timerkeys-XXXXXX";
static char path[300], dd[320], dp[360];

static void put(const char *p, const char *body) {
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static int load(const char *keys, const char *dropin, service_t *s) {
    char body[2048];
    snprintf(body, sizeof body, "name=t\nexec=/bin/true\n%s", keys);
    put(path, body);
    if (dropin) {
        mkdir(dd, 0755);
        put(dp, dropin);
    }
    memset(s, 0, sizeof *s);
    int r = service_load_one(path, s);
    if (dropin) {
        unlink(dp);
        rmdir(dd);
    }
    return r;
}

int main(void) {
    static service_t s;
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/t.svc", dir);
    snprintf(dd, sizeof dd, "%s.d", path);
    snprintf(dp, sizeof dp, "%s/10-x.conf", dd);

    assert(load("on_calendar=Mon..Fri 09:00\n", NULL, &s) == 0 && s.timer_cal_n == 1 &&
           s.timer_cal[0].dow == 0x3E &&
           (s.flags & (SVC_TIMER | SVC_TIMER_CALENDAR | SVC_ONESHOT)) == (SVC_TIMER | SVC_TIMER_CALENDAR | SVC_ONESHOT));
    assert(load("on_calendar=03:00\non_calendar=Sun 1..7 01:00\n", NULL, &s) == 0 && s.timer_cal_n == 2);
    assert(load("on_calendar=1:00\non_calendar=2:00\non_calendar=3:00\non_calendar=4:00\n", NULL, &s) == 0 &&
           s.timer_cal_n == 4);
    assert(load("on_calendar=1:00\non_calendar=2:00\non_calendar=3:00\non_calendar=4:00\non_calendar=5:00\n",
                NULL, &s) == -1);
    /* a drop-in's on_calendar replaces the base schedule, it doesn't add one */
    assert(load("on_calendar=03:00\n", "on_calendar=04:00\n", &s) == 0 && s.timer_cal_n == 1 &&
           s.timer_cal[0].hours == 1u << 4);
    assert(load("on_calendar=03:00\non_calendar=05:00\n", "on_calendar=04:00\non_calendar=06:00\n", &s) == 0 &&
           s.timer_cal_n == 2 && s.timer_cal[0].hours == 1u << 4 && s.timer_cal[1].hours == 1u << 6);
    assert(load("on_calendar=03:00\non_calendar=05:00\n", "nice=1\n", &s) == 0 && s.timer_cal_n == 2);
    /* empty stays ignored (#514) */
    assert(load("on_calendar=\n", NULL, &s) == 0 && !s.timer_cal_n && !(s.flags & SVC_TIMER));
    assert(load("on_calendar=03:00\n", "on_calendar=\n", &s) == 0 && s.timer_cal_n == 1);
    assert(load("on_calendar=bogus\n", NULL, &s) == -1);
    assert(load("on_calendar=*:*/0\n", NULL, &s) == -1);

    assert(load("on_calendar=03:00\nrandomized_delay_sec=3600\n", NULL, &s) == 0 && s.timer_jitter_sec == 3600);
    assert(load("on_boot_sec=10\nrandomized_delay_sec=2678400\n", NULL, &s) == 0 && s.timer_jitter_sec == 2678400);
    assert(load("randomized_delay_sec=2678401\n", NULL, &s) == -1);
    assert(load("randomized_delay_sec=-1\n", NULL, &s) == -1);
    assert(load("randomized_delay_sec=x\n", NULL, &s) == -1);
    assert(load("", NULL, &s) == 0 && s.timer_jitter_sec == 0);

    unlink(path);
    rmdir(dir);
    printf("all timer key tests passed\n");
    return 0;
}
