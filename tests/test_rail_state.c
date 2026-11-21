#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../rail.h"
#include "../schema.h"

static char out[1 << 16];
static size_t outn;
static int64_t now;

static int cap_write(const char *b, size_t n) {
    if (outn + n < sizeof out) { memcpy(out + outn, b, n); outn += n; out[outn] = 0; }
    return (int)n;
}
static int64_t cap_now(void) { return now; }
static time_t cap_wall(void) { return 1795192927; }
static const rail_io_t io = { cap_write, cap_now, cap_wall };

static int count(const char *needle) {
    int c = 0;
    for (const char *p = out; (p = strstr(p, needle)); p++) c++;
    return c;
}
static void reset(int reexeced) {
    outn = 0; out[0] = 0; now = 0;
    rail_init(&io, 1, reexeced, 0);
}
#define FAIL(...) do { fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n--- output ---\n%s\n", out); exit(1); } while (0)

static rail_svc_t S(const char *name, uint8_t state) {
    rail_svc_t s = { .name = name, .state = state };
    return s;
}

static void map_events(void) {
    rail_svc_t s = S("a", STATE_RECOVERY);
    s.restart_pending = 1;
    /* the retry that follows carries the restart: one line per death */
    assert(rail_map_event("died", &s) == CHIP_NONE);
    assert(rail_map_event("friction", &s) == CHIP_NONE);
    assert(rail_map_event("retry", &s) == CHIP_RETRY);
    assert(rail_map_event("retry-deep", &s) == CHIP_RETRY);
    s.restart_pending = 0;
    assert(rail_map_event("died", &s) == CHIP_CUT);
    s = S("a", STATE_EXCISED);
    s.failed = 1;
    assert(rail_map_event("76-no-restart", &s) == CHIP_CUT);
    s.failed = 0;
    assert(rail_map_event("76-no-restart", &s) == CHIP_EXITED);
    assert(rail_map_event("killed-SIGSEGV-core", &s) == CHIP_NONE);
    assert(rail_map_event("card-2/3", &s) == CHIP_FAILED);
    assert(rail_map_event("condition-skip path_exists:/x", &s) == CHIP_NONE);
    assert(rail_map_event("start-timeout", &s) == CHIP_NONE);   /* a cause; the 76-excised after it is the CUT */
    assert(rail_map_event("reload-fail", &s) == CHIP_FAILED);
    assert(rail_map_event("timer-fire", &s) == CHIP_TIMER);
    assert(rail_map_event("path-fire IN_CREATE:/x", &s) == CHIP_TIMER);
    assert(rail_map_event("no-such-event-ever", &s) == CHIP_NONE);
    assert(!rail_event_known("no-such-event-ever"));
    assert(rail_event_known("main-pid pid=4"));
}

static void spawn_delay(void) {
    rail_svc_t s = S("slow", STATE_NEW_PROCESS);
    reset(0);
    s.pid = 10; s.starting = 1;
    rail_event(&s, 0, "spawn");
    if (count("SPAWN")) FAIL("SPAWN printed at once");
    now = 1999; rail_tick(&s, 1, now);
    if (count("SPAWN")) FAIL("SPAWN before 2 s");
    now = 2000; rail_tick(&s, 1, now);
    if (count("SPAWN") != 1) FAIL("no SPAWN at 2 s");
    now = 9000; rail_tick(&s, 1, now);
    if (count("SPAWN") != 1) FAIL("SPAWN repeated");

    reset(0);
    s = S("quick", STATE_NEW_PROCESS); s.pid = 11; s.starting = 1;
    rail_event(&s, 0, "spawn");
    now = 500; s.state = STATE_SETTLED; s.starting = 0;
    rail_event(&s, 0, "notify-ready");
    now = 3000; rail_tick(&s, 1, now);
    if (count("SPAWN") || count("ONLINE") != 1) FAIL("ready before 2 s: want ONLINE only");

    reset(0);
    s = S("doomed", STATE_NEW_PROCESS); s.pid = 12; s.starting = 1;
    rail_event(&s, 0, "spawn");
    now = 1500;
    rail_event(&s, 0, "start-timeout");
    s.state = STATE_EXCISED; s.failed = 1; s.starting = 0;
    rail_event(&s, 0, "76-excised");
    now = 4000; rail_tick(&s, 1, now);
    if (count("SPAWN") || count(" CUT ") != 1) FAIL("cut before 2 s: want CUT only");
}

static void one_line_per_death(void) {
    rail_svc_t s = S("crashy", STATE_RECOVERY);
    reset(0);
    for (int k = 1; k <= 4; k++) {
        s.state = STATE_RECOVERY; s.restart_pending = 1;
        rail_event(&s, 0, "killed-SIGSEGV-core");
        rail_event(&s, 0, "died");
        s.state = STATE_FRICTION;
        rail_event(&s, 0, "friction");
        s.state = STATE_NEW_PROCESS; s.restart_count = k;
        rail_event(&s, 0, k % 2 ? "retry" : "retry-deep");
        rail_event(&s, 0, "restart-hold");
    }
    if (count("RETRY") != 4) FAIL("want one RETRY line per death");
    if (!count(" RETRY 4 ") || count(" RETRY 5 ")) FAIL("RETRY n must be the engine's restart count");
    if (count("killed-SIGSEGV-core") != 4) FAIL("RETRY reason should be the cause of death");

    /* readiness-lost is a cause: the retry or the 76-* excise after it is the outcome */
    reset(0);
    s = S("rp", STATE_FUNDAMENTAL);
    assert(rail_map_event("readiness-lost", &s) == CHIP_NONE);
    rail_event(&s, 0, "readiness-lost");
    s.state = STATE_EXCISED; s.failed = 1;
    rail_event(&s, 0, "76-restart-policy");
    if (count(" CUT ") != 1 || !count("readiness-lost")) FAIL("CUT should carry readiness-lost as its reason");
}

static void fast_ready_single_line(void) {
    rail_svc_t s = S("fast", STATE_NEW_PROCESS);
    reset(0);
    s.pid = 5; s.starting = 1;
    rail_event(&s, 0, "spawn");
    s.state = STATE_SETTLED; s.starting = 0;
    rail_event(&s, 0, "notify-ready");
    rail_event(&s, 0, "promote");
    s.state = STATE_FULL_TRUST;
    rail_event(&s, 0, "promote");
    if (count("ONLINE") != 1) FAIL("want exactly one ONLINE");
    s.state = STATE_PERFECT;
    rail_event(&s, 0, "promote");
    if (count("PERFECT") != 1) FAIL("want a PERFECT line after ONLINE");
    rail_event(&s, 0, "promote");
    if (count("PERFECT") != 1) FAIL("PERFECT repeated");
}

static void settle(void) {
    rail_svc_t v[4] = { S("a", STATE_SETTLED), S("b", STATE_NEW_PROCESS), S("c", STATE_EXCISED), S("d", STATE_NEW_PROCESS) };
    reset(0);
    /* b: condition false, never starts; c: standby (not failed); d: starting */
    v[3].pid = 9; v[3].starting = 1;
    now = 100; rail_tick(v, 4, now);
    if (count("rail settled")) FAIL("finish before arm");
    v[3].starting = 0; v[3].state = STATE_SETTLED;
    now = 200; rail_tick(v, 4, now);
    if (count("rail settled")) FAIL("finish before arm (idle)");
    rail_arm_settle();
    v[3].starting = 1;
    now = 300; rail_tick(v, 4, now);
    if (count("rail settled")) FAIL("finish while d starting");
    now = 2710; v[3].starting = 0; v[3].state = STATE_SETTLED;
    rail_event(&v[3], 3, "notify-ready");
    if (rail_tick(v, 4, now) != 3710) FAIL("next wake should be the end of the 1 s quiet window");
    if (count("rail settled")) FAIL("finish inside the 1 s quiet window (a dependent may be about to spawn)");
    now = 3710; rail_tick(v, 4, now);
    if (count("rail settled in 2.71s") != 1) FAIL("finish line missing or wrong time");
    if (!count(" 2 ONLINE ") || !count(" 2 WAITING ")) FAIL("bucket counts");
    now = 9000; rail_tick(v, 4, now);
    if (count("rail settled") != 1) FAIL("finish printed twice");
    v[0].restart_count = 1;
    rail_event(&v[0], 0, "retry");
    if (!strstr(out, "╍")) FAIL("post-settle lines are not runtime shape");
}

static void timeout_120(void) {
    rail_svc_t v[1] = { S("stuck", STATE_NEW_PROCESS) };
    reset(0);
    v[0].pid = 3; v[0].starting = 1;
    rail_arm_settle();
    now = 119999; rail_tick(v, 1, now);
    if (count("still starting")) FAIL("timeout early");
    now = 120000; rail_tick(v, 1, now);
    if (count("rail still starting after 120s") != 1 || !count(" 1 STARTING ")) FAIL("timeout line");
    v[0].starting = 0; v[0].state = STATE_SETTLED;
    now = 130000;
    rail_event(&v[0], 0, "notify-ready");
    rail_tick(v, 1, now);
    if (count("rail settled") || count("still starting") != 1) FAIL("second finish after timeout");
    if (!count("ONLINE")) FAIL("late ONLINE missing");
}

static void counts_total(void) {
    static const uint8_t states[] = { STATE_FUNDAMENTAL, STATE_FRICTION, STATE_SETTLED, STATE_NEW_PROCESS, STATE_RECOVERY,
                                      STATE_FULL_TRUST, STATE_PERFECT, STATE_EXCISED, STATE_DORMANT };
    rail_svc_t v[64];
    int n = 0;
    for (size_t i = 0; i < sizeof states; i++)
        for (int f = 0; f < 2; f++)
            for (int st = 0; st < 2; st++) {
                v[n] = S("x", states[i]); v[n].failed = f; v[n].starting = st; n++;
            }
    rail_counts_t c;
    rail_counts(v, n, &c);
    if (c.online + c.waiting + c.friction + c.cut + c.starting != n) FAIL("buckets don't sum to n");
    rail_svc_t e = S("x", STATE_EXCISED);
    rail_counts(&e, 1, &c); assert(c.waiting == 1);
    e.failed = 1; rail_counts(&e, 1, &c); assert(c.cut == 1);
    e = S("x", STATE_DORMANT); rail_counts(&e, 1, &c); assert(c.friction == 1);
    e = S("x", STATE_PERFECT); rail_counts(&e, 1, &c); assert(c.online == 1);
}

static void reexec_runtime(void) {
    rail_svc_t v[1] = { S("a", STATE_NEW_PROCESS) };
    reset(1);
    rail_banner("1", "h", 1);
    if (outn) FAIL("banner after re-exec");
    rail_arm_settle();
    v[0].pid = 2; v[0].starting = 1;
    rail_event(&v[0], 0, "spawn");
    v[0].starting = 0; v[0].state = STATE_SETTLED;
    rail_event(&v[0], 0, "notify-ready");
    now = 200000; rail_tick(v, 1, now);
    if (count("rail settled") || count("still starting")) FAIL("finish after re-exec");
    if (!count("╍") || count("┣")) FAIL("not runtime shape after re-exec");
}

static void services_added_after_settle(void) {
    rail_svc_t v[3] = { S("a", STATE_SETTLED), S("b", STATE_NEW_PROCESS), S("c", STATE_NEW_PROCESS) };
    reset(0);
    rail_arm_settle();
    now = 1000; rail_tick(v, 1, now);
    if (count("rail settled") != 1) FAIL("no settle");
    v[1].pid = 7; v[1].starting = 1; v[2].pid = 8; v[2].starting = 1;
    rail_event(&v[1], 1, "spawn");
    rail_event(&v[2], 2, "spawn");
    now = 5000; rail_tick(v, 3, now);
    if (count("rail settled") != 1 || count("SPAWN") != 2) FAIL("added services");
}

static void shutdown_before_settle(void) {
    rail_svc_t v[1] = { S("a", STATE_NEW_PROCESS) };
    reset(0);
    rail_arm_settle();
    v[0].pid = 2; v[0].starting = 1;
    rail_shutdown_begin();
    v[0].starting = 0; v[0].state = STATE_SETTLED;
    now = 500000; rail_tick(v, 1, now);
    if (count("rail settled") || count("still starting")) FAIL("finish after shutdown began");
    rail_shutdown_outcome("a", CHIP_KILLED);
    if (!count(" KILLED ")) FAIL("shutdown outcome line");
}

/* tests/rail_events.txt is the source's event list (test_rail_events.sh keeps it honest) */
static void every_event_classified(void) {
    FILE *f = fopen("tests/rail_events.txt", "r");
    char l[128];
    int n = 0;
    if (!f) FAIL("tests/rail_events.txt: run from the repo root");
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\n")] = 0;
        char ev[160];
        /* a "prefix*" key stands for any name built at runtime from that prefix */
        snprintf(ev, sizeof ev, "%s", l);
        char *star = strchr(ev, '*');
        if (star) strcpy(star, "x");
        if (!rail_event_known(ev)) FAIL("event %s has no entry in rail.c's table", l);
        rail_svc_t s = S("x", STATE_EXCISED);
        if (rail_map_event(ev, &s) == CHIP_NONE &&
            (strstr(l, "fail") || strstr(l, "timeout") || strstr(l, "abort") || strstr(l, "exhausted") ||
             strstr(l, "died") || strstr(l, "fallback-down") || strstr(l, "readiness-lost")) &&
            !rail_event_why(ev)[0])
            FAIL("failure-sounding event %s is quiet without a justification", l);
        n++;
    }
    fclose(f);
    if (n < 80) FAIL("only %d events listed", n);
}

/* review C1: FULL_TRUST is "spawned, waiting for readiness" in this engine */
static void full_trust_is_starting(void) {
    assert(rail_is_starting(STATE_FULL_TRUST, 42, 0) == 1);
    assert(rail_is_starting(STATE_NEW_PROCESS, 42, 0) == 1);
    assert(rail_is_starting(STATE_FULL_TRUST, 0, 0) == 0);
    assert(rail_is_starting(STATE_NEW_PROCESS, 42, 1) == 0);   /* held on its socket */
    assert(rail_is_starting(STATE_FUNDAMENTAL, 42, 0) == 0);
    rail_svc_t v[1] = { S("slowd", STATE_FULL_TRUST) };
    rail_counts_t c;
    rail_counts(v, 1, &c);
    if (c.online) FAIL("FULL_TRUST counted ONLINE");
    reset(0);
    v[0].pid = 7; v[0].starting = rail_is_starting(v[0].state, v[0].pid, 0);
    rail_event(&v[0], 0, "spawn");
    rail_arm_settle();
    assert(rail_map_event("notify-ready", &v[0]) == CHIP_NONE);
    now = 5000; rail_tick(v, 1, now);
    if (count("rail settled")) FAIL("settled while a service waits for readiness");
    if (count("SPAWN") != 1) FAIL("slow start shows no SPAWN");
    now = 120000; rail_tick(v, 1, now);
    if (count("still starting after 120s") != 1) FAIL("120 s line never fires for a never-ready service");
}

/* review I1/I2/I3/I4 */
static void causes_and_outcomes(void) {
    rail_svc_t s = S("pf", STATE_FULL_TRUST);
    reset(0);
    rail_event(&s, 0, "pid_file-missing");
    s.state = STATE_RECOVERY; s.restart_pending = 1;
    s.state = STATE_NEW_PROCESS; s.restart_count = 1;
    rail_event(&s, 0, "retry");
    if (count(" CUT ") || !count("RETRY 1") || !count("pid_file-missing")) FAIL("I1: pid_file-missing is a cause");

    reset(0);
    s = S("st", STATE_FULL_TRUST);
    rail_event(&s, 0, "start-timeout");
    s.state = STATE_RECOVERY;
    rail_event(&s, 0, "timeout-recovery");
    s.state = STATE_NEW_PROCESS; s.restart_count = 1;
    rail_event(&s, 0, "retry");
    if (count(" CUT ") || count("RETRY") != 1 || !count("start-timeout")) FAIL("I2: start-timeout + restart = one RETRY");
    reset(0);
    s = S("st2", STATE_FULL_TRUST);
    rail_event(&s, 0, "start-timeout");
    s.state = STATE_EXCISED; s.failed = 1;
    rail_event(&s, 0, "76-excised");
    if (count(" CUT ") != 1 || !count("start-timeout")) FAIL("I2: start-timeout + excise = one CUT with the cause");

    reset(0);
    s = S("stale", STATE_FUNDAMENTAL);
    rail_event(&s, 0, "killed-SIGTERM");
    s.state = STATE_FULL_TRUST;
    rail_event(&s, 0, "spawn");
    s.state = STATE_RECOVERY; s.restart_pending = 1;
    rail_event(&s, 0, "died");
    s.state = STATE_NEW_PROCESS; s.restart_count = 1;
    rail_event(&s, 0, "retry");
    if (count("killed-SIGTERM") || !count("died")) FAIL("I3: a stale cause from before the last spawn");

    reset(0);
    s = S("probe", STATE_EXCISED); s.failed = 1;
    rail_event(&s, 0, "promote");
    if (count(" CUT ") != 1 || !count("probe-failed")) FAIL("I4: a failed probe at promote shows nothing");
}

int main(void) {
    every_event_classified();
    full_trust_is_starting();
    causes_and_outcomes();
    setenv("TZ", "UTC", 1);
    tzset();
    map_events();
    spawn_delay();
    one_line_per_death();
    fast_ready_single_line();
    settle();
    timeout_120();
    counts_total();
    reexec_runtime();
    services_added_after_settle();
    shutdown_before_settle();
    printf("test_rail_state: ok\n");
    return 0;
}
