#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define CTL_SOCK_PATH "/run/schema-init.sock"

int main(int argc, char **argv) {
    char cmd[256];
    char *rbuf;
    size_t cap = 65536;
    int fd, i;
    size_t rlen;
    ssize_t n;
    struct sockaddr_un addr;

    if (argc < 2) {
        fprintf(stderr, "usage: schema-ctl status [--json|--kv]|list|timing|reload [--evict]|start <svc>|stop <svc>|up <svc>|down <svc>|restart <svc>|add <path>|pet <svc>|reset [<svc>]|reboot|poweroff\n");
        return 1;
    }


    cmd[0] = '\0';
    for (i = 1; i < argc; i++) {
        if (i > 1) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 2);
    }
    strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);

    rbuf = malloc(cap);
    if (!rbuf) { perror("malloc"); return 1; }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); free(rbuf); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CTL_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        /* try local fallback */
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, "./run/schema-init.sock", sizeof(addr.sun_path) - 1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("connect");
            close(fd);
            free(rbuf);
            return 1;
        }
    }

    write(fd, cmd, strlen(cmd));

    rlen = 0;
    for (;;) {
        if (rlen + 1 >= cap) {
            char *nb = realloc(rbuf, cap * 2);
            if (!nb) { perror("realloc"); close(fd); free(rbuf); return 1; }
            rbuf = nb;
            cap *= 2;
        }
        n = read(fd, rbuf + rlen, cap - 1 - rlen);
        if (n <= 0) break;
        rlen += (size_t)n;
        rbuf[rlen] = '\0';
        if (rlen >= 2 && rbuf[rlen - 2] == '.' && rbuf[rlen - 1] == '\n') {
            rbuf[rlen - 2] = '\0';
            break;
        }
    }

    printf("%s", rbuf);
    close(fd);
    free(rbuf);
    return 0;
}
