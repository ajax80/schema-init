#ifndef RAIL_H
#define RAIL_H

#include <stddef.h>
#include <stdint.h>

/* The styled boot rail on a text VT. rail.log and every non-VT stdout keep
 * service_log's plain line; this is presentation only. */

typedef enum {
    CHIP_NONE, CHIP_ONLINE, CHIP_PERFECT, CHIP_DONE, CHIP_SPAWN, CHIP_WAITING, CHIP_RETRY,
    CHIP_DORMANT, CHIP_CUT, CHIP_STOPPED, CHIP_EXITED, CHIP_KILLED, CHIP_STUCK, CHIP_TIMER
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

#endif
