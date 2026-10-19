#define main recorder_main
#include "../schema-recorder.c"
#undef main
#include <assert.h>

#define DASH "\xe2\x80\x94"
#define TIMES "\xc3\x97"

static rec_shm_t mem;
static uint64_t base;
static char dir[256];
static int fails;

static void at(uint64_t mono) { wall_off = (long)(base + (mono - 1000)) - (long)time(NULL); }

static void reset(int keep_culprits) {
    shm = &mem;
    memset(shm, 0, sizeof *shm);
    shm->magic = REC_SHM_MAGIC;
    memset(ep, 0, sizeof ep);
    memset(live, 0, sizeof live);
    memset(started, 0, sizeof started);
    memset(live_ts, 0, sizeof live_ts);
    if (!keep_culprits) { ncul = 0; memset(cul, 0, sizeof cul); }
}

static void check(int ok, const char *what) {
    printf("%s %s  [state=%d tier=%d line=%s]\n", ok ? "ok  " : "FAIL", what, shm->arrow.state, shm->arrow.tier, shm->arrow.line);
    if (!ok) fails++;
}

static void hog(uint64_t start_mono) {
    at(start_mono);
    rec_episode_fire(&ep[1], 1, start_mono, now_wall());
    rec_copy(ep[1].top.comm, sizeof ep[1].top.comm, "cosmic-player");
    ep[1].top.count = 8;
    ep[1].top.top_pid = 4242;
    rec_copy(ep[1].chain, sizeof ep[1].chain, "cosmic-player" ARROW_L "kioworker" ARROW_L "dolphin");
    at(start_mono + 1);
    started[1] = 1;
    psi_event(1, "episode-start", REC_TIER_TRACE, start_mono + 1, now_wall());
}

static void pressure_until(uint64_t mono) { ep[1].last_fire_mono = mono; at(mono); }

static void sess(uint64_t mono, int rc) {
    char b[200];
    at(mono);
    snprintf(b, sizeof b, "ts=%llu src=session kind=plasma-exit rc=%d", (unsigned long long)now_wall(), rc);
    handle_datagram(b);
}

static void tick(uint64_t mono) { at(mono); check_episodes(mono); }

static int has(const char *s) { return strstr(shm->arrow.line, s) != NULL; }

static void reload_parity(const char *tag) {
    rec_arrow_t before = shm->arrow;
    char what[256];
    uint64_t now = now_wall();
    reset(0);
    load(now);
    snprintf(what, sizeof what, "%s: reload gives the same arrow (was: %s)", tag, before.line);
    check(!memcmp(&before, &shm->arrow, sizeof before), what);
}

static void use_dir(const char *sub) {
    static char d[300];
    snprintf(d, sizeof d, "%s/%s", dir, sub);
    mkdir(d, 0700);
    dir_path = d;
}

static void find_line(const char *needle, char *line, size_t n) {
    DIR *d = opendir(dir_path);
    struct dirent *de;
    line[0] = 0;
    while (d && (de = readdir(d))) {
        FILE *f;
        if (strncmp(de->d_name, "events-", 7) || !strstr(de->d_name, ".log")) continue;
        f = fdopen(openat(dirfd(d), de->d_name, O_RDONLY | O_CLOEXEC), "r");
        while (f && fgets(line, (int)n, f)) if (strstr(line, needle)) { fclose(f); closedir(d); return; }
        if (f) fclose(f);
        line[0] = 0;
    }
    if (d) closedir(d);
}

static void scenario_a(void) {
    puts("== A: live at 120 s, session exits 20 min in, hogs stop 5 min later");
    use_dir("a");
    reset(0);
    hog(1000);
    pressure_until(1125);
    tick(1125);
    check(shm->arrow.state == 1 && has("CPU pinned 2 min by cosmic-player " TIMES "8"), "A: live episode arrow");
    pressure_until(2000);
    tick(2000);
    repick();
    check(shm->arrow.state == 1 && has("CPU pinned 16 min"), "A: live text refreshed to 16 min");
    pressure_until(2200);
    sess(2200, 1);
    check(shm->arrow.state == 1 && shm->arrow.tier == REC_TIER_LIVE &&
          has("desktop session ended " DASH " CPU pinned 20 min by cosmic-player " TIMES "8"), "A: session line takes the live arrow");
    pressure_until(2500);
    tick(2500);
    repick();
    check(shm->arrow.state == 1 && has("desktop session ended " DASH " CPU pinned 25 min"), "A: live session line refreshed to 25 min");
    tick(2540);
    check(shm->arrow.state == 2 && shm->arrow.tier == REC_TIER_SESSION && shm->arrow.resolved_ts == now_wall() &&
          has("desktop session ended " DASH " CPU pinned 25 min by cosmic-player " TIMES "8") && !has("time this week"),
          "A: held after resolution with final duration");
    at(2540 + 11 * 3600);
    repick();
    check(rec_arrow_shade(&shm->arrow, now_wall()) == 1 && has("desktop session ended"), "A: still bright 11 h later");
    reload_parity("A");
}

static void scenario_b(int second) {
    puts(second ? "== B again an hour later: recurrence" : "== B: session exits at 60 s (before live), hogs die with it");
    use_dir("b");
    reset(second);
    hog(1000);
    pressure_until(1060);
    sess(1060, 0);
    check(has("desktop session ended " DASH " CPU pinned 1 min by cosmic-player " TIMES "8"), "B: session line shown");
    tick(1075);
    check(shm->arrow.state == 1 && has("desktop session ended " DASH " CPU pinned 1 min by cosmic-player " TIMES "8"),
          "B: still the session line after the episode goes live");
    tick(1095);
    check(shm->arrow.state == 2 && shm->arrow.tier == REC_TIER_SESSION &&
          has("desktop session ended " DASH " CPU pinned 1 min by cosmic-player " TIMES "8"), "B: held after resolution");
    if (second) check(has("(2nd time this week)"), "B: recurrence on the session line");
    else check(!has("time this week"), "B: first time has no recurrence");
    reload_parity(second ? "B2" : "B");
}

static void grey_yields(void) {
    rec_event_t e;
    puts("== grey arrow yields to a fresh minor failure");
    use_dir("g");
    reset(0);
    base = (uint64_t)time(NULL) - 20 * 3600;
    at(1000);
    memset(&e, 0, sizeof e);
    e.ts = now_wall(); e.src = REC_SRC_PID1; e.crit = 1;
    rec_copy(e.kind, sizeof e.kind, "died"); rec_copy(e.subj, sizeof e.subj, "sddm");
    record(&e);
    at(1000 + 14 * 3600);
    memset(&e, 0, sizeof e);
    e.ts = now_wall(); e.src = REC_SRC_PID1;
    rec_copy(e.kind, sizeof e.kind, "died"); rec_copy(e.subj, sizeof e.subj, "minor.svc");
    record(&e);
    check(has("minor.svc died"), "grey crit line replaced by new minor");
}

static void earlyoom_strip(void) {
    char b[300], line[1024] = "";
    puts("== earlyoom cmdline stays out of the ring");
    use_dir("e");
    reset(0);
    at(1000);
    snprintf(b, sizeof b, "ts=%llu src=earlyoom kind=kill comm=hog pid=9 msg=/usr/bin/hog --secret-token abc",
             (unsigned long long)now_wall());
    handle_datagram(b);
    check(shm->ring[(shm->head - 1) % REC_RING].msg[0] == '\0', "earlyoom ring msg empty");
    find_line("secret-token", line, sizeof line);
    check(strstr(line, "secret-token") != NULL, "earlyoom file keeps cmdline");
}

static void chain_box_boot(void) {
    char line[1024] = "", cmd[400], *p;
    rec_hog_t h;
    const rec_event_t *r;
    puts("== parent chain, box state, boot id");
    use_dir("c");
    reset(0);
    memset(snap_b, 0, 3 * sizeof snap_b[0]);
    snap_b[0].pid = 4242; snap_b[0].ppid = 300; strcpy(snap_b[0].comm, "cosmic-player");
    snap_b[1].pid = 300; snap_b[1].ppid = 200; strcpy(snap_b[1].comm, "kioworker");
    snap_b[2].pid = 200; snap_b[2].ppid = 1; strcpy(snap_b[2].comm, "dolphin");
    nb = 3;
    memset(&h, 0, sizeof h);
    strcpy(h.comm, "cosmic-player"); h.top_pid = 4242;
    chain_of(&h, cmd, 96);
    check(!strcmp(cmd, "cosmic-player" ARROW_L "kioworker" ARROW_L "dolphin"), "chain joined with arrows, no underscores");
    rec_copy(boot_id, sizeof boot_id, "0f3a9c21");
    box_state();
    rec_copy(box, sizeof box, " cpu=91% mem=0% swap=512M/8192M temp=71C");
    hog(1000);
    rec_copy(ep[1].cmdline, sizeof ep[1].cmdline, "cosmic-player --thumbnail a.mkv");
    pressure_until(1125);
    tick(1125);
    r = &shm->ring[(shm->head - 1) % REC_RING];
    check(!strcmp(r->msg, "CPU pinned 2 min by cosmic-player " TIMES "8") && !strcmp(r->boot, "0f3a9c21"), "ring msg has no box state or cmdline; boot set");
    find_line("kind=episode-live", line, sizeof line);
    p = strstr(line, " boot=0f3a9c21 msg=CPU pinned 2 min by cosmic-player " TIMES "8 | cpu=91% mem=0% swap=512M/8192M temp=71C [cosmic-player --thumbnail a.mkv]");
    check(p != NULL, "file line carries boot, box state and cmdline");
    if (!p) printf("     file: %s", line);
    box[0] = 0;
}

static void prune_on_new_day(void) {
    char old[400];
    struct timespec ts[2];
    rec_event_t e;
    int fd;
    puts("== a new day's file prunes files older than 7 days");
    use_dir("p");
    reset(0);
    snprintf(old, sizeof old, "%s/events-2026-01-01.log", dir_path);
    fd = open(old, O_WRONLY | O_CREAT, 0600);
    close(fd);
    ts[0].tv_sec = ts[1].tv_sec = time(NULL) - 8 * 86400;
    ts[0].tv_nsec = ts[1].tv_nsec = 0;
    utimensat(AT_FDCWD, old, ts, 0);
    cur_day[0] = 0;
    wall_off = 0;
    memset(&e, 0, sizeof e);
    e.ts = (uint64_t)time(NULL); e.src = REC_SRC_PID1;
    rec_copy(e.kind, sizeof e.kind, "started"); rec_copy(e.subj, sizeof e.subj, "x");
    record(&e);
    check(access(old, F_OK) != 0, "8-day-old file removed when the day's file opens");
}

int main(void) {
    snprintf(dir, sizeof dir, "/tmp/schema-test-scenario-%d", (int)getpid());
    mkdir(dir, 0700);
    setenv("TZ", "UTC", 1);
    tzset();
    base = (uint64_t)time(NULL) - 4 * 3600;
    t0 = base;
    scenario_a();
    base += 3600;
    scenario_b(0);
    base = (uint64_t)time(NULL) - 3600;
    scenario_b(1);
    grey_yields();
    earlyoom_strip();
    chain_box_boot();
    prune_on_new_day();
    {
        char cmd[300];
        snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
        if (system(cmd)) {}
    }
    if (fails) { printf("test_recorder_scenario: %d FAILED\n", fails); return 1; }
    printf("test_recorder_scenario: OK\n");
    return 0;
}
