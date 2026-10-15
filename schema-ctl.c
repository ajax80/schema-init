#include <dirent.h>
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

#define G_MAX_NODES 512
#define G_MAX_EDGES 8
#define G_PID1_MAX_SERVICES 88
#define G_PID1_MAX_GROUPS 16

struct g_node { char name[64]; char file[64]; char deps[G_MAX_EDGES][64]; int ndeps; int group; int seen; };

static struct g_node g_nodes[G_MAX_NODES];
static int g_n;

static struct g_node *g_find(const char *name) {
    for (int i = 0; i < g_n; i++) if (!strcmp(g_nodes[i].name, name)) return &g_nodes[i];
    return NULL;
}

static struct g_node *g_find_arg(const char *arg) {
    struct g_node *n = g_find(arg);
    for (int i = 0; !n && i < g_n; i++) if (!strcmp(g_nodes[i].file, arg)) n = &g_nodes[i];
    return n;
}

static const char *g_dir(void) {
    const char *e = getenv("SCHEMA_SVC_DIR");
    return e && e[0] ? e : SVC_DIR;
}

static int g_load_file(const char *path, struct g_node *n, int dropin, int *has_exec) {
    FILE *f = fopen(path, "r");
    char line[512];
    const char *depkey = n->group ? "member" : "dep";
    int r;
    if (!f) return -1;
    while ((r = svc_read_line(f, line, sizeof line)) != 0) {
        char *eq = strchr(line, '='), *val;
        if (r < 0) { fclose(f); return -1; }
        line[strcspn(line, "\r\n")] = '\0';
        if (!eq || line[0] == '#') continue;
        *eq = '\0';
        val = eq + 1;
        if (!strcmp(line, "name")) {
            if (dropin) { fclose(f); return -1; }
            if (val[0]) snprintf(n->name, sizeof n->name, "%s", val);
        } else if (!strcmp(line, "exec") && val[0])
            *has_exec = 1;
        else if (!strcmp(line, depkey)) {
            if (!val[strspn(val, " \t")]) { if (dropin) n->ndeps = 0; }
            else if (n->ndeps < G_MAX_EDGES) snprintf(n->deps[n->ndeps++], 64, "%s", val);
        }
    }
    fclose(f);
    return 0;
}

static void g_load(void) {
    DIR *d = opendir(g_dir());
    struct dirent *e;
    int nsvc = 0, ngrp = 0;
    if (!d) return;
    while ((e = readdir(d)) && g_n < G_MAX_NODES) {
        size_t l = strlen(e->d_name);
        int grp = l > 4 && !strcmp(e->d_name + l - 4, ".grp");
        char path[SVC_DROPIN_PATH], files[SVC_DROPIN_MAX][SVC_DROPIN_PATH], *at;
        struct g_node *n = &g_nodes[g_n];
        int has_exec = 0, ok = 1;
        if (!grp && (l <= 4 || strcmp(e->d_name + l - 4, ".svc"))) continue;
        memset(n, 0, sizeof *n);
        snprintf(n->file, sizeof n->file, "%.*s", (int)(l - 4), e->d_name);
        at = strchr(n->file, '@');
        if (!grp && at && !at[1]) continue;
        snprintf(path, sizeof path, "%s/%s", g_dir(), e->d_name);
        n->group = grp;
        if (g_load_file(path, n, 0, &has_exec) < 0) continue;
        if (grp) {
            if (!n->name[0]) continue;
            ngrp++;
        } else {
            int nf = svc_dropin_list(path, files, SVC_DROPIN_MAX);
            for (int i = 0; i < nf && ok; i++) ok = g_load_file(files[i], n, 1, &has_exec) == 0;
            if (!ok || !has_exec) continue;
            if (at || !n->name[0]) snprintf(n->name, sizeof n->name, "%s", n->file);
            nsvc++;
        }
        g_n++;
    }
    closedir(d);
    if (nsvc > G_PID1_MAX_SERVICES)
        fprintf(stderr, "schema-ctl: %d services; PID 1 loads only the first %d\n", nsvc, G_PID1_MAX_SERVICES);
    if (ngrp > G_PID1_MAX_GROUPS)
        fprintf(stderr, "schema-ctl: %d groups; PID 1 loads only the first %d\n", ngrp, G_PID1_MAX_GROUPS);
}

static void g_mark(const char *name) {
    struct g_node *n = g_find(name);
    if (!n || n->seen) return;
    n->seen = 1;
    for (int i = 0; i < n->ndeps; i++) g_mark(n->deps[i]);
}

static void g_q(const char *s) {
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') putchar('\\');
        putchar(*s);
    }
    putchar('"');
}

static int cmd_graph(const char *root) {
    char missing[G_MAX_NODES][64];
    int nmiss = 0;
    g_load();
    if (root) {
        struct g_node *r = g_find_arg(root);
        if (!r) { fprintf(stderr, "schema-ctl: no service or group named %s in %s\n", root, g_dir()); return 1; }
        g_mark(r->name);
    } else
        for (int i = 0; i < g_n; i++) g_nodes[i].seen = 1;
    printf("digraph schema {\n  rankdir=LR;\n  node [shape=ellipse];\n");
    for (int i = 0; i < g_n; i++) {
        struct g_node *n = &g_nodes[i];
        if (!n->seen) continue;
        printf("  "); g_q(n->name); printf("%s;\n", n->group ? " [shape=box]" : "");
        for (int j = 0; j < n->ndeps; j++) {
            if (!g_find(n->deps[j])) {
                int dup = 0;
                for (int k = 0; k < nmiss; k++) if (!strcmp(missing[k], n->deps[j])) { dup = 1; break; }
                if (!dup && nmiss < G_MAX_NODES) {
                    snprintf(missing[nmiss++], 64, "%s", n->deps[j]);
                    printf("  "); g_q(n->deps[j]); printf(" [color=red, fontcolor=red, style=dashed];\n");
                }
            }
            printf("  "); g_q(n->name); printf(" -> "); g_q(n->deps[j]);
            printf("%s;\n", n->group ? " [style=dashed]" : "");
        }
    }
    printf("}\n");
    return 0;
}

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
        "  graph [<svc>]          dependency graph as Graphviz DOT (A -> B: A waits\n"
        "                         for B; boxes are groups, red = missing); read\n"
        "                         from disk. View: schema-ctl graph | dot -Tsvg\n"
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

    if (strcmp(argv[1], "graph") == 0) {
        if (argc > 3 || (argc == 3 && (!argv[2][0] || strchr(argv[2], '/')))) {
            fprintf(stderr, "usage: schema-ctl graph [<svc>]\n");
            return 1;
        }
        return cmd_graph(argc == 3 ? argv[2] : NULL);
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
        int connected = 0;
        /* try local fallback only when explicitly requested */
        if (getenv("SCHEMA_CTL_FALLBACK")) {
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, "./run/schema-init.sock", sizeof(addr.sun_path) - 1);
            connected = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        }
        if (!connected) {
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
