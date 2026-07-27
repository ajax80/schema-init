/*
 * schema-board — limp-mode recovery surface, increments 1-3.
 *
 * A standalone reader of the PID-1 shared-memory export (schema_shm.h,
 * "/schema-init"). It paints the live weight-state of every service as the OS's
 * LED color-board, refreshing whenever PID 1 bumps `seq`.
 *
 * It depends on nothing graphical: its own process, its own shm snapshot, so it
 * survives a frozen compositor — measured, `seq` kept advancing with
 * kwin_wayland in state T. Since 2026-07-27 it is also *visible* during that
 * freeze: VT_PROCESS mediation in scripts/schema-logind.py hands the console
 * over even when the compositor is too wedged to acknowledge the handoff.
 *
 *   schema-board                    live board, refreshes on shm changes
 *   schema-board --once             print one snapshot and exit (for vmtest)
 *   schema-board --tty /dev/tty8    own a console, reachable with ctrl-alt-F8
 *   schema-board --tty /dev/tty8 --interactive
 *                                   ...and apply a card to a lit slot
 *
 * Watching is read-only and needs no root: it reads shm, never the control
 * socket, so it still works when the socket is wedged. Only --interactive
 * touches the socket, only on an explicit y, and only with a command the
 * operator could have typed themselves.
 *
 * See docs/superpowers/specs/2026-06-14-limp-mode-design.md.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include "schema.h"
#include "schema_shm.h"

#define CTL_SOCK_PATH "/run/schema-init.sock"

static volatile sig_atomic_t running = 1;
static void on_sig(int s) { (void)s; running = 0; }

/* Interactive state (increment 3). The board stays read-only until a card is
 * confirmed: browsing never touches the control socket. */
enum { MODE_BROWSE = 0, MODE_CONFIRM };

static int   interactive = 0;
static int   tty_in = -1;
static int   raw_active = 0;
static struct termios saved_tio;
static int   sel = 0;
static int   mode = MODE_BROWSE;
static char  msg[256] = "";

static void tty_restore(void) {
    if (raw_active && tty_in >= 0) {
        tcsetattr(tty_in, TCSANOW, &saved_tio);
        raw_active = 0;
    }
}

/* ICANON/ECHO off so keys arrive unbuffered and unechoed. ISIG stays ON so
 * ctrl-C still works — a recovery surface must never trap its operator. */
static int tty_raw(int fd) {
    struct termios t;
    if (tcgetattr(fd, &saved_tio) < 0) return -1;
    t = saved_tio;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) < 0) return -1;
    raw_active = 1;
    atexit(tty_restore);
    return 0;
}

/* Which card a lit slot gets. Deterministic and narrow on purpose: the board
 * may only ever do what the operator could type themselves. */
static const char *card_for(uint8_t state) {
    switch (state) {
        case STATE_DORMANT: return "reset";   /* gave up — clear counters, re-queue */
        case STATE_EXCISED: return "start";   /* removed from the rail */
        default:            return "restart";
    }
}

/* Send one command to PID 1 and collect its reply. Every socket operation is
 * bounded: a recovery surface must not hang because the thing it is trying to
 * recover stopped answering. */
static int apply_card(const char *verb, const char *name, char *out, size_t outsz) {
    struct sockaddr_un addr;
    struct timeval tv;
    char cmd[320];
    size_t rlen = 0;
    ssize_t n;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(out, outsz, "socket: %s", strerror(errno)); return -1; }

    tv.tv_sec = 3; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CTL_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        snprintf(out, outsz, "%s: %s%s", CTL_SOCK_PATH, strerror(errno),
                 (errno == EACCES || errno == EPERM) ? " (needs root)" : "");
        close(fd);
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "%s %s\n", verb, name);
    if (write(fd, cmd, strlen(cmd)) < 0) {
        snprintf(out, outsz, "write: %s", strerror(errno));
        close(fd);
        return -1;
    }

    for (;;) {
        if (rlen + 1 >= outsz) break;
        n = read(fd, out + rlen, outsz - 1 - rlen);
        if (n <= 0) break;
        rlen += (size_t)n;
        out[rlen] = '\0';
        if (rlen >= 2 && out[rlen - 2] == '.' && out[rlen - 1] == '\n') {
            out[rlen - 2] = '\0';
            break;
        }
    }
    if (rlen == 0) snprintf(out, outsz, "no reply (timeout)");
    close(fd);

    /* Collapse to one line — the board has a single status row. */
    for (char *p = out; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    return 0;
}

static const char *st_name(uint8_t s) {
    switch (s) {
        case STATE_FUNDAMENTAL: return "FUNDAMENTAL";
        case STATE_FRICTION:    return "FRICTION";
        case STATE_SETTLED:     return "SETTLED";
        case STATE_NEW_PROCESS: return "NEW_PROCESS";
        case STATE_RECOVERY:    return "RECOVERY";
        case STATE_FULL_TRUST:  return "FULL_TRUST";
        case STATE_PERFECT:     return "PERFECT";
        case STATE_EXCISED:     return "EXCISED";
        case STATE_DORMANT:     return "DORMANT";
        default:                return "?";
    }
}

/* LED color language (see spec): purple=go, green=stable/new, yellow=failing,
 * blue=attention, red=broke. Returns an ANSI SGR sequence. */
static const char *st_color(uint8_t s) {
    switch (s) {
        case STATE_PERFECT:     return "\033[35m";   /* purple — go/heaven   */
        case STATE_FULL_TRUST:
        case STATE_FUNDAMENTAL:
        case STATE_SETTLED:     return "\033[32m";   /* green  — stable      */
        case STATE_NEW_PROCESS: return "\033[2;32m"; /* dim green — new      */
        case STATE_RECOVERY:
        case STATE_FRICTION:    return "\033[33m";   /* yellow — failing     */
        case STATE_DORMANT:     return "\033[34m";   /* blue   — attention   */
        case STATE_EXCISED:     return "\033[31m";   /* red    — broke       */
        default:                return "\033[37m";   /* white  — unknown     */
    }
}

static const char *sys_name(uint8_t s) {
    switch (s) {
        case 0:  return "RUNNING";
        case 13: return "SHUTDOWN";
        case 14: return "RESTART";
        default: return "?";
    }
}

/* Consistent snapshot: read seq, copy, re-read seq; retry while it moved.
 * PID 1 bumps seq after a full write (init.c shm_update), so a stable seq
 * across the copy means the snapshot is internally consistent. */
static int snapshot(const schema_shm_t *live, schema_shm_t *out) {
    const volatile uint32_t *seqp = &live->seq;
    int tries;
    for (tries = 0; tries < 1000; tries++) {
        uint32_t s1 = *seqp;
        memcpy(out, live, sizeof(*out));
        uint32_t s2 = *seqp;
        if (s1 == s2) return 0;
    }
    return -1;
}

static void render(const schema_shm_t *s) {
    int i, valid;
    fputs("\033[2J\033[H", stdout);                       /* clear + home */
    printf("\033[1m schema-init — limp-mode board   "
           "system: %s   seq:%u   services:%d\033[0m\n",
           sys_name(s->system_state), s->seq, s->count);
    printf(" %-28s %-13s %4s %8s %5s\n",
           "SERVICE", "STATE", "WT", "PID", "RST");
    printf(" ------------------------------------------------------------------\n");

    valid = s->count;
    if (valid < 0) valid = 0;
    if (valid > SCHEMA_SHM_MAX) valid = SCHEMA_SHM_MAX;
    for (i = 0; i < valid; i++) {
        const shm_svc_t *v = &s->svc[i];
        char name[65];
        memcpy(name, v->name, 64);
        name[64] = '\0';
        printf("%s %-28.28s %s%-13s\033[0m %4u %8d %5d%s\n",
               (interactive && i == sel) ? "\033[7m" : "",
               name, st_color(v->state), st_name(v->state),
               v->weight, v->child_pid, v->restart_count,
               (interactive && i == sel) ? "\033[0m" : "");
    }

    if (s->group_count > 0) {
        int gc = s->group_count;
        if (gc > SCHEMA_SHM_MAX_GROUPS) gc = SCHEMA_SHM_MAX_GROUPS;
        printf("\n %-28s %-13s %5s\n", "GROUP", "STATE", "MEM");
        printf(" --------------------------------------------------\n");
        for (i = 0; i < gc; i++) {
            const shm_group_t *g = &s->groups[i];
            char gname[65];
            memcpy(gname, g->name, 64);
            gname[64] = '\0';
            printf(" %-28.28s %s%-13s\033[0m %5d\n",
                   gname, st_color(g->state), st_name(g->state),
                   g->member_count);
        }
    }

    if (interactive) {
        char name[65];
        int have = (valid > 0 && sel >= 0 && sel < valid);
        if (have) {
            memcpy(name, s->svc[sel].name, 64);
            name[64] = '\0';
        }
        printf("\n");
        if (mode == MODE_CONFIRM && have) {
            const char *verb = card_for(s->svc[sel].state);
            printf(" \033[1m▸ %s %s?\033[0m\n", verb, name);
            printf("   will run: schema-ctl %s %s\n", verb, name);
            printf("   \033[7m[y]\033[0m apply   \033[7m[n]\033[0m cancel\n");
        } else {
            printf(" \033[2m↑/↓ or j/k select   enter: card   ctrl-C: quit\033[0m\n");
            if (have)
                printf(" \033[2mcard for %s: schema-ctl %s %s\033[0m\n",
                       name, card_for(s->svc[sel].state), name);
        }
        if (msg[0]) printf(" %s\n", msg);
    }
    fflush(stdout);
}

static void usage(FILE *out) {
    fprintf(out,
        "usage: schema-board [--tty <device>] [--interactive] [--once]\n"
        "\n"
        "  --tty <device>   paint a dedicated console instead of stdout, e.g.\n"
        "                   --tty /dev/tty8 — reachable with ctrl-alt-F8 even\n"
        "                   when the desktop is wedged\n"
        "  --interactive    allow applying a card to the selected service.\n"
        "                   Browsing stays read-only; nothing is sent to PID 1\n"
        "                   until a card is confirmed with y. Needs root and a\n"
        "                   --tty (or a terminal on stdin).\n"
        "  --once           print one snapshot and exit\n"
        "  --help           this text\n"
        "  --version        print version and exit\n"
        "\n"
        "Reads the PID 1 shared-memory export, not the control socket, so the\n"
        "board keeps working when the socket is wedged and needs no root to\n"
        "watch. Only applying a card talks to the socket, and that needs root.\n");
}

/* Repoint stdout at a console so render() paints there unchanged. Services are
 * spawned with stdout on a logfile, so a board run as a .svc needs this. */
static int open_tty(const char *dev) {
    int t = open(dev, (interactive ? O_RDWR : O_WRONLY) | O_NOCTTY);
    if (t < 0) { fprintf(stderr, "schema-board: %s: %s\n", dev, strerror(errno)); return -1; }
    if (dup2(t, STDOUT_FILENO) < 0) { perror("dup2"); close(t); return -1; }
    if (interactive) {
        tty_in = dup(t);
        if (tty_in < 0) { perror("dup"); close(t); return -1; }
        if (tty_raw(tty_in) < 0) {
            fprintf(stderr, "schema-board: %s: raw mode: %s\n", dev, strerror(errno));
            close(tty_in);
            tty_in = -1;
            interactive = 0;
        }
    }
    if (t != STDOUT_FILENO) close(t);
    fputs("\033[9;0]", stdout);   /* linux console: disable screen blanking */
    fputs("\033[?25l", stdout);   /* hide cursor */
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv) {
    const char *tty = NULL;
    int once = 0;
    int fd, i;
    int dirty, valid;
    schema_shm_t *live;
    schema_shm_t snap;
    uint32_t last_seq = (uint32_t)-1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0 || strcmp(argv[i], "-1") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--tty") == 0 && i + 1 < argc) {
            tty = argv[++i];
        } else if (strcmp(argv[i], "--interactive") == 0) {
            interactive = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("schema-board %s\n", SCHEMA_INIT_VERSION);
            return 0;
        } else {
            usage(stderr);
            return 1;
        }
    }

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

    if (tty && open_tty(tty) != 0) return 1;

    /* No --tty: fall back to the terminal already on stdin. */
    if (interactive && tty_in < 0) {
        if (isatty(STDIN_FILENO) && tty_raw(STDIN_FILENO) == 0) {
            tty_in = STDIN_FILENO;
        } else {
            fprintf(stderr, "schema-board: --interactive needs a terminal; continuing read-only\n");
            interactive = 0;
        }
    }

    fd = shm_open(SCHEMA_SHM_NAME, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "schema-board: cannot open shm %s (is schema-init PID 1?)\n",
                SCHEMA_SHM_NAME);
        return 1;
    }
    live = mmap(NULL, sizeof(*live), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (live == MAP_FAILED) {
        fprintf(stderr, "schema-board: mmap failed\n");
        return 1;
    }

    while (running) {
        if (snapshot(live, &snap) != 0) {
            if (once) { fprintf(stderr, "schema-board: shm churning, no stable read\n"); return 2; }
            usleep(50 * 1000);
            continue;
        }
        if (once) { render(&snap); break; }

        dirty = 0;
        if (snap.seq != last_seq) { dirty = 1; last_seq = snap.seq; }

        valid = snap.count;
        if (valid < 0) valid = 0;
        if (valid > SCHEMA_SHM_MAX) valid = SCHEMA_SHM_MAX;
        if (sel >= valid) { sel = valid ? valid - 1 : 0; dirty = 1; }
        if (sel < 0) { sel = 0; dirty = 1; }

        if (!interactive || tty_in < 0) {
            if (dirty) render(&snap);
            usleep(200 * 1000);   /* 5 Hz poll */
            continue;
        }

        {
            struct pollfd pfd;
            char buf[16];
            ssize_t n, k;

            pfd.fd = tty_in;
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN)) {
                n = read(tty_in, buf, sizeof(buf));
                for (k = 0; k < n; k++) {
                    char c = buf[k];
                    /* arrow keys arrive as ESC [ A / ESC [ B */
                    if (c == 0x1b && k + 2 < n && buf[k + 1] == '[') {
                        if (buf[k + 2] == 'A') { sel--; k += 2; dirty = 1; continue; }
                        if (buf[k + 2] == 'B') { sel++; k += 2; dirty = 1; continue; }
                    }
                    if (mode == MODE_CONFIRM) {
                        if (c == 'y' || c == 'Y') {
                            if (valid > 0) {
                                char name[65], reply[256];
                                const char *verb = card_for(snap.svc[sel].state);
                                memcpy(name, snap.svc[sel].name, 64);
                                name[64] = '\0';
                                snprintf(msg, sizeof(msg), "\033[33m… %s %s\033[0m", verb, name);
                                render(&snap);
                                if (apply_card(verb, name, reply, sizeof(reply)) == 0)
                                    snprintf(msg, sizeof(msg), "\033[32m✓ %s %s: %.120s\033[0m", verb, name, reply);
                                else
                                    snprintf(msg, sizeof(msg), "\033[31m✗ %s %s: %.120s\033[0m", verb, name, reply);
                            }
                            mode = MODE_BROWSE;
                            dirty = 1;
                        } else if (c == 'n' || c == 'N' || c == 0x1b || c == 'q') {
                            snprintf(msg, sizeof(msg), "\033[2mcancelled\033[0m");
                            mode = MODE_BROWSE;
                            dirty = 1;
                        }
                        continue;
                    }
                    switch (c) {
                        case 'k': case 'K': sel--; dirty = 1; break;
                        case 'j': case 'J': sel++; dirty = 1; break;
                        case 'g':           sel = 0; dirty = 1; break;
                        case 'G':           sel = valid ? valid - 1 : 0; dirty = 1; break;
                        case '\r': case '\n':
                            if (valid > 0) { mode = MODE_CONFIRM; msg[0] = '\0'; dirty = 1; }
                            break;
                        default: break;
                    }
                }
                if (sel < 0) sel = 0;
                if (sel >= valid) sel = valid ? valid - 1 : 0;
            }
        }
        if (dirty) render(&snap);
    }

    if (tty) { fputs("\033[?25h\n", stdout); fflush(stdout); }
    munmap(live, sizeof(*live));
    return 0;
}
