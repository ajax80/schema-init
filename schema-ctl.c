#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sysexits.h>

#include "schema.h"
#include "svc_dropins.h"
#include "schema_shm.h"
#include "ctl_colour.h"
#include "ctl_log.h"
#include "recorder_logic.h"
#include "verify_baseline.h"
#include "verify.h"
#include "verify_gate.h"

#define CTL_SOCK_PATH "/run/schema-init.sock"

#define G_MAX_NODES 512
#define G_MAX_EDGES 8
#define G_PID1_MAX_SERVICES SCHEMA_SHM_MAX
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

/* --user points both at $XDG_RUNTIME_DIR/schema-user */
static const char *ctl_sock = CTL_SOCK_PATH;
static const char *svc_dir = SVC_DIR;
static int user_mode;

static const char *g_dir(void) {
    const char *e = getenv("SCHEMA_SVC_DIR");
    return e && e[0] ? e : svc_dir;
}

static int g_load_file(const char *path, struct g_node *n, int dropin, int *has_exec) {
    FILE *f = fopen(path, "r");
    char line[512];
    const char *depkey = n->group ? "member" : "dep";
    int r, over = 0;
    if (!f) return -1;
    while ((r = svc_read_line(f, line, sizeof line)) != 0) {
        char *eq = strchr(line, '='), *val;
        if (r < 0) {
            fprintf(stderr, "schema-ctl: %s: line longer than %zu bytes — %s\n", path, sizeof line - 1,
                    n->group ? "ignored" : "not loaded");
            if (n->group) continue;
            fclose(f);
            return -1;
        }
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
            else if (n->group) {
                if (!over++)
                    fprintf(stderr, "schema-ctl: %s: more than %d member= lines — PID 1 holds the group EXCISED\n", path, G_MAX_EDGES);
            } else {
                fprintf(stderr, "schema-ctl: %s: more than %d dep= lines — not loaded\n", path, G_MAX_EDGES);
                fclose(f);
                return -1;
            }
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
            if (nf < 0) {
                fprintf(stderr, "schema-ctl: %s: a drop-in directory is not owned by root or is writable by others — not loaded\n", path);
                continue;
            }
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

static int trace_ev(const rec_event_t *e, const char *msg) {
    char tb[32], who[100], kind[sizeof e->kind], body[sizeof e->msg];
    time_t t = (time_t)e->ts;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &tm);
    if (e->subj[0] && e->comm[0] && strcmp(e->subj, e->comm)) snprintf(who, sizeof who, "%s/%s", e->comm, e->subj);
    else snprintf(who, sizeof who, "%s", e->subj[0] ? e->subj : e->comm);
    rec_copy(kind, sizeof kind, e->kind);
    rec_copy(body, sizeof body, msg);
    rec_scrub(who);
    rec_scrub(kind);
    rec_scrub(body);
    printf("%s  %s  %s  %s  pid=%d  rc=%d\n", tb, e->src >= 1 && e->src <= REC_SRC_PSI ? rec_src_names[e->src] : "?",
           kind, who, (int)e->pid, (int)e->rc);
    if (body[0]) printf("    %s\n", body);
    return e->src == REC_SRC_PSI;
}

static int cmd_trace(const char *since_arg) {
    const char *name = getenv("SCHEMA_RECORDER_SHM"), *dir = getenv("SCHEMA_RECORDER_DIR");
    uint64_t since = 0;
    int fd, psi = 0;
    static rec_shm_t snap;
    const rec_shm_t *m;
    struct dirent **nl;
    uint32_t i;
    int n;

    if (since_arg) {
        int h, mi;
        char *end;
        if (sscanf(since_arg, "%d:%d", &h, &mi) == 2 && h >= 0 && h < 24 && mi >= 0 && mi < 60) {
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = 0;
            since = (uint64_t)mktime(&tm);
            if (since > (uint64_t)now) since -= 86400;
        } else {
            since = strtoull(since_arg, &end, 10);
            if (*end || !*since_arg) { fprintf(stderr, "usage: schema-ctl trace [--since <epoch|HH:MM>]\n"); return 1; }
        }
    }
    fd = shm_open(name ? name : REC_SHM_NAME, O_RDONLY, 0);
    m = fd < 0 ? NULL : rec_shm_map(fd);
    if (fd >= 0) close(fd);
    if (!m || rec_snapshot_ring(m, &snap) != 0) {
        fprintf(stderr, "schema-ctl: no flight recorder running (enable schema-recorder.svc)\n");
        return 1;
    }
    if (geteuid() == 0 && (n = scandir(dir ? dir : REC_EVENTS_DIR, &nl, NULL, alphasort)) >= 0) {
        int k;
        uint64_t lo = since;
        if (!lo && snap.count) lo = snap.ring[(snap.head - snap.count) % REC_RING].ts;
        for (k = 0; k < n; k++) {
            char path[512], line[1024];
            FILE *f;
            if (!strncmp(nl[k]->d_name, "events-", 7) && strstr(nl[k]->d_name, ".log")) {
                snprintf(path, sizeof path, "%s/%s", dir ? dir : REC_EVENTS_DIR, nl[k]->d_name);
                f = fopen(path, "r");
                while (f && fgets(line, sizeof line, f)) {
                    rec_event_t e;
                    if (rec_parse(line, &e) == 0 && e.ts >= lo) (void)trace_ev(&e, e.msg);
                }
                if (f) fclose(f);
            }
            free(nl[k]);
        }
        free(nl);
        return 0;
    }
    for (i = 0; i < snap.count; i++) {
        const rec_event_t *e = &snap.ring[(snap.head - snap.count + i) % REC_RING];
        if (e->ts >= since) psi |= trace_ev(e, e->msg);
    }
    if (psi) puts("(run as root for full command lines)");
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
        "  reload <svc>           exec_reload= or reload_signal= on a running service\n"
        "  reexec [--force] [<path>]\n"
        "                         replace the PID 1 binary in place (default: the\n"
        "                         one it booted from); services keep running.\n"
        "                         Runs verify --quiet first and refuses on exit 1, 2\n"
        "                         or 3, or when verify can't run; --force skips it\n"
        "  start <svc>            start a service (alias: up)\n"
        "  stop <svc>             stop it and hold it down (alias: down)\n"
        "  restart <svc>\n"
        "  add <svc|path>         load one .svc file at runtime; a bare name\n"
        "                         means %s/<name>.svc\n"
        "  cat <svc>              print <svc>.svc and its drop-ins in the order\n"
        "                         they apply (read from disk, not from PID 1)\n"
        "  top                    live per-service CPU and memory (runs schema-top;\n"
        "                         no root needed)\n"
        "  log [-f] [-n N] <svc>\n"
        "                         the service's log: the last 50 lines (-n 0:\n"
        "                         all); -f follows it across logrotate. Reads\n"
        "                         /var/log/schema-init (or /run/log/...); root\n"
        "                         for system services. `log rail` is the boot\n"
        "                         rail's log\n"
        "  graph [<svc>]          dependency graph as Graphviz DOT (A -> B: A waits\n"
        "                         for B; boxes are groups, red = missing); read\n"
        "                         from disk. View: schema-ctl graph | dot -Tsvg\n"
        "  trace [--since <t>]    flight recorder: recent crashes, session ends and\n"
        "                         pressure episodes, oldest first; <t> is an epoch\n"
        "                         or HH:MM today. Read from shared memory; root sees\n"
        "                         full command lines\n"
        "  verify [--quiet] [--accept] [--arming]\n"
        "                         before a reboot: would the next boot come back, and\n"
        "                         would the way back work? Checks the boot entry GRUB\n"
        "                         picks next, its services against the last good\n"
        "                         boot's baseline, the boot guard's fallback, and the\n"
        "                         disks. Read-only; root for the full report. Exit 0\n"
        "                         pass, 1 FAIL, 2 unaccepted CHANGE or lost baseline,\n"
        "                         3 unresolved, 64 usage. --quiet: findings and the\n"
        "                         result only; --accept: accept the current CHANGE\n"
        "                         set (refused while anything FAILs); --arming:\n"
        "                         judge the guard as if armed (schema-snapshot arm)\n"
        "  verify --write-baseline [--wait-settled <secs>]\n"
        "                         save what this boot runs (the running binary's\n"
        "                         --check) as the baseline the next boot is diffed\n"
        "                         against; --wait-settled first waits for <secs>\n"
        "                         (60 is the norm) of healthy services, no restarts\n"
        "  pet <svc>              feed a service's watchdog\n"
        "  reset [<svc>]          clear restart/dormant counters and retry <svc>;\n"
        "                         no argument clears every counter and retries\n"
        "                         only what failed, not what was stopped or finished\n"
        "  reboot [--force]\n"
        "  poweroff [--force]     from a terminal (stdin and stderr ttys), run\n"
        "                         verify --quiet first and refuse on exit 1 or 2;\n"
        "                         unresolved or unavailable: warn and go ahead.\n"
        "                         --force skips it; scripts are never gated\n"
        "\n"
        "  --user <cmd...>        talk to this user's schema-user instead of PID 1\n"
        "  --user daemon-reload   re-import the user's unit files, then reload\n"
        "  --help                 this text\n"
        "  --version              print version and exit\n"
        "\n"
        "Talks to PID 1 over %s.\nExits 1 when PID 1 replies \"err:\".\n", SVC_DIR, CTL_SOCK_PATH);
}

/* schema-import --user's manifest: enabled user units it did not generate,
 * one "<unit>\t<reason>" per line. Shown so none is silently dead. */
static const char *skip_manifest;

static void json_str(const char *s) {
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20) printf("\\u%04x", (unsigned char)*s);
        else putchar(*s);
    }
    putchar('"');
}

enum { SKIP_TEXT, SKIP_KV, SKIP_JSON };

/* json: `comma` = the engine's array already has entries */
static void print_skipped(int mode, int comma) {
    char line[1024];
    FILE *f = fopen(skip_manifest, "re");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');
        if (!tab) continue;
        *tab = '\0';
        if (nl) *nl = '\0';
        if (mode == SKIP_KV) {
            printf("service.%s.state=skipped\nservice.%s.reason=%s\n", line, line, tab + 1);
        } else if (mode == SKIP_JSON) {
            printf("%s\n    {\n      \"name\": ", comma ? "," : "");
            json_str(line);
            printf(",\n      \"pid\": 0,\n      \"state\": \"skipped\",\n      \"restarts\": 0,\n      \"reason\": ");
            json_str(tab + 1);
            printf("\n    }");
            comma = 1;
        } else {
            printf("  %-24s skipped (%s)\n", line, tab + 1);
        }
    }
    fclose(f);
}

/* 0: whole reply (lone "." line stripped), 1: cut short, -1: no reply
 * (reported, unless quiet: then errno says why) */
static int ctl_request(const char *req, char **reply, int quiet) {
    char *rbuf;
    size_t cap = 65536, rlen = 0;
    ssize_t n;
    int fd, whole = 0;
    struct sockaddr_un addr;

    *reply = NULL;
    rbuf = malloc(cap);
    if (!rbuf) { perror("malloc"); return -1; }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); free(rbuf); return -1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ctl_sock, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int first_errno = errno;
        int connected = 0;
        /* try local fallback only when explicitly requested */
        if (!user_mode && getenv("SCHEMA_CTL_FALLBACK")) {
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, "./run/schema-init.sock", sizeof(addr.sun_path) - 1);
            connected = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        }
        if (!connected && quiet) {
            close(fd);
            free(rbuf);
            errno = first_errno;
            return -1;
        }
        if (!connected) {
            fprintf(stderr, "%s: %s\n", ctl_sock, strerror(first_errno));
            if (user_mode && first_errno == ENOENT)
                fprintf(stderr, "no socket there — is schema-user running for this user?\n");
            else if (user_mode)
                ;
            else if (first_errno == EACCES || first_errno == EPERM)
                /* not echoing req: the client can't know PID 1's (or a hook's) commands, and
                   "try sudo schema-ctl <typo>" reads as if the command exists */
                fprintf(stderr, "schema-ctl talks to PID 1 as root: run it with sudo\n");
            else if (first_errno == ENOENT)
                fprintf(stderr, "no socket there — is schema-init running as PID 1?\n");
            close(fd);
            free(rbuf);
            return -1;
        }
    }

    write(fd, req, strlen(req));

    rbuf[0] = '\0';
    for (;;) {
        if (rlen + 1 >= cap) {
            char *nb = realloc(rbuf, cap * 2);
            if (!nb) { perror("realloc"); close(fd); free(rbuf); return -1; }
            rbuf = nb;
            cap *= 2;
        }
        n = read(fd, rbuf + rlen, cap - 1 - rlen);
        if (n <= 0) break;
        rlen += (size_t)n;
        rbuf[rlen] = '\0';
        if (rlen >= 2 && rbuf[rlen - 2] == '.' && rbuf[rlen - 1] == '\n' && (rlen == 2 || rbuf[rlen - 3] == '\n')) {
            rbuf[rlen - 2] = '\0';
            whole = 1;
            break;
        }
    }
    close(fd);
    *reply = rbuf;
    return !whole;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* what had failed (not critical) when the wait settled: recorded beside the baseline */
static char settled_failed[1024];

static int wait_settled(unsigned window_s) {
    const uint64_t give_up = mono_ms() + settle_give_up_ms(window_s);
    settle_t st = {0};
    int conn_err = 0;
    for (;;) {
        char *kv;
        /* PID 1 may not answer yet: retried every second, reported once at the end */
        int r = ctl_request("status --kv\n", &kv, 1);
        conn_err = r < 0 ? errno : 0;
        if (r == 0 && strncmp(kv, "err:", 4)) {
            if (settle_step(&st, kv, mono_ms(), window_s)) {
                settle_failed(kv, settled_failed, sizeof settled_failed);
                free(kv);
                return 0;
            }
        } else
            memset(&st, 0, sizeof st);
        free(kv);
        if (mono_ms() >= give_up) {
            if (conn_err) fprintf(stderr, "schema-ctl: %s: %s\n", ctl_sock, strerror(conn_err));
            return -1;
        }
        sleep(1);
    }
}

static char *slurp_fd(int fd, size_t *len) {
    size_t cap = 65536, n = 0;
    char *b = malloc(cap);
    ssize_t r;
    if (!b) return NULL;
    while ((r = read(fd, b + n, cap - n)) != 0) {
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        n += (size_t)r;
        if (n == cap) {
            char *nb = realloc(b, cap *= 2);
            if (!nb) { free(b); return NULL; }
            b = nb;
        }
    }
    *len = n;
    return b;
}

/* 1 when the running image is not the one PID 1 booted; -1 when that can't be told (said why) */
static int reexecd(const char *exe) {
    char *kv, *ver, image[64];
    size_t len;
    int p[2], st, r;
    pid_t pid;
    struct stat ist;
    if (stat("/proc/1/exe", &ist)) {
        fprintf(stderr, "schema-ctl: /proc/1/exe: %s\n", strerror(errno));
        return -1;
    }
    snprintf(image, sizeof image, "%llu:%llu", (unsigned long long)ist.st_dev, (unsigned long long)ist.st_ino);
    r = ctl_request("status --kv\n", &kv, 1);
    if (r != 0 || !strncmp(kv, "err:", 4)) {
        fprintf(stderr, "schema-ctl: PID 1 did not answer status --kv%s%s; whether it was re-exec'd since boot is not known\n",
                r < 0 ? ": " : "", r < 0 ? strerror(errno) : "");
        free(kv);
        return -1;
    }
    if (pipe2(p, O_CLOEXEC) < 0) {
        free(kv);
        return -1;
    }
    if ((pid = fork()) == 0) {
        dup2(p[1], 1);
        execl("/proc/1/exe", exe, "--version", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    ver = pid < 0 ? NULL : slurp_fd(p[0], &len);
    close(p[0]);
    while (pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    if (!ver) {
        fprintf(stderr, "schema-ctl: could not run /proc/1/exe --version\n");
        free(kv);
        return -1;
    }
    ver[len] = '\0';
    r = reexecd_since_boot(kv, ver, image);
    free(kv);
    free(ver);
    return r;
}

static int cmd_write_baseline(long wait_s) {
    char exe[PATH_MAX] = "", cmdline[4096] = "";
#ifdef SCHEMA_VERIFY_TESTHOOKS
    const char *dir = getenv("SCHEMA_VERIFY_DIR");
#else
    const char *dir = NULL;
#endif
    int fd, p[2], status;
    size_t len;
    char *out;
    ssize_t n;
    pid_t pid;

    if (geteuid() != 0) {
        fprintf(stderr, "schema-ctl verify --write-baseline: needs root (it runs /proc/1/exe)\n");
        return 1;
    }
    n = readlink("/proc/1/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = '\0';
    if (!pid1_exe_is_schema_init(exe)) {
        fprintf(stderr, "schema-ctl: PID 1 is %s, not schema-init — nothing to baseline\n", exe[0] ? exe : "unknown");
        return 1;
    }
    if (wait_s >= 0 && wait_settled((unsigned)wait_s) < 0) {
        fprintf(stderr, "schema-ctl: services did not settle within %llu minutes — baseline not written\n",
                (unsigned long long)(settle_give_up_ms((unsigned)wait_s) / 60000));
        return 3;
    }
    int rx = reexecd(exe);
    if (rx > 0)
        fprintf(stderr, "schema-ctl: PID 1 was re-exec'd since boot; the baseline is written at the next good boot\n");
    else if (rx)
        fprintf(stderr, "schema-ctl: baseline not written\n");
    if (rx)
        return 1;
    fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    n = fd < 0 ? -1 : read(fd, cmdline, sizeof cmdline - 1);
    if (fd >= 0) close(fd);
    if (n <= 0) { fprintf(stderr, "schema-ctl: /proc/cmdline: %s\n", strerror(errno)); return 1; }
    cmdline[n] = '\0';
    cmdline[strcspn(cmdline, "\n")] = '\0';

    if (pipe2(p, O_CLOEXEC) < 0) { perror("pipe"); return 1; }
    pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        /* the running image, even when dnf has replaced the file on disk */
        char *av[] = { "/proc/1/exe", "--check", "--root", "/", "--cmdline", cmdline, NULL };
        dup2(p[1], 1);
        execv(av[0], av);
        fprintf(stderr, "schema-ctl: /proc/1/exe: %s\n", strerror(errno));
        _exit(127);
    }
    close(p[1]);
    out = slurp_fd(p[0], &len);
    close(p[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) ;
    if (!out) { perror("read"); return 1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) > 1) {
        fprintf(stderr, "schema-ctl: /proc/1/exe --check failed — baseline not written\n");
        free(out);
        return 1;
    }
    /* an older PID 1 binary rejects --check with usage and exit 1 */
    if (len < sizeof BASELINE_MAGIC - 1 || memcmp(out, BASELINE_MAGIC, sizeof BASELINE_MAGIC - 1)) {
        fprintf(stderr, "schema-ctl: PID 1's binary has no --check (older than this schema-ctl) — no baseline\n");
        free(out);
        return 1;
    }
    if (WEXITSTATUS(status) == 1)
        fprintf(stderr, "schema-ctl: warning: some services did not load on this boot — the baseline records them as they are\n");
    if (!dir || !dir[0]) {
        dir = VERIFY_DIR_DEFAULT;
        mkdir("/var/lib/schema-init", 0755);
    }
    if (baseline_write(dir, out, len, time(NULL)) < 0) {
        if (errno == EROFS)
            fprintf(stderr, "schema-ctl: %s: var not persistent (tmpfs) — baseline not written\n", dir);
        else if (errno == EPERM)
            fprintf(stderr, "schema-ctl: %s: not owned by uid %d — refusing to write the baseline there\n", dir, (int)geteuid());
        else
            fprintf(stderr, "schema-ctl: %s/baseline: %s\n", dir, strerror(errno));
        free(out);
        return 1;
    }
    free(out);
    /* always rewritten, so an older boot's list never outlives its baseline */
    if (verify_state_put(dir, "boot-failed", settled_failed, strlen(settled_failed)) < 0)
        fprintf(stderr, "schema-ctl: %s/boot-failed: %s\n", dir, strerror(errno));
    printf("baseline written: %s/baseline\n", dir);
    if (settled_failed[0])
        printf("failed at this boot (not critical, not blocking): %s\n", settled_failed);
    return 0;
}

int main(int argc, char **argv) {
    char cmd[256];
    char *rbuf;
    int i, r, whole;
#ifdef SCHEMA_VERIFY_TESTHOOKS
    const char *tsock = getenv("SCHEMA_VERIFY_CTL_SOCK");
    if (tsock && tsock[0]) ctl_sock = tsock;
#endif

    if (argc > 1 && strcmp(argv[1], "--user") == 0) {
        static char usock[PATH_MAX], udir[PATH_MAX], uskip[PATH_MAX];
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        /* log reads $XDG_STATE_HOME, not the runtime dir: ssh sessions often lack it */
        if (argc > 2 && !strcmp(argv[2], "log"))
            return ctl_log_main(argc - 1, argv + 1, 1);
        if (!xdg || !xdg[0]) {
            fprintf(stderr, "schema-ctl --user: XDG_RUNTIME_DIR is not set\n");
            return 1;
        }
        snprintf(usock, sizeof usock, "%s/schema-user/ctl.sock", xdg);
        snprintf(udir, sizeof udir, "%s/schema-user/services", xdg);
        snprintf(uskip, sizeof uskip, "%s/schema-user/skipped", xdg);
        skip_manifest = uskip;
        ctl_sock = usock;
        svc_dir = udir;
        user_mode = 1;
        argv++;
        argc--;
    }

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

    if (strcmp(argv[1], "verify") == 0) {
        long wait_s = -1;
        char *end;
        verify_opts_t vo = { 0 };
        int bad = user_mode;
        if (!user_mode && argc == 3 && !strcmp(argv[2], "--write-baseline"))
            return cmd_write_baseline(-1);
        if (!user_mode && argc == 5 && !strcmp(argv[2], "--write-baseline") && !strcmp(argv[3], "--wait-settled")) {
            wait_s = strtol(argv[4], &end, 10);
            if (argv[4][0] && !*end && wait_s >= 0 && wait_s <= 3600) return cmd_write_baseline(wait_s);
            bad = 1;
        }
        for (i = 2; i < argc && !bad; i++) {
            if (!strcmp(argv[i], "--quiet")) vo.quiet = 1;
            else if (!strcmp(argv[i], "--accept")) vo.accept = 1;
            else if (!strcmp(argv[i], "--arming")) vo.arming = 1;
            else bad = 1;
        }
        if (bad) {
            fprintf(stderr, "usage: schema-ctl verify [--quiet] [--accept] [--arming]\n"
                            "       schema-ctl verify --write-baseline [--wait-settled <secs>]\n");
            /* not 1: a gate reads 1 as a FAIL */
            return EX_USAGE;
        }
        verify_opts_env(&vo);
        return verify_run(&vo, stdout);
    }

    if (strcmp(argv[1], "log") == 0)
        return ctl_log_main(argc, argv, user_mode);
    if (!user_mode && strcmp(argv[1], "top") == 0) {
        argv[1] = "schema-top";
        execvp(argv[1], argv + 1);
        fprintf(stderr, "schema-ctl: schema-top: %s\n", strerror(errno));
        return 1;
    }
    if (strcmp(argv[1], "graph") == 0) {
        if (argc > 3 || (argc == 3 && (!argv[2][0] || strchr(argv[2], '/')))) {
            fprintf(stderr, "usage: schema-ctl graph [<svc>]\n");
            return 1;
        }
        return cmd_graph(argc == 3 ? argv[2] : NULL);
    }

    if (strcmp(argv[1], "trace") == 0) {
        if (argc == 2) return cmd_trace(NULL);
        if (argc == 4 && !strcmp(argv[2], "--since")) return cmd_trace(argv[3]);
        fprintf(stderr, "usage: schema-ctl trace [--since <epoch|HH:MM>]\n");
        return 1;
    }

    if (strcmp(argv[1], "cat") == 0) {
        char path[SVC_DROPIN_PATH], files[SVC_DROPIN_MAX + 1][SVC_DROPIN_PATH];
        int nf;
        if (argc != 3 || !argv[2][0] || strchr(argv[2], '/')) {
            fprintf(stderr, "usage: schema-ctl cat <svc>\n");
            return 1;
        }
        snprintf(path, sizeof path, "%s/%s.svc", svc_dir, argv[2]);
        if (access(path, R_OK) < 0) {
            fprintf(stderr, "schema-ctl: %s: %s\n", path, strerror(errno));
            return 1;
        }
        snprintf(files[0], sizeof files[0], "%s", path);
        nf = svc_dropin_list(path, files + 1, SVC_DROPIN_MAX);
        if (nf < 0) {
            fprintf(stderr, "schema-ctl: %s: a drop-in directory is not owned by root or is writable by "
                    "others — PID 1 refuses this service; drop-ins not shown\n", path);
            nf = 0;
        }
        nf++;
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

    char addpath[SVC_DROPIN_PATH];
    if (strcmp(argv[1], "add") == 0 && argc == 3 && argv[2][0] && !strchr(argv[2], '/')) {
        size_t l = strlen(argv[2]);
        int has_ext = l > 4 && strcmp(argv[2] + l - 4, ".svc") == 0;
        snprintf(addpath, sizeof addpath, "%s/%s%s", svc_dir, argv[2], has_ext ? "" : ".svc");
        argv[2] = addpath;
    }

    if (!user_mode && (!strcmp(argv[1], "reboot") || !strcmp(argv[1], "poweroff") || !strcmp(argv[1], "reexec"))) {
        int force = 0, n = 2;
        for (i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--force")) force = 1;
            else argv[n++] = argv[i];
        }
        argc = n;
        char act[PATH_MAX + 8];
        snprintf(act, sizeof act, "%s%s%s", argv[1], argc == 3 ? " " : "", argc == 3 ? argv[2] : "");
        if (verify_gate(act, force, isatty(0) && isatty(2), stderr)) return 1;
    }

    cmd[0] = '\0';
    for (i = 1; i < argc; i++) {
        size_t len = strlen(cmd);
        if (len >= sizeof(cmd) - 2) break;
        if (i > 1) { strncat(cmd, " ", sizeof(cmd) - len - 1); len = strlen(cmd); }
        strncat(cmd, argv[i], sizeof(cmd) - len - 1);
    }
    strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);

    r = ctl_request(cmd, &rbuf, 0);
    if (r < 0) return 1;
    whole = r == 0;

    i = !whole || strncmp(rbuf, "err:", 4) == 0;
    char *jend = NULL;
    if (skip_manifest && !i && argc == 3 && !strcmp(argv[1], "status") && !strcmp(argv[2], "--json"))
        jend = strstr(rbuf, "\n  ]\n}");
    if (jend) {
        /* splice the skipped units in before the engine's closing "]" */
        *jend = '\0';
        printf("%s", rbuf);
        print_skipped(SKIP_JSON, jend > rbuf && jend[-1] == '}');
        printf("\n  ]\n}\n");
    } else if (!i && !strcmp(argv[1], "status") && argc <= 3 &&
               !(argc == 3 && (!strcmp(argv[2], "--json") || !strcmp(argv[2], "--kv"))) && colour_wanted(1)) {
        status_colourize(rbuf, stdout);
    } else if (whole && !strncmp(rbuf, "err: unknown: ", 14)) {
        /* also what PID 1 says to a known verb missing its argument ("start") */
        rbuf[strcspn(rbuf, "\n")] = '\0';
        fprintf(stderr, "schema-ctl: unknown command '%s' — schema-ctl --help lists the commands and their arguments\n",
                rbuf + 14);
    } else {
        printf("%s", rbuf);
    }
    if (skip_manifest && !i && !strcmp(argv[1], "status") &&
        (argc == 2 || (argc == 3 && !strcmp(argv[2], "--kv"))))
        print_skipped(argc == 3 ? SKIP_KV : SKIP_TEXT, 0);
    if (!whole)
        fprintf(stderr, "schema-ctl: reply cut short: the command may not have run\n");
    free(rbuf);
    return i;
}
