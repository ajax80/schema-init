#ifndef CTL_GATE_H
#define CTL_GATE_H
#include <string.h>
#include <sys/types.h>

/* Read-only verbs any connected uid may run. Everything else needs uid 0.
 * Match the first whitespace-delimited token exactly so e.g. "statusfoo" is
 * not treated as "status" (anchored allowlist); "status --json" still passes. */
static inline int ctl_is_readonly(const char *line) {
    size_t n = strcspn(line, " \t");
    return (n == 6 && memcmp(line, "status", 6) == 0)
        || (n == 4 && memcmp(line, "list", 4) == 0)
        || (n == 6 && memcmp(line, "timing", 6) == 0)
        || (n == 7 && memcmp(line, "analyze", 7) == 0);
}

/* Peer identity from SO_PEERCRED: root and PID 1's own euid run everything;
 * anyone else the socket let in (group schema, 0660) gets read-only verbs. */
static inline int ctl_peer_may(uid_t peer, uid_t self, const char *line) {
    return peer == 0 || peer == self || ctl_is_readonly(line);
}
#endif
