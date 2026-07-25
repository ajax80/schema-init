#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "schema.h"

#define CTL_SOCK_PATH "/run/schema-init.sock"

static void usage(FILE *out) {
    fprintf(out,
        "usage: schema-ctl <command> [args]\n"
        "\n"
        "  status [--json|--kv]   service table: name, pid, state, restarts\n"
        "  list                   service names, one per line\n"
        "  timing                 how long each service took to start\n"
        "  reload [--evict]       re-read service files; --evict also SIGTERMs\n"
        "                         services no longer present in config\n"
        "  start <svc>            start a service (alias: up)\n"
        "  stop <svc>             stop it and hold it down (alias: down)\n"
        "  restart <svc>\n"
        "  add <path>             load one .svc file at runtime\n"
        "  pet <svc>              feed a service's watchdog\n"
        "  reset [<svc>]          clear restart/dormant counters and retry;\n"
        "                         no argument resets every service\n"
        "  reboot\n"
        "  poweroff\n"
        "\n"
        "  --help                 this text\n"
        "  --version              print version and exit\n"
        "\n"
        "Talks to PID 1 over %s.\n", CTL_SOCK_PATH);
}

int main(int argc, char **argv) {
    char cmd[256];
    char *rbuf;
    size_t cap = 65536;
    int fd, i;
    size_t rlen;
    ssize_t n;
    struct sockaddr_un addr;

    if (argc < 2) {
        usage(stderr);
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout);
        return 0;
    }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("schema-ctl %s\n", SCHEMA_INIT_VERSION);
        return 0;
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
