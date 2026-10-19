#include "recorder_logic.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#define MAXPROC 4096
#define ARROW_L "\xe2\x86\x90"

static rec_shm_t *shm;
static const char *sock_path = REC_SOCK_PATH, *dir_path = REC_EVENTS_DIR;
static long wall_off;
static rec_culprit_t cul[64];
static int ncul;
static rec_episode_t ep[3];
static int live[3], started[3];
static uint64_t live_ts[3], t0;
static int tfd = -1, pending;
static char boot_id[12], box[96], cur_day[40];
static rec_proc_t snap_a[MAXPROC], snap_b[MAXPROC];
static int na, nb;

static uint64_t now_wall(void) { return (uint64_t)((long)time(NULL) + wall_off); }

static uint64_t mono_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec;
}

static int counted(const rec_event_t *e) {
    return e->tier != REC_TIER_TRACE && e->src != REC_SRC_SESSION &&
           !(e->src == REC_SRC_PSI && !strcmp(e->kind, "episode-end"));
}

static void count_culprit(const rec_event_t *e, uint64_t now) {
    const char *k = rec_culprit_key(e);
    if (counted(e) && *k) rec_culprit_count(cul, &ncul, 64, k, e->ts, now);
}

static void repick(void) {
    rec_live_t lv[3];
    rec_arrow_t a;
    int k;
    memset(lv, 0, sizeof lv);
    for (k = 1; k <= 2; k++) {
        if (!ep[k].active) continue;
        lv[k].start = ep[k].start_wall;
        lv[k].live_ts = live[k] ? live_ts[k] : 0;
        rec_episode_text(&ep[k], ep[k].last_fire_mono, lv[k].text, sizeof lv[k].text);
    }
    rec_best_arrow(shm, lv, t0, now_wall(), cul, ncul, &a);
    if (memcmp(&a, &shm->arrow, sizeof a)) rec_arrow_set(shm, &a);
}

static void prune(uint64_t now) {
    DIR *d = opendir(dir_path);
    struct dirent *de;
    if (!d) return;
    while ((de = readdir(d))) {
        struct stat st;
        if (strncmp(de->d_name, "events-", 7) || !strstr(de->d_name, ".log")) continue;
        if (fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && (uint64_t)st.st_mtime + 7 * 86400 < now)
            unlinkat(dirfd(d), de->d_name, 0);
    }
    closedir(d);
}

static void append_line(const rec_event_t *e) {
    char buf[700], path[512], day[40];
    time_t t = (time_t)e->ts;
    struct tm tm;
    int n, fd;
    localtime_r(&t, &tm);
    snprintf(day, sizeof day, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    if (strcmp(day, cur_day)) { rec_copy(cur_day, sizeof cur_day, day); prune(now_wall()); }
    snprintf(path, sizeof path, "%s/events-%s.log", dir_path, day);
    n = rec_format(buf, sizeof buf - 1, e);
    if (n < 0) return;
    buf[n++] = '\n';
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return;
    if (write(fd, buf, (size_t)n) < 0) {}
    if (e->tier >= REC_TIER_CRIT) fdatasync(fd);
    close(fd);
}

static void ring_strip(rec_event_t *e) {
    char *p;
    if (e->src == REC_SRC_EARLYOOM) e->msg[0] = '\0';
    if (e->src != REC_SRC_PSI) return;
    if ((p = strstr(e->msg, " | "))) *p = '\0';
    if ((p = strstr(e->msg, " ["))) *p = '\0';
}

static void record(rec_event_t *e) {
    rec_event_t ring;
    e->tier = (uint8_t)rec_tier(e);
    rec_copy(e->boot, sizeof e->boot, boot_id);
    ring = *e;
    append_line(e);
    ring_strip(&ring);
    rec_ring_push(shm, &ring);
    count_culprit(&ring, now_wall());
    repick();
}

static void handle_datagram(char *buf) {
    rec_event_t e;
    if (rec_parse(buf, &e) != 0) return;
    if (e.src == REC_SRC_PSI) return;
    e.tier = 0;
    if (e.src == REC_SRC_SESSION && !strcmp(e.kind, "plasma-exit")) {
        char why[160], key[32];
        rec_episode_t none;
        int k, hit = 0;
        memset(&none, 0, sizeof none);
        if (rec_correlate(shm, &none, &e, why, sizeof why, key, sizeof key)) hit = 1;
        else for (k = 1; k <= 2 && !hit; k++)
            if (rec_correlate(shm, &ep[k], &e, why, sizeof why, key, sizeof key)) { hit = 1; ep[k].overlapped = 1; }
        if (hit) {
            e.tier = REC_TIER_SESSION;
            rec_copy(e.msg, sizeof e.msg, why);
            rec_copy(e.comm, sizeof e.comm, key);
        }
    }
    record(&e);
}

static int scan(rec_proc_t *out) {
    DIR *d = opendir("/proc");
    struct dirent *de;
    int n = 0;
    if (!d) return 0;
    while (n < MAXPROC && (de = readdir(d))) {
        char path[300], buf[512], *lp, *rp;
        unsigned long long ut, st;
        int fd, r, ppid;
        size_t l;
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/stat", de->d_name);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        r = (int)read(fd, buf, sizeof buf - 1);
        close(fd);
        if (r <= 0) continue;
        buf[r] = '\0';
        lp = strchr(buf, '(');
        rp = strrchr(buf, ')');
        if (!lp || !rp || rp < lp) continue;
        if (sscanf(rp + 2, "%*c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ppid, &ut, &st) != 3) continue;
        l = (size_t)(rp - lp - 1);
        if (l >= sizeof out[n].comm) l = sizeof out[n].comm - 1;
        memcpy(out[n].comm, lp + 1, l);
        out[n].comm[l] = '\0';
        rec_scrub(out[n].comm);
        out[n].pid = atoi(buf);
        out[n].ppid = ppid;
        out[n].ticks = ut + st;
        n++;
    }
    closedir(d);
    return n;
}

static void chain_of(const rec_hog_t *top, char *chain, size_t n) {
    const rec_proc_t *p = NULL;
    int i, lvl;
    rec_copy(chain, n, top->comm);
    for (i = 0; i < nb; i++) if (snap_b[i].pid == top->top_pid) { p = &snap_b[i]; break; }
    for (lvl = 0; p && lvl < 4 && p->ppid > 1; lvl++) {
        const rec_proc_t *q = NULL;
        size_t l = strlen(chain);
        for (i = 0; i < nb; i++) if (snap_b[i].pid == p->ppid) { q = &snap_b[i]; break; }
        if (!q || l + strlen(q->comm) + 4 >= n) break;
        snprintf(chain + l, n - l, ARROW_L "%.31s", q->comm);
        p = q;
    }
}

static void cmdline_of(int pid, char *out, size_t n) {
    char path[48];
    int fd, r, i;
    out[0] = '\0';
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    r = (int)read(fd, out, n - 1);
    close(fd);
    if (r < 0) r = 0;
    out[r] = '\0';
    for (i = 0; i < r; i++) if (!out[i]) out[i] = ' ';
    rec_scrub(out);
    while (r > 0 && out[r - 1] == ' ') out[--r] = '\0';
}

static void arm(int sec) {
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_sec = sec;
    timerfd_settime(tfd, 0, &its, NULL);
}

static int slurp(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC), r;
    if (fd < 0) return -1;
    r = (int)read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = '\0';
    return r;
}

static void box_add(size_t *l, const char *fmt, unsigned long long a, unsigned long long b) {
    int r;
    if (*l >= sizeof box) return;
    r = snprintf(box + *l, sizeof box - *l, fmt, a, b);
    if (r > 0) *l += (size_t)r;
}

static void box_state(void) {
    char b[4096], path[64], *p;
    unsigned long long st = 0, sf = 0, o, c;
    double v;
    long mx = -1;
    size_t l = 0;
    int i;
    box[0] = '\0';
    if (slurp("/proc/pressure/cpu", b, sizeof b) > 0 && sscanf(b, "some avg10=%lf", &v) == 1)
        box_add(&l, " cpu=%llu%%", (unsigned long long)v, 0);
    if (slurp("/proc/pressure/memory", b, sizeof b) > 0 && sscanf(b, "some avg10=%lf", &v) == 1)
        box_add(&l, " mem=%llu%%", (unsigned long long)v, 0);
    if (slurp("/proc/meminfo", b, sizeof b) > 0) {
        if ((p = strstr(b, "SwapTotal:"))) st = strtoull(p + 10, NULL, 10);
        if ((p = strstr(b, "SwapFree:"))) sf = strtoull(p + 9, NULL, 10);
        if (st) box_add(&l, " swap=%lluM/%lluM", (st - sf) >> 10, st >> 10);
    }
    if (slurp("/sys/block/zram0/mm_stat", b, sizeof b) > 0 && sscanf(b, "%llu %llu", &o, &c) == 2)
        box_add(&l, " zram=%lluM>%lluM", o >> 20, c >> 20);
    for (i = 0; i < 32; i++) {
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
        if (slurp(path, b, sizeof b) <= 0) break;
        if (atol(b) > mx) mx = atol(b);
    }
    if (mx >= 0) box_add(&l, " temp=%lluC", (unsigned long long)(mx / 1000), 0);
    if (l >= sizeof box) box[sizeof box - 1] = '\0';
}

static void psi_event(int k, const char *kind, int tier, uint64_t end_mono, uint64_t ts) {
    rec_event_t e;
    char txt[700];
    size_t l;
    memset(&e, 0, sizeof e);
    e.ts = ts;
    e.src = REC_SRC_PSI;
    e.tier = (uint8_t)tier;
    rec_copy(e.kind, sizeof e.kind, kind);
    rec_copy(e.comm, sizeof e.comm, ep[k].top.comm);
    e.pid = ep[k].top.top_pid;
    rec_copy(e.subj, sizeof e.subj, ep[k].chain);
    rec_episode_text(&ep[k], end_mono, txt, sizeof txt);
    l = strlen(txt);
    if (box[0]) l += (size_t)snprintf(txt + l, sizeof txt - l, " |%s", box);
    if (ep[k].cmdline[0]) snprintf(txt + l, sizeof txt - l, " [%s]", ep[k].cmdline);
    rec_copy(e.msg, sizeof e.msg, txt);
    record(&e);
}

static void end_episode(int k, uint64_t mono) {
    int arrow = rec_episode_arrow(&ep[k], mono);
    psi_event(k, "episode-end", ep[k].top.comm[0] && arrow ? REC_TIER_MINOR : REC_TIER_TRACE, ep[k].last_fire_mono, now_wall());
    live[k] = started[k] = 0;
    live_ts[k] = 0;
    ep[k].active = 0;
    repick();
}

static void on_sample(uint64_t mono) {
    rec_hog_t top[1];
    int k;
    nb = scan(snap_b);
    box_state();
    if (rec_hogs(snap_a, na, snap_b, nb, top, 1) > 0) {
        for (k = 1; k <= 2; k++) {
            if (!ep[k].active) continue;
            ep[k].top = top[0];
            chain_of(&top[0], ep[k].chain, sizeof ep[k].chain);
            cmdline_of(top[0].top_pid, ep[k].cmdline, sizeof ep[k].cmdline);
            if (!started[k]) {
                started[k] = 1;
                psi_event(k, "episode-start", REC_TIER_TRACE, mono, now_wall());
            }
        }
    }
    repick();
}

static int check_episodes(uint64_t mono) {
    int k, any = 0;
    for (k = 1; k <= 2; k++) {
        if (!ep[k].active) continue;
        if (rec_episode_check(&ep[k], mono)) { end_episode(k, mono); continue; }
        any = 1;
        if (!live[k] && ep[k].top.comm[0] && (ep[k].overlapped || mono - ep[k].start_mono >= 120)) {
            live[k] = 1;
            live_ts[k] = now_wall();
            psi_event(k, "episode-live", REC_TIER_LIVE, mono, live_ts[k]);
        }
    }
    return any;
}

static void on_timer(void) {
    uint64_t x, mono = mono_s();
    int was = pending;
    if (read(tfd, &x, sizeof x) < 0) return;
    if (pending) {
        on_sample(mono);
        pending = 0;
    }
    if (!check_episodes(mono)) return;
    if (was) arm(20);
    else {
        na = scan(snap_a);
        pending = 1;
        arm(1);
    }
}

static void on_psi(int k) {
    uint64_t mono = mono_s();
    int was = ep[k].active;
    rec_episode_fire(&ep[k], k, mono, now_wall());
    if (was || pending) return;
    na = scan(snap_a);
    pending = 1;
    arm(1);
}

static int psi_open(const char *path, const char *trig, size_t len) {
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0 && write(fd, trig, len) < 0) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}

static void load(uint64_t now) {
    DIR *d;
    struct dirent *de;
    char *names[32];
    char last[512] = "";
    int n = 0, i;
    prune(now);
    if (!(d = opendir(dir_path))) return;
    while ((de = readdir(d)) && n < 32)
        if (!strncmp(de->d_name, "events-", 7) && strstr(de->d_name, ".log")) names[n++] = strdup(de->d_name);
    closedir(d);
    for (i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[j], names[i]) < 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }
    for (i = 0; i < n; i++) {
        char path[512], line[1024];
        FILE *f;
        snprintf(path, sizeof path, "%s/%s", dir_path, names[i]);
        rec_copy(last, sizeof last, path);
        free(names[i]);
        f = fopen(path, "r");
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            rec_event_t e;
            if (rec_parse(line, &e) != 0) continue;
            e.tier = (uint8_t)rec_tier(&e);
            ring_strip(&e);
            count_culprit(&e, now);
            if (e.ts + 36 * 3600 < now) continue;
            rec_ring_push(shm, &e);
        }
        fclose(f);
    }
    repick();
    if (last[0]) {
        char c;
        int fd = open(last, O_RDWR | O_APPEND | O_CLOEXEC);
        if (fd >= 0) {
            off_t sz = lseek(fd, 0, SEEK_END);
            if (sz > 0 && pread(fd, &c, 1, sz - 1) == 1 && c != '\n') {
                if (write(fd, "\n", 1) < 0) {}
            }
            close(fd);
        }
    }
}

static int send_mode(int argc, char **argv) {
    char line[700], tmp[200];
    const char *msg = NULL;
    rec_event_t e;
    int i, n;
    n = snprintf(line, sizeof line, "ts=%llu", (unsigned long long)now_wall());
    for (i = 2; i < argc && n > 0 && (size_t)n < sizeof line; i++) {
        char *eq;
        if (!strncmp(argv[i], "msg=", 4)) { msg = argv[i] + 4; continue; }
        rec_copy(tmp, sizeof tmp, argv[i]);
        eq = strchr(tmp, '=');
        if (eq) rec_word(eq + 1, sizeof tmp - (size_t)(eq + 1 - tmp), argv[i] + (eq + 1 - tmp));
        n += snprintf(line + n, sizeof line - (size_t)n, " %s", tmp);
    }
    if (msg && n > 0 && (size_t)n < sizeof line)
        snprintf(line + n, sizeof line - (size_t)n, " msg=%s", msg);
    if (rec_parse(line, &e) == 0) rec_send(sock_path, &e);
    return 0;
}

int main(int argc, char **argv) {
    const char *env;
    struct pollfd pf[5];
    struct sockaddr_un a;
    sigset_t sm;
    char parent[256], *sl;
    int sfd, sigfd, quit = 0, shm_fd, nopsi, cpu_fd = -1, mem_fd = -1;
    uint64_t now;
    mode_t um;

    if ((env = getenv("SCHEMA_RECORDER_SOCK"))) sock_path = env;
    if ((env = getenv("SCHEMA_RECORDER_DIR"))) dir_path = env;
    if ((env = getenv("SCHEMA_RECORDER_FAKETIME"))) wall_off = atol(env) - (long)time(NULL);
    if (argc > 1 && !strcmp(argv[1], "send")) return send_mode(argc, argv);

    now = t0 = now_wall();
    sigemptyset(&sm);
    sigaddset(&sm, SIGTERM);
    sigaddset(&sm, SIGINT);
    sigprocmask(SIG_BLOCK, &sm, NULL);
    sigfd = signalfd(-1, &sm, SFD_CLOEXEC);
    if (sigfd < 0) sigprocmask(SIG_UNBLOCK, &sm, NULL);
    if (slurp("/proc/sys/kernel/random/boot_id", parent, sizeof parent) > 0) {
        parent[8] = '\0';
        rec_copy(boot_id, sizeof boot_id, parent);
    }

    mkdir(dir_path, 0700);
    rec_copy(parent, sizeof parent, sock_path);
    if ((sl = strrchr(parent, '/')) && sl != parent) { *sl = '\0'; mkdir(parent, 0755); }

    env = getenv("SCHEMA_RECORDER_SHM");
    if (!env) env = REC_SHM_NAME;
    shm_unlink(env);
    shm_fd = shm_open(env, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (shm_fd < 0 || ftruncate(shm_fd, sizeof(rec_shm_t)) < 0) { perror("schema-recorder: shm"); return 1; }
    fchmod(shm_fd, 0644);
    shm = mmap(NULL, sizeof *shm, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) { perror("schema-recorder: mmap"); return 1; }
    memset(shm, 0, sizeof *shm);
    load(now);
    shm->magic = REC_SHM_MAGIC;

    sfd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    rec_copy(a.sun_path, sizeof a.sun_path, sock_path);
    unlink(sock_path);
    um = umask(0177);
    if (sfd < 0 || bind(sfd, (struct sockaddr *)&a, sizeof a) < 0) { perror("schema-recorder: bind"); return 1; }
    umask(um);
    chmod(sock_path, 0600);

    nopsi = getenv("SCHEMA_RECORDER_NOPSI") != NULL;
    if (nopsi) fprintf(stderr, "schema-recorder: pressure capture off (disabled)\n");
    else {
        cpu_fd = psi_open("/proc/pressure/cpu", "some 250000 1000000", sizeof "some 250000 1000000");
        if (cpu_fd < 0) fprintf(stderr, "schema-recorder: pressure capture off (%s)\n", strerror(errno));
        else {
            mem_fd = psi_open("/proc/pressure/memory", "some 200000 1000000", sizeof "some 200000 1000000");
            if (mem_fd < 0) fprintf(stderr, "schema-recorder: pressure capture off (%s)\n", strerror(errno));
        }
    }
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    pf[0] = (struct pollfd){ sfd, POLLIN, 0 };
    pf[1] = (struct pollfd){ cpu_fd, POLLPRI, 0 };
    pf[2] = (struct pollfd){ mem_fd, POLLPRI, 0 };
    pf[3] = (struct pollfd){ tfd, POLLIN, 0 };
    pf[4] = (struct pollfd){ sigfd, POLLIN, 0 };
    while (!quit) {
        if (poll(pf, 5, -1) < 0) continue;
        if (pf[0].revents & POLLIN) {
            char buf[1024];
            ssize_t r;
            while ((r = recv(sfd, buf, sizeof buf - 1, MSG_DONTWAIT)) >= 0) {
                buf[r] = '\0';
                handle_datagram(buf);
            }
        }
        if (pf[1].revents & POLLPRI) on_psi(1);
        if (pf[2].revents & POLLPRI) on_psi(2);
        if (pf[1].revents & POLLERR) pf[1].fd = -1;
        if (pf[2].revents & POLLERR) pf[2].fd = -1;
        if (pf[3].revents & POLLIN) on_timer();
        if (pf[4].revents & POLLIN) quit = 1;
    }
    unlink(sock_path);
    shm_unlink(env);
    return 0;
}
