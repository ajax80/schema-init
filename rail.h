#ifndef RAIL_H
#define RAIL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* The styled boot rail on a text VT. rail.log and every non-VT stdout keep
 * service_log's plain line; this is presentation only. */

typedef enum {
    CHIP_NONE, CHIP_ONLINE, CHIP_PERFECT, CHIP_DONE, CHIP_SPAWN, CHIP_WAITING, CHIP_RETRY,
    CHIP_DORMANT, CHIP_CUT, CHIP_STOPPED, CHIP_EXITED, CHIP_KILLED, CHIP_STUCK, CHIP_TIMER,
    CHIP_FAILED   /* an action failed (reload, timer run, stop command); the service lives on */
} rail_chip_t;

typedef struct {
    const char *name;
    rail_chip_t chip;
    int retry_n;
    char reason[96];
    int64_t t_ms;   /* ms since PID 1 start, or epoch ms when runtime */
    int runtime;
} rail_line_t;

typedef struct { int online, waiting, friction, cut, starting; } rail_counts_t;

#define RAIL_NAME_COLS 17

size_t rail_fmt_line(char *out, size_t sz, const rail_line_t *l, int fontmode);
size_t rail_fmt_banner(char *out, size_t sz, const char *word, const char *version, const char *host, int nsvc,
                       int fontmode);
size_t rail_fmt_finish(char *out, size_t sz, const rail_counts_t *c, int64_t settled_ms, int timed_out, int fontmode);
size_t rail_fmt_shutdown_end(char *out, size_t sz, int64_t ms, int reboot, int fontmode);
size_t rail_fmt_note(char *out, size_t sz, const char *text, int fontmode);
int rail_name_cols(char *out, size_t sz, const char *name, int fontmode);

/* What the rail needs to know about a service, filled by PID 1 from service_t
 * (rail.c stays free of the engine's internals). */
typedef struct {
    const char *name;
    uint8_t state;        /* STATE_* */
    int pid;
    int starting;         /* spawned and not ready, or queued behind one that is */
    int failed;           /* the engine's svc_failed() */
    int restart_pending;  /* a death will be followed by a restart */
    int restart_count;    /* the engine's own count, shown as RETRY n */
} rail_svc_t;

typedef struct {
    int (*write)(const char *buf, size_t n);
    int64_t (*now_ms)(void);    /* monotonic */
    time_t (*wall)(void);
} rail_io_t;

void rail_init(const rail_io_t *io, int fontmode, int reexeced, int64_t pid1_start_ms);
void rail_banner(const char *version, const char *host, int nsvc);
void rail_event(const rail_svc_t *s, int idx, const char *event);
int64_t rail_tick(const rail_svc_t *svcs, int n, int64_t now_ms);
void rail_arm_settle(void);
void rail_shutdown_begin(void);
void rail_shutdown_banner(void);
void rail_shutdown_outcome(const char *name, rail_chip_t chip);
void rail_shutdown_note(const char *text);
void rail_shutdown_end(int64_t ms, int reboot);
rail_chip_t rail_map_event(const char *event, const rail_svc_t *s);
int rail_event_known(const char *event);
const char *rail_event_why(const char *event);
void rail_counts(const rail_svc_t *svcs, int n, rail_counts_t *c);

#endif
