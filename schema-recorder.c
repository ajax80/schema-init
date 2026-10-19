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
static uint64_t live_ts[3];
static int tfd = -1, pending;
static rec_proc_t snap_a[MAXPROC], snap_b[MAXPROC];
static int na, nb;

static uint64_t now_wall(void) { return (uint64_t)((long)time(NULL) + wall_off); }

static uint64_t mono_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec;
}

static const char *culprit_key(const rec_event_t *e) {
    return e->src == REC_SRC_PID1 ? e->subj : e->src == REC_SRC_SESSION ? "" : e->comm;
}

static int counted(const rec_event_t *e) {
    return e->tier != REC_TIER_TRACE && !(e->src == REC_SRC_PSI && !strcmp(e->kind, "episode-end"));
}

static uint32_t count_culprit(const rec_event_t *e, uint64_t now) {
    const char *k = culprit_key(e);
    if (!counted(e) || !*k) return 0;
    return (uint32_t)rec_culprit_count(cul, &ncul, 64, k, e->ts, now);
}

static void arrow_take(const rec_event_t *e, uint32_t recurrence, int loading, uint64_t now) {
    rec_arrow_t a;
    memset(&a, 0, sizeof a);
    a.state = e->tier == REC_TIER_LIVE && !loading ? 1 : 2;
    a.tier = e->tier;
    a.event_ts = e->ts;
    a.resolved_ts = a.state == 2 ? (loading && e->tier == REC_TIER_LIVE ? now : e->ts) : 0;
    a.recurrence = recurrence;
    rec_render(a.line, sizeof a.line, e, recurrence);
    rec_arrow_set(shm, &a);
}

static void append_line(const rec_event_t *e) {
    char buf[700], path[512];
    time_t t = (time_t)e->ts;
    struct tm tm;
    int n, fd;
    localtime_r(&t, &tm);
    snprintf(path, sizeof path, "%s/events-%04d-%02d-%02d.log", dir_path, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    n = rec_format(buf, sizeof buf - 1, e);
    if (n < 0) return;
    buf[n++] = '\n';
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return;
    if (write(fd, buf, (size_t)n) < 0) {}
    if (e->tier >= REC_TIER_CRIT) fdatasync(fd);
    close(fd);
}

static void strip_cmd(rec_event_t *e) {
    char *p;
    if (e->src == REC_SRC_PSI && (p = strstr(e->msg, " ["))) *p = '\0';
}

static void record(rec_event_t *e, int pick) {
    uint64_t now = now_wall();
    rec_event_t ring = *e;
    uint32_t r;
    e->tier = (uint8_t)rec_tier(e);
    ring.tier = e->tier;
    append_line(e);
    strip_cmd(&ring);
    rec_ring_push(shm, &ring);
    r = count_culprit(&ring, now);
    if (pick && rec_arrow_pick(&shm->arrow, &ring, now)) arrow_take(&ring, r, 0, now);
}

static void handle_datagram(char *buf) {
    rec_event_t e;
    if (rec_parse(buf, &e) != 0) return;
    if (e.src == REC_SRC_PSI) return;
    e.tier = 0;
    if (e.src == REC_SRC_SESSION && !strcmp(e.kind, "plasma-exit")) {
        char why[160];
        rec_episode_t none;
        int k, hit = 0;
        memset(&none, 0, sizeof none);
        if (rec_correlate(shm, &none, &e, why, sizeof why)) hit = 1;
        else for (k = 1; k <= 2 && !hit; k++)
            if (rec_correlate(shm, &ep[k], &e, why, sizeof why)) { hit = 1; ep[k].overlapped = 1; }
        if (hit) {
            e.tier = REC_TIER_SESSION;
            rec_copy(e.msg, sizeof e.msg, why);
        }
    }
    record(&e, 1);
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
        if (!q || l + strlen(q->comm) + 5 >= n) break;
        snprintf(chain + l, n - l, " " ARROW_L " %.31s", q->comm);
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

static void psi_event(int k, const char *kind, int tier, uint64_t end_mono) {
    rec_event_t e;
    char txt[700];
    memset(&e, 0, sizeof e);
    e.ts = now_wall();
    e.src = REC_SRC_PSI;
    e.tier = (uint8_t)tier;
    rec_copy(e.kind, sizeof e.kind, kind);
    rec_copy(e.comm, sizeof e.comm, ep[k].top.comm);
    e.pid = ep[k].top.top_pid;
    rec_copy(e.subj, sizeof e.subj, ep[k].chain);
    rec_episode_text(&ep[k], end_mono, txt, sizeof txt);
    if (ep[k].cmdline[0] && strlen(txt) + strlen(ep[k].cmdline) + 4 < sizeof txt)
        snprintf(txt + strlen(txt), sizeof txt - strlen(txt), " [%s]", ep[k].cmdline);
    rec_copy(e.msg, sizeof e.msg, txt);
    record(&e, !(k && !strcmp(kind, "episode-end") && live[k]));
}

static void end_episode(int k, uint64_t mono) {
    int arrow = rec_episode_arrow(&ep[k], mono);
    uint64_t now = now_wall();
    psi_event(k, "episode-end", ep[k].top.comm[0] && arrow ? REC_TIER_MINOR : REC_TIER_TRACE, ep[k].last_fire_mono);
    if (live[k]) {
        rec_arrow_t a = shm->arrow;
        if (a.state == 1 && a.event_ts == live_ts[k]) {
            a.state = 2;
            a.tier = REC_TIER_MINOR;
            a.resolved_ts = now;
            rec_arrow_set(shm, &a);
        }
    }
    live[k] = started[k] = 0;
    ep[k].active = 0;
}

static void on_sample(uint64_t mono) {
    rec_hog_t top[1];
    int k;
    nb = scan(snap_b);
    if (rec_hogs(snap_a, na, snap_b, nb, top, 1) > 0) {
        for (k = 1; k <= 2; k++) {
            if (!ep[k].active) continue;
            ep[k].top = top[0];
            chain_of(&top[0], ep[k].chain, sizeof ep[k].chain);
            cmdline_of(top[0].top_pid, ep[k].cmdline, sizeof ep[k].cmdline);
            if (!started[k]) {
                started[k] = 1;
                psi_event(k, "episode-start", REC_TIER_TRACE, mono);
            }
        }
    }
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
            psi_event(k, "episode-live", REC_TIER_LIVE, mono);
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
    DIR *d = opendir(dir_path);
    struct dirent *de;
    char *names[32];
    char last[512] = "";
    uint64_t live_open = 0;
    int n = 0, i;
    if (!d) return;
    while ((de = readdir(d)) && n < 32) {
        char path[512];
        struct stat st;
        if (strncmp(de->d_name, "events-", 7) || !strstr(de->d_name, ".log")) continue;
        snprintf(path, sizeof path, "%s/%s", dir_path, de->d_name);
        if (stat(path, &st) == 0 && (uint64_t)st.st_mtime + 7 * 86400 < now) { unlink(path); continue; }
        names[n++] = strdup(de->d_name);
    }
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
            uint32_t r;
            if (rec_parse(line, &e) != 0) continue;
            e.tier = (uint8_t)rec_tier(&e);
            strip_cmd(&e);
            r = count_culprit(&e, now);
            if (e.ts + 36 * 3600 < now) continue;
            if (e.src == REC_SRC_PSI && e.tier == REC_TIER_LIVE) e.tier = REC_TIER_MINOR;
            rec_ring_push(shm, &e);
            if (e.src == REC_SRC_PSI && !strcmp(e.kind, "episode-end") && live_open && shm->arrow.event_ts == live_open) {
                rec_arrow_t a = shm->arrow;
                a.resolved_ts = e.ts;
                rec_arrow_set(shm, &a);
                live_open = 0;
            }
            if (rec_arrow_pick(&shm->arrow, &e, now)) {
                arrow_take(&e, r, 1, now);
                if (e.src == REC_SRC_PSI && !strcmp(e.kind, "episode-live")) live_open = e.ts;
            }
        }
        fclose(f);
    }
    if (live_open && shm->arrow.event_ts == live_open) {
        rec_arrow_t a = shm->arrow;
        a.resolved_ts = now;
        rec_arrow_set(shm, &a);
    }
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

    now = now_wall();
    sigemptyset(&sm);
    sigaddset(&sm, SIGTERM);
    sigaddset(&sm, SIGINT);
    sigprocmask(SIG_BLOCK, &sm, NULL);
    sigfd = signalfd(-1, &sm, SFD_CLOEXEC);

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
    return 0;
}
