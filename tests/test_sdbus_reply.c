#include "../sdbus_reply.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    sdbus_replies *t = sdbus_replies_new();

    /* record caller 1 -> callee 2 (serial 42); reply from 2 routes to 1 once */
    sdbus_replies_record(t, 2, 42, 1);
    assert(sdbus_replies_match(t, 2, 42, -1) == 1);
    assert(sdbus_replies_match(t, 2, 42, -1) == -1);      /* consumed */

    /* reply from the wrong connection does not match */
    sdbus_replies_record(t, 2, 43, 1);
    assert(sdbus_replies_match(t, 5, 43, -1) == -1);      /* wrong callee */
    assert(sdbus_replies_match(t, 2, 99, -1) == -1);      /* wrong serial */
    assert(sdbus_replies_match(t, 2, 43, -1) == 1);       /* correct */

    /* purge by caller removes pending entries where conn is the caller */
    sdbus_replies_record(t, 2, 50, 1);
    sdbus_replies_purge(t, 1);
    assert(sdbus_replies_match(t, 2, 50, -1) == -1);

    /* purge by callee removes pending entries where conn is the callee */
    sdbus_replies_record(t, 7, 60, 3);
    sdbus_replies_purge(t, 7);
    assert(sdbus_replies_match(t, 7, 60, -1) == -1);

    /* independent entries survive an unrelated purge */
    sdbus_replies_record(t, 8, 70, 4);
    sdbus_replies_purge(t, 999);
    assert(sdbus_replies_match(t, 8, 70, -1) == 4);

    sdbus_replies_free(t);

    /* F2: per-connection serials collide across callers. Two callers (10, 11)
       each send serial 1 to the same callee (2); the reply's resolved
       destination (want_caller) disambiguates so each reply reaches its own
       caller, not merely the first (callee, serial) entry found. */
    sdbus_replies *t2 = sdbus_replies_new();
    sdbus_replies_record(t2, 2, 1, 10);
    sdbus_replies_record(t2, 2, 1, 11);
    assert(sdbus_replies_match(t2, 2, 1, 11) == 11);      /* reply destined for 11 */
    assert(sdbus_replies_match(t2, 2, 1, 10) == 10);      /* the other -> 10, not 11 */
    assert(sdbus_replies_match(t2, 2, 1, 10) == -1);      /* both consumed */
    /* unknown destination (-1) still falls back to first live (callee, serial) */
    sdbus_replies_record(t2, 3, 5, 20);
    assert(sdbus_replies_match(t2, 3, 5, -1) == 20);
    sdbus_replies_free(t2);

    /* pending_on: when a callee vanishes, enumerate the callers awaiting its reply
       so each can be sent a NoReply. Skip the disconnecting conn itself (except). */
    sdbus_replies *t3 = sdbus_replies_new();
    sdbus_replies_record(t3, 9, 100, 1);   /* caller 1 awaits callee 9 */
    sdbus_replies_record(t3, 9, 101, 2);   /* caller 2 awaits callee 9 */
    sdbus_replies_record(t3, 9, 102, 9);   /* callee 9 called itself */
    sdbus_replies_record(t3, 5, 103, 3);   /* unrelated callee */
    int callers[8]; uint32_t serials[8];
    int np = sdbus_replies_pending_on(t3, 9, 9, callers, serials, 8);
    assert(np == 2);                        /* callers 1 and 2, not the self-call (except=9) */
    int got1 = 0, got2 = 0;
    for (int i = 0; i < np; i++) {
        if (callers[i] == 1 && serials[i] == 100) got1 = 1;
        if (callers[i] == 2 && serials[i] == 101) got2 = 1;
    }
    assert(got1 && got2);
    /* the enumerator must not mutate: entries still route normally afterward */
    assert(sdbus_replies_match(t3, 9, 100, 1) == 1);
    assert(sdbus_replies_match(t3, 5, 103, -1) == 3);
    sdbus_replies_free(t3);

    /* reply timeout: a pending call has a future deadline; before it, nothing is
       reaped; at/after it, the entry is reaped and its caller reported for NoReply. */
    sdbus_replies *t4 = sdbus_replies_new();
    long before = sdbus__now_ms();
    sdbus_replies_record(t4, 2, 200, 1);
    long dl = sdbus_replies_next_deadline(t4);
    assert(dl >= before + SDBUS_REPLY_TIMEOUT_MS);       /* deadline is in the future */
    int ec[4]; uint32_t es[4];
    assert(sdbus_replies_reap_expired(t4, before, ec, es, 4) == 0);   /* not yet due */
    assert(sdbus_replies_match(t4, 2, 200, -1) == 1);    /* still live, routes normally */
    /* re-record (match consumed it), then reap past the deadline */
    sdbus_replies_record(t4, 2, 201, 5);
    long past = sdbus__now_ms() + SDBUS_REPLY_TIMEOUT_MS + 1000;
    int ne = sdbus_replies_reap_expired(t4, past, ec, es, 4);
    assert(ne == 1 && ec[0] == 5 && es[0] == 201);
    assert(sdbus_replies_match(t4, 2, 201, -1) == -1);   /* reaped -> gone */
    assert(sdbus_replies_next_deadline(t4) == -1);       /* nothing pending */
    sdbus_replies_free(t4);

    printf("all sdbus_reply tests passed\n");
    return 0;
}
