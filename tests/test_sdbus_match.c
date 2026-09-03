#include "../sdbus_match.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    sdbus_matchset *m = sdbus_match_new();

    /* a specific signal rule */
    assert(sdbus_match_add(m,
        "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'") == 0);

    /* matching signal -> 1 */
    assert(sdbus_match_signal(m, "org.freedesktop.DBus", "NameOwnerChanged",
                              "/org/freedesktop/DBus", "org.freedesktop.DBus") == 1);
    /* wrong interface -> 0 */
    assert(sdbus_match_signal(m, "org.other", "NameOwnerChanged", "/p", "s") == 0);
    /* wrong member -> 0 */
    assert(sdbus_match_signal(m, "org.freedesktop.DBus", "NameAcquired", "/p", "s") == 0);

    /* exact-string removal stops matching */
    assert(sdbus_match_remove(m,
        "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'") == 0);
    assert(sdbus_match_signal(m, "org.freedesktop.DBus", "NameOwnerChanged",
                              "/org/freedesktop/DBus", "org.freedesktop.DBus") == 0);
    /* removing a non-present rule -> -1 */
    assert(sdbus_match_remove(m, "type='signal'") == -1);

    /* a bare type='signal' rule matches any signal */
    assert(sdbus_match_add(m, "type='signal'") == 0);
    assert(sdbus_match_signal(m, "any.iface", "AnyMember", "/any", "any") == 1);

    /* path_namespace matches under the namespace */
    sdbus_matchset *m2 = sdbus_match_new();
    assert(sdbus_match_add(m2, "type='signal',path_namespace='/org/example'") == 0);
    assert(sdbus_match_signal(m2, "i", "M", "/org/example/Foo", "s") == 1);
    assert(sdbus_match_signal(m2, "i", "M", "/org/example", "s") == 1);
    assert(sdbus_match_signal(m2, "i", "M", "/org/other", "s") == 0);

    /* malformed rules -> -1 */
    sdbus_matchset *m3 = sdbus_match_new();
    assert(sdbus_match_add(m3, "type signal") == -1);          /* no '=' */
    assert(sdbus_match_add(m3, "type=signal") == -1);          /* unquoted */
    assert(sdbus_match_add(m3, "type='signal") == -1);         /* unterminated quote */

    /* a non-signal-typed rule never matches a signal */
    sdbus_matchset *m4 = sdbus_match_new();
    assert(sdbus_match_add(m4, "type='method_call',interface='i'") == 0);
    assert(sdbus_match_signal(m4, "i", "M", "/p", "s") == 0);

    sdbus_match_free(m); sdbus_match_free(m2); sdbus_match_free(m3); sdbus_match_free(m4);
    printf("all sdbus_match tests passed\n");
    return 0;
}
