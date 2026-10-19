#include "../recorder_logic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void roundtrip(void) {
    rec_event_t e, p;
    char buf[1024];
    memset(&e, 0, sizeof e);
    e.ts = 1760000000; e.src = REC_SRC_EARLYOOM; e.crit = 1; e.tier = 3; e.pid = 42; e.rc = 9;
    strcpy(e.kind, "kill"); strcpy(e.comm, "cosmic-player"); strcpy(e.subj, "unit.svc");
    strcpy(e.msg, "--thumbnail a b.mkv");
    assert(rec_format(buf, sizeof buf, &e) > 0);
    memset(&p, 0, sizeof p);
    assert(rec_parse(buf, &p) == 0);
    assert(p.ts == e.ts && p.src == e.src && p.crit == e.crit && p.tier == 3 && p.pid == 42 && p.rc == 9);
    assert(!strcmp(p.kind, "kill") && !strcmp(p.comm, "cosmic-player"));
    assert(!strcmp(p.subj, "unit.svc") && !strcmp(p.msg, "--thumbnail a b.mkv"));
}

static void hostile_comm(void) {
    rec_event_t e, p;
    char buf[1024];
    memset(&e, 0, sizeof e);
    e.ts = 1; e.src = REC_SRC_EARLYOOM;
    strcpy(e.kind, "kill"); strcpy(e.comm, "evil name=x\nsrc=pid1");
    assert(rec_format(buf, sizeof buf, &e) > 0);
    assert(rec_parse(buf, &p) == 0);
    assert(p.src == REC_SRC_EARLYOOM);
    assert(!strpbrk(p.comm, " =\n"));
}

static void msg_last(void) {
    rec_event_t e, p;
    char buf[1024];
    memset(&e, 0, sizeof e);
    e.ts = 1; e.src = REC_SRC_PID1;
    strcpy(e.kind, "k"); strcpy(e.msg, "a=b c=d");
    assert(rec_format(buf, sizeof buf, &e) > 0);
    assert(rec_parse(buf, &p) == 0);
    assert(!strcmp(p.msg, "a=b c=d"));
}

static void msg_unterminated(void) {
    rec_event_t e, p;
    char buf[1024];
    memset(&e, 0, sizeof e);
    e.ts = 1; e.src = REC_SRC_PID1;
    strcpy(e.kind, "k");
    memset(e.msg, 'x', sizeof e.msg);
    assert(rec_format(buf, sizeof buf, &e) > 0);
    assert(strlen(buf) < 400);
    assert(rec_parse(buf, &p) == 0);
    assert(strlen(p.msg) == sizeof p.msg - 1);
    memset(&e, 0, sizeof e);
    e.ts = 1; e.src = REC_SRC_PID1;
    strcpy(e.kind, "k"); strcpy(e.msg, "a\nb\rc");
    assert(rec_format(buf, sizeof buf, &e) > 0);
    assert(!strpbrk(buf, "\n\r"));
}

static void missing_required(void) {
    rec_event_t e;
    assert(rec_parse("pid=3 msg=x", &e) == -1);
}

static void send_no_listener(void) {
    rec_event_t e;
    memset(&e, 0, sizeof e);
    e.src = REC_SRC_PID1; strcpy(e.kind, "k");
    alarm(2);
    rec_send("/nonexistent/sock", &e);
    alarm(0);
}


static rec_event_t mk(int src, const char *kind, const char *comm, const char *subj, int rc, uint64_t ts) {
    rec_event_t e;
    memset(&e, 0, sizeof e);
    e.src = (uint8_t)src; e.rc = rc; e.ts = ts;
    strcpy(e.kind, kind); strcpy(e.comm, comm); strcpy(e.subj, subj);
    return e;
}

static void tier_app_crash_trace(void) {
    rec_event_t e = mk(REC_SRC_COREDUMP, "dump", "obsidian", "", 7, 1);
    assert(rec_tier(&e) == REC_TIER_TRACE);
}

static void tier_kwin_session(void) {
    rec_event_t e = mk(REC_SRC_COREDUMP, "dump", "kwin_wayland", "", 11, 1);
    assert(rec_tier(&e) == REC_TIER_SESSION);
    e = mk(REC_SRC_COREDUMP, "dump", "foo", "foo.svc", 11, 1);
    assert(rec_tier(&e) == REC_TIER_CRIT);
}

static void tier_logout(void) {
    rec_event_t e = mk(REC_SRC_SESSION, "plasma-exit", "", "", 0, 1);
    assert(rec_tier(&e) == REC_TIER_TRACE);
    e.rc = 1;
    assert(rec_tier(&e) == REC_TIER_SESSION);
}

static void tier_misc(void) {
    rec_event_t e = mk(REC_SRC_PID1, "died", "", "x.svc", 0, 1);
    assert(rec_tier(&e) == REC_TIER_MINOR);
    e.crit = 1;
    assert(rec_tier(&e) == REC_TIER_CRIT);
    strcpy(e.kind, "survival-enter");
    assert(rec_tier(&e) == REC_TIER_MINOR);
    strcpy(e.kind, "started");
    assert(rec_tier(&e) == REC_TIER_TRACE);
    e = mk(REC_SRC_EARLYOOM, "kill", "x", "", 0, 1);
    assert(rec_tier(&e) == REC_TIER_MINOR);
    e = mk(REC_SRC_PSI, "cpu", "x", "", 0, 1);
    e.tier = REC_TIER_LIVE;
    assert(rec_tier(&e) == REC_TIER_LIVE);
}

static void pick_precedence(void) {
    rec_arrow_t a;
    rec_event_t c;
    memset(&a, 0, sizeof a);
    a.state = 2; a.tier = REC_TIER_CRIT; a.event_ts = 100; a.resolved_ts = 150;
    c = mk(REC_SRC_PID1, "died", "", "x", 0, 200);
    assert(rec_tier(&c) == REC_TIER_MINOR);
    assert(rec_arrow_pick(&a, &c, 300) == 0);
    c = mk(REC_SRC_SESSION, "plasma-exit", "", "", 1, 50);
    assert(rec_arrow_pick(&a, &c, 300) == 1);
    c = mk(REC_SRC_PID1, "died", "", "x", 0, 200);
    assert(rec_arrow_pick(&a, &c, 150 + 36 * 3600 + 1) == 1);
    a.tier = REC_TIER_MINOR;
    assert(rec_arrow_pick(&a, &c, 300) == 1);
    c.ts = 50;
    assert(rec_arrow_pick(&a, &c, 300) == 0);
    c.ts = 100;
    a.event_ts = 100;
    assert(rec_arrow_pick(&a, &c, 300) == 1);
}

static void pick_upgrade_and_trace(void) {
    rec_arrow_t a;
    rec_event_t c = mk(REC_SRC_SESSION, "plasma-exit", "", "", 0, 200);
    memset(&a, 0, sizeof a);
    a.state = 2; a.tier = REC_TIER_CRIT; a.event_ts = 100; a.resolved_ts = 150;
    assert(rec_arrow_pick(&a, &c, 300) == 0);
    c.tier = REC_TIER_SESSION;
    assert(rec_arrow_pick(&a, &c, 300) == 1);
    memset(&a, 0, sizeof a);
    c.tier = 0;
    assert(rec_arrow_pick(&a, &c, 300) == 0);
    a.state = 2; a.resolved_ts = 10;
    assert(rec_arrow_pick(&a, &c, 10 + 40 * 3600) == 0);
}

static void culprit_evict(void) {
    rec_culprit_t tab[3];
    int n = 0;
    uint64_t d = 86400, b = 1000000, now = b + 10 * d;
    memset(tab, 0, sizeof tab);
    rec_culprit_count(tab, &n, 3, "keep", b, b);
    rec_culprit_count(tab, &n, 3, "stale", b + d, b + d);
    rec_culprit_count(tab, &n, 3, "mid", b + 2 * d, b + 2 * d);
    assert(rec_culprit_count(tab, &n, 3, "keep", now - d, now - d) == 1);
    assert(rec_culprit_count(tab, &n, 3, "mid", now - 2 * d, now - 2 * d) == 2);
    assert(rec_culprit_count(tab, &n, 3, "new", now, now) == 1);
    assert(rec_culprit_count(tab, &n, 3, "keep", now, now) == 2);
    assert(!strcmp(tab[1].key, "new"));
    assert(rec_culprit_count(tab, &n, 3, "old", b, now) == 0);
}

static void arrow_set(void) {
    static rec_shm_t m;
    rec_arrow_t a, o;
    memset(&a, 0, sizeof a);
    a.state = 1; a.tier = REC_TIER_CRIT; a.event_ts = 77; strcpy(a.line, "x");
    rec_arrow_set(&m, &a);
    assert((m.seq & 1) == 0 && m.seq == 2);
    assert(rec_snapshot_arrow(&m, &o) == 0);
    assert(!memcmp(&a, &o, sizeof a));
}

static void shade(void) {
    rec_arrow_t a;
    memset(&a, 0, sizeof a);
    assert(rec_arrow_shade(&a, 5) == 0);
    a.state = 1;
    assert(rec_arrow_shade(&a, 999999) == 1);
    a.state = 2; a.resolved_ts = 1000;
    assert(rec_arrow_shade(&a, 1000 + 11 * 3600 + 59 * 60) == 1);
    assert(rec_arrow_shade(&a, 1000 + 12 * 3600 + 60) == 2);
    assert(rec_arrow_shade(&a, 1000 + 36 * 3600 + 60) == 0);
    assert(rec_arrow_shade(&a, 500) == 1);
}

static void recurrence(void) {
    rec_culprit_t tab[4];
    int n = 0;
    uint64_t d = 86400, b = 1000000;
    memset(tab, 0, sizeof tab);
    assert(rec_culprit_count(tab, &n, 4, "cosmic-player", b, b) == 1);
    assert(rec_culprit_count(tab, &n, 4, "cosmic-player", b + 2 * d, b + 2 * d) == 2);
    assert(rec_culprit_count(tab, &n, 4, "cosmic-player", b + 5 * d, b + 5 * d) == 3);
    assert(rec_culprit_count(tab, &n, 4, "cosmic-player", b + 8 * d, b + 8 * d) == 3);
    assert(rec_culprit_count(tab, &n, 4, "cosmic-player", b + 13 * d, b + 13 * d) == 2);
    assert(rec_culprit_count(tab, &n, 4, "other", b + 13 * d, b + 13 * d) == 1);
    assert(n == 2);
}

static void render_example(void) {
    rec_event_t e = mk(REC_SRC_PSI, "cpu", "cosmic-player", "", 0, 20000ull * 86400 + 21 * 3600 + 42 * 60);
    char buf[256];
    setenv("TZ", "UTC", 1);
    tzset();
    strcpy(e.msg, "CPU pinned 20 min by cosmic-player \xc3\x97" "8");
    rec_render(buf, sizeof buf, &e, 1);
    assert(!strcmp(buf, "\xe2\x96\xb6 21:42 CPU pinned 20 min by cosmic-player \xc3\x97" "8"));
    rec_render(buf, sizeof buf, &e, 2);
    assert(strstr(buf, "8 (2nd time this week)"));
    rec_render(buf, sizeof buf, &e, 3);
    assert(strstr(buf, "(3rd time this week)"));
    rec_render(buf, sizeof buf, &e, 12);
    assert(strstr(buf, "(12th time this week)"));
    rec_render(buf, sizeof buf, &e, 21);
    assert(strstr(buf, "(21st time this week)"));
}

static void render_session(void) {
    rec_event_t e = mk(REC_SRC_SESSION, "plasma-exit", "", "", 1, 20000ull * 86400 + 21 * 3600 + 42 * 60);
    char buf[256];
    setenv("TZ", "UTC", 1);
    tzset();
    strcpy(e.msg, "kwin_wayland crashed (SIGSEGV)");
    rec_render(buf, sizeof buf, &e, 1);
    assert(!strcmp(buf, "\xe2\x96\xb6 21:42 desktop session ended \xe2\x80\x94 kwin_wayland crashed (SIGSEGV)"));
    e.msg[0] = 0;
    rec_render(buf, sizeof buf, &e, 1);
    assert(!strcmp(buf, "\xe2\x96\xb6 21:42 desktop session ended"));
}

static void render_others(void) {
    rec_event_t e = mk(REC_SRC_COREDUMP, "dump", "foo", "", 11, 20000ull * 86400);
    char buf[256];
    setenv("TZ", "UTC", 1);
    tzset();
    rec_render(buf, sizeof buf, &e, 1);
    assert(strstr(buf, "foo crashed (SIGSEGV)"));
    e = mk(REC_SRC_EARLYOOM, "kill", "bar", "", 9, 20000ull * 86400);
    rec_render(buf, sizeof buf, &e, 1);
    assert(strstr(buf, "earlyoom killed bar"));
    e = mk(REC_SRC_PID1, "died", "", "x.svc", 0, 20000ull * 86400);
    rec_render(buf, sizeof buf, &e, 1);
    assert(strstr(buf, "x.svc died"));
}

static void seqlock(void) {
    static rec_shm_t m, s;
    rec_arrow_t a;
    int i;
    memset(&m, 0, sizeof m);
    m.magic = REC_SHM_MAGIC;
    for (i = 0; i < 300; i++) {
        rec_event_t e = mk(REC_SRC_PID1, "died", "", "x", 0, (uint64_t)i + 1);
        rec_ring_push(&m, &e);
    }
    assert(m.count == REC_RING);
    assert(m.ring[(m.head - 1) % REC_RING].ts == 300);
    assert((m.seq & 1) == 0);
    assert(rec_snapshot_ring(&m, &s) == 0);
    assert(s.count == REC_RING && s.head == m.head);
    assert(rec_snapshot_arrow(&m, &a) == 0);
    m.seq = 1;
    assert(rec_snapshot_ring(&m, &s) == -1);
}

static rec_proc_t mkp(int pid, const char *comm, uint64_t ticks) {
    rec_proc_t p;
    memset(&p, 0, sizeof p);
    p.pid = pid; p.ppid = 1; p.ticks = ticks;
    strcpy(p.comm, comm);
    return p;
}

static void hogs_group(void) {
    rec_proc_t a[10], b[10];
    rec_hog_t out[4];
    int i, n;
    for (i = 0; i < 8; i++) { a[i] = mkp(100 + i, "cosmic-player", 50); b[i] = mkp(100 + i, "cosmic-player", 185); }
    a[8] = mkp(200, "kwin_wayland", 10); b[8] = mkp(200, "kwin_wayland", 30);
    a[9] = mkp(300, "foo", 0); b[9] = mkp(300, "bar", 5000);
    n = rec_hogs(a, 10, b, 10, out, 4);
    assert(n == 2);
    assert(!strcmp(out[0].comm, "cosmic-player") && out[0].count == 8 && out[0].ticks == 1080 && out[0].top_pid == 107);
    assert(!strcmp(out[1].comm, "kwin_wayland") && out[1].ticks == 20);
}

static void episode_short_no_arrow(void) {
    rec_episode_t ep;
    uint64_t t;
    memset(&ep, 0, sizeof ep);
    for (t = 0; t <= 90; t++) rec_episode_fire(&ep, 1, t, 1000 + t);
    assert(ep.active && ep.start_mono == 0 && ep.last_fire_mono == 90 && ep.start_wall == 1000);
    assert(!rec_episode_check(&ep, 119));
    assert(rec_episode_check(&ep, 121));
    assert(!rec_episode_arrow(&ep, 121));
}

static void episode_long_arrow(void) {
    rec_episode_t ep;
    uint64_t t;
    memset(&ep, 0, sizeof ep);
    for (t = 0; t <= 1200; t++) rec_episode_fire(&ep, 1, t, 1000 + t);
    assert(!rec_episode_check(&ep, 1210));
    assert(rec_episode_check(&ep, 1231));
    assert(rec_episode_arrow(&ep, 1231));
}

static void episode_overlap_arrow(void) {
    rec_episode_t ep;
    uint64_t t;
    memset(&ep, 0, sizeof ep);
    for (t = 0; t <= 60; t++) rec_episode_fire(&ep, 1, t, 1000 + t);
    assert(!rec_episode_arrow(&ep, 91));
    ep.overlapped = 1;
    assert(rec_episode_arrow(&ep, 91));
}

static void episode_text(void) {
    rec_episode_t ep;
    char buf[128];
    memset(&ep, 0, sizeof ep);
    ep.kind = 1; ep.start_mono = 100;
    strcpy(ep.top.comm, "cosmic-player"); ep.top.count = 8;
    rec_episode_text(&ep, 1300, buf, sizeof buf);
    assert(!strcmp(buf, "CPU pinned 20 min by cosmic-player \xc3\x97" "8"));
    ep.top.count = 1;
    rec_episode_text(&ep, 1300, buf, sizeof buf);
    assert(!strcmp(buf, "CPU pinned 20 min by cosmic-player"));
    ep.kind = 2;
    rec_episode_text(&ep, 50, buf, sizeof buf);
    assert(!strcmp(buf, "memory pressure 0 min by cosmic-player"));
}

static void correlate_kwin(void) {
    static rec_shm_t m;
    rec_event_t e, x;
    rec_episode_t ep;
    char why[160];
    memset(&m, 0, sizeof m);
    memset(&ep, 0, sizeof ep);
    e = mk(REC_SRC_COREDUMP, "", "kwin_wayland", "", 11, 995);
    rec_ring_push(&m, &e);
    x = mk(REC_SRC_SESSION, "plasma-exit", "", "", 0, 1000);
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 1);
    assert(!strcmp(why, "kwin_wayland crashed (SIGSEGV)"));
}

static void correlate_none_logout(void) {
    static rec_shm_t m;
    rec_event_t e, x;
    rec_episode_t ep;
    char why[160];
    memset(&m, 0, sizeof m);
    memset(&ep, 0, sizeof ep);
    e = mk(REC_SRC_COREDUMP, "", "kwin_wayland", "", 11, 975);
    rec_ring_push(&m, &e);
    x = mk(REC_SRC_SESSION, "plasma-exit", "", "", 0, 1000);
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 0);
    e = mk(REC_SRC_COREDUMP, "", "kwin_wayland", "", 11, 1001);
    rec_ring_push(&m, &e);
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 0);
}

static void correlate_episode(void) {
    static rec_shm_t m;
    rec_event_t x;
    rec_episode_t ep;
    char why[160];
    memset(&m, 0, sizeof m);
    memset(&ep, 0, sizeof ep);
    rec_episode_fire(&ep, 1, 0, 100);
    rec_episode_fire(&ep, 1, 1200, 1300);
    strcpy(ep.top.comm, "cosmic-player"); ep.top.count = 8;
    x = mk(REC_SRC_SESSION, "plasma-exit", "", "", 0, 1310);
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 1);
    assert(!strcmp(why, "CPU pinned 20 min by cosmic-player \xc3\x97" "8"));
    assert(!ep.overlapped);
    ep.active = 0;
    x.ts = 1320;
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 1);
    x.ts = 1321;
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 0);
    x.ts = 1400;
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 0);
}

static void correlate_priority(void) {
    static rec_shm_t m;
    rec_event_t e, x;
    rec_episode_t ep;
    char why[160];
    memset(&m, 0, sizeof m);
    memset(&ep, 0, sizeof ep);
    rec_episode_fire(&ep, 1, 0, 100);
    rec_episode_fire(&ep, 1, 900, 1000);
    strcpy(ep.top.comm, "hog"); ep.top.count = 2;
    x = mk(REC_SRC_SESSION, "plasma-exit", "", "", 0, 1000);
    e = mk(REC_SRC_PID1, "died", "", "foo.svc", 0, 995);
    rec_ring_push(&m, &e);
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 1 && !strcmp(why, "foo.svc died"));
    e = mk(REC_SRC_EARLYOOM, "kill", "plasmashell", "", 9, 990);
    rec_ring_push(&m, &e);
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 1 && !strcmp(why, "earlyoom killed plasmashell"));
    e = mk(REC_SRC_COREDUMP, "", "ksmserver", "", 6, 985);
    rec_ring_push(&m, &e);
    e = mk(REC_SRC_COREDUMP, "", "plasmashell", "", 99, 992);
    rec_ring_push(&m, &e);
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 1 && !strcmp(why, "plasmashell crashed (SIG99)"));
}

static void hogs_max_heaviest(void) {
    rec_proc_t a[6], b[6];
    rec_hog_t out[1];
    int i;
    for (i = 0; i < 5; i++) { a[i] = mkp(10 + i, "light", 0); b[i] = mkp(10 + i, "light", 5 + (uint64_t)i); }
    a[5] = mkp(900, "heavy", 0); b[5] = mkp(900, "heavy", 500);
    assert(rec_hogs(a, 6, b, 6, out, 1) == 1);
    assert(!strcmp(out[0].comm, "heavy") && out[0].ticks == 500);
}

static void correlate_app_crash_ignored(void) {
    static rec_shm_t m;
    rec_event_t e, x;
    rec_episode_t ep;
    char why[160];
    memset(&m, 0, sizeof m);
    memset(&ep, 0, sizeof ep);
    e = mk(REC_SRC_COREDUMP, "", "obsidian", "", 11, 995);
    rec_ring_push(&m, &e);
    x = mk(REC_SRC_SESSION, "plasma-exit", "", "", 0, 1000);
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 0);
    e = mk(REC_SRC_COREDUMP, "", "kwin_wayland", "", 11, 996);
    rec_ring_push(&m, &e);
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 1 && strstr(why, "kwin_wayland"));
}

static void correlate_unordered_ring(void) {
    static rec_shm_t m;
    rec_event_t e, x;
    rec_episode_t ep;
    char why[160];
    memset(&m, 0, sizeof m);
    memset(&ep, 0, sizeof ep);
    e = mk(REC_SRC_COREDUMP, "", "kwin_wayland", "", 11, 995);
    rec_ring_push(&m, &e);
    e = mk(REC_SRC_PID1, "died", "", "old.svc", 0, 900);
    rec_ring_push(&m, &e);
    x = mk(REC_SRC_SESSION, "plasma-exit", "", "", 0, 1000);
    assert(rec_correlate(&m, &ep, &x, why, sizeof why) == 1 && strstr(why, "kwin_wayland"));
}

int main(void) {
    roundtrip();
    hostile_comm();
    msg_last();
    msg_unterminated();
    missing_required();
    send_no_listener();
    tier_app_crash_trace();
    tier_kwin_session();
    tier_logout();
    tier_misc();
    pick_precedence();
    pick_upgrade_and_trace();
    culprit_evict();
    arrow_set();
    shade();
    recurrence();
    render_example();
    render_session();
    render_others();
    seqlock();
    hogs_group();
    episode_short_no_arrow();
    episode_long_arrow();
    episode_overlap_arrow();
    episode_text();
    correlate_kwin();
    correlate_none_logout();
    correlate_episode();
    correlate_priority();
    hogs_max_heaviest();
    correlate_app_crash_ignored();
    correlate_unordered_ring();
    printf("test_recorder: OK\n");
    return 0;
}
