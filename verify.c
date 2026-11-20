#include "verify.h"
#include "verify_baseline.h"
#include "verify_boot.h"
#include "verify_diff.h"
#include "verify_disk.h"
#include "verify_root.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAXENV 64
#define MAXENTS 64

typedef enum { T_HEAD, T_PASS, T_NOTE, T_WARN, T_CHANGE, T_LOST, T_UNRES, T_FAIL } tag_t;
typedef struct { tag_t t; char *s; } line_t;

typedef struct {
    line_t *l;
    int n;
    char env[MAXENV][2][256];
    int nenv;
    bls_entry_t *ents;
    int nents;
    root_report_t *rep; /* per entry, filled on first use */
    int *done;
    const verify_opts_t *o;
    char hash[65];
    int changes; /* diff_baseline's CHANGEs: the acceptable kind */
    char rej[4][256]; /* grubenv-named entries whose file could not be read */
    int nrej;
    int cut; /* more entries than MAXENTS: the rest were not read */
} ctx_t;

static void emit(ctx_t *c, tag_t t, const char *fmt, ...) {
    va_list ap;
    char *s;
    va_start(ap, fmt);
    if (vasprintf(&s, fmt, ap) < 0) s = NULL;
    va_end(ap);
    if (!(c->n & (c->n - 1))) c->l = realloc(c->l, (size_t)(c->n ? c->n * 2 : 1) * sizeof *c->l);
    c->l[c->n++] = (line_t){ t, s };
}

/* an input that couldn't be read: unprivileged, that is what it needs */
static void unreadable(ctx_t *c, const char *what, const char *path, int e) {
    if (e == EACCES || e == EPERM) emit(c, T_UNRES, "needs root: %s %s", what, path);
    else emit(c, T_UNRES, "%s %s: %s", what, path, strerror(e));
}

static const char *env_get(ctx_t *c, const char *k) {
    for (int i = c->nenv - 1; i >= 0; i--)
        if (!strcmp(c->env[i][0], k)) return c->env[i][1][0] ? c->env[i][1] : NULL;
    return NULL;
}

void verify_opts_env(verify_opts_t *o) {
    static const struct { const char *var, *def; size_t off; } m[] = {
#define F(f, var, def) { var, def, offsetof(verify_opts_t, f) }
        F(state_dir, "SCHEMA_VERIFY_DIR", VERIFY_DIR_DEFAULT),
        F(grubenv, "SCHEMA_VERIFY_GRUBENV", "/boot/grub2/grubenv"),
        F(bls_dir, "SCHEMA_VERIFY_BLS_DIR", NULL),
        F(grubcfg, "SCHEMA_VERIFY_GRUBCFG", "/boot/grub2/grub.cfg"),
        F(fstab, "SCHEMA_VERIFY_FSTAB", "/etc/fstab"),
        F(kmsg_path, "SCHEMA_VERIFY_KMSG", "/dev/kmsg"),
        F(devdir, "SCHEMA_VERIFY_DEVDIR", NULL),
        F(esp, "SCHEMA_VERIFY_ESP", "/boot/efi"),
        F(cmdline_txt, "SCHEMA_VERIFY_CMDLINE_TXT", NULL),
        F(pi_init, "SCHEMA_VERIFY_PI_INIT", "/usr/sbin/schema-init"),
        F(inspect, "SCHEMA_VERIFY_INSPECT", NULL),
#undef F
    };
    for (size_t i = 0; i < sizeof m / sizeof *m; i++) {
        /* tests only: a shipped schema-ctl, run as root by the gates, takes no paths from env */
#ifdef SCHEMA_VERIFY_TESTHOOKS
        const char *v = getenv(m[i].var);
#else
        const char *v = NULL;
#endif
        *(const char **)((char *)o + m[i].off) = v && *v ? v : m[i].def;
    }
    if (!o->cmdline_txt)
        o->cmdline_txt = access("/boot/firmware/cmdline.txt", F_OK) ? "/boot/cmdline.txt" : "/boot/firmware/cmdline.txt";
}

static char *slurp(int fd, size_t *len) {
    char *b = NULL, chunk[8192];
    size_t l = 0;
    ssize_t r;
    FILE *m = open_memstream(&b, &l);
    for (;;) {
        r = read(fd, chunk, sizeof chunk);
        if (r > 0) fwrite(chunk, 1, (size_t)r, m);
        else if (r == 0 || (errno != EINTR && errno != EPIPE)) break;
    }
    fclose(m);
    if (len) *len = l;
    return b;
}

static char *read_path(const char *path, int *err) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *err = errno;
        return NULL;
    }
    char *b = slurp(fd, NULL);
    close(fd);
    return b;
}

/* stdout and stderr of argv, in full; the exit code, -1 when it didn't run, -2 when
 * there was nowhere to capture it (the child never starts: it would write into the report) */
static int run_capture(char *const argv[], char **out, size_t *olen, char **err, size_t *elen) {
    int fo = memfd_create("schema-verify-out", MFD_CLOEXEC), fe = memfd_create("schema-verify-err", MFD_CLOEXEC);
    if (fo < 0 || fe < 0) {
        int e = errno;
        if (fo >= 0) close(fo);
        if (fe >= 0) close(fe);
        *out = *err = NULL;
        *olen = *elen = 0;
        errno = e;
        return -2;
    }
    pid_t p = fork();
    int st = 0;
    if (p == 0) {
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        dup2(fo, 1);
        dup2(fe, 2);
        execv(argv[0], argv);
        _exit(127);
    }
    while (p > 0 && waitpid(p, &st, 0) < 0 && errno == EINTR) ;
    lseek(fo, 0, SEEK_SET);
    lseek(fe, 0, SEEK_SET);
    *out = slurp(fo, olen);
    *err = slurp(fe, elen);
    close(fo);
    close(fe);
    if (p < 0 || !WIFEXITED(st) || WEXITSTATUS(st) == 127) return -1;
    return WEXITSTATUS(st);
}

/* the stand-in child: its stderr carries the records, its stdout the --check report */
static int inspect_stub(ctx_t *c, const bls_entry_t *e, root_report_t *r) {
    char opts[4096], *out, *ctl;
    size_t ol, cl;
    if (entry_precheck(e, c->env, c->nenv, opts, sizeof opts, r)) return r->fail ? 1 : 3;
    if (run_capture((char *[]){ (char *)c->o->inspect, (char *)e->id, opts, NULL }, &out, &ol, &ctl, &cl) == -2) {
        cl = (size_t)asprintf(&ctl, "U cannot capture the inspection: memfd_create: %s\n", strerror(errno));
        ol = 0;
    }
    root_report_fill(ctl, cl, out, ol, r);
    free(ctl);
    return r->fail ? 1 : r->nunres ? 3 : 0;
}

static root_report_t *inspect(ctx_t *c, int i) {
    if (!c->done[i]) {
        if (c->o->inspect) inspect_stub(c, &c->ents[i], &c->rep[i]);
        else root_inspect(&c->ents[i], c->env, c->nenv, NULL, &c->rep[i]);
        c->done[i] = 1;
    }
    return &c->rep[i];
}

static int ent_index(ctx_t *c, const char *id) {
    for (int i = 0; id && i < c->nents; i++)
        if (!strcmp(c->ents[i].id, id)) return i;
    return -1;
}

static int count_errors(const char *t) {
    int n = 0;
    for (const char *p = t; p && (p = strstr(p, "\nerror: ")); p++) n++;
    return n;
}

/* SEV for what the entry's own checks found: FAIL for a boot that will happen, WARN
 * for a spare. errs: count its --check error: lines here (the actual next boot's are
 * judged against the baseline under Services instead) */
static void report_entry(ctx_t *c, const char *role, int i, tag_t sev, int errs) {
    root_report_t *r = inspect(c, i);
    char who[400];
    snprintf(who, sizeof who, "%s%s%s", role, *role ? " " : "", c->ents[i].id);
    for (int k = 0; k < r->nreasons; k++) emit(c, sev, "%s: %s", who, r->reasons[k]);
    for (int k = 0; k < r->nunres; k++) {
        if (sev == T_WARN) emit(c, T_WARN, "%s: unresolved: %s", who, r->unres[k]);
        else if (!strcmp(r->unres[k], "needs root")) emit(c, T_UNRES, "needs root: %s", who);
        else emit(c, T_UNRES, "%s: %s", who, r->unres[k]);
    }
    if (!r->fail && !r->nunres)
        emit(c, T_PASS, "%s: entry checks pass; init=%s%s", who, r->init_path, r->is_schema_init ? " (schema-init)" : "");
    int ne = errs ? count_errors(r->check_out) : 0;
    if (ne) emit(c, T_WARN, "%s: %d service%s fail to load there", who, ne, ne > 1 ? "s" : "");
}

/* rpmvercmp's rule, enough for BLS file names */
int bls_vercmp(const char *a, const char *b) {
    while (*a || *b) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b) break;
        const char *sa = a, *sb = b;
        int num = !!isdigit((unsigned char)*a); /* isdigit gives 2048, not 1 */
        if (num != !!isdigit((unsigned char)*b)) return num ? 1 : -1;
        if (num) {
            while (*sa == '0') sa++;
            while (*sb == '0') sb++;
            for (a = sa; isdigit((unsigned char)*a); a++) ;
            for (b = sb; isdigit((unsigned char)*b); b++) ;
            if (a - sa != b - sb) return a - sa > b - sb ? 1 : -1;
        } else {
            for (a = sa; isalpha((unsigned char)*a); a++) ;
            for (b = sb; isalpha((unsigned char)*b); b++) ;
        }
        size_t la = (size_t)(a - sa), lb = (size_t)(b - sb);
        int r = memcmp(sa, sb, la < lb ? la : lb);
        if (r || la != lb) return r ? (r > 0 ? 1 : -1) : (la > lb ? 1 : -1);
    }
    return *a ? 1 : *b ? -1 : 0;
}

/* blscfg lists entries newest first; only an unset saved_entry depends on it. By file
 * name: Fedora's blscfg compares the version key first, which names the same kernel. */
static int ent_cmp(const void *x, const void *y) {
    return bls_vercmp(((const bls_entry_t *)y)->id, ((const bls_entry_t *)x)->id);
}

void bls_sort(bls_entry_t *ents, int n) {
    qsort(ents, (size_t)n, sizeof *ents, ent_cmp);
}

static int conf_file(const struct dirent *d) {
    size_t l = strlen(d->d_name);
    return l > 5 && !strcmp(d->d_name + l - 5, ".conf");
}

/* blsdir= is relative to GRUB's root: a separate /boot, or the btrfs top level, whose
 * first component is then the running root's subvolume */
static const char *bls_dir_for(ctx_t *c, char *buf, size_t n) {
    const char *bd = env_get(c, "blsdir");
    char line[4096], root[4096] = "", r[4096], mp[4096];
    if (c->o->bls_dir) return c->o->bls_dir;
    if (!bd) return "/boot/loader/entries";
    if (boot_is_separate("/proc/self/mountinfo")) {
        snprintf(buf, n, "/boot%s", bd);
        return buf;
    }
    FILE *f = fopen("/proc/self/mountinfo", "r");
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "%*s %*s %*s %4095s %4095s", r, mp) == 2 && !strcmp(mp, "/"))
            snprintf(root, sizeof root, "%s", r);
    if (f) fclose(f);
    size_t rl = strlen(root);
    if (!strcmp(root, "/")) rl = 0;
    if (!root[0] || (rl && (strncmp(bd, root, rl) || bd[rl] != '/'))) {
        emit(c, T_UNRES, "grubenv blsdir=%s is outside the running root (%s): boot entries not read", bd,
             root[0] ? root : "unknown");
        return NULL;
    }
    snprintf(buf, n, "%s", bd + rl);
    return buf;
}

static int load_entries(ctx_t *c) {
    char buf[PATH_MAX], p[PATH_MAX + 256];
    const char *dir = bls_dir_for(c, buf, sizeof buf);
    struct dirent **d;
    if (!dir) return -1;
    int nd = scandir(dir, &d, conf_file, alphasort);
    if (nd < 0) {
        unreadable(c, "boot entries", dir, errno);
        return -1;
    }
    c->cut = nd > MAXENTS;
    if (c->cut) emit(c, T_UNRES, "too many boot entries in %s (%d, %d read)", dir, nd, MAXENTS);
    for (int i = 0; i < nd; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, d[i]->d_name);
        if (c->nents < MAXENTS && !bls_parse(p, &c->ents[c->nents])) {
            c->nents++;
        } else if (c->nents < MAXENTS) {
            /* the guard's own entry unreadable is unknown, not "names no entry" */
            int e = errno, key = 0;
            size_t il = strlen(d[i]->d_name) - 5;
            static const char *const k[] = { "saved_entry", "next_entry", "schema_fallback_id" };
            for (size_t j = 0; j < 3; j++) {
                const char *v = env_get(c, k[j]);
                key |= v && strlen(v) == il && !strncmp(v, d[i]->d_name, il);
            }
            emit(c, key ? T_UNRES : T_WARN, "boot entry %s could not be read: %s", p, strerror(e));
            if (key && c->nrej < 4) snprintf(c->rej[c->nrej++], sizeof c->rej[0], "%.*s", (int)il, d[i]->d_name);
        }
        free(d[i]);
    }
    free(d);
    if (!c->nents) {
        emit(c, T_UNRES, "no boot entries in %s", dir);
        return -1;
    }
    bls_sort(c->ents, c->nents);
    return 0;
}

static int has_line(const char *t, const char *pfx) {
    size_t l = strlen(pfx);
    for (const char *p = t; p; p = (p = strchr(p, '\n')) ? p + 1 : NULL)
        if (!strncmp(p, pfx, l)) return 1;
    return 0;
}

static void services_report(ctx_t *c, char *text) {
    char *base;
    if (has_line(text, "next_init=systemd")) {
        emit(c, T_PASS, "next boot is systemd (schema-init-pi's choice); services not checked");
        return;
    }
    int bs = baseline_state(c->o->state_dir, &base), n = -1, max = 256;
    finding_t *f = NULL;
    /* the 0700 state dir hides the marker too: unprivileged, that is not "never established" */
    if (bs == BASE_NEVER && geteuid() != 0 && access(c->o->state_dir, X_OK) && errno == EACCES) bs = -1;
    if (bs == BASE_OK) {
        do {
            max = n > max ? n : max;
            f = realloc(f, (size_t)max * sizeof *f);
            n = diff_baseline(base, text, f, max, c->hash);
        } while (n > max);
        free(base);
    }
    if (n < 0) {
        /* without a baseline every error: line is the next boot's own */
        for (char *p = text; (p = strstr(p, "\nerror: ")); p++)
            emit(c, T_FAIL, "%.*s", (int)strcspn(p + 8, "\n"), p + 8);
        if (bs < 0)
            emit(c, T_UNRES, "needs root: baseline %s", c->o->state_dir);
        else if (bs == BASE_NEVER)
            emit(c, T_WARN, "no baseline yet; the first good boot writes one");
        else
            emit(c, T_LOST, "baseline lost: %s/established is there but the baseline is missing or unreadable; "
                            "the next good boot rewrites it (--accept can't clear this)", c->o->state_dir);
        free(f);
        return;
    }
    int bad = 0;
    for (int i = 0; i < n; i++) {
        static const tag_t map[] = { [F_FAIL] = T_FAIL, [F_CHANGE] = T_CHANGE, [F_WARN] = T_WARN, [F_NOTE] = T_NOTE };
        emit(c, map[f[i].kind], "%s", f[i].text);
        c->changes += f[i].kind == F_CHANGE;
        bad |= f[i].kind == F_FAIL || f[i].kind == F_CHANGE;
    }
    if (!bad) emit(c, T_PASS, "services match the baseline");
    free(f);
}

/* the Pi's next boot: schema-init --check on cmdline.txt; check.c applies the wrapper's rule */
/* NULL with *nocap set: the output could not be captured, which is unresolved, not "not applicable" */
static char *pi_check(ctx_t *c, int *nocap) {
    int e;
    char *cl = read_path(c->o->cmdline_txt, &e), *out, *err;
    size_t ol, el;
    if (!cl) return NULL;
    cl[strcspn(cl, "\n")] = '\0';
    int rc = run_capture((char *[]){ (char *)c->o->pi_init, "--check", "--root", "/", "--cmdline", cl, NULL },
                         &out, &ol, &err, &el);
    int re = errno;
    free(cl);
    free(err);
    *nocap = rc == -2;
    errno = re;
    if ((rc != 0 && rc != 1) || strncmp(out, BASELINE_MAGIC, sizeof BASELINE_MAGIC - 1)) {
        free(out);
        return NULL;
    }
    return out;
}

static int fstab_has(const char *fstab, const char *dir) {
    FILE *f = setmntent(fstab, "r");
    struct mntent *m;
    int found = 0;
    while (f && !found && (m = getmntent(f))) found = !strcmp(m->mnt_dir, dir);
    if (f) endmntent(f);
    return found;
}

/* ported from scripts/schema-snapshot esp_grubcfg_is_stub: a grub.cfg with 00_header
 * on the ESP is a full config that loads the ESP's own grubenv, so the guard's
 * variables in /boot/grub2/grubenv never reach it */
static void esp_check(ctx_t *c) {
    char efi[PATH_MAX], p[PATH_MAX + 300], line[1024];
    int found = 0, full = 0;
    snprintf(efi, sizeof efi, "%s/EFI", c->o->esp);
    DIR *d = opendir(efi);
    if (!d) {
        if (fstab_has(c->o->fstab, "/boot/efi")) emit(c, T_UNRES, "ESP %s not mounted: its grub.cfg not checked", c->o->esp);
        else emit(c, T_NOTE, "no ESP in fstab: ESP grub.cfg not applicable");
        return;
    }
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s/grub.cfg", efi, e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) {
            if (errno != ENOENT && errno != ENOTDIR) unreadable(c, "ESP config", p, errno);
            continue;
        }
        found++;
        int is_full = 0;
        while (!is_full && fgets(line, sizeof line, f)) is_full = !strncmp(line, "### BEGIN /etc/grub.d/00_header", 31);
        fclose(f);
        if (is_full)
            emit(c, T_FAIL, "ESP /EFI/%s/grub.cfg is a full GRUB config that loads its own grubenv, not the guard's; "
                            "replace it with a configfile stub to /boot/grub2", e->d_name);
        full += is_full;
    }
    closedir(d);
    if (!found) emit(c, T_NOTE, "no grub.cfg on the ESP");
    else if (!full) emit(c, T_PASS, "ESP grub.cfg is a stub");
}

static void bootok_check(ctx_t *c, root_report_t *r) {
    const char *t = r ? r->check_out : NULL, *p;
    char exe[512] = "";
    if (r && !t && r->init_path[0] && !r->is_schema_init) {
        emit(c, T_NOTE, "schema-bootok: next boot is %s, not schema-init; not checked", r->init_path);
        return;
    }
    if (!t) {
        emit(c, T_UNRES, "schema-bootok: the next boot's services could not be read; not checked");
        return;
    }
    if ((p = strstr(t, "\nerror: schema-bootok.svc:"))) {
        p += 26;
        emit(c, T_FAIL, "schema-bootok.svc does not load on the next boot:%.*s", (int)strcspn(p, "\n"), p);
        return;
    }
    if (!(p = strstr(t, "\nschema-bootok.exec="))) {
        emit(c, T_FAIL, "schema-bootok is not installed on the next boot's rail: an armed guard would roll back good boots");
        return;
    }
    p += 20;
    snprintf(exe, sizeof exe, "%.*s", (int)strcspn(p, "\t\n"), p);
    if (r->bootok_exec == 1) emit(c, T_PASS, "schema-bootok loads on the next boot; exec=%s is executable", exe);
    else if (r->bootok_exec == 2) emit(c, T_FAIL, "schema-bootok exec=%s is not executable in the next boot's root", exe);
    else emit(c, T_UNRES, "schema-bootok exec=%s: not checked", exe);
}

static void ways_back(ctx_t *c, int nbrc, const nextboot_t *nb, root_report_t *act) {
    int e = 0, guard = env_get(c, "schema_fallback_id") || env_get(c, "boot_counter");
    char *cfg = read_path(c->o->grubcfg, &e);
    for (int i = 0; i < c->nents; i++)
        guard |= !strncmp(c->ents[i].id, "schema-snap-", 12) || !strncmp(c->ents[i].id, "schema-good", 11);
    guard |= cfg && strstr(cfg, "schema_fallback_id");
    /* a box that never used the guard has no way back to judge; arm's own gate runs this
     * after a snapshot exists, when it does */
    if (!guard) {
        emit(c, T_NOTE, "boot guard not in use (no schema_fallback_id, boot_counter, schema-snap-* or schema-good* entry); not checked");
        free(cfg);
        return;
    }
    int fb = ent_index(c, nb->fallback);
    /* an unreadable or cut-off entry is unknown (already UNRESOLVED), not missing */
    int rej = c->cut;
    for (int i = 0; nb->fallback && i < c->nrej; i++) rej |= !strcmp(c->rej[i], nb->fallback);
    if (nbrc == 2 && rej) ;
    else if (nbrc == 2) emit(c, T_FAIL, "schema_fallback_id=%s names no boot entry", nb->fallback);
    else if (fb < 0) emit(c, T_FAIL, "no schema-snap-* entry for the guard to fall back to");
    else report_entry(c, "guard fallback", fb, T_FAIL, 1);
    for (int i = 0; i < c->nents; i++)
        if (i != fb && (!strncmp(c->ents[i].id, "schema-snap-", 12) || !strncmp(c->ents[i].id, "schema-good", 11)))
            report_entry(c, "", i, T_WARN, 1);

    if (!cfg) unreadable(c, "GRUB config", c->o->grubcfg, e);
    else if (grubcfg_live_fallback(cfg)) emit(c, T_PASS, "09_schema_fallback is live in %s", c->o->grubcfg);
    else emit(c, T_FAIL, "09_schema_fallback is not live code in %s: the guard can't fire "
                         "(make install-bootguard; grub2-mkconfig -o %s)", c->o->grubcfg, c->o->grubcfg);
    free(cfg);
    esp_check(c);
    bootok_check(c, act);
}

static char *read_kmsg(const char *path, int *err) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        *err = errno;
        return NULL;
    }
    /* /dev/kmsg: one record per read, EAGAIN at the end, EPIPE past an overwritten one */
    char *b = slurp(fd, NULL);
    close(fd);
    return b;
}

static void disks(ctx_t *c) {
    int e = 0, n = -1, max = 128, bad = 0;
    finding_t *f = NULL;
    if (access(c->o->fstab, R_OK)) {
        unreadable(c, "fstab", c->o->fstab, errno);
        return;
    }
    char *k = read_kmsg(c->o->kmsg_path, &e);
    if (!k) unreadable(c, "kernel log", c->o->kmsg_path, e);
    do {
        max = n > max ? n : max;
        f = realloc(f, (size_t)max * sizeof *f);
        n = disk_findings(c->o->fstab, k, c->o->devdir, f, max);
    } while (n > max);
    for (int i = 0; i < n; i++) {
        static const tag_t map[] = { [F_FAIL] = T_FAIL, [F_CHANGE] = T_CHANGE, [F_WARN] = T_WARN, [F_NOTE] = T_NOTE };
        emit(c, map[f[i].kind], "%s", f[i].text);
        bad |= f[i].kind == F_FAIL;
    }
    if (!bad) emit(c, T_PASS, "required local filesystems present");
    free(f);
    free(k);
}

int verify_run(const verify_opts_t *o, FILE *out) {
    ctx_t c = { .o = o };
    nextboot_t nb = { 0 };
    int nbrc = 0, act = -1, nor = -1, pi = 0, boot_ok = 0;
    char *pi_out = NULL;

    c.ents = calloc(MAXENTS, sizeof *c.ents);
    c.rep = calloc(MAXENTS, sizeof *c.rep);
    c.done = calloc(MAXENTS, sizeof *c.done);

    emit(&c, T_HEAD, "Next boot");
    c.nenv = grubenv_read(o->grubenv, c.env, MAXENV);
    if (c.nenv < 0 && errno == ENOENT && !access(o->cmdline_txt, R_OK)) {
        pi = 1;
        c.nenv = 0;
        emit(&c, T_NOTE, "boot entries: not applicable (no GRUB; the next boot is %s)", o->cmdline_txt);
    } else if (c.nenv < 0) {
        unreadable(&c, "grubenv", o->grubenv, errno);
    } else if (!load_entries(&c)) {
        boot_ok = 1;
        nbrc = nextboot_resolve(c.env, c.nenv, c.ents, c.nents, &nb);
        act = ent_index(&c, nb.actual);
        nor = ent_index(&c, nb.normal);
        const char *saved = env_get(&c, "saved_entry"), *next = env_get(&c, "next_entry");
        if (act < 0)
            emit(&c, T_UNRES, "actual next boot: which entry GRUB picks could not be resolved (saved_entry=%s next_entry=%s "
                              "boot_counter=%s)", saved ? saved : "", next ? next : "",
                 env_get(&c, "boot_counter") ? env_get(&c, "boot_counter") : "");
        else
            report_entry(&c, "actual next boot", act, T_FAIL, 0);
        if (nor < 0) emit(&c, T_UNRES, "normal default: saved_entry=%s names no boot entry", saved ? saved : "");
        else if (nor == act) emit(&c, T_NOTE, "normal default %s: the same entry", c.ents[nor].id);
        else report_entry(&c, "normal default", nor, T_FAIL, 1);
        if (nb.fallback) emit(&c, T_NOTE, "guard fallback %s: checked under Ways back", nb.fallback);
    }

    emit(&c, T_HEAD, "Services");
    root_report_t *ar = act >= 0 ? inspect(&c, act) : NULL;
    if (pi) {
        int nocap = 0;
        if ((pi_out = pi_check(&c, &nocap))) services_report(&c, pi_out);
        else if (nocap) emit(&c, T_UNRES, "services: cannot capture --check output: %s", strerror(errno));
        else emit(&c, T_NOTE, "services: not applicable (the next boot's inputs could not be determined)");
    } else if (!ar && geteuid() != 0 && !o->inspect) {
        emit(&c, T_UNRES, "needs root: the next boot's services");
    } else if (!ar) {
        emit(&c, T_UNRES, "services: the next boot entry is unresolved; not checked");
    } else if (ar->check_out) {
        services_report(&c, ar->check_out);
    } else if (ar->init_path[0] && !ar->is_schema_init) {
        emit(&c, ar->fail ? T_NOTE : T_PASS, "next boot is %s; services not checked", ar->init_path);
    } else if (geteuid() != 0 && !o->inspect) {
        emit(&c, T_UNRES, "needs root: the next boot's services");
    } else {
        emit(&c, T_UNRES, "services: the next boot's --check gave no report; not checked");
    }

    emit(&c, T_HEAD, "Ways back");
    if (pi) emit(&c, T_NOTE, "not applicable (no GRUB)");
    else if (!boot_ok && geteuid() != 0 && !o->inspect) emit(&c, T_UNRES, "needs root: ways back");
    else if (!boot_ok) emit(&c, T_UNRES, "boot entries unread: ways back not checked");
    else ways_back(&c, nbrc, &nb, ar);

    emit(&c, T_HEAD, "Disks");
    disks(&c);

    int fail = 0, change = 0, warn = 0, unres = 0, lost = 0, accepted = 0;
    for (int i = 0; i < c.n; i++) {
        fail += c.l[i].t == T_FAIL;
        change += c.l[i].t == T_CHANGE || c.l[i].t == T_LOST;
        lost += c.l[i].t == T_LOST;
        warn += c.l[i].t == T_WARN;
        unres += c.l[i].t == T_UNRES;
    }
    if (c.changes && accepted_matches(o->state_dir, c.hash)) accepted = 1;
    if (o->accept) {
        if (fail) fprintf(stderr, "schema-ctl verify: a FAIL can't be accepted; nothing written\n");
        else if (lost) fprintf(stderr, "schema-ctl verify: a lost baseline can't be accepted; the next good boot rewrites it\n");
        else if (!c.changes) fprintf(stderr, "schema-ctl verify: no change to accept\n");
        else if (!accepted && accept_write(o->state_dir, c.hash))
            fprintf(stderr, "schema-ctl verify: %s/accepted: %s\n", o->state_dir, strerror(errno));
        else accepted = 1;
    }

    int first = 1;
    for (int i = 0; i < c.n; i++) {
        static const char *const name[] = { [T_PASS] = "PASS", [T_NOTE] = "note", [T_WARN] = "WARN",
                                            [T_CHANGE] = "CHANGE", [T_LOST] = "CHANGE", [T_UNRES] = "UNRESOLVED",
                                            [T_FAIL] = "FAIL" };
        tag_t t = c.l[i].t;
        if (o->quiet && (t == T_HEAD || t == T_PASS || t == T_NOTE)) continue;
        if (t == T_HEAD) fprintf(out, "%s%s\n", first ? "" : "\n", c.l[i].s);
        else fprintf(out, "[%s%s] %s\n", name[t], t == T_CHANGE && accepted ? " accepted" : "", c.l[i].s);
        first = 0;
    }
    fprintf(out, "%sResult: %d fail, %d change%s, %d warn", o->quiet ? "" : "\n", fail, change,
            !change ? "" : accepted && !lost ? " (accepted)" : " (not accepted)", warn);
    if (unres) fprintf(out, ", %d unresolved", unres);
    fputc('\n', out);

    for (int i = 0; i < c.n; i++) free(c.l[i].s);
    for (int i = 0; i < MAXENTS; i++) free(c.rep[i].check_out);
    free(c.l);
    free(c.ents);
    free(c.rep);
    free(c.done);
    free(pi_out);
    if (fail) return 1;
    if ((change && !accepted) || lost) return 2;
    return unres ? 3 : 0;
}
