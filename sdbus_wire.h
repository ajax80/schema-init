#ifndef SDBUS_WIRE_H
#define SDBUS_WIRE_H

/* Manual D-Bus header parser + sender-rewrite, so the broker can forward
   messages the way dbus-daemon does: parse/validate the HEADER for routing,
   forward the body (and unix fds) opaquely. This is what lets fd-bearing
   messages pass — libdbus's public demarshal cannot decode a UNIX_FD message
   from bytes alone. libdbus is still used for the driver's own args/replies
   (which never carry fds). Wire format per the D-Bus spec: 12-byte fixed header
   + an ARRAY of STRUCT(BYTE code, VARIANT value) header fields, padded to 8;
   body follows. Endianness is byte 0 ('l'/'B'). */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SDBUS_TYPE_METHOD_CALL   1
#define SDBUS_TYPE_METHOD_RETURN 2
#define SDBUS_TYPE_ERROR         3
#define SDBUS_TYPE_SIGNAL        4
#define SDBUS_FLAG_NO_REPLY      0x1
#define SDBUS_MAX_MSG (128 * 1024 * 1024)   /* D-Bus default max message size */

typedef struct {
    int endian_le;
    int type;
    unsigned flags;
    uint32_t serial;
    uint32_t reply_serial; int has_reply_serial;
    const char *path, *interface, *member, *error_name, *destination, *sender, *signature;
    const char *arg0;          /* first body arg iff a string/objpath, else NULL */
    uint32_t unix_fds; int has_unix_fds;
    int body_offset, body_len, total_len;
} sdbus_wire_msg;

static inline uint32_t sdbus__rd_u32(const unsigned char *p, int le) {
    return le ? ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24)
              : ((uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24);
}
static inline int sdbus__align(int off, int a) { return (off + (a - 1)) & ~(a - 1); }

/* Parse one message header. Returns total message length (>0), 0 if the buffer
   does not yet hold a complete message, -1 if corrupt. String fields point into
   buf (wire strings are NUL-terminated, so they are valid C strings). */
static inline int sdbus_wire_parse(const unsigned char *buf, int len, sdbus_wire_msg *m) {
    memset(m, 0, sizeof *m);
    if (len < 16) return 0;
    if (buf[0] != 'l' && buf[0] != 'B') return -1;
    m->endian_le = buf[0] == 'l';
    m->type = buf[1];
    m->flags = buf[2];
    uint32_t body_len = sdbus__rd_u32(buf + 4, m->endian_le);
    m->serial = sdbus__rd_u32(buf + 8, m->endian_le);
    uint32_t farr = sdbus__rd_u32(buf + 12, m->endian_le);
    /* bound attacker-controlled lengths before any signed arithmetic */
    if (farr > (uint32_t)SDBUS_MAX_MSG || body_len > (uint32_t)SDBUS_MAX_MSG) return -1;
    int header_end = 16 + (int)farr;                 /* farr bounded -> no overflow */
    m->body_offset = sdbus__align(header_end, 8);
    long total = (long)m->body_offset + (long)body_len;   /* both bounded -> fits in long */
    if (total > SDBUS_MAX_MSG) return -1;
    m->body_len = (int)body_len;
    m->total_len = (int)total;
    if (len < m->total_len) return 0;                /* incomplete */

    int off = 16;
    while (off < header_end) {
        off = sdbus__align(off, 8);
        if (off + 2 > header_end) break;
        int code = buf[off++];
        int siglen = buf[off++];
        if (off + siglen + 1 > header_end) return -1;
        char sig = buf[off];
        off += siglen + 1;                           /* signature bytes + NUL */
        const char *sval = NULL; uint32_t uval = 0;
        if (sig == 's' || sig == 'o') {
            off = sdbus__align(off, 4);
            if (off + 4 > header_end) return -1;
            uint32_t sl = sdbus__rd_u32(buf + off, m->endian_le); off += 4;
            uint32_t avail = (uint32_t)(header_end - off);   /* off <= header_end here */
            if (sl >= avail) return -1;                      /* need sl bytes + a NUL */
            if (buf[off + sl] != 0) return -1;               /* enforce NUL terminator */
            sval = (const char *)buf + off; off += (int)sl + 1;
        } else if (sig == 'g') {
            if (off >= header_end) return -1;
            int gl = buf[off++];
            uint32_t avail = (uint32_t)(header_end - off);
            if ((uint32_t)gl >= avail) return -1;            /* need gl bytes + a NUL */
            if (buf[off + gl] != 0) return -1;
            sval = (const char *)buf + off; off += gl + 1;
        } else if (sig == 'u') {
            off = sdbus__align(off, 4);
            if (off + 4 > header_end) return -1;
            uval = sdbus__rd_u32(buf + off, m->endian_le); off += 4;
        } else {
            return -1;                               /* unexpected header field type */
        }
        switch (code) {
            case 1: m->path = sval; break;
            case 2: m->interface = sval; break;
            case 3: m->member = sval; break;
            case 4: m->error_name = sval; break;
            case 5: m->reply_serial = uval; m->has_reply_serial = 1; break;
            case 6: m->destination = sval; break;
            case 7: m->sender = sval; break;
            case 8: m->signature = sval; break;
            case 9: m->unix_fds = uval; m->has_unix_fds = 1; break;
            default: break;                          /* ignore unknown fields */
        }
    }
    /* extract arg0 as a C string when the body's first argument is a string or
       object path — the only arg0 kinds a match rule constrains (PropertiesChanged
       carries the interface name, NameOwnerChanged the bus name). Pointer into buf,
       same lifetime as the header string fields. */
    if (m->signature && (m->signature[0] == 's' || m->signature[0] == 'o')
            && m->body_len >= 5) {
        const unsigned char *b = buf + m->body_offset;   /* 8-aligned => 4-aligned */
        uint32_t sl = sdbus__rd_u32(b, m->endian_le);
        if (sl < (uint32_t)(m->body_len - 4) && b[4 + sl] == 0)
            m->arg0 = (const char *)b + 4;
    }
    return m->total_len;
}

/* count of unix fds this message carries (from the UNIX_FDS header field). */
static inline int sdbus_wire_n_fds(const sdbus_wire_msg *m) {
    return m->has_unix_fds ? (int)m->unix_fds : 0;
}

/* --- writer for rebuilding the header with a replaced SENDER --- */
typedef struct { unsigned char *b; int len, cap, le; } sdbus__w;
static inline void sdbus__w_need(sdbus__w *w, int n) {
    if (w->len + n > w->cap) { w->cap = (w->len + n) * 2 + 64; w->b = realloc(w->b, w->cap); }
}
static inline void sdbus__w_align(sdbus__w *w, int a) {
    int t = sdbus__align(w->len, a);
    sdbus__w_need(w, t - w->len);
    while (w->len < t) w->b[w->len++] = 0;
}
static inline void sdbus__w_byte(sdbus__w *w, unsigned char v) { sdbus__w_need(w, 1); w->b[w->len++] = v; }
static inline void sdbus__w_u32(sdbus__w *w, uint32_t v) {
    sdbus__w_align(w, 4); sdbus__w_need(w, 4);
    if (w->le) { w->b[w->len++]=v&0xff; w->b[w->len++]=(v>>8)&0xff; w->b[w->len++]=(v>>16)&0xff; w->b[w->len++]=(v>>24)&0xff; }
    else       { w->b[w->len++]=(v>>24)&0xff; w->b[w->len++]=(v>>16)&0xff; w->b[w->len++]=(v>>8)&0xff; w->b[w->len++]=v&0xff; }
}
static inline void sdbus__w_str_field(sdbus__w *w, int code, char sigc, const char *val) {
    sdbus__w_align(w, 8);
    sdbus__w_byte(w, (unsigned char)code);
    sdbus__w_byte(w, 1); sdbus__w_byte(w, (unsigned char)sigc); sdbus__w_byte(w, 0);  /* variant sig */
    if (sigc == 'g') {
        int l = (int)strlen(val);
        sdbus__w_byte(w, (unsigned char)l);
        sdbus__w_need(w, l + 1); memcpy(w->b + w->len, val, l + 1); w->len += l + 1;
    } else {                                          /* 's' or 'o' */
        int l = (int)strlen(val);
        sdbus__w_u32(w, (uint32_t)l);
        sdbus__w_need(w, l + 1); memcpy(w->b + w->len, val, l + 1); w->len += l + 1;
    }
}
static inline void sdbus__w_u32_field(sdbus__w *w, int code, uint32_t val) {
    sdbus__w_align(w, 8);
    sdbus__w_byte(w, (unsigned char)code);
    sdbus__w_byte(w, 1); sdbus__w_byte(w, 'u'); sdbus__w_byte(w, 0);
    sdbus__w_u32(w, val);
}

/* Rebuild the message with SENDER set to new_sender, preserving all other
   header fields and forwarding the body verbatim. Produces a fresh malloc'd
   buffer (caller frees). Returns 0 on success, -1 on failure. */
static inline int sdbus_wire_reforward(const unsigned char *orig, const sdbus_wire_msg *m,
                                       const char *new_sender, unsigned char **out, int *outlen) {
    sdbus__w fw = {0}; fw.le = m->endian_le;         /* header fields, without the array-len */
    if (m->path)       sdbus__w_str_field(&fw, 1, 'o', m->path);
    if (m->interface)  sdbus__w_str_field(&fw, 2, 's', m->interface);
    if (m->member)     sdbus__w_str_field(&fw, 3, 's', m->member);
    if (m->error_name) sdbus__w_str_field(&fw, 4, 's', m->error_name);
    if (m->has_reply_serial) sdbus__w_u32_field(&fw, 5, m->reply_serial);
    if (m->destination) sdbus__w_str_field(&fw, 6, 's', m->destination);
    sdbus__w_str_field(&fw, 7, 's', new_sender);      /* SENDER: always our verified name */
    if (m->signature)  sdbus__w_str_field(&fw, 8, 'g', m->signature);
    if (m->has_unix_fds) sdbus__w_u32_field(&fw, 9, m->unix_fds);
    int farr_len = fw.len;

    sdbus__w w = {0}; w.le = m->endian_le;
    sdbus__w_need(&w, 12);
    memcpy(w.b, orig, 12); w.len = 12;                /* fixed header (endian/type/flags/ver/body_len/serial) */
    sdbus__w_u32(&w, (uint32_t)farr_len);             /* header-fields array length (offset 12) */
    sdbus__w_need(&w, farr_len);
    memcpy(w.b + w.len, fw.b, farr_len); w.len += farr_len;
    sdbus__w_align(&w, 8);                            /* pad to body boundary */
    sdbus__w_need(&w, m->body_len);
    memcpy(w.b + w.len, orig + m->body_offset, m->body_len); w.len += m->body_len;

    free(fw.b);
    *out = w.b; *outlen = w.len;
    return 0;
}

#endif
