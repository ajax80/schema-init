#include "../sdbus_route.h"
#include <dbus/dbus.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* marshal a DBusMessage and parse its header; the buffer is kept alive (wire
   strings point into it) and freed at the end. */
static unsigned char *g_bufs[64]; static int g_nbufs;
static sdbus_wire_msg wtake(DBusMessage *m, dbus_uint32_t serial) {
    dbus_message_set_serial(m, serial);
    char *raw = NULL; int len = 0;
    assert(dbus_message_marshal(m, &raw, &len));
    unsigned char *keep = malloc(len);
    memcpy(keep, raw, len); dbus_free(raw);
    g_bufs[g_nbufs++] = keep;
    sdbus_wire_msg wm;
    assert(sdbus_wire_parse(keep, len, &wm) == len);
    return wm;
}

int main(void) {
    sdbus_names *names = sdbus_names_new();
    sdbus_replies *replies = sdbus_replies_new();

    sdbus_conn c1 = {0}, c2 = {0}, c3 = {0};
    c1.id = 1; c1.uid = 1000; c1.gids[0] = 1000; c1.n_gids = 1;
    c2.id = 2; c2.uid = 1000; c2.gids[0] = 1000; c2.n_gids = 1;
    c3.id = 3; c3.uid = 1000; c3.gids[0] = 1000; c3.n_gids = 1;
    c1.unique = sdbus_names_alloc_unique(names, 1);
    c2.unique = sdbus_names_alloc_unique(names, 2);
    c3.unique = sdbus_names_alloc_unique(names, 3);
    sdbus_transition t[1]; int nt;
    sdbus_names_request(names, 2, "org.svc", 0, t, &nt);
    c3.matches = sdbus_match_new();
    sdbus_match_add(c3.matches, "type='signal',interface='org.sig'");

    sdbus_conn *all[] = { &c1, &c2, &c3 };
    int tg[8], synth, denied, n;

    sdbus_policy *allow = sdbus_policy_parse(
        "context = default\nallow = send_destination:*\nallow = send_type:signal\n");

    /* 1. method_call to org.svc -> unicast to owner conn 2, records pending reply */
    DBusMessage *call = dbus_message_new_method_call("org.svc", "/p", "org.if", "M");
    sdbus_wire_msg cm = wtake(call, 5);
    n = sdbus_route_targets(&cm, &c1, names, all, 3, allow, replies, &synth, &denied, tg, 8);
    assert(n == 1 && tg[0] == 2 && !synth && !denied);

    /* 2. the reply from conn 2 routes back to conn 1, bypassing policy */
    DBusMessage *ret = dbus_message_new_method_return(call);
    sdbus_wire_msg rm = wtake(ret, 6);
    assert(rm.has_reply_serial && rm.reply_serial == 5);
    n = sdbus_route_targets(&rm, &c2, names, all, 3, allow, replies, &synth, &denied, tg, 8);
    assert(n == 1 && tg[0] == 1);
    sdbus_wire_msg rm2 = wtake(ret, 7);
    n = sdbus_route_targets(&rm2, &c2, names, all, 3, allow, replies, &synth, &denied, tg, 8);
    assert(n == 0);                                   /* consumed */

    /* 3. method_call to an unowned name -> synth ServiceUnknown */
    DBusMessage *call2 = dbus_message_new_method_call("org.nope", "/p", "org.if", "M");
    sdbus_wire_msg cm2 = wtake(call2, 8);
    n = sdbus_route_targets(&cm2, &c1, names, all, 3, allow, replies, &synth, &denied, tg, 8);
    assert(n == 0 && synth == 1 && !denied);

    /* 4. broadcast signal -> only conns whose match set accepts it (conn 3) */
    DBusMessage *sig = dbus_message_new_signal("/p", "org.sig", "Changed");
    sdbus_wire_msg sm = wtake(sig, 9);
    n = sdbus_route_targets(&sm, &c1, names, all, 3, allow, replies, &synth, &denied, tg, 8);
    assert(n == 1 && tg[0] == 3);

    /* 5. directed signal -> unicast to the destination owner */
    DBusMessage *dsig = dbus_message_new_signal("/p", "org.sig", "Changed");
    dbus_message_set_destination(dsig, "org.svc");
    sdbus_wire_msg dsm = wtake(dsig, 10);
    n = sdbus_route_targets(&dsm, &c1, names, all, 3, allow, replies, &synth, &denied, tg, 8);
    assert(n == 1 && tg[0] == 2);

    /* 6. policy denial sets *denied */
    sdbus_policy *narrow = sdbus_policy_parse(
        "context = default\nallow = send_destination:org.allowed\n");
    DBusMessage *call3 = dbus_message_new_method_call("org.svc", "/p", "org.if", "M");
    sdbus_wire_msg cm3 = wtake(call3, 11);
    n = sdbus_route_targets(&cm3, &c1, names, all, 3, narrow, replies, &synth, &denied, tg, 8);
    assert(n == 0 && denied == 1 && !synth);

    dbus_message_unref(call); dbus_message_unref(ret); dbus_message_unref(call2);
    dbus_message_unref(sig); dbus_message_unref(dsig); dbus_message_unref(call3);
    sdbus_policy_free(allow); sdbus_policy_free(narrow);
    sdbus_match_free(c3.matches);
    sdbus_names_free(names); sdbus_replies_free(replies);
    for (int i = 0; i < g_nbufs; i++) free(g_bufs[i]);
    printf("all sdbus_route tests passed\n");
    return 0;
}
