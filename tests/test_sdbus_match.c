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
                              "/org/freedesktop/DBus", "org.freedesktop.DBus", NULL, 0, NULL) == 1);
    /* wrong interface -> 0 */
    assert(sdbus_match_signal(m, "org.other", "NameOwnerChanged", "/p", "s", NULL, 0, NULL) == 0);
    /* wrong member -> 0 */
    assert(sdbus_match_signal(m, "org.freedesktop.DBus", "NameAcquired", "/p", "s", NULL, 0, NULL) == 0);

    /* exact-string removal stops matching */
    assert(sdbus_match_remove(m,
        "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'") == 0);
    assert(sdbus_match_signal(m, "org.freedesktop.DBus", "NameOwnerChanged",
                              "/org/freedesktop/DBus", "org.freedesktop.DBus", NULL, 0, NULL) == 0);
    /* removing a non-present rule -> -1 */
    assert(sdbus_match_remove(m, "type='signal'") == -1);

    /* a bare type='signal' rule matches any signal */
    assert(sdbus_match_add(m, "type='signal'") == 0);
    assert(sdbus_match_signal(m, "any.iface", "AnyMember", "/any", "any", NULL, 0, NULL) == 1);

    /* path_namespace matches under the namespace */
    sdbus_matchset *m2 = sdbus_match_new();
    assert(sdbus_match_add(m2, "type='signal',path_namespace='/org/example'") == 0);
    assert(sdbus_match_signal(m2, "i", "M", "/org/example/Foo", "s", NULL, 0, NULL) == 1);
    assert(sdbus_match_signal(m2, "i", "M", "/org/example", "s", NULL, 0, NULL) == 1);
    assert(sdbus_match_signal(m2, "i", "M", "/org/other", "s", NULL, 0, NULL) == 0);

    /* malformed rules -> -1 */
    sdbus_matchset *m3 = sdbus_match_new();
    assert(sdbus_match_add(m3, "type signal") == -1);          /* no '=' */
    assert(sdbus_match_add(m3, "type=signal") == -1);          /* unquoted */
    assert(sdbus_match_add(m3, "type='signal") == -1);         /* unterminated quote */

    /* a non-signal-typed rule never matches a signal */
    sdbus_matchset *m4 = sdbus_match_new();
    assert(sdbus_match_add(m4, "type='method_call',interface='i'") == 0);
    assert(sdbus_match_signal(m4, "i", "M", "/p", "s", NULL, 0, NULL) == 0);

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
                              "/org/freedesktop/UDisks2", ":1.6", owned, 1, NULL) == 1);
    /* same unique emitter but NOT owning that name -> no match */
    assert(sdbus_match_signal(m5, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                              "/org/freedesktop/UDisks2", ":1.6", NULL, 0, NULL) == 0);

    /* a rule naming the emitter by its unique name matches that unique sender,
       and rejects a different emitter that merely owns some other name */
    sdbus_matchset *m6 = sdbus_match_new();
    assert(sdbus_match_add(m6, "type='signal',sender=':1.6'") == 0);
    assert(sdbus_match_signal(m6, "i", "M", "/p", ":1.6", NULL, 0, NULL) == 1);
    assert(sdbus_match_signal(m6, "i", "M", "/p", ":1.7", owned, 1, NULL) == 0);

    /* arg0 exact: PropertiesChanged carries the interface name as arg0. A proxy
       watching one interface must not receive the other 25k interfaces' changes. */
    sdbus_matchset *m7 = sdbus_match_new();
    assert(sdbus_match_add(m7, "type='signal',interface='org.freedesktop.DBus.Properties',"
                               "member='PropertiesChanged',arg0='org.foo.Bar'") == 0);
    assert(sdbus_match_signal(m7, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                              "/p", ":1.9", NULL, 0, "org.foo.Bar") == 1);   /* right iface */
    assert(sdbus_match_signal(m7, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                              "/p", ":1.9", NULL, 0, "org.other.Iface") == 0); /* wrong iface */
    assert(sdbus_match_signal(m7, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                              "/p", ":1.9", NULL, 0, NULL) == 0);             /* no string arg0 */

    /* arg0namespace: NameOwnerChanged watch over a name subtree (g_bus_watch_name
       family). Matches the name itself and any dotted child, not a prefix collision. */
    sdbus_matchset *m8 = sdbus_match_new();
    assert(sdbus_match_add(m8, "type='signal',member='NameOwnerChanged',"
                               "arg0namespace='org.foo'") == 0);
    assert(sdbus_match_signal(m8, "i", "NameOwnerChanged", "/p", "s", NULL, 0, "org.foo") == 1);
    assert(sdbus_match_signal(m8, "i", "NameOwnerChanged", "/p", "s", NULL, 0, "org.foo.Bar") == 1);
    assert(sdbus_match_signal(m8, "i", "NameOwnerChanged", "/p", "s", NULL, 0, "org.foobar") == 0);
    assert(sdbus_match_signal(m8, "i", "NameOwnerChanged", "/p", "s", NULL, 0, "org.other") == 0);
    assert(sdbus_match_signal(m8, "i", "NameOwnerChanged", "/p", "s", NULL, 0, NULL) == 0);

    /* a rule without any arg constraint ignores arg0 entirely (over-deliver-safe) */
    sdbus_matchset *m9 = sdbus_match_new();
    assert(sdbus_match_add(m9, "type='signal',member='PropertiesChanged'") == 0);
    assert(sdbus_match_signal(m9, "i", "PropertiesChanged", "/p", "s", NULL, 0, "anything") == 1);
    assert(sdbus_match_signal(m9, "i", "PropertiesChanged", "/p", "s", NULL, 0, NULL) == 1);

    sdbus_match_free(m); sdbus_match_free(m2); sdbus_match_free(m3); sdbus_match_free(m4);
    sdbus_match_free(m5); sdbus_match_free(m6); sdbus_match_free(m7); sdbus_match_free(m8);
    sdbus_match_free(m9);
    printf("all sdbus_match tests passed\n");
    return 0;
}
