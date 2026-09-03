#ifndef SDBUS_REPLY_H
#define SDBUS_REPLY_H

/* Pending-reply table: records that a caller sent a method_call (serial) to a
   callee awaiting a reply. A method_return/error from that callee carrying the
   reply_serial routes back to the caller (and consumes the entry). This both
   routes replies and is the fact the requested-reply policy exemption relies on.
   Unmatched replies are dropped by the caller (spec: prevents reply spoofing). */

#include <stdint.h>
#include <stdlib.h>

typedef struct { int callee, caller; uint32_t serial; int live; } sdbus_reply_ent;
struct sdbus_replies { sdbus_reply_ent *ents; int n; };
typedef struct sdbus_replies sdbus_replies;

static inline sdbus_replies *sdbus_replies_new(void) { return calloc(1, sizeof(sdbus_replies)); }

static inline void sdbus_replies_record(sdbus_replies *t, int callee, uint32_t serial, int caller) {
    for (int i = 0; i < t->n; i++)             /* reuse a dead slot */
        if (!t->ents[i].live) {
            t->ents[i].callee = callee; t->ents[i].serial = serial;
            t->ents[i].caller = caller; t->ents[i].live = 1;
            return;
        }
    t->ents = realloc(t->ents, (t->n + 1) * sizeof *t->ents);
    t->ents[t->n].callee = callee; t->ents[t->n].serial = serial;
    t->ents[t->n].caller = caller; t->ents[t->n].live = 1;
    t->n++;
}

/* Returns the caller conn to deliver the reply to, consuming the entry; -1 if no
   matching pending call (the reply should be dropped). */
static inline int sdbus_replies_match(sdbus_replies *t, int from, uint32_t reply_serial) {
    for (int i = 0; i < t->n; i++)
        if (t->ents[i].live && t->ents[i].callee == from && t->ents[i].serial == reply_serial) {
            t->ents[i].live = 0;
            return t->ents[i].caller;
        }
    return -1;
}

/* Purge every entry where conn is caller or callee (on disconnect). */
static inline void sdbus_replies_purge(sdbus_replies *t, int conn) {
    for (int i = 0; i < t->n; i++)
        if (t->ents[i].live && (t->ents[i].caller == conn || t->ents[i].callee == conn))
            t->ents[i].live = 0;
}

static inline void sdbus_replies_free(sdbus_replies *t) {
    if (!t) return;
    free(t->ents); free(t);
}

#endif
