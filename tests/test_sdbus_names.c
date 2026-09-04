#include "../sdbus_names.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    sdbus_names *r = sdbus_names_new();
    sdbus_transition t[8];
    int n;

    /* unique names are monotonic ":1.N" */
    const char *u1 = sdbus_names_alloc_unique(r, 101);
    const char *u2 = sdbus_names_alloc_unique(r, 102);
    assert(strcmp(u1, ":1.1") == 0);
    assert(strcmp(u2, ":1.2") == 0);
    assert(strcmp(sdbus_names_unique(r, 101), ":1.1") == 0);

    /* first requester becomes primary owner, transition none->101 */
    assert(sdbus_names_request(r, 101, "org.x", 0, t, &n) == SDBUS_REQ_PRIMARY_OWNER);
    assert(n == 1 && t[0].old_owner == -1 && t[0].new_owner == 101 && !strcmp(t[0].name, "org.x"));
    assert(sdbus_names_owner(r, "org.x") == 101);

    /* re-request by current owner -> ALREADY_OWNER, no transition */
    assert(sdbus_names_request(r, 101, "org.x", 0, t, &n) == SDBUS_REQ_ALREADY_OWNER);
    assert(n == 0);

    /* second requester DO_NOT_QUEUE -> EXISTS, no transition */
    assert(sdbus_names_request(r, 102, "org.x", SDBUS_REQ_DO_NOT_QUEUE, t, &n) == SDBUS_REQ_EXISTS);
    assert(n == 0);
    assert(sdbus_names_owner(r, "org.x") == 101);

    /* second requester without DO_NOT_QUEUE -> IN_QUEUE */
    assert(sdbus_names_request(r, 102, "org.x", 0, t, &n) == SDBUS_REQ_IN_QUEUE);
    assert(n == 0);

    /* re-request by a waiter -> IN_QUEUE */
    assert(sdbus_names_request(r, 102, "org.x", 0, t, &n) == SDBUS_REQ_IN_QUEUE);

    /* owner releases -> next in queue promoted, transition 101->102 */
    assert(sdbus_names_release(r, 101, "org.x", t, &n) == SDBUS_REL_RELEASED);
    assert(n == 1 && t[0].old_owner == 101 && t[0].new_owner == 102);
    assert(sdbus_names_owner(r, "org.x") == 102);

    /* last owner releases -> name unowned, transition 102->none */
    assert(sdbus_names_release(r, 102, "org.x", t, &n) == SDBUS_REL_RELEASED);
    assert(n == 1 && t[0].old_owner == 102 && t[0].new_owner == -1);
    assert(sdbus_names_owner(r, "org.x") == -1);

    /* release of an unowned name -> NON_EXISTENT; release by non-owner -> NOT_OWNER */
    assert(sdbus_names_release(r, 101, "org.x", t, &n) == SDBUS_REL_NON_EXISTENT);
    sdbus_names_request(r, 101, "org.y", 0, t, &n);
    assert(sdbus_names_release(r, 999, "org.y", t, &n) == SDBUS_REL_NOT_OWNER);

    /* REPLACE_EXISTING on an ALLOW_REPLACEMENT owner takes the name; old owner
       queued behind (regains it on the new owner's release) */
    sdbus_names *r2 = sdbus_names_new();
    assert(sdbus_names_request(r2, 1, "org.z", SDBUS_REQ_ALLOW_REPLACEMENT, t, &n) == SDBUS_REQ_PRIMARY_OWNER);
    assert(sdbus_names_request(r2, 2, "org.z", SDBUS_REQ_REPLACE_EXISTING, t, &n) == SDBUS_REQ_PRIMARY_OWNER);
    assert(n == 1 && t[0].old_owner == 1 && t[0].new_owner == 2);
    assert(sdbus_names_owner(r2, "org.z") == 2);
    assert(sdbus_names_release(r2, 2, "org.z", t, &n) == SDBUS_REL_RELEASED);
    assert(t[0].new_owner == 1);                    /* old owner regained it */

    /* REPLACE_EXISTING WITHOUT allow-replacement on the owner is refused */
    sdbus_names *r3 = sdbus_names_new();
    sdbus_names_request(r3, 1, "org.w", 0, t, &n);  /* no ALLOW_REPLACEMENT */
    assert(sdbus_names_request(r3, 2, "org.w", SDBUS_REQ_REPLACE_EXISTING, t, &n) == SDBUS_REQ_IN_QUEUE);
    assert(sdbus_names_owner(r3, "org.w") == 1);

    /* disconnect of the primary owner promotes the queue */
    sdbus_names *r4 = sdbus_names_new();
    sdbus_names_request(r4, 1, "org.a", 0, t, &n);
    sdbus_names_request(r4, 2, "org.a", 0, t, &n);
    sdbus_names_request(r4, 1, "org.b", 0, t, &n);
    sdbus_names_disconnect(r4, 1, t, &n, 8);
    assert(n == 2);                                 /* org.a: 1->2, org.b: 1->none */
    assert(sdbus_names_owner(r4, "org.a") == 2);
    assert(sdbus_names_owner(r4, "org.b") == -1);

    /* list returns only owned names */
    const char **owned; int c = sdbus_names_list(r4, &owned);
    assert(c == 1 && !strcmp(owned[0], "org.a"));
    free(owned);

    /* F1: a conn owning more names than the caller's transition buffer must
       cap the emitted transitions, never overflow it — and still drop every
       holder. (RequestName has no per-conn ownership cap.) */
    sdbus_names *r5 = sdbus_names_new();
    for (int i = 0; i < 20; i++) {
        char nm[24]; sprintf(nm, "org.f1.n%d", i);
        sdbus_names_request(r5, 1, nm, 0, t, &n);
    }
    sdbus_transition small[4]; int sn = -1;
    sdbus_names_disconnect(r5, 1, small, &sn, 4);
    assert(sn >= 0 && sn <= 4);                     /* capped to buffer, no overflow */
    assert(sdbus_names_owner(r5, "org.f1.n0") == -1);   /* every holder removed regardless */
    assert(sdbus_names_owner(r5, "org.f1.n19") == -1);
    const char **o5; assert(sdbus_names_list(r5, &o5) == 0); free(o5);

    /* count_held / holds: the per-conn name-cap primitives. count_held counts one
       per name held (primary or queued); holds is position-agnostic. */
    sdbus_names *r6 = sdbus_names_new();
    sdbus_names_request(r6, 1, "org.h.a", 0, t, &n);   /* conn 1 primary */
    sdbus_names_request(r6, 1, "org.h.b", 0, t, &n);   /* conn 1 primary */
    sdbus_names_request(r6, 2, "org.h.a", 0, t, &n);   /* conn 2 queued behind 1 */
    assert(sdbus_names_count_held(r6, 1) == 2);
    assert(sdbus_names_count_held(r6, 2) == 1);        /* queued still counts */
    assert(sdbus_names_count_held(r6, 3) == 0);
    assert(sdbus_names_holds(r6, 1, "org.h.a") == 1);
    assert(sdbus_names_holds(r6, 2, "org.h.a") == 1);  /* queued -> holds */
    assert(sdbus_names_holds(r6, 2, "org.h.b") == 0);
    assert(sdbus_names_holds(r6, 3, "org.never") == 0);

    sdbus_names_free(r); sdbus_names_free(r2); sdbus_names_free(r3); sdbus_names_free(r4);
    sdbus_names_free(r5); sdbus_names_free(r6);
    printf("all sdbus_names tests passed\n");
    return 0;
}
