#ifndef PI_BOOTGUARD_H
#define PI_BOOTGUARD_H

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum pi_target { PI_CURRENT, PI_KNOWN_GOOD, PI_SYSTEMD };

struct pi_state {
    int attempts;
    enum pi_target last_target;
};

static const char *const pi_target_names[] = { "current", "known-good", "systemd" };

/* A torn write from a power cut must never crash the wrapper: any doubt boots current. */
static inline int pi_state_parse(const char *buf, struct pi_state *st) {
    st->attempts = 0;
    st->last_target = PI_CURRENT;
    if (!buf || strncmp(buf, "attempts=", 9) != 0)
        return 0;
    const char *p = buf + 9;
    if (*p < '0' || *p > '9')
        return 0;
    long long v = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (*p - '0');
        if (v > INT_MAX)
            return 0;
    }
    if (*p != '\n' || strncmp(p + 1, "last_target=", 12) != 0)
        return 0;
    const char *name = p + 13;
    for (int t = PI_CURRENT; t <= PI_SYSTEMD; t++) {
        size_t l = strlen(pi_target_names[t]);
        if (strncmp(name, pi_target_names[t], l) == 0 && (name[l] == '\n' || name[l] == '\0')) {
            st->attempts = (int)v;
            st->last_target = (enum pi_target)t;
            return 0;
        }
    }
    return 0;
}

static inline int pi_state_format(const struct pi_state *st, char *buf, size_t n) {
    int r = snprintf(buf, n, "attempts=%d\nlast_target=%s\n", st->attempts,
                     pi_target_names[st->last_target]);
    return (r < 0 || (size_t)r >= n) ? -1 : r;
}

static inline enum pi_target pi_decide(struct pi_state *st, int known_good_differs) {
    if (st->attempts == 0 || st->attempts == 1) {
        st->attempts++;
        st->last_target = PI_CURRENT;
    } else if (st->attempts == 2 && known_good_differs) {
        st->attempts = 3;
        st->last_target = PI_KNOWN_GOOD;
    } else {
        st->attempts = 0;
        st->last_target = PI_SYSTEMD;
    }
    return st->last_target;
}

#endif
