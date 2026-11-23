/* schema-top: see board_top.h. Linked into schema-board; picked by argv[0]. */
#include "board_top.h"
#include "state_colors.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#define TOP_CGROOT "/sys/fs/cgroup/schema-init"
#define NAME_W 22

double top_cpu_pct(uint64_t prev_usec, uint64_t cur_usec, int64_t elapsed_us, double keep) {
    /* an early wake on a seq change gives a tiny window: keep the last value */
    if (elapsed_us < 100000 || cur_usec < prev_usec) return keep;
    return (double)(cur_usec - prev_usec) * 100.0 / (double)elapsed_us;
}

static int read_small(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = '\0';
    return 0;
}

int top_read_cgroup(const char *cgroot, const char *svc, uint64_t *usage_usec, long long *mem) {
    char p[PATH_MAX], b[512];
    char *u;
    if (strchr(svc, '/') || !*svc) return -1;
    snprintf(p, sizeof p, "%s/%s/memory.current", cgroot, svc);
    if (read_small(p, b, sizeof b)) return -1;
    *mem = atoll(b);
    snprintf(p, sizeof p, "%s/%s/cpu.stat", cgroot, svc);
    if (read_small(p, b, sizeof b) || !(u = strstr(b, "usage_usec "))) return -1;
    *usage_usec = strtoull(u + 11, NULL, 10);
    return 0;
}

static int64_t mono_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* samples are matched by name: the table can reorder and services come and go */
void top_sample_all(const schema_shm_t *s, top_sample_t *samp, int n, const char *cgroot) {
    static top_sample_t prev[SCHEMA_SHM_MAX];
    memcpy(prev, samp, sizeof prev);
    int64_t now = mono_us();
    for (int i = 0; i < n; i++) {
        top_sample_t *c = &samp[i], *p = NULL;
        for (int j = 0; j < SCHEMA_SHM_MAX && !p; j++)
            if (prev[j].have && !strncmp(prev[j].name, s->svc[i].name, 64)) p = &prev[j];
        memset(c, 0, sizeof *c);
        memcpy(c->name, s->svc[i].name, 63);
        if (top_read_cgroup(cgroot, c->name, &c->usage_usec, &c->mem)) continue;
        c->have = 1;
        c->mono_us = now;
        if (p) {
            c->cpu = top_cpu_pct(p->usage_usec, c->usage_usec, now - p->mono_us, p->cpu);
            c->have_cpu = p->have_cpu || now - p->mono_us >= 100000;
            if (now - p->mono_us < 100000) {   /* too close: carry the old sample's base */
                c->usage_usec = p->usage_usec;
                c->mono_us = p->mono_us;
            }
        }
    }
}

static const schema_shm_t *sort_s;
static const top_sample_t *sort_m;
static int sort_key, sort_rev;

static int cmp(const void *a, const void *b) {
    int i = *(const int *)a, j = *(const int *)b, r = 0;
    const top_sample_t *x = &sort_m[i], *y = &sort_m[j];
    if (sort_key == TOP_SORT_CPU) {
        double u = x->have_cpu ? x->cpu : -1, v = y->have_cpu ? y->cpu : -1;
        r = (u < v) - (u > v);
    } else if (sort_key == TOP_SORT_MEM) {
        long long u = x->have ? x->mem : -1, v = y->have ? y->mem : -1;
        r = (u < v) - (u > v);
    }
    if (!r) r = strncmp(sort_s->svc[i].name, sort_s->svc[j].name, 64);
    return sort_rev ? -r : r;
}

void top_sort(const schema_shm_t *s, const top_sample_t *samp, int *order, int n, int key, int reverse) {
    for (int i = 0; i < n; i++) order[i] = i;
    sort_s = s, sort_m = samp, sort_key = key, sort_rev = reverse;
    qsort(order, (size_t)n, sizeof *order, cmp);
}

static void fmt_mem(long long b, char *out, size_t n) {
    if (b < 1024) snprintf(out, n, "%lldB", b);
    else if (b < 1024LL * 1024) snprintf(out, n, "%.1fK", b / 1024.0);
    else if (b < 1024LL * 1024 * 1024) snprintf(out, n, "%.1fM", b / 1048576.0);
    else snprintf(out, n, "%.1fG", b / 1073741824.0);
}

static int utf8_term(void) {
    const char *v = getenv("LC_ALL");
    if (!v || !*v) v = getenv("LC_CTYPE");
    if (!v || !*v) v = getenv("LANG");
    return v && (strstr(v, "UTF-8") || strstr(v, "utf8") || strstr(v, "UTF8") || strstr(v, "utf-8"));
}

/* a name is a file name, so anything: control bytes shown as '?', cut to NAME_W */
static void put_name(FILE *out, const char *raw) {
    char n[65];
    size_t len = strnlen(raw, 64), i;
    for (i = 0; i < len; i++) n[i] = (unsigned char)raw[i] < 0x20 || raw[i] == 0x7f ? '?' : raw[i];
    n[len] = '\0';
    if (len > NAME_W) fprintf(out, "%.*s%s", NAME_W - 1, n, utf8_term() ? "\xe2\x80\xa6" : "~");
    else fprintf(out, "%-*s", NAME_W, n);
}

/* copy a rendered line, stopping at cols visible columns (SGR and UTF-8
 * continuation bytes are not columns) */
static void put_clipped(FILE *out, const char *line, int cols) {
    int vis = 0;
    for (const char *p = line; *p && *p != '\n'; p++) {
        if (*p == '\033') {
            const char *e = p + 1;
            if (*e == '[') while (*++e && !(*e >= '@' && *e <= '~'));
            fwrite(p, 1, (size_t)(e - p + (*e != 0)), out);
            p = *e ? e : e - 1;
            continue;
        }
        if (((unsigned char)*p & 0xc0) != 0x80 && ++vis > cols) break;
        fputc(*p, out);
    }
    fputs("\033[0m\n", out);
}

static void line_out(FILE *out, int cols, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void line_out(FILE *out, int cols, const char *fmt, ...) {
    char *b = NULL;
    size_t n = 0;
    FILE *m = open_memstream(&b, &n);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(m, fmt, ap);
    va_end(ap);
    fclose(m);
    put_clipped(out, b, cols);
    free(b);
}

static const char *sys_label(uint8_t st) {
    return st == 13 ? "SHUTDOWN" : st == 14 ? "RESTART" : "RUNNING";
}

void top_render(FILE *out, const schema_shm_t *s, const top_sample_t *samp, const int *order, int n,
                int key, int reverse, int first, int rows, int cols) {
    static const char *keys[] = { "CPU", "MEM", "NAME" };
    int ok = 0, fr = 0, dor = 0, exc = 0;
    long long mem = 0;
    char host[64] = "", m[32], counts[160] = "", range[48] = "";
    gethostname(host, sizeof host - 1);
    for (int i = 0; i < n; i++) {
        uint8_t st = s->svc[i].state;
        if (st == STATE_FRICTION || st == STATE_RECOVERY) fr++;
        else if (st == STATE_DORMANT) dor++;
        else if (st == STATE_EXCISED) exc++;
        else ok++;
        if (samp[i].have) mem += samp[i].mem;
    }
    if (first < 0) first = 0;
    if (rows < 1) rows = 1;
    if (n > rows) snprintf(range, sizeof range, "   [%d-%d of %d]", first + 1, first + rows < n ? first + rows : n, n);
    line_out(out, cols, "\033[1m schema-top \xe2\x80\x94 %s   %s   %d services   sort: %s%s\033[0m%s",
             host, sys_label(s->system_state), n, keys[key], reverse ? " (rev)" : "", range);
    char *c = counts;
    size_t left = sizeof counts;
#define ADD(v, w) if (v) { int k = snprintf(c, left, "%s%d %s", c == counts ? "" : " \xc2\xb7 ", v, w); c += k; left -= (size_t)k; }
    ADD(ok, "ok") ADD(fr, "friction") ADD(dor, "dormant") ADD(exc, "excised")
#undef ADD
    fmt_mem(mem, m, sizeof m);
    line_out(out, cols, " %s \xc2\xb7 mem %s", counts, m);
#define H(k, txt) (key == (k) ? "\033[1m" : ""), txt, (key == (k) ? "\033[0m" : "")
    line_out(out, cols, " %s%-*s%s %-11s %7s %s%5s%s %s%6s%s %4s",
             key == TOP_SORT_NAME ? "\033[1m" : "", NAME_W, "SERVICE", key == TOP_SORT_NAME ? "\033[0m" : "",
             "STATE", "PID", H(TOP_SORT_CPU, "CPU%"), H(TOP_SORT_MEM, "MEM"), "RST");
#undef H
    for (int r = first; r < n && r < first + rows; r++) {
        int i = order[r];
        const shm_svc_t *v = &s->svc[i];
        const top_sample_t *x = &samp[i];
        char *b = NULL;
        size_t bn = 0;
        FILE *L = open_memstream(&b, &bn);
        fputc(' ', L);
        put_name(L, v->name);
        fprintf(L, " %s%-11s\033[0m %7d", state_ansi(v->state), state_label(v->state), v->child_pid);
        if (x->have_cpu) {
            const char *col = x->cpu >= 100 ? "\033[1;31m" : x->cpu >= 50 ? "\033[33m" : x->cpu == 0 ? "\033[2m" : "";
            fprintf(L, " %s%5.*f\033[0m", col, x->cpu >= 1000 ? 0 : 1, x->cpu);
        } else {
            fputs("      ", L);
        }
        if (x->have) {
            fmt_mem(x->mem, m, sizeof m);
            fprintf(L, " %6s", m);
        } else {
            fputs("       ", L);
        }
        fprintf(L, " %4d", v->restart_count);
        fclose(L);
        put_clipped(out, b, cols);
        free(b);
    }
}

static void term_size(int *cols, int *rows) {
    const char *e = getenv("SCHEMA_BOARD_SIZE");
    struct winsize ws;
    *cols = 80, *rows = 24;
    if (e && sscanf(e, "%dx%d", cols, rows) == 2) return;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
        *cols = ws.ws_col, *rows = ws.ws_row;
}

/* consistent copy: re-read while PID 1 moved seq under us */
static void snap(const schema_shm_t *live, schema_shm_t *out) {
    for (int t = 0; t < 8; t++) {
        uint32_t a = *(const volatile uint32_t *)&live->seq;
        memcpy(out, live, sizeof *out);
        if (*(const volatile uint32_t *)&live->seq == a) return;
    }
}

static struct termios saved_tio;
static int raw_on;

static void raw_off(void) {
    if (!raw_on) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
    fputs("\033[?25h\033[0m\n", stdout);
    fflush(stdout);
    raw_on = 0;
}

static void on_signal(int sig) {
    raw_off();
    signal(sig, SIG_DFL);
    raise(sig);
}

static int count_of(const schema_shm_t *s) {
    return s->count < 0 ? 0 : s->count > SCHEMA_SHM_MAX ? SCHEMA_SHM_MAX : s->count;
}

/* every key that arrived, arrows as j/k (a paste or held key comes in one read) */
static int read_keys(char *keys, int max) {
    unsigned char b[64];
    ssize_t r = read(STDIN_FILENO, b, sizeof b);
    int n = 0;
    for (ssize_t i = 0; i < r && n < max; i++) {
        if (b[i] == 0x1b && i + 2 < r && b[i + 1] == '[') {
            if (b[i + 2] == 'A') keys[n++] = 'k';
            else if (b[i + 2] == 'B') keys[n++] = 'j';
            i += 2;
        } else {
            keys[n++] = (char)b[i];
        }
    }
    return n;
}

int top_main(int argc, char **argv) {
    static schema_shm_t s;
    static top_sample_t samp[SCHEMA_SHM_MAX];
    int once = 0, order[SCHEMA_SHM_MAX], cols, rows, key = TOP_SORT_CPU, rev = 0, first = 0;
    const char *name = getenv("SCHEMA_BOARD_SHM"), *cg = getenv("SCHEMA_BOARD_CGROOT");
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--once")) once = 1;
    if (!name) name = SCHEMA_SHM_NAME;
    if (!cg) cg = TOP_CGROOT;
    int fd = shm_open(name, O_RDONLY, 0);
    const schema_shm_t *live = fd < 0 ? NULL : schema_shm_map(fd);
    if (!live) {
        fprintf(stderr, "schema-top: cannot read PID 1's table %s: is this box running schema-init?\n", name);
        return 1;
    }
    snap(live, &s);
    top_sample_all(&s, samp, count_of(&s), cg);
    if (once) {
        usleep(500000);
        snap(live, &s);
        int n = count_of(&s);
        top_sample_all(&s, samp, n, cg);
        term_size(&cols, &rows);
        top_sort(&s, samp, order, n, key, rev);
        top_render(stdout, &s, samp, order, n, key, rev, 0, n, cols);
        return 0;
    }
    if (tcgetattr(STDIN_FILENO, &saved_tio) == 0) {
        struct termios t = saved_tio;
        t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
        raw_on = 1;
        atexit(raw_off);
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        signal(SIGHUP, on_signal);
    }
    fputs("\033[?25l", stdout);
    int64_t next = 0;
    uint32_t seen = (uint32_t)-1;
    int dirty = 1;
    for (;;) {
        int64_t now = mono_us();
        if (now >= next) {
            snap(live, &s);
            top_sample_all(&s, samp, count_of(&s), cg);
            next = now + 1000000;
            dirty = 1;
        } else if (*(const volatile uint32_t *)&live->seq != seen) {
            snap(live, &s);
            top_sample_all(&s, samp, count_of(&s), cg);
            dirty = 1;
        }
        seen = s.seq;
        if (dirty) {
            int n = count_of(&s);
            term_size(&cols, &rows);
            int win = rows - 4 > 1 ? rows - 4 : 1;
            if (first > n - win) first = n - win;
            if (first < 0) first = 0;
            top_sort(&s, samp, order, n, key, rev);
            fputs("\033[H\033[2J", stdout);
            top_render(stdout, &s, samp, order, n, key, rev, first, win, cols);
            fflush(stdout);
            dirty = 0;
        }
        /* wake for a key, a seq change (checked every 200 ms) or the 1 s tick */
        int64_t left = (next - mono_us()) / 1000;
        struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
        if (poll(&p, 1, left < 0 ? 0 : left > 200 ? 200 : (int)left) > 0) {
            if (p.revents & (POLLHUP | POLLERR)) return 0;
            char k[64];
            int nk = read_keys(k, (int)sizeof k);
            for (int i = 0; i < nk; i++) {
                switch (k[i]) {
                case 'q': return 0;
                case 'c': key = TOP_SORT_CPU; first = 0; break;
                case 'm': key = TOP_SORT_MEM; first = 0; break;
                case 'n': key = TOP_SORT_NAME; first = 0; break;
                case 'r': rev = !rev; break;
                case 'j': first++; break;
                case 'k': first--; break;
                default: continue;
                }
                dirty = 1;
            }
        }
    }
}
