#ifndef SDBUS_CODEC_H
#define SDBUS_CODEC_H

/* Thin wrapper over libdbus, used ONLY as a wire codec: demarshal a byte buffer
   into a DBusMessage, expose the header fields the broker routes on, stamp the
   verified sender, and marshal back to bytes. All routing/policy/names live
   elsewhere. */

#include <dbus/dbus.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    DBusMessage *msg;            /* owned; free with sdbus_msg_free */
    int type;                    /* DBUS_MESSAGE_TYPE_* */
    dbus_uint32_t serial;
    dbus_uint32_t reply_serial;
    int has_reply_serial;
    const char *destination, *path, *interface, *member, *signature, *error_name, *sender;
} sdbus_msg;

static void sdbus__msg_fill(sdbus_msg *out) {
    DBusMessage *m = out->msg;
    out->type        = dbus_message_get_type(m);
    out->serial      = dbus_message_get_serial(m);
    out->reply_serial = dbus_message_get_reply_serial(m);
    out->has_reply_serial = out->reply_serial != 0;
    out->destination = dbus_message_get_destination(m);
    out->path        = dbus_message_get_path(m);
    out->interface   = dbus_message_get_interface(m);
    out->member      = dbus_message_get_member(m);
    out->signature   = dbus_message_get_signature(m);
    out->error_name  = dbus_message_get_error_name(m);
    out->sender      = dbus_message_get_sender(m);
}

/* Demarshal one complete message from buf[0..len). Returns bytes consumed (>0),
   0 if a full message is not yet buffered, -1 on protocol error. */
static int sdbus_codec_take(const unsigned char *buf, int len, sdbus_msg *out) {
    memset(out, 0, sizeof *out);
    int needed = dbus_message_demarshal_bytes_needed((const char *)buf, len);
    if (needed < 0) return -1;
    if (needed == 0 || needed > len) return 0;   /* incomplete */
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_demarshal((const char *)buf, needed, &err);
    if (!m) { dbus_error_free(&err); return -1; }
    out->msg = m;
    sdbus__msg_fill(out);
    return needed;
}

static void sdbus_msg_free(sdbus_msg *out) {
    if (out && out->msg) { dbus_message_unref(out->msg); out->msg = NULL; }
}

/* Stamp the verified unique sender, then marshal to a fresh malloc'd buffer the
   caller frees with free(). Returns 0 on success, -1 on failure. */
static int sdbus_codec_emit(sdbus_msg *m, const char *sender_unique,
                            unsigned char **bytes, int *len) {
    if (sender_unique && !dbus_message_set_sender(m->msg, sender_unique)) return -1;
    char *dbytes = NULL;
    int dlen = 0;
    if (!dbus_message_marshal(m->msg, &dbytes, &dlen)) return -1;
    unsigned char *copy = malloc(dlen);
    if (!copy) { dbus_free(dbytes); return -1; }
    memcpy(copy, dbytes, dlen);
    dbus_free(dbytes);
    *bytes = copy;
    *len = dlen;
    return 0;
}

/* Number of unix fds the message declares (count of 'h' in its signature).
   Authoritative fd relay is the event loop's job (SCM_RIGHTS); this reports how
   many the message expects so the loop knows what to attach. */
static int sdbus_msg_n_fds(const sdbus_msg *m) {
    int n = 0;
    const char *s = m->signature;
    if (!s) return 0;
    for (; *s; s++) if (*s == DBUS_TYPE_UNIX_FD) n++;
    return n;
}

#endif
