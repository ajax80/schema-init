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

    printf("all sdbus_conn tests passed\n");
    return 0;
}
