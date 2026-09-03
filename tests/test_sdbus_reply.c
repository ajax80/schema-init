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

    printf("all sdbus_reply tests passed\n");
    return 0;
}
