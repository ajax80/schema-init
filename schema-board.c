/*
 * schema-board — limp-mode recovery surface, increments 1-3.
 *
 * A standalone reader of the PID-1 shared-memory export (schema_shm.h,
 * "/schema-init"). It paints the live weight-state of every service as the OS's
 * LED color-board, refreshing whenever PID 1 bumps `seq`.
 *
 * It depends on nothing graphical: its own process, its own shm snapshot, so it
 * survives a frozen compositor — measured, PID 1 kept updating it with
 * kwin_wayland in state T (seq moves only on a change since 2026-11-13). Since 2026-07-27 it is also *visible* during that
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
#include <stdarg.h>
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
#include "recorder_logic.h"
#include "rail_glyphs.h"
#include <sys/ioctl.h>

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
 * PID 1 bumps seq after a write that changed something (engine.c shm_update), so a stable seq
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

static const rec_shm_t *rmap;
static dev_t rdev;
static ino_t rino;
static long roff;
static time_t rlast;
static char arrow_line[sizeof ((rec_arrow_t *)0)->line];
static int arrow_shade;
static uint32_t arrow_seq;

static void rec_remap(void) {
    const char *name = getenv("SCHEMA_RECORDER_SHM");
    struct stat st;
    int fd = shm_open(name ? name : REC_SHM_NAME, O_RDONLY, 0);
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0) close(fd);
        if (rmap) { munmap((void *)rmap, sizeof *rmap); rmap = NULL; }
        return;
    }
    if (!rmap || st.st_dev != rdev || st.st_ino != rino) {
        const rec_shm_t *m = rec_shm_map(fd);
        if (rmap) munmap((void *)rmap, sizeof *rmap);
        rmap = m;
        rdev = st.st_dev;
        rino = st.st_ino;
    }
    close(fd);
}

static int rec_poll(void) {
    struct timespec ts;
    rec_arrow_t a;
    int shade = 0, changed;
    uint32_t seq = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (!rlast || ts.tv_sec - rlast >= 10) { rlast = ts.tv_sec; rec_remap(); }
    arrow_line[0] = '\0';
    if (rmap && rec_snapshot_arrow(rmap, &a) == 0) {
        seq = __atomic_load_n(&rmap->seq, __ATOMIC_ACQUIRE);
        shade = rec_arrow_shade(&a, (uint64_t)((long)time(NULL) + roff));
        if (shade) { memcpy(arrow_line, a.line, sizeof arrow_line); arrow_line[sizeof arrow_line - 1] = '\0'; rec_scrub(arrow_line); }
    }
    changed = seq != arrow_seq || shade != arrow_shade;
    arrow_seq = seq;
    arrow_shade = shade;
    return changed;
}

/* bytes of arrow_line that fill at most `left` columns, cut on a UTF-8 boundary */
static int arrow_bytes(int left) {
    int i = 0;
    while (arrow_line[i] && left > 0) {
        i++;
        while (((unsigned char)arrow_line[i] & 0xc0) == 0x80) i++;
        left--;
    }
    return i;
}

static void put_arrow(void) {
    struct winsize ws;
    int cols = 80;
    if (!arrow_shade || !arrow_line[0]) return;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) cols = ws.ws_col;
    printf(" %s%.*s\033[0m\n", arrow_shade == 1 ? "\033[1;31m" : "\033[90m", arrow_bytes(cols - 2), arrow_line);
}

static void render_plain(const schema_shm_t *s) {
    int i, valid;
    fputs("\033[2J\033[H", stdout);                       /* clear + home */
    printf("\033[1m schema-init — limp-mode board   "
           "system: %s   seq:%u   services:%d\033[0m\n",
           sys_name(s->system_state), s->seq, s->count);
    put_arrow();
    printf(" %-28s %-13s %4s %8s %5s %5s\n",
           "SERVICE", "STATE", "WT", "PID", "RST", "CARDS");
    printf(" ------------------------------------------------------------------\n");

    valid = s->count;
    if (valid < 0) valid = 0;
    if (valid > SCHEMA_SHM_MAX) valid = SCHEMA_SHM_MAX;
    for (i = 0; i < valid; i++) {
        const shm_svc_t *v = &s->svc[i];
        char name[65];
        memcpy(name, v->name, 64);
        name[64] = '\0';
        char deck[8] = "";
        if (v->card_count) snprintf(deck, sizeof deck, "%u/%u", v->card_played, v->card_count);
        int held = (v->limp & SHM_LIMP_STANDBY) && v->state == STATE_EXCISED;
        printf("%s %-28.28s %s%-13s\033[0m %4u %8d %5d %5s%s%s\n",
               (interactive && i == sel) ? "\033[7m" : "",
               name, held ? "\033[2m" : st_color(v->state), held ? "STANDBY" : st_name(v->state),
               v->weight, v->child_pid, v->restart_count, deck,
               v->limp & SHM_LIMP_FALLBACK ? "  \033[33mlimping: fallback in play\033[0m"
               : v->limp & SHM_LIMP_STANDIN ? "  \033[33mstanding in\033[0m"
               : "",
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

/* ---- the rail look: same font, palette, chips and glyphs as PID 1's boot rail ---- */

enum { STYLE_PLAIN = 0, STYLE_ASCII, STYLE_FONT };
#define ROW_W  78   /* visible columns per cell; two cells + gutter fit 160 */
#define COL_W  80
#define NAME_W 22
#define MAXLN  128

static int style = STYLE_PLAIN;
static int scr_cols = 80, scr_rows = 24;

/* Plain for pipes, --once and serial: vmtest and the recorder test read that. */
static int board_style(int once) {
    const char *e = getenv("SCHEMA_BOARD_STYLE");
    struct console_font_op f = { .op = KD_FONT_OP_GET, .width = 32, .height = 32, .charcount = 512 };
    int m = -1;
    if (e) return !strcmp(e, "font") ? STYLE_FONT : !strcmp(e, "ascii") ? STYLE_ASCII : STYLE_PLAIN;
    if (once || ioctl(STDOUT_FILENO, KDGETMODE, &m) < 0 || m != KD_TEXT) return STYLE_PLAIN;
    /* NULL data asks only the size: the rail glyphs exist only in schema-12x24 */
    return ioctl(STDOUT_FILENO, KDFONTOP, &f) == 0 && f.width == 12 && f.height == 24 ? STYLE_FONT : STYLE_ASCII;
}

static void screen_size(void) {
    struct winsize ws;
    const char *e = getenv("SCHEMA_BOARD_SIZE");
    if (!(e && sscanf(e, "%dx%d", &scr_cols, &scr_rows) == 2) &&
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        scr_cols = ws.ws_col;
        scr_rows = ws.ws_row;
    }
    /* lines[] holds MAXLN; past that the footer (and the y/n prompt) would vanish */
    if (scr_rows > MAXLN - 1) scr_rows = MAXLN - 1;
}

static const char *st_chip(uint8_t s) {
    switch (s) {
        case STATE_PERFECT:     return "30;45";
        case STATE_FULL_TRUST:
        case STATE_FUNDAMENTAL:
        case STATE_SETTLED:     return "30;42";
        case STATE_NEW_PROCESS: return "2;30;42";
        case STATE_RECOVERY:
        case STATE_FRICTION:    return "30;43";
        case STATE_DORMANT:     return "30;44";
        case STATE_EXCISED:     return "30;41";
        default:                return "30;47";
    }
}

typedef struct { char *p; size_t sz, n; } sbuf_t;

static void sput(sbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sput(sbuf_t *b, const char *fmt, ...) {
    va_list ap;
    int r;
    if (b->n >= b->sz) return;
    va_start(ap, fmt);
    r = vsnprintf(b->p + b->n, b->sz - b->n, fmt, ap);
    va_end(ap);
    if (r > 0) b->n = b->n + (size_t)r < b->sz ? b->n + (size_t)r : b->sz - 1;
}

/* Names come from .svc files: printable ASCII as-is, anything else one '?',
 * so nothing a name holds can move the cursor on tty8. Fixed width. */
static void put_name(sbuf_t *b, const char *in, int w, const struct rail_alphabet *ab) {
    const unsigned char *s = (const unsigned char *)in;
    int total = 0, cols = 0;
    for (const unsigned char *q = s; *q; q++) if ((*q & 0xc0) != 0x80) total++;
    for (; *s && cols < w; s++) {
        if ((*s & 0xc0) == 0x80) continue;
        if (total > w && cols == w - 1) { sput(b, "%s", ab->trunc); cols++; break; }
        sput(b, "%c", *s >= 0x20 && *s < 0x7f ? *s : '?');
        cols++;
    }
    for (; cols < w; cols++) sput(b, " ");
}

static void put_lead(sbuf_t *b, int selected, const struct rail_alphabet *ab) {
    if (selected) sput(b, "\033[1;93m%s\033[0m ", style == STYLE_FONT ? "━►" : "->");
    else sput(b, "\033[35m%s\033[0m ", ab->branch);
}

static void cell_svc(sbuf_t *b, const shm_svc_t *v, int selected, const struct rail_alphabet *ab) {
    char name[65], deck[8] = "";
    int held = (v->limp & SHM_LIMP_STANDBY) && v->state == STATE_EXCISED;
    int vis = 2 + 1 + NAME_W + 1 + 13 + 1 + 21;
    memcpy(name, v->name, 64);
    name[64] = '\0';
    if (v->card_count) snprintf(deck, sizeof deck, "%u/%u", v->card_played, v->card_count);
    put_lead(b, selected, ab);
    sput(b, selected ? "\033[1;97m" : "\033[97m");
    put_name(b, name, NAME_W, ab);
    sput(b, "\033[0m \033[%sm %-11s \033[0m \033[90m%3u %7d %3d %5s\033[0m",
         held ? "2;30;47" : st_chip(v->state), held ? "STANDBY" : st_name(v->state),
         v->weight, v->child_pid, v->restart_count, deck);
    if (v->limp & (SHM_LIMP_FALLBACK | SHM_LIMP_STANDIN)) {
        sput(b, " \033[33m%s\033[0m", v->limp & SHM_LIMP_FALLBACK ? "limping " : "stand-in");
        vis += 9;
    }
    for (; vis < ROW_W; vis++) sput(b, " ");
}

static void cell_group(sbuf_t *b, const shm_group_t *g, const struct rail_alphabet *ab) {
    char name[65];
    int vis = 2 + 1 + NAME_W + 1 + 13 + 1 + 11;
    memcpy(name, g->name, 64);
    name[64] = '\0';
    sput(b, "\033[36m%s\033[0m \033[96m", ab->branch_rt);
    put_name(b, name, NAME_W, ab);
    sput(b, "\033[0m \033[%sm %-11s \033[0m \033[90m%3d members\033[0m", st_chip(g->state), st_name(g->state),
         g->member_count);
    for (; vis < ROW_W; vis++) sput(b, " ");
}

/* Cut a built line to `cols` visible columns: a VT wraps anything wider, and
 * on a full screen that wrap scrolls the banner off. SGR runs cost nothing. */
static void clip_line(char *l, size_t sz, int cols) {
    int vis = 0;
    for (char *p = l; *p; ) {
        if (*p == '\033' && p[1] == '[') {
            p += 2;
            while (*p && !(*p >= 0x40 && *p <= 0x7e)) p++;
            if (*p) p++;
            continue;
        }
        if (vis == cols) { snprintf(p, sz - (size_t)(p - l), "\033[0m"); return; }
        vis++;
        p++;
        while ((*p & 0xc0) == 0x80) p++;
    }
}

static char lines[MAXLN][2048];
static int nlines;

static sbuf_t newline(void) {
    sbuf_t b = { lines[nlines], sizeof lines[0], 0 };
    lines[nlines][0] = '\0';
    if (nlines < MAXLN - 1) nlines++;
    return b;
}

/* Column-major slots for `items` cells in `cols` x `rows`: two columns when one
 * would overflow and the screen is wide enough; the last slot becomes "… N more"
 * when even that overflows. Returns the slots drawn. */
static int board_layout(int items, int cols, int rows, int *ncol, int *per_col) {
    int cap;
    if (rows < 1) rows = 1;
    *ncol = items > rows && cols >= COL_W + ROW_W ? 2 : 1;
    cap = *ncol * rows;
    *per_col = items <= cap ? (items + *ncol - 1) / *ncol : rows;
    return items <= cap ? items : cap;
}

static void render_styled(const schema_shm_t *s) {
    static const uint8_t order[] = { STATE_PERFECT, STATE_FUNDAMENTAL, STATE_SETTLED, STATE_FULL_TRUST,
                                     STATE_NEW_PROCESS, STATE_RECOVERY, STATE_FRICTION, STATE_DORMANT,
                                     STATE_EXCISED };
    const struct rail_alphabet *ab = style == STYLE_FONT ? &rail_font_ab : &rail_ascii_ab;
    char host[65] = "";
    int i, valid = s->count, gc = s->group_count, cnt[sizeof order] = { 0 };
    sbuf_t b;

    if (valid < 0) valid = 0;
    if (valid > SCHEMA_SHM_MAX) valid = SCHEMA_SHM_MAX;
    if (gc < 0) gc = 0;
    if (gc > SCHEMA_SHM_MAX_GROUPS) gc = SCHEMA_SHM_MAX_GROUPS;
    gethostname(host, sizeof host - 1);
    screen_size();
    nlines = 0;

    b = newline();
    sput(&b, "\033[35m%s\033[0m \033[95mS C H E M A\033[36m//\033[92mB O A R D\033[0m \033[35m%s\033[0m  \033[90m",
         ab->banner_l, ab->banner_r);
    put_name(&b, host, (int)strnlen(host, 32), ab);
    sput(&b, " %s %s %s %d services %s seq %u\033[0m", ab->sep, sys_name(s->system_state), ab->sep, valid, ab->sep,
         s->seq);
    if (arrow_shade && arrow_line[0]) {
        b = newline();
        sput(&b, "\033[35m%s\033[0m %s%.*s\033[0m", ab->track_v, arrow_shade == 1 ? "\033[1;31m" : "\033[90m",
             arrow_bytes(scr_cols - 3), arrow_line);
    }

    for (i = 0; i < valid; i++)
        for (size_t k = 0; k < sizeof order; k++) if (s->svc[i].state == order[k]) cnt[k]++;
    {
        int items = valid + gc, head = nlines, foot = 1 + (interactive ? 1 + !!msg[0] : 0);
        int ncol, per_col, shown = board_layout(items, scr_cols, scr_rows - head - foot, &ncol, &per_col);
        for (int r = 0; r < per_col; r++) {
            int any = 0;
            for (int c = 0; c < ncol; c++) {
                int k = c * per_col + r;
                if (k >= shown) break;
                if (!any) { b = newline(); any = 1; }
                else sput(&b, "  ");
                if (shown < items && k == shown - 1)
                    sput(&b, "\033[90m%s %d more\033[0m%*s", ab->trunc, items - shown + 1, ROW_W - 8, "");
                else if (k < valid) cell_svc(&b, &s->svc[k], interactive && k == sel, ab);
                else cell_group(&b, &s->groups[k - valid], ab);
            }
        }
    }

    b = newline();
    sput(&b, "\033[35m%s\033[0m", ab->finish);
    for (size_t k = 0; k < sizeof order; k++)
        if (cnt[k]) sput(&b, " \033[%sm %d %s \033[0m", st_chip(order[k]), cnt[k], st_name(order[k]));
    if (interactive) {
        int have = valid > 0 && sel >= 0 && sel < valid;
        char name[65] = "";
        if (have) { memcpy(name, s->svc[sel].name, 64); name[64] = '\0'; }
        b = newline();
        if (mode == MODE_CONFIRM && have) {
            const char *verb = card_for(s->svc[sel].state);
            sput(&b, "\033[1;93m%s\033[0m \033[1m%s ", style == STYLE_FONT ? "━►" : "->", verb);
            put_name(&b, name, (int)strnlen(name, 64), ab);
            sput(&b, "?\033[0m  \033[90mschema-ctl %s ", verb);
            put_name(&b, name, (int)strnlen(name, 64), ab);
            sput(&b, "\033[0m   \033[30;42m y \033[0m apply  \033[30;47m n \033[0m cancel");
        } else {
            sput(&b, "\033[90m%s up/down or j/k  enter: card  ctrl-C: quit", ab->sep);
            if (have) {
                sput(&b, "   card for ");
                put_name(&b, name, (int)strnlen(name, 64), ab);
                sput(&b, ": schema-ctl %s", card_for(s->svc[sel].state));
            }
            sput(&b, "\033[0m");
        }
        if (msg[0]) { b = newline(); sput(&b, " %s", msg); }
    }

    /* no newline after the last line: a full screen must not scroll the banner off */
    fputs("\033[H\033[2J", stdout);
    for (i = 0; i < nlines; i++) {
        clip_line(lines[i], sizeof lines[i], scr_cols);
        printf("%s%s", i ? "\n" : "", lines[i]);
    }
    if (style != STYLE_PLAIN && !isatty(STDOUT_FILENO)) fputs("\n", stdout);
    fflush(stdout);
}

/* The board can start before the boot loads schema-12x24 on its VT, and a
 * setfont changes the size: pick the look and size again, redraw on a change. */
static int view_changed(int once) {
    int st = board_style(once), c = scr_cols, r = scr_rows;
    screen_size();
    if (st == style && c == scr_cols && r == scr_rows) return 0;
    style = st;
    return 1;
}

static void render(const schema_shm_t *s) {
    if (style == STYLE_PLAIN) render_plain(s);
    else render_styled(s);
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
    const schema_shm_t *live;
    const char *env;
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

    fd = shm_open((env = getenv("SCHEMA_BOARD_SHM")) ? env : SCHEMA_SHM_NAME, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "schema-board: cannot open shm %s (is schema-init PID 1?)\n",
                SCHEMA_SHM_NAME);
        return 1;
    }
    live = schema_shm_map(fd);
    close(fd);
    if (!live) {
        fprintf(stderr, "schema-board: shm %s is from another schema-init version; reboot or schema-ctl reexec\n",
                SCHEMA_SHM_NAME);
        return 1;
    }

    style = board_style(once);
    /* test hook: show a selection in a --once render */
    if (once && style != STYLE_PLAIN && (env = getenv("SCHEMA_BOARD_SEL"))) { interactive = 1; sel = atoi(env); }
    if ((env = getenv("SCHEMA_RECORDER_FAKETIME"))) roff = atol(env) - (long)time(NULL);
    rec_poll();

    while (running) {
        if (snapshot(live, &snap) != 0) {
            if (once) { fprintf(stderr, "schema-board: shm churning, no stable read\n"); return 2; }
            usleep(50 * 1000);
            continue;
        }
        if (once) { render(&snap); break; }

        dirty = 0;
        if (snap.seq != last_seq) { dirty = 1; last_seq = snap.seq; }
        if (rec_poll()) dirty = 1;
        if (view_changed(once)) dirty = 1;

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
    munmap((void *)live, sizeof(*live));
    return 0;
}
