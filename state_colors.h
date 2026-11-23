#ifndef STATE_COLORS_H
#define STATE_COLORS_H
/* The state names and LED colour language shared by schema-board, schema-top and
 * schema-ctl status, so the three can't drift: purple=go, green=stable/new,
 * yellow=failing, blue=attention, red=broke. */
#include "schema.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline const char *state_label(uint8_t s) {
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

static inline const char *state_ansi(uint8_t s) {
    switch (s) {
        case STATE_PERFECT:     return "\033[35m";
        case STATE_FULL_TRUST:
        case STATE_FUNDAMENTAL:
        case STATE_SETTLED:     return "\033[32m";
        case STATE_NEW_PROCESS: return "\033[2;32m";
        case STATE_RECOVERY:
        case STATE_FRICTION:    return "\033[33m";
        case STATE_DORMANT:     return "\033[34m";
        case STATE_EXCISED:     return "\033[31m";
        default:                return "\033[37m";
    }
}

/* the state for an exact name of len bytes (not NUL-terminated), or -1 */
static inline int state_from_name(const char *name, size_t len) {
    static const uint8_t all[] = { STATE_FUNDAMENTAL, STATE_FRICTION, STATE_SETTLED, STATE_NEW_PROCESS,
                                   STATE_RECOVERY, STATE_FULL_TRUST, STATE_PERFECT, STATE_EXCISED, STATE_DORMANT };
    for (size_t i = 0; i < sizeof all; i++) {
        const char *n = state_label(all[i]);
        if (strlen(n) == len && !memcmp(n, name, len)) return all[i];
    }
    return -1;
}
#endif
