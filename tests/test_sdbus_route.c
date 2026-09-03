#include "../sdbus_route.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* take a DBusMessage into an sdbus_msg via a marshal round-trip */
static sdbus_msg take(DBusMessage *m, dbus_uint32_t serial) {
    dbus_message_set_serial(m, serial);
    char *raw = NULL; int len = 0;
    assert(dbus_message_marshal(m, &raw, &len));
    sdbus_msg out;
    assert(sdbus_codec_take((unsigned char *)raw, len, &out) == len);
    dbus_free(raw);
    return out;
}

int main(void) {
    sdbus_names *names = sdbus_names_new();
    sdbus_replies *replies = sdbus_replies_new();

    /* three connections; conn 2 owns org.svc */
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
    sdbus_msg cm = take(call, 5);
    n = sdbus_route_targets(&cm, &c1, names, all, 3, allow, replies, &synth, &denied, tg, 8);
    assert(n == 1 && tg[0] == 2 && !synth && !denied);

    /* 2. the reply from conn 2 routes back to conn 1, bypassing policy */
    DBusMessage *ret = dbus_message_new_method_return(call);
    sdbus_msg rm = take(ret, 6);
    assert(rm.has_reply_serial && rm.reply_serial == 5);
    n = sdbus_route_targets(&rm, &c2, names, all, 3, allow, replies, &synth, &denied, tg, 8);
    assert(n == 1 && tg[0] == 1);
    /* consumed: a second identical reply drops */
    sdbus_msg rm2 = take(ret, 7);
    n = sdbus_route_targets(&rm2, &c2, names, all, 3, allow, replies, &synth, &denied, tg, 8);
    assert(n == 0);

    /* 3. method_call to an unowned name -> synth ServiceUnknown */
    DBusMessage *call2 = dbus_message_new_method_call("org.nope", "/p", "org.if", "M");
    sdbus_msg cm2 = take(call2, 8);
    n = sdbus_route_targets(&cm2, &c1, names, all, 3, allow, replies, &synth, &denied, tg, 8);
    assert(n == 0 && synth == 1 && !denied);

    /* 4. broadcast signal -> only conns whose match set accepts it (conn 3) */
    DBusMessage *sig = dbus_message_new_signal("/p", "org.sig", "Changed");
    sdbus_msg sm = take(sig, 9);
    n = sdbus_route_targets(&sm, &c1, names, all, 3, allow, replies, &synth, &denied, tg, 8);
    assert(n == 1 && tg[0] == 3);

    /* 5. directed signal -> unicast to the destination owner */
    DBusMessage *dsig = dbus_message_new_signal("/p", "org.sig", "Changed");
    dbus_message_set_destination(dsig, "org.svc");
    sdbus_msg dsm = take(dsig, 10);
    n = sdbus_route_targets(&dsm, &c1, names, all, 3, allow, replies, &synth, &denied, tg, 8);
    assert(n == 1 && tg[0] == 2);

    /* 6. policy denial sets *denied */
    sdbus_policy *narrow = sdbus_policy_parse(
        "context = default\nallow = send_destination:org.allowed\n");
    DBusMessage *call3 = dbus_message_new_method_call("org.svc", "/p", "org.if", "M");
    sdbus_msg cm3 = take(call3, 11);
    n = sdbus_route_targets(&cm3, &c1, names, all, 3, narrow, replies, &synth, &denied, tg, 8);
    assert(n == 0 && denied == 1 && !synth);

    sdbus_msg_free(&cm); sdbus_msg_free(&rm); sdbus_msg_free(&rm2);
    sdbus_msg_free(&cm2); sdbus_msg_free(&sm); sdbus_msg_free(&dsm); sdbus_msg_free(&cm3);
    dbus_message_unref(call); dbus_message_unref(ret); dbus_message_unref(call2);
    dbus_message_unref(sig); dbus_message_unref(dsig); dbus_message_unref(call3);
    sdbus_policy_free(allow); sdbus_policy_free(narrow);
    sdbus_match_free(c3.matches);
    sdbus_names_free(names); sdbus_replies_free(replies);
    printf("all sdbus_route tests passed\n");
    return 0;
}
