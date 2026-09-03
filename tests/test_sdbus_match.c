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
                              "/org/freedesktop/DBus", "org.freedesktop.DBus", NULL, 0) == 1);
    /* wrong interface -> 0 */
    assert(sdbus_match_signal(m, "org.other", "NameOwnerChanged", "/p", "s", NULL, 0) == 0);
    /* wrong member -> 0 */
    assert(sdbus_match_signal(m, "org.freedesktop.DBus", "NameAcquired", "/p", "s", NULL, 0) == 0);

    /* exact-string removal stops matching */
    assert(sdbus_match_remove(m,
        "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'") == 0);
    assert(sdbus_match_signal(m, "org.freedesktop.DBus", "NameOwnerChanged",
                              "/org/freedesktop/DBus", "org.freedesktop.DBus", NULL, 0) == 0);
    /* removing a non-present rule -> -1 */
    assert(sdbus_match_remove(m, "type='signal'") == -1);

    /* a bare type='signal' rule matches any signal */
    assert(sdbus_match_add(m, "type='signal'") == 0);
    assert(sdbus_match_signal(m, "any.iface", "AnyMember", "/any", "any", NULL, 0) == 1);

    /* path_namespace matches under the namespace */
    sdbus_matchset *m2 = sdbus_match_new();
    assert(sdbus_match_add(m2, "type='signal',path_namespace='/org/example'") == 0);
    assert(sdbus_match_signal(m2, "i", "M", "/org/example/Foo", "s", NULL, 0) == 1);
    assert(sdbus_match_signal(m2, "i", "M", "/org/example", "s", NULL, 0) == 1);
    assert(sdbus_match_signal(m2, "i", "M", "/org/other", "s", NULL, 0) == 0);

    /* malformed rules -> -1 */
    sdbus_matchset *m3 = sdbus_match_new();
    assert(sdbus_match_add(m3, "type signal") == -1);          /* no '=' */
    assert(sdbus_match_add(m3, "type=signal") == -1);          /* unquoted */
    assert(sdbus_match_add(m3, "type='signal") == -1);         /* unterminated quote */

    /* a non-signal-typed rule never matches a signal */
    sdbus_matchset *m4 = sdbus_match_new();
    assert(sdbus_match_add(m4, "type='method_call',interface='i'") == 0);
    assert(sdbus_match_signal(m4, "i", "M", "/p", "s", NULL, 0) == 0);

    /* sender= resolution: a rule naming the emitter by a WELL-KNOWN name must
       match a signal whose wire sender is the emitter's UNIQUE name, so long as
       that connection owns the well-known name. This is the automount bug: gvfs
       and Solid subscribe with sender='org.freedesktop.UDisks2', but udisksd
       emits InterfacesAdded as ":1.6". */
    sdbus_matchset *m5 = sdbus_match_new();
    assert(sdbus_match_add(m5, "type='signal',interface='org.freedesktop.DBus.ObjectManager',"
                               "member='InterfacesAdded',sender='org.freedesktop.UDisks2'") == 0);
    const char *owned[] = { "org.freedesktop.UDisks2" };
    /* emitter :1.6 owns the well-known name -> match */
    assert(sdbus_match_signal(m5, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                              "/org/freedesktop/UDisks2", ":1.6", owned, 1) == 1);
    /* same unique emitter but NOT owning that name -> no match */
    assert(sdbus_match_signal(m5, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                              "/org/freedesktop/UDisks2", ":1.6", NULL, 0) == 0);

    /* a rule naming the emitter by its unique name matches that unique sender,
       and rejects a different emitter that merely owns some other name */
    sdbus_matchset *m6 = sdbus_match_new();
    assert(sdbus_match_add(m6, "type='signal',sender=':1.6'") == 0);
    assert(sdbus_match_signal(m6, "i", "M", "/p", ":1.6", NULL, 0) == 1);
    assert(sdbus_match_signal(m6, "i", "M", "/p", ":1.7", owned, 1) == 0);

    sdbus_match_free(m); sdbus_match_free(m2); sdbus_match_free(m3); sdbus_match_free(m4);
    sdbus_match_free(m5); sdbus_match_free(m6);
    printf("all sdbus_match tests passed\n");
    return 0;
}
