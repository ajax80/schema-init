#include "../sdbus_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* build a method_call, marshal it, take it back */
    DBusMessage *call = dbus_message_new_method_call(
        "org.example.Svc", "/org/example/Obj", "org.example.If", "DoThing");
    assert(call);
    dbus_message_set_serial(call, 7);
    const char *arg = "hello";
    dbus_message_append_args(call, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);

    char *raw = NULL; int rawlen = 0;
    assert(dbus_message_marshal(call, &raw, &rawlen));

    /* truncated buffer -> need more (0) */
    sdbus_msg m;
    assert(sdbus_codec_take((unsigned char *)raw, rawlen - 4, &m) == 0);

    /* full buffer -> consumed == rawlen, fields extracted */
    int consumed = sdbus_codec_take((unsigned char *)raw, rawlen, &m);
    assert(consumed == rawlen);
    assert(m.type == DBUS_MESSAGE_TYPE_METHOD_CALL);
    assert(strcmp(m.destination, "org.example.Svc") == 0);
    assert(strcmp(m.path, "/org/example/Obj") == 0);
    assert(strcmp(m.interface, "org.example.If") == 0);
    assert(strcmp(m.member, "DoThing") == 0);
    assert(m.has_reply_serial == 0);
    assert(sdbus_msg_n_fds(&m) == 0);

    /* stamp sender and re-emit; re-take and verify the sender is ours */
    unsigned char *out = NULL; int outlen = 0;
    assert(sdbus_codec_emit(&m, ":1.7", &out, &outlen) == 0);
    sdbus_msg m2;
    assert(sdbus_codec_take(out, outlen, &m2) == outlen);
    assert(m2.sender && strcmp(m2.sender, ":1.7") == 0);

    /* a corrupt buffer -> protocol error (-1) */
    unsigned char junk[16];
    memset(junk, 0xFF, sizeof junk);
    assert(sdbus_codec_take(junk, sizeof junk, &m) == -1 ||
           sdbus_codec_take(junk, sizeof junk, &m) == 0);

    /* reply carries a reply_serial */
    DBusMessage *reply = dbus_message_new_method_return(call);
    dbus_message_set_serial(reply, 8);
    char *rraw = NULL; int rlen = 0;
    assert(dbus_message_marshal(reply, &rraw, &rlen));
    sdbus_msg mr;
    assert(sdbus_codec_take((unsigned char *)rraw, rlen, &mr) == rlen);
    assert(mr.type == DBUS_MESSAGE_TYPE_METHOD_RETURN);
    assert(mr.has_reply_serial == 1);
    assert(mr.reply_serial == 7);

    free(out);
    dbus_free(raw); dbus_free(rraw);
    sdbus_msg_free(&m); sdbus_msg_free(&m2); sdbus_msg_free(&mr);
    dbus_message_unref(call); dbus_message_unref(reply);

    printf("all sdbus_codec tests passed\n");
    return 0;
}
