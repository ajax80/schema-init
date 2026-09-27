/* Regression tests for the forward-validation gate (sdbus_wire_forwardable) and
   the header field-type enforcement in sdbus_wire_parse. A peer must not be able
   to make the broker relay a message that a receiving client would choke on; the
   broker rejects such a message at the sender instead. */
#include "../sdbus_codec.h"
#include "../sdbus_wire.h"
#include <dbus/dbus.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* minimal little-endian header builder: append one field, spec-padded */
static int pad8(unsigned char *b, int n) { while (n % 8) b[n++] = 0; return n; }

static int add_str(unsigned char *b, int n, int code, char t, const char *v) {
    n = pad8(b, n);
    b[n++] = (unsigned char)code; b[n++] = 1; b[n++] = (unsigned char)t; b[n++] = 0;
    if (t == 'g') {
        int l = (int)strlen(v);
        b[n++] = (unsigned char)l;
        memcpy(b + n, v, l + 1); n += l + 1;
    } else {                                     /* 's' or 'o': 4-aligned u32 len */
        while ((16 + n) % 4) b[n++] = 0;
        int l = (int)strlen(v);
        b[n++] = l & 0xff; b[n++] = (l >> 8) & 0xff; b[n++] = (l >> 16) & 0xff; b[n++] = (l >> 24) & 0xff;
        memcpy(b + n, v, l + 1); n += l + 1;
    }
    return n;
}

/* assemble fixed header + field array (built into fbuf, flen) + body */
static int build(unsigned char *out, int type, unsigned serial,
                 const unsigned char *fbuf, int flen,
                 const unsigned char *body, int blen) {
    out[0] = 'l'; out[1] = (unsigned char)type; out[2] = 0; out[3] = 1;
    out[4] = blen & 0xff; out[5] = (blen >> 8) & 0xff; out[6] = (blen >> 16) & 0xff; out[7] = (blen >> 24) & 0xff;
    out[8] = serial & 0xff; out[9] = (serial >> 8) & 0xff; out[10] = (serial >> 16) & 0xff; out[11] = (serial >> 24) & 0xff;
    out[12] = flen & 0xff; out[13] = (flen >> 8) & 0xff; out[14] = (flen >> 16) & 0xff; out[15] = (flen >> 24) & 0xff;
    int n = 16;
    memcpy(out + n, fbuf, flen); n += flen;
    n = pad8(out, n);
    if (blen) { memcpy(out + n, body, blen); n += blen; }
    return n;
}

int main(void) {
    unsigned char out[1024], f[512], body[64];
    sdbus_wire_msg w;

    /* --- baseline: a well-formed signal validates and forwards --- */
    {
        DBusMessage *sig = dbus_message_new_signal("/org/o", "org.foo.Bar", "Changed");
        dbus_message_set_serial(sig, 3);
        const char *s = "hi";
        dbus_message_append_args(sig, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
        char *raw = NULL; int rawlen = 0;
        assert(dbus_message_marshal(sig, &raw, &rawlen));
        assert(sdbus_wire_parse((unsigned char *)raw, rawlen, &w) == rawlen);
        assert(sdbus_wire_forwardable((unsigned char *)raw, rawlen, &w) == 1);
        dbus_free(raw); dbus_message_unref(sig);
    }

    /* --- SIGNATURE (field 8) carried as an 's' with a >255 length: the parser
       must reject it outright (spec type for field 8 is 'g'). This is the
       reforward-truncation bug's root cause. --- */
    {
        char big[300]; memset(big, 's', sizeof big - 1); big[299] = 0;
        int flen = 0;
        flen = add_str(f, flen, 1, 'o', "/org/o");
        flen = add_str(f, flen, 2, 's', "org.foo.Bar");
        flen = add_str(f, flen, 3, 's', "Changed");
        flen = add_str(f, flen, 8, 's', big);       /* wrong type for SIGNATURE */
        int len = build(out, SDBUS_TYPE_SIGNAL, 4, f, flen, NULL, 0);
        assert(sdbus_wire_parse(out, len, &w) == -1);
    }

    /* --- PATH (field 1) carried as an 's' whose value is not an object path:
       parser rejects the wrong field type. --- */
    {
        int flen = 0;
        flen = add_str(f, flen, 1, 's', "not a path");
        flen = add_str(f, flen, 2, 's', "org.foo.Bar");
        flen = add_str(f, flen, 3, 's', "Changed");
        int len = build(out, SDBUS_TYPE_SIGNAL, 5, f, flen, NULL, 0);
        assert(sdbus_wire_parse(out, len, &w) == -1);
    }

    /* --- body shorter than its SIGNATURE claims: header parses, but the
       forward validator's libdbus demarshal rejects the body/sig mismatch. --- */
    {
        int flen = 0;
        flen = add_str(f, flen, 1, 'o', "/org/o");
        flen = add_str(f, flen, 2, 's', "org.foo.Bar");
        flen = add_str(f, flen, 3, 's', "Changed");
        flen = add_str(f, flen, 8, 'g', "s");        /* claims a string body... */
        int blen = 0;                                 /* string len says 100, only 4 bytes follow */
        body[blen++] = 100; body[blen++] = 0; body[blen++] = 0; body[blen++] = 0;
        body[blen++] = 'a'; body[blen++] = 'b'; body[blen++] = 'c'; body[blen++] = 0;
        int len = build(out, SDBUS_TYPE_SIGNAL, 6, f, flen, body, blen);
        assert(sdbus_wire_parse(out, len, &w) == len);   /* header framing is fine */
        assert(sdbus_wire_forwardable(out, len, &w) == 0);   /* body-vs-sig caught */
    }

    /* --- a signal missing the required MEMBER field: rejected. --- */
    {
        int flen = 0;
        flen = add_str(f, flen, 1, 'o', "/org/o");
        flen = add_str(f, flen, 2, 's', "org.foo.Bar");
        int len = build(out, SDBUS_TYPE_SIGNAL, 7, f, flen, NULL, 0);
        assert(sdbus_wire_parse(out, len, &w) == len);
        assert(sdbus_wire_forwardable(out, len, &w) == 0);
    }

    /* --- a syntactically invalid INTERFACE name: rejected by dbus_validate. --- */
    {
        int flen = 0;
        flen = add_str(f, flen, 1, 'o', "/org/o");
        flen = add_str(f, flen, 2, 's', "not..a..iface");
        flen = add_str(f, flen, 3, 's', "Changed");
        int len = build(out, SDBUS_TYPE_SIGNAL, 8, f, flen, NULL, 0);
        assert(sdbus_wire_parse(out, len, &w) == len);
        assert(sdbus_wire_forwardable(out, len, &w) == 0);
    }

    printf("all sdbus_forward tests passed\n");
    return 0;
}
