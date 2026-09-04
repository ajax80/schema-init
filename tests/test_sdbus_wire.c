#include "../sdbus_wire.h"
#include <dbus/dbus.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* build a real method_call, marshal it, parse the header manually */
    DBusMessage *call = dbus_message_new_method_call("org.example.Svc",
        "/org/example/Obj", "org.example.If", "DoThing");
    dbus_message_set_serial(call, 42);
    const char *arg = "payload";
    dbus_message_append_args(call, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
    char *raw = NULL; int rawlen = 0;
    assert(dbus_message_marshal(call, &raw, &rawlen));

    sdbus_wire_msg m;
    /* incomplete buffer -> need more */
    assert(sdbus_wire_parse((unsigned char *)raw, 15, &m) == 0);
    assert(sdbus_wire_parse((unsigned char *)raw, rawlen - 3, &m) == 0);

    int total = sdbus_wire_parse((unsigned char *)raw, rawlen, &m);
    assert(total == rawlen);
    assert(m.type == DBUS_MESSAGE_TYPE_METHOD_CALL);
    assert(m.serial == 42);
    assert(m.destination && !strcmp(m.destination, "org.example.Svc"));
    assert(m.path && !strcmp(m.path, "/org/example/Obj"));
    assert(m.interface && !strcmp(m.interface, "org.example.If"));
    assert(m.member && !strcmp(m.member, "DoThing"));
    assert(m.signature && !strcmp(m.signature, "s"));
    assert(m.arg0 && !strcmp(m.arg0, "payload"));    /* first string arg extracted */
    assert(m.has_reply_serial == 0);
    assert(sdbus_wire_n_fds(&m) == 0);

    /* a real PropertiesChanged signal: arg0 is the interface name the change is
       for, which is what property-watch match rules filter on */
    DBusMessage *pc = dbus_message_new_signal("/org/o", "org.freedesktop.DBus.Properties",
                                              "PropertiesChanged");
    dbus_message_set_serial(pc, 7);
    const char *iface = "org.foo.Bar";
    dbus_message_append_args(pc, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
    char *praw = NULL; int prawlen = 0;
    assert(dbus_message_marshal(pc, &praw, &prawlen));
    sdbus_wire_msg mp;
    assert(sdbus_wire_parse((unsigned char *)praw, prawlen, &mp) == prawlen);
    assert(mp.type == DBUS_MESSAGE_TYPE_SIGNAL);
    assert(mp.arg0 && !strcmp(mp.arg0, "org.foo.Bar"));
    dbus_message_unref(pc); dbus_free(praw);

    /* a signal whose first arg is NOT a string -> arg0 stays NULL */
    DBusMessage *us = dbus_message_new_signal("/org/o", "org.example.If", "Tick");
    dbus_message_set_serial(us, 8);
    dbus_uint32_t n = 99;
    dbus_message_append_args(us, DBUS_TYPE_UINT32, &n, DBUS_TYPE_INVALID);
    char *uraw = NULL; int urawlen = 0;
    assert(dbus_message_marshal(us, &uraw, &urawlen));
    sdbus_wire_msg mu;
    assert(sdbus_wire_parse((unsigned char *)uraw, urawlen, &mu) == urawlen);
    assert(mu.arg0 == NULL);
    dbus_message_unref(us); dbus_free(uraw);

    /* a signal with no body at all -> arg0 NULL, no over-read */
    DBusMessage *eb = dbus_message_new_signal("/org/o", "org.example.If", "Empty");
    dbus_message_set_serial(eb, 9);
    char *eraw = NULL; int erawlen = 0;
    assert(dbus_message_marshal(eb, &eraw, &erawlen));
    sdbus_wire_msg me;
    assert(sdbus_wire_parse((unsigned char *)eraw, erawlen, &me) == erawlen);
    assert(me.arg0 == NULL);
    dbus_message_unref(eb); dbus_free(eraw);

    /* reforward with a stamped sender; libdbus must demarshal the result and see
       the new sender plus every original field and the body intact */
    unsigned char *fwd = NULL; int fwdlen = 0;
    assert(sdbus_wire_reforward((unsigned char *)raw, &m, ":1.99", &fwd, &fwdlen) == 0);
    DBusError e; dbus_error_init(&e);
    DBusMessage *rt = dbus_message_demarshal((char *)fwd, fwdlen, &e);
    assert(rt != NULL);
    assert(!strcmp(dbus_message_get_sender(rt), ":1.99"));
    assert(!strcmp(dbus_message_get_destination(rt), "org.example.Svc"));
    assert(!strcmp(dbus_message_get_path(rt), "/org/example/Obj"));
    assert(!strcmp(dbus_message_get_interface(rt), "org.example.If"));
    assert(!strcmp(dbus_message_get_member(rt), "DoThing"));
    assert(dbus_message_get_serial(rt) == 42);
    const char *out = NULL;
    assert(dbus_message_get_args(rt, &e, DBUS_TYPE_STRING, &out, DBUS_TYPE_INVALID));
    assert(!strcmp(out, "payload"));                 /* body forwarded verbatim */

    /* a reply already carrying a sender: reforward replaces it */
    DBusMessage *reply = dbus_message_new_method_return(call);
    dbus_message_set_serial(reply, 43);
    dbus_message_set_sender(reply, ":1.5");
    char *rraw = NULL; int rrawlen = 0;
    assert(dbus_message_marshal(reply, &rraw, &rrawlen));
    sdbus_wire_msg mr;
    assert(sdbus_wire_parse((unsigned char *)rraw, rrawlen, &mr) == rrawlen);
    assert(mr.type == DBUS_MESSAGE_TYPE_METHOD_RETURN);
    assert(mr.has_reply_serial == 1 && mr.reply_serial == 42);
    assert(mr.sender && !strcmp(mr.sender, ":1.5"));
    unsigned char *fwd2 = NULL; int fwd2len = 0;
    assert(sdbus_wire_reforward((unsigned char *)rraw, &mr, ":1.7", &fwd2, &fwd2len) == 0);
    DBusMessage *rt2 = dbus_message_demarshal((char *)fwd2, fwd2len, &e);
    assert(rt2 && !strcmp(dbus_message_get_sender(rt2), ":1.7"));
    assert(dbus_message_get_reply_serial(rt2) == 42);

    /* --- adversarial input: the parser must reject, never over-read --- */
    /* oversized header-fields array length (offset 12) */
    unsigned char *bad = malloc(rawlen);
    memcpy(bad, raw, rawlen);
    bad[12] = bad[13] = bad[14] = bad[15] = 0xFF;
    sdbus_wire_msg mb;
    assert(sdbus_wire_parse(bad, rawlen, &mb) == -1);
    /* oversized body length (offset 4) */
    memcpy(bad, raw, rawlen);
    bad[4] = bad[5] = bad[6] = bad[7] = 0xFF;
    assert(sdbus_wire_parse(bad, rawlen, &mb) == -1);
    free(bad);

    /* hand-crafted header claiming a 4GB string field -> reject, no over-read */
    unsigned char h[24] = {0};
    h[0] = 'l'; h[1] = 1; h[3] = 1;                   /* LE, method_call, ver 1 */
    /* body_len=0 @4, serial=1 @8 */ h[8] = 1;
    h[12] = 8;                                        /* farr = 8 (fields region 16..24) */
    h[16] = 1; h[17] = 1; h[18] = 'o'; h[19] = 0;     /* field: PATH, variant 'o' */
    h[20] = h[21] = h[22] = h[23] = 0xFF;             /* string length = 0xFFFFFFFF */
    sdbus_wire_msg mh;
    assert(sdbus_wire_parse(h, sizeof h, &mh) == -1);

    /* single-byte mutation sweep: parse must never report more than we passed */
    for (int p = 0; p < rawlen; p++) {
        unsigned char *mut = malloc(rawlen);
        memcpy(mut, raw, rawlen);
        mut[p] ^= 0xFF;
        sdbus_wire_msg mm;
        int r = sdbus_wire_parse(mut, rawlen, &mm);
        assert(r <= rawlen);                          /* never claims beyond the buffer */
        free(mut);
    }

    free(fwd); free(fwd2);
    dbus_message_unref(call); dbus_message_unref(reply);
    dbus_message_unref(rt); dbus_message_unref(rt2);
    dbus_free(raw); dbus_free(rraw);
    printf("all sdbus_wire tests passed\n");
    return 0;
}
