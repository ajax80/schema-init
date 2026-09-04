#include "../sdbus_conn.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>

static int fd_open(int fd) { return fcntl(fd, F_GETFD) != -1; }

int main(void) {
    /* F3: fds bound to un-flushed outbound chunks must be closed on teardown,
       not leaked. Enqueue two chunks carrying live pipe read-ends, then free
       the conn's fields with the queue never flushed. */
    sdbus_conn c;
    memset(&c, 0, sizeof c);

    int p0[2], p1[2];
    assert(pipe(p0) == 0 && pipe(p1) == 0);
    int f0 = p0[0], f1 = p1[0];
    assert(fd_open(f0) && fd_open(f1));

    unsigned char body[] = {1, 2, 3, 4};
    int fds0[] = { f0 };
    int fds1[] = { f1 };
    sdbus_conn_enqueue(&c, body, sizeof body, fds0, 1);
    sdbus_conn_enqueue(&c, body, sizeof body, fds1, 1);
    assert(c.n_oq == 2 && sdbus_conn_has_out(&c));

    sdbus_conn_free_fields(&c);
    assert(!fd_open(f0) && !fd_open(f1));      /* both closed, no leak */

    /* the write-ends are unrelated fds; free them so a later reuse can't mask
       an accidental close above */
    close(p0[1]); close(p1[1]);

    /* an already-relayed chunk (marked nfds=0 by the flush path) must NOT be
       closed again on teardown — its number may have been reused elsewhere. */
    sdbus_conn c2;
    memset(&c2, 0, sizeof c2);
    int keep[2];
    assert(pipe(keep) == 0);
    int survivor = keep[0];
    sdbus_conn_enqueue(&c2, body, sizeof body, &survivor, 1);
    c2.oq[0].nfds = 0;                          /* simulate: fds already sent */
    sdbus_conn_free_fields(&c2);
    assert(fd_open(survivor));                  /* untouched by teardown */
    close(survivor); close(keep[1]);

    /* a normal enqueue tracks its unsent bytes and never flags overflow */
    sdbus_conn c3; memset(&c3, 0, sizeof c3);
    sdbus_conn_enqueue(&c3, body, sizeof body, NULL, 0);
    assert(c3.oq_bytes == (long)sizeof body && c3.oq_over == 0 && c3.n_oq == 1);
    sdbus_conn_free_fields(&c3);
    assert(c3.oq_bytes == 0 && c3.oq_over == 0);

    /* backlog cap: a message that would exceed the ceiling is dropped (not queued),
       the connection is flagged for reap, and the fd it carried is closed. The
       oversized len never reaches the memcpy because the drop path returns first. */
    sdbus_conn c4; memset(&c4, 0, sizeof c4);
    int big[2]; assert(pipe(big) == 0);
    int bigfd = big[0];
    sdbus_conn_enqueue(&c4, body, SDBUS_MAX_OUTGOING_BYTES + 1, &bigfd, 1);
    assert(c4.oq_over == 1 && c4.n_oq == 0 && c4.oq_bytes == 0);
    assert(!fd_open(bigfd));                     /* dropped msg's fd closed, no leak */
    close(big[1]);
    sdbus_conn_free_fields(&c4);

    printf("all sdbus_conn tests passed\n");
    return 0;
}
