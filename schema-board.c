/*
 * schema-board — limp-mode recovery surface, Increment 1 (read-only board).
 *
 * A standalone, read-only reader of the PID-1 shared-memory export
 * (schema_shm.h, "/schema-init"). It paints the live weight-state of every
 * service as the OS's LED color-board, refreshing whenever PID 1 bumps `seq`.
 *
 * It depends on nothing graphical: it is its own process reading a shm
 * snapshot, so it survives a frozen compositor. This increment is display
 * only — no input, no mitigation (see docs/superpowers/specs/2026-06-14-limp-mode-design.md).
 *
 *   schema-board          live board, refreshes on shm changes
 *   schema-board --once   print one snapshot and exit (for vmtest)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "schema.h"
#include "schema_shm.h"

static volatile sig_atomic_t running = 1;
static void on_sig(int s) { (void)s; running = 0; }

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
        printf(" %-28.28s %s%-13s\033[0m %4u %8d %5d\n",
               name, st_color(v->state), st_name(v->state),
               v->weight, v->child_pid, v->restart_count);
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
    fflush(stdout);
}

int main(int argc, char **argv) {
    int once = (argc > 1 && (strcmp(argv[1], "--once") == 0 ||
                             strcmp(argv[1], "-1") == 0));
    int fd;
    schema_shm_t *live;
    schema_shm_t snap;
    uint32_t last_seq = (uint32_t)-1;

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

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
        if (snap.seq != last_seq) { render(&snap); last_seq = snap.seq; }
        usleep(200 * 1000);   /* 5 Hz poll */
    }

    munmap(live, sizeof(*live));
    return 0;
}
