#ifndef SDBUS_AUTH_H
#define SDBUS_AUTH_H

/* SASL EXTERNAL auth handshake. The broker runs it directly (libdbus never sees
   the socket). Only EXTERNAL is offered; the authorization identity is the uid
   already read from SO_PEERCRED, so a claimed uid must match it. Supports
   NEGOTIATE_UNIX_FD. Auth bytes accumulate in conn->in; responses append to
   conn->out. */

#include "sdbus_conn.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* the bus GUID reported in OK; 32 hex chars, stable per process */
static inline const char *sdbus__guid(void) {
    static char g[33];
    if (!g[0]) {
        unsigned seed = (unsigned)getpid() * 2654435761u + 0x9e3779b9u;
        for (int i = 0; i < 32; i++) {
            seed = seed * 1103515245u + 12345u;
            g[i] = "0123456789abcdef"[(seed >> 16) & 0xf];
        }
        g[32] = '\0';
    }
    return g;
}

/* decode an even-length hex string into decimal; returns -1 on bad hex.
   For EXTERNAL, the data is the ASCII-decimal uid, hex-encoded. */
static inline long sdbus__hex_uid(const char *hex, int len) {
    if (len == 0) return -2;                 /* empty -> use peer uid */
    if (len % 2) return -1;
    char dec[64]; int dl = 0;
    for (int i = 0; i + 1 < len && dl < 63; i += 2) {
        int hi, lo;
        char a = hex[i], b = hex[i + 1];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        else return -1;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        else return -1;
        dec[dl++] = (char)((hi << 4) | lo);
    }
    dec[dl] = '\0';
    char *end;
    long v = strtol(dec, &end, 10);
    if (*end) return -1;
    return v;
}

static inline void sdbus__send(sdbus_conn *c, const char *s) {
    sdbus_conn_out_append(c, s, (int)strlen(s));
}

/* Feed newly-read auth bytes. Returns 1 when BEGIN reached (authed), 0
   mid-handshake, -1 on unrecoverable garbage. On BEGIN, any bytes after the
   BEGIN line are left in conn->in as the first message bytes. */
static inline int sdbus_auth_feed(sdbus_conn *c, const unsigned char *buf, int len) {
    sdbus_conn_in_append(c, buf, len);

    if (!c->saw_nul) {                       /* first byte of the stream is NUL */
        if (c->in_len == 0) return 0;
        if (c->in[0] != 0) return -1;
        c->saw_nul = 1;
        sdbus_conn_in_consume(c, 1);
    }

    for (;;) {
        /* find a complete CRLF-terminated line */
        int nl = -1;
        for (int i = 0; i + 1 < c->in_len; i++)
            if (c->in[i] == '\r' && c->in[i + 1] == '\n') { nl = i; break; }
        if (nl < 0) return 0;                /* need more */

        char line[512];
        int ll = nl < (int)sizeof line - 1 ? nl : (int)sizeof line - 1;
        memcpy(line, c->in, ll); line[ll] = '\0';
        sdbus_conn_in_consume(c, nl + 2);    /* drop line + CRLF */

        if (!strncmp(line, "AUTH EXTERNAL", 13)) {
            const char *arg = line + 13;
            while (*arg == ' ') arg++;
            if (!*arg) {                          /* two-step: request the identity */
                sdbus__send(c, "DATA\r\n");
                continue;
            }
            long claimed = sdbus__hex_uid(arg, (int)strlen(arg));   /* one-step: identity inline */
            if (claimed == -1) { sdbus__send(c, "REJECTED EXTERNAL\r\n"); continue; }
            if (claimed != -2 && claimed != (long)c->uid) {
                sdbus__send(c, "REJECTED EXTERNAL\r\n"); continue;
            }
            char ok[80];
            snprintf(ok, sizeof ok, "OK %s\r\n", sdbus__guid());
            sdbus__send(c, ok);
        } else if (!strncmp(line, "DATA", 4) && (line[4] == '\0' || line[4] == ' ')) {
            const char *arg = line + 4;           /* EXTERNAL identity response */
            while (*arg == ' ') arg++;
            long claimed = sdbus__hex_uid(arg, (int)strlen(arg));   /* empty -> use peer uid */
            if (claimed == -1 || (claimed != -2 && claimed != (long)c->uid)) {
                sdbus__send(c, "REJECTED EXTERNAL\r\n"); continue;
            }
            char ok[80];
            snprintf(ok, sizeof ok, "OK %s\r\n", sdbus__guid());
            sdbus__send(c, ok);
        } else if (!strcmp(line, "AUTH") || !strncmp(line, "AUTH ", 5)) {
            sdbus__send(c, "REJECTED EXTERNAL\r\n");   /* only EXTERNAL offered */
        } else if (!strcmp(line, "NEGOTIATE_UNIX_FD")) {
            c->negotiated_fd = 1;
            sdbus__send(c, "AGREE_UNIX_FD\r\n");
        } else if (!strcmp(line, "CANCEL")) {
            sdbus__send(c, "REJECTED EXTERNAL\r\n");
        } else if (!strcmp(line, "BEGIN")) {
            c->authed = 1;
            return 1;                        /* remaining in-bytes are message data */
        } else {
            sdbus__send(c, "ERROR\r\n");
        }
    }
}

#endif
