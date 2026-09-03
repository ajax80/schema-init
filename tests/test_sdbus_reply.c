#include "../sdbus_reply.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    sdbus_replies *t = sdbus_replies_new();

    /* record caller 1 -> callee 2 (serial 42); reply from 2 routes to 1 once */
    sdbus_replies_record(t, 2, 42, 1);
    assert(sdbus_replies_match(t, 2, 42) == 1);
    assert(sdbus_replies_match(t, 2, 42) == -1);      /* consumed */

    /* reply from the wrong connection does not match */
    sdbus_replies_record(t, 2, 43, 1);
    assert(sdbus_replies_match(t, 5, 43) == -1);      /* wrong callee */
    assert(sdbus_replies_match(t, 2, 99) == -1);      /* wrong serial */
    assert(sdbus_replies_match(t, 2, 43) == 1);       /* correct */

    /* purge by caller removes pending entries where conn is the caller */
    sdbus_replies_record(t, 2, 50, 1);
    sdbus_replies_purge(t, 1);
    assert(sdbus_replies_match(t, 2, 50) == -1);

    /* purge by callee removes pending entries where conn is the callee */
    sdbus_replies_record(t, 7, 60, 3);
    sdbus_replies_purge(t, 7);
    assert(sdbus_replies_match(t, 7, 60) == -1);

    /* independent entries survive an unrelated purge */
    sdbus_replies_record(t, 8, 70, 4);
    sdbus_replies_purge(t, 999);
    assert(sdbus_replies_match(t, 8, 70) == 4);

    sdbus_replies_free(t);
    printf("all sdbus_reply tests passed\n");
    return 0;
}
