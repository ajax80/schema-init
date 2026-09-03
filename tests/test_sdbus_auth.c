#include "../sdbus_auth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

/* does conn->out contain the substring s? */
static int out_has(sdbus_conn *c, const char *s) {
    if (c->out_len == 0) return 0;
    char tmp[4096];
    int n = c->out_len < (int)sizeof tmp - 1 ? c->out_len : (int)sizeof tmp - 1;
    memcpy(tmp, c->out, n); tmp[n] = '\0';
    return strstr(tmp, s) != NULL;
}
static void out_reset(sdbus_conn *c) { c->out_len = 0; }

int main(void) {
    /* --- auth handshake against a fake conn (uid set directly) --- */
    sdbus_conn c = {0};
    c.uid = 1000;

    const char *m1 = "\0AUTH EXTERNAL 31303030\r\n";   /* NUL + hex("1000") */
    assert(sdbus_auth_feed(&c, (const unsigned char *)m1, 25) == 0);
    assert(out_has(&c, "OK "));
    assert(c.authed == 0);
    out_reset(&c);

    assert(sdbus_auth_feed(&c, (const unsigned char *)"NEGOTIATE_UNIX_FD\r\n", 19) == 0);
    assert(out_has(&c, "AGREE_UNIX_FD"));
    assert(c.negotiated_fd == 1);
    out_reset(&c);

    assert(sdbus_auth_feed(&c, (const unsigned char *)"BEGIN\r\n", 7) == 1);
    assert(c.authed == 1);
    sdbus_conn_free_fields(&c);

    /* --- uid mismatch is rejected --- */
    sdbus_conn c2 = {0};
    c2.uid = 1000;
    /* hex("0") = 30 -> claims uid 0, but peer is 1000 */
    const char *bad = "\0AUTH EXTERNAL 30\r\n";
    assert(sdbus_auth_feed(&c2, (const unsigned char *)bad, 19) == 0);
    assert(out_has(&c2, "REJECTED"));
    assert(c2.authed == 0);
    sdbus_conn_free_fields(&c2);

    /* --- a non-EXTERNAL mechanism is rejected, connection stays open --- */
    sdbus_conn c3 = {0};
    c3.uid = 1000;
    const char *dbn = "\0AUTH DBUS_COOKIE_SHA1 abcd\r\n";
    assert(sdbus_auth_feed(&c3, (const unsigned char *)dbn, 29) == 0);
    assert(out_has(&c3, "REJECTED"));
    assert(c3.authed == 0);
    sdbus_conn_free_fields(&c3);

    /* --- fragmented delivery: a line split across two feeds --- */
    sdbus_conn c4 = {0};
    c4.uid = 1000;
    assert(sdbus_auth_feed(&c4, (const unsigned char *)"\0AUTH EXTERNAL 3130", 19) == 0);
    assert(sdbus_auth_feed(&c4, (const unsigned char *)"3030\r\n", 6) == 0);
    assert(out_has(&c4, "OK "));
    sdbus_conn_free_fields(&c4);

    /* --- capture_creds over a socketpair: peer is ourselves --- */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    sdbus_conn c5 = {0};
    c5.fd = sv[0];
    sdbus_conn_capture_creds(&c5);
    assert(c5.uid == getuid());
    assert(c5.n_gids >= 1);
    close(sv[0]); close(sv[1]);

    printf("all sdbus_auth tests passed\n");
    return 0;
}
