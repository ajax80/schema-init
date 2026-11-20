#include "check.h"
#include "engine.h"
#include "group.h"
#include "karg.h"
#include "pi_target.h"
#include "schema.h"
#include "service.h"
#include "svc_dropins.h"
#include "svc_serial.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char **v; int n, cap; } lines_t;

static void lines_add(lines_t *l, const char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = strdup(s);
}

static int str_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* cmdline words as the kernel splits them, up to a bare "--": the words after it
 * are init's argv, not boot options. Caller frees. */
static char (*boot_words(const char *cmdline, int *n))[512] {
    int max = (int)strlen(cmdline) / 2 + 1;
    char (*w)[512] = calloc(max, sizeof *w);
    *n = karg_split(cmdline, w, max);
    for (int i = 0; i < *n; i++)
        if (!strcmp(w[i], "--")) *n = i;
    return w;
}

/* The last word starting with key, as the kernel keeps it for init= and the env. */
static int word_val(const char *cmdline, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    int found = 0, nw;
    char (*w)[512] = boot_words(cmdline, &nw);
    for (int i = 0; i < nw; i++)
        if (strncmp(w[i], key, kl) == 0) {
            snprintf(out, n, "%s", w[i] + kl);
            found = 1;
        }
    free(w);
    return found;
}

static size_t root_len(const char *root) {
    size_t n = strlen(root);
    while (n && root[n - 1] == '/') n--;
    return n;
}

/* -1 off the Pi; else what schema-init-pi would exec next boot. */
static int pi_next_target(const char *root, const char *cmdline) {
    char init[PATH_MAX] = "", r[PATH_MAX / 2];
    struct pi_state ps;
    word_val(cmdline, "init=", init, sizeof init);
    const char *base = strrchr(init, '/');
    base = base ? base + 1 : init;
    if (strcmp(base, "schema-init-pi") != 0) return -1;
    snprintf(r, sizeof r, "%.*s", (int)root_len(root), root);
    pi_state_load(r, &ps);
    return pi_choose_target(&ps, pi_known_good_differs(r));
}

const char *check_svc_dir(const char *root, const char *cmdline, char *buf, size_t n) {
    int rl = (int)root_len(root);
    char v[PATH_MAX / 2];
    struct stat st;
    int pt = pi_next_target(root, cmdline);
    /* the wrapper unsets a cmdline SCHEMA_SVC_DIR and sets its own only for known-good */
    if (pt >= 0) {
        snprintf(buf, n, "%.*s%s", rl, root,
                 pt == PI_KNOWN_GOOD ? "/var/lib/schema-init/known-good/services" : SVC_DIR);
        return buf;
    }
    /* svc_dir_from_env()'s rule: absolute and an existing directory */
    if (word_val(cmdline, "SCHEMA_SVC_DIR=", v, sizeof v) && v[0] == '/') {
        snprintf(buf, n, "%.*s%s", rl, root, v);
        if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode)) return buf;
    }
    snprintf(buf, n, "%.*s%s", rl, root, SVC_DIR);
    return buf;
}

static int boot_critical(const char *w) {
    static const char *const keys[] = { "root=", "rootflags=", "init=", "rd.", "modprobe.",
                                        "SCHEMA_SVC_DIR=", "schema.hardening_default=" };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        if (strncmp(w, keys[i], strlen(keys[i])) == 0) return 1;
    return 0;
}

static int svc_file(const struct dirent *e) {
    size_t n = strlen(e->d_name);
    return n >= 5 && strcmp(e->d_name + n - 4, ".svc") == 0;
}

static int redirect(int fd) {
    fflush(stdout);
    fflush(stderr);
    dup2(fd, STDOUT_FILENO);
    return dup2(fd, STDERR_FILENO);
}

/* Loader lines name the root they were read under; the baseline must not. */
static void strip_root(char *s, const char *root, size_t rl) {
    if (!rl) return;
    for (char *p = s; (p = strstr(p, root));) {
        if (p[rl] == '/') memmove(p, p + rl, strlen(p + rl) + 1);
        else p++;
    }
}

static const char *untag(const char *s) {
    const char *e;
    if (s[0] == '[' && (e = strstr(s, "] "))) return e + 2;
    return s;
}

typedef struct { const char *s; int i; } ordered_t;

/* by key; a repeated key keeps cmdline order, since the last one is the one in force */
static int key_cmp(const void *a, const void *b) {
    const ordered_t *x = a, *y = b;
    size_t xl = strcspn(x->s, "="), yl = strcspn(y->s, "=");
    int c = memcmp(x->s, y->s, xl < yl ? xl : yl);
    if (c) return c;
    if (xl != yl) return xl < yl ? -1 : 1;
    return x->i - y->i;
}

static void print_header(const char *cmdline, int hon, const char *rel_dir) {
    char line[1024];
    printf("schema-verify-baseline v1\nbinary_version=%s\n", SCHEMA_INIT_VERSION);
    lines_t cl = { 0 };
    int nw;
    char (*w)[512] = boot_words(cmdline, &nw);
    for (int i = 0; i < nw; i++) {
        snprintf(line, sizeof line, "cmdline.%s", w[i]);
        if (boot_critical(w[i])) lines_add(&cl, line);
    }
    free(w);
    ordered_t *o = calloc(cl.n ? cl.n : 1, sizeof *o);
    for (int i = 0; i < cl.n; i++) o[i] = (ordered_t){ cl.v[i], i };
    qsort(o, cl.n, sizeof *o, key_cmp);
    for (int i = 0; i < cl.n; i++) puts(o[i].s);
    printf("hardening_default=%s\nsvc_dir=%s\n", hon ? "on" : "off", rel_dir);
}

int check_main(const char *root_arg, const char *cmdline, const char *svc_dir_override) {
    /* half-size root: every path built under it must still fit PATH_MAX */
    char root[PATH_MAX / 2], dir[PATH_MAX], path[PATH_MAX + 256], hd[64] = "";
    size_t rl = root_len(root_arg);
    snprintf(root, sizeof root, "%.*s", (int)rl, root_arg);

    /* pid1_hardening_default(), from the target's inputs instead of /proc and the live /etc */
    snprintf(path, sizeof path, "%s/etc/schema-init/hardening-default", root);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int have = fd >= 0;
    if (have) { ssize_t n = read(fd, hd, sizeof hd - 1); if (n > 0) hd[n] = '\0'; close(fd); }
    int hon = hardening_default_resolve(cmdline, have ? hd : NULL);
    service_set_hardening_default(hon);
    /* user=/group= resolve against the host's NSS, which is right only for
     * --root /; a foreign root needs a chroot (a later PR) */

    if (svc_dir_override && svc_dir_override[0] != '/') {
        fprintf(stderr, "schema-init --check: --svc-dir %s: must be absolute (it is read under --root)\n", svc_dir_override);
        return 2;
    }
    if (svc_dir_override) snprintf(dir, sizeof dir, "%s%s", root, svc_dir_override);
    else check_svc_dir(root, cmdline, dir, sizeof dir);
    /* services_load builds dir/<file> (and drop-in paths) in 512 bytes;
     * past that every file would fail to open and read as broken */
    if (strlen(dir) > SVC_DROPIN_PATH - 128) {
        fprintf(stderr, "schema-init --check: %s: path too long (the loader needs it under %d bytes)\n",
                dir, SVC_DROPIN_PATH - 128);
        return 2;
    }
    /* a systemd next boot runs none of these services; saying nothing would read as checked */
    static const char *const next_init[] = { "schema-init", "known-good", "systemd" };
    int pt = pi_next_target(root, cmdline);
    if (pt == PI_SYSTEMD) {
        print_header(cmdline, hon, dir + rl);
        printf("next_init=systemd\n");
        fflush(stdout);
        return 0;
    }
    /* PID 1's ./services fallback would read the caller's cwd, not the target */
    engine_paths.svc_fallback = NULL;

    FILE *cap = tmpfile();
    if (!cap) {
        fprintf(stderr, "schema-init --check: tmpfile: %s\n", strerror(errno));
        return 2;
    }
    int out = dup(STDOUT_FILENO), err = dup(STDERR_FILENO);
    redirect(fileno(cap));
    int cycles = engine_load(dir);

    /* Which files failed is decided by loading each one again, not by the
     * wording of the loader's messages. Their repeat chatter is discarded. */
    int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    redirect(devnull);
    struct dirent **ents;
    int ne = scandir(dir, &ents, svc_file, svc_dropin_cmp);
    char *bad = calloc(ne > 0 ? ne : 1, 1);
    char (*claimed)[64] = calloc(MAX_SERVICES, 64);
    int nclaimed = 0;
    for (int e = 0; e < ne; e++) {
        service_t *t = calloc(1, sizeof *t);
        snprintf(path, sizeof path, "%s/%s", dir, ents[e]->d_name);
        bad[e] = 1;
        if (service_load_one(path, t) != 0) continue;
        int k, c;
        for (k = 0; k < svc_count && strcmp(services[k].name, t->name); k++)
            ;
        for (c = 0; c < nclaimed && strcmp(claimed[c], t->name); c++)
            ;
        if (k < svc_count && c == nclaimed && nclaimed < MAX_SERVICES) {
            snprintf(claimed[nclaimed++], 64, "%s", t->name);
            bad[e] = 0;
        }
    }
    redirect(out);
    dup2(err, STDERR_FILENO);
    close(out);
    close(err);
    close(devnull);

    lines_t capl = { 0 };
    char line[1024];
    rewind(cap);
    while (fgets(line, sizeof line, cap)) {
        line[strcspn(line, "\n")] = 0;
        strip_root(line, root, rl);
        if (line[0]) lines_add(&capl, line);
    }
    fclose(cap);
    char *used = calloc(capl.n ? capl.n : 1, 1);

    print_header(cmdline, hon, dir + rl);
    if (pt >= 0) printf("next_init=%s\n", next_init[pt]);

    int errors = 0;
    for (int e = 0; e < ne; e++) {
        if (!bad[e]) continue;
        const char *dn = ents[e]->d_name, *reason = "not loaded";
        char rel[PATH_MAX + 256];
        /* by path only: a " name: " line may be another file's service of that name */
        snprintf(rel, sizeof rel, "%s/%s", dir + rl, dn);
        for (int i = 0; i < capl.n; i++)
            if (!used[i] && strstr(capl.v[i], rel)) {
                used[i] = 1;
                reason = untag(capl.v[i]);
                break;
            }
        /* masking is deliberate; an error here would block every re-exec */
        snprintf(path, sizeof path, "%s/%s.masked", dir, dn);
        if (access(path, F_OK) == 0) {
            printf("note: %s: masked\n", dn);
            continue;
        }
        printf("error: %s: %s\n", dn, reason);
        errors++;
    }
    if (cycles > 0) { printf("error: %s: dependency cycle\n", dir + rl); errors++; }
    if (svc_count == 0) { printf("error: %s: no services loaded\n", dir + rl); errors++; }
    for (int i = 0; i < capl.n; i++)
        if (!used[i]) printf("note: %s\n", untag(capl.v[i]));

    lines_t body = { 0 };
    char *ser = NULL;
    size_t sl = 0;
    FILE *ms = open_memstream(&ser, &sl);
    svc_serialize_all(services, svc_count, ms);
    fclose(ms);
    for (char *s = ser, *nl; s && *s; s = nl + 1) {
        nl = strchr(s, '\n');
        if (!nl) { lines_add(&body, s); break; }
        *nl = 0;
        lines_add(&body, s);
    }
    int ng;
    const group_t *g = engine_groups(&ng);
    for (int i = 0; i < ng; i++) {
        const char *m[MAX_MEMBERS];
        int o = snprintf(line, sizeof line, "grp.%s.members=", g[i].name);
        for (int k = 0; k < g[i].member_count; k++) m[k] = g[i].member_name[k];
        qsort(m, g[i].member_count, sizeof *m, str_cmp);
        for (int k = 0; k < g[i].member_count && o < (int)sizeof line - 1; k++) {
            int r = snprintf(line + o, sizeof line - o, "%s%s", k ? " " : "", m[k]);
            /* a cut write stops the list: o never passes the buffer */
            o = r < 0 || r >= (int)sizeof line - o ? (int)sizeof line - 1 : o + r;
        }
        lines_add(&body, line);
    }
    if (body.n) qsort(body.v, body.n, sizeof *body.v, str_cmp);
    for (int i = 0; i < body.n; i++) puts(body.v[i]);
    fflush(stdout);
    return errors ? 1 : 0;
}
