#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define CTL_SOCK_PATH "/run/schema-init.sock"

int main(int argc, char **argv) {
    char cmd[256];
    char rbuf[4096];
    int fd, i, rlen;
    ssize_t n;
    struct sockaddr_un addr;

    if (argc < 2) {
        fprintf(stderr, "usage: schema-ctl status|list|timing|start <svc>|stop <svc>|restart <svc>|add <path>\n");
        return 1;
    }

    cmd[0] = '\0';
    for (i = 1; i < argc; i++) {
        if (i > 1) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 2);
    }
    strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CTL_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    write(fd, cmd, strlen(cmd));

    rlen = 0;
    while (rlen < (int)sizeof(rbuf) - 1) {
        n = read(fd, rbuf + rlen, sizeof(rbuf) - 1 - (size_t)rlen);
        if (n <= 0) break;
        rlen += (int)n;
        rbuf[rlen] = '\0';
        if (rlen >= 2 && rbuf[rlen - 2] == '.' && rbuf[rlen - 1] == '\n') {
            rbuf[rlen - 2] = '\0';
            break;
        }
    }

    printf("%s", rbuf);
    close(fd);
    return 0;
}
