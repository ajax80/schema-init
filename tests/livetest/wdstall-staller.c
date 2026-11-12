/* Misbehaving control-socket clients for wdstall-vm.sh (the #479 audit's
 * "hardware watchdog during a stall" gap). Built -static for the initramfs.
 *   stuck <sock> <secs>: sends "status --kv", never reads, holds the socket
 *   drip  <sock> <secs>: one byte of "status" every 100 ms, never a newline
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int conn(const char *path) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    strncpy(a.sun_path, path, sizeof a.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    return fd;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    /* the server times a dripped line out and closes: the next write must
     * return EPIPE and reconnect, not die on SIGPIPE */
    signal(SIGPIPE, SIG_IGN);
    int secs = atoi(argv[3]);
    time_t end = time(NULL) + secs;
    if (strcmp(argv[1], "stuck") == 0) {
        int fd = conn(argv[2]);
        if (fd < 0) return 1;
        write(fd, "status --kv\n", 12);
        while (time(NULL) < end) sleep(1);   /* never reads */
        return 0;
    }
    if (strcmp(argv[1], "drip") == 0) {
        struct timespec t = { 0, 100 * 1000 * 1000 };
        for (;;) {                            /* server may time the line out: reconnect */
            int fd = conn(argv[2]);
            if (fd < 0) return 1;
            for (const char *p = "status"; ; p = *(p + 1) ? p + 1 : p) {
                if (time(NULL) >= end) return 0;
                if (write(fd, p, 1) != 1) break;
                nanosleep(&t, NULL);
            }
            close(fd);
        }
    }
    return 2;
}
