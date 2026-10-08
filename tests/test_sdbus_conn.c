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
    assert(c3.oq_bytes == (long)(sizeof body + sizeof(sdbus_outchunk)) && c3.oq_over == 0 && c3.n_oq == 1);
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

    /* a reader that keeps up but never fully drains: one message always in
       flight. The chunk array must stay bounded, not grow by one per message. */
    sdbus_conn c5; memset(&c5, 0, sizeof c5);
    sdbus_conn_enqueue(&c5, body, sizeof body, NULL, 0);
    for (int i = 0; i < 100000; i++) {
        sdbus_conn_enqueue(&c5, body, sizeof body, NULL, 0);
        free(c5.oq[c5.oq_head].b);              /* simulate flush_conn sending the head */
        c5.oq_bytes -= sizeof body + sizeof(sdbus_outchunk);
        c5.oq_head++;
    }
    assert(c5.n_oq - c5.oq_head == 1 && c5.oq_cap <= 16 && c5.oq_over == 0);
    assert(c5.oq_bytes == (long)(sizeof body + sizeof(sdbus_outchunk)));
    sdbus_conn_free_fields(&c5);

    /* tiny messages: the cap counts each chunk's struct, so a flood of 4-byte
       messages stops at the ceiling instead of costing 20x it in structs */
    sdbus_conn c6; memset(&c6, 0, sizeof c6);
    long n6 = 0;
    while (!c6.oq_over) { sdbus_conn_enqueue(&c6, body, sizeof body, NULL, 0); n6++; }
    assert(c6.oq_bytes <= SDBUS_MAX_OUTGOING_BYTES);
    assert((long)c6.oq_cap * (long)sizeof(sdbus_outchunk) <= 2L * SDBUS_MAX_OUTGOING_BYTES);
    assert(n6 - 1 == SDBUS_MAX_OUTGOING_BYTES / (long)(sizeof body + sizeof(sdbus_outchunk)));
    sdbus_conn_free_fields(&c6);

    printf("all sdbus_conn tests passed\n");
    return 0;
}
