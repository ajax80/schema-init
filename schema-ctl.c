#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "schema.h"
#include "svc_dropins.h"

#define CTL_SOCK_PATH "/run/schema-init.sock"

static void usage(FILE *out) {
    fprintf(out,
        "usage: schema-ctl <command> [args]\n"
        "\n"
        "  status [--json|--kv]   service table: name, pid, state, restarts\n"
        "  status <svc>           one service, with each hardening knob's value\n"
        "                         and source (explicit / default / dropped)\n"
        "  list                   service names, one per line\n"
        "  analyze [<svc>]        boot critical chain + waterfall, and how each\n"
        "                         service proved ready; <svc>: its chain only\n"
        "  timing                 per-service cost (spawn→ready) + readiness instant,\n"
        "                         sorted by cost — the boot critical path first\n"
        "  reload [--evict]       re-read service files; --evict also SIGTERMs\n"
        "                         services no longer present in config\n"
        "  reexec [<path>]        replace the PID 1 binary in place (default: the\n"
        "                         one it booted from); services keep running\n"
        "  start <svc>            start a service (alias: up)\n"
        "  stop <svc>             stop it and hold it down (alias: down)\n"
        "  restart <svc>\n"
        "  add <path>             load one .svc file at runtime\n"
        "  cat <svc>              print <svc>.svc and its drop-ins in the order\n"
        "                         they apply (read from disk, not from PID 1)\n"
        "  pet <svc>              feed a service's watchdog\n"
        "  reset [<svc>]          clear restart/dormant counters and retry;\n"
        "                         no argument resets every service\n"
        "  reboot\n"
        "  poweroff\n"
        "\n"
        "  --help                 this text\n"
        "  --version              print version and exit\n"
        "\n"
        "Talks to PID 1 over %s.\nExits 1 when PID 1 replies \"err:\".\n", CTL_SOCK_PATH);
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

    if (strcmp(argv[1], "cat") == 0) {
        char path[SVC_DROPIN_PATH], files[SVC_DROPIN_MAX + 1][SVC_DROPIN_PATH];
        int nf;
        if (argc != 3 || !argv[2][0] || strchr(argv[2], '/')) {
            fprintf(stderr, "usage: schema-ctl cat <svc>\n");
            return 1;
        }
        snprintf(path, sizeof path, "%s/%s.svc", SVC_DIR, argv[2]);
        if (access(path, R_OK) < 0) {
            fprintf(stderr, "schema-ctl: %s: %s\n", path, strerror(errno));
            return 1;
        }
        snprintf(files[0], sizeof files[0], "%s", path);
        nf = 1 + svc_dropin_list(path, files + 1, SVC_DROPIN_MAX);
        for (i = 0; i < nf; i++) {
            FILE *f = fopen(files[i], "r");
            char line[512];
            if (!f) {
                fprintf(stderr, "schema-ctl: %s: %s\n", files[i], strerror(errno));
                return 1;
            }
            printf("%s# %s\n", i ? "\n" : "", files[i]);
            while (fgets(line, sizeof line, f)) fputs(line, stdout);
            fclose(f);
        }
        return 0;
    }


    cmd[0] = '\0';
    for (i = 1; i < argc; i++) {
        size_t len = strlen(cmd);
        if (len >= sizeof(cmd) - 2) break;
        if (i > 1) { strncat(cmd, " ", sizeof(cmd) - len - 1); len = strlen(cmd); }
        strncat(cmd, argv[i], sizeof(cmd) - len - 1);
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
        int first_errno = errno;
        /* try local fallback */
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, "./run/schema-init.sock", sizeof(addr.sun_path) - 1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "%s: %s\n", CTL_SOCK_PATH, strerror(first_errno));
            if (first_errno == EACCES || first_errno == EPERM)
                fprintf(stderr, "schema-ctl talks to PID 1 as root: try sudo schema-ctl %s", cmd);
            else if (first_errno == ENOENT)
                fprintf(stderr, "no socket there — is schema-init running as PID 1?\n");
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
    i = strncmp(rbuf, "err:", 4) == 0;
    close(fd);
    free(rbuf);
    return i;
}
