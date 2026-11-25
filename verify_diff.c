#include "verify_diff.h"
#include "verify_baseline.h"
#include "svc_keys.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAGIC_PFX "schema-verify-baseline v"
#define SHOW_NAMES 6

typedef struct { const char *svc, *key, *val, *src; } ent_t;
typedef struct { const char *name, *members; } grp_t;
typedef struct {
    char *buf;
    char **hdr, **err, **note;
    int nh, ne, nn;
    ent_t *ent;
    int nent;
    grp_t *grp;
    int ngrp;
    int systemd;
} snap_t;
/* old/new NULL: the line isn't there */
typedef struct { const char *key, *old, *new, *src, *svc; } chg_t;
typedef struct { finding_t *out; int max, n; FILE *hash; } ctx_t;

/* grows at each power of two */
#define PUSH(arr, n, ...) do { \
        if (!((n) & ((n) - 1))) (arr) = realloc((arr), (size_t)((n) ? (n) * 2 : 1) * sizeof *(arr)); \
        (arr)[(n)++] = __VA_ARGS__; \
    } while (0)

static void add(ctx_t *c, fkind_t k, const char *fmt, ...) {
    va_list ap;
    if (c->n < c->max) {
        c->out[c->n].kind = k;
        va_start(ap, fmt);
        vsnprintf(c->out[c->n].text, sizeof c->out[c->n].text, fmt, ap);
        va_end(ap);
    }
    c->n++;
}

/* the hash covers every changed value in full, not the summary lines */
static void hash_line(ctx_t *c, const char *what, const char *old, const char *new) {
    fprintf(c->hash, "%s %c%s -> %c%s\n", what, old ? '=' : '-', old ? old : "", new ? '=' : '-', new ? new : "");
}

/* N from line 1, -1 if it isn't exactly "schema-verify-baseline vN" */
static int version(const char *t) {
    const char *p = t + sizeof MAGIC_PFX - 1;
    int v = 0;
    if (strncmp(t, MAGIC_PFX, sizeof MAGIC_PFX - 1) || *p < '0' || *p > '9') return -1;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (v > 100000) return -1;
        v = v * 10 + (*p - '0');
    }
    return *p == '\n' || !*p ? v : -1;
}

static int pfx(const char *s, const char *p) {
    return !strncmp(s, p, strlen(p));
}

static void body_line(snap_t *s, char *l) {
    char *tab = strchr(l, '\t'), *eq;
    ent_t e = { 0 };
    size_t ll, best = 0;
    if (tab) {
        *tab = '\0';
        e.src = pfx(tab + 1, "src=") ? tab + 5 : tab + 1;
    }
    eq = strchr(l, '=');
    e.val = eq ? eq + 1 : "";
    if (eq) *eq = '\0';
    ll = strlen(l);
    for (int i = 0; i < svc_keys_n; i++) {
        size_t kl = strlen(svc_keys[i]);
        if (kl > best && ll > kl + 1 && l[ll - kl - 1] == '.' && !strcmp(l + ll - kl, svc_keys[i])) best = kl;
    }
    if (!best && pfx(l, "grp.") && ll > 12 && !strcmp(l + ll - 8, ".members")) {
        l[ll - 8] = '\0';
        PUSH(s->grp, s->ngrp, (grp_t){ l + 4, e.val });
        return;
    }
    /* a key this binary doesn't know (an older serializer's): its last segment */
    if (!best) {
        char *d = strrchr(l, '.');
        best = d ? ll - (size_t)(d - l) - 1 : 0;
    }
    e.svc = l;
    e.key = l + ll - best;
    if (best && best < ll) l[ll - best - 1] = '\0';
    else e.key = "";
    PUSH(s->ent, s->nent, e);
}

static int ent_cmp(const void *a, const void *b) {
    const ent_t *x = a, *y = b;
    int c = strcmp(x->svc, y->svc);
    return c ? c : strcmp(x->key, y->key);
}

static void parse(snap_t *s, const char *text) {
    static const char *const hdr[] = { "binary_version=", "created=", "cmdline.", "hardening_default=", "svc_dir=" };
    int in_hdr = 1, after_dir = 0;
    char *p, *l;

    memset(s, 0, sizeof *s);
    s->buf = p = strdup(text);
    strsep(&p, "\n");
    while ((l = strsep(&p, "\n"))) {
        if (!*l) continue;
        if (in_hdr) {
            int h = pfx(l, "next_init=");
            for (size_t i = 0; !after_dir && !h && i < sizeof hdr / sizeof *hdr; i++) h = pfx(l, hdr[i]);
            if (h) {
                if (pfx(l, "svc_dir=")) after_dir = 1;
                if (!strcmp(l, "next_init=systemd")) s->systemd = 1;
                if (pfx(l, "next_init=")) in_hdr = 0;
                if (!pfx(l, "binary_version=") && !pfx(l, "created=")) PUSH(s->hdr, s->nh, l);
                continue;
            }
            in_hdr = 0;
        }
        if (pfx(l, "error: ")) PUSH(s->err, s->ne, l);
        else if (pfx(l, "note: ")) PUSH(s->note, s->nn, l);
        else body_line(s, l);
    }
    if (s->nent) qsort(s->ent, (size_t)s->nent, sizeof *s->ent, ent_cmp);
}

static void snap_free(snap_t *s) {
    free(s->buf);
    free(s->hdr);
    free(s->err);
    free(s->note);
    free(s->ent);
    free(s->grp);
}

/* a value as shown: long ones cut, an empty one visible */
static const char *dv(char *b, size_t n, const char *v) {
    if (!v) return "unset";
    if (!*v) return "\"\"";
    if (strlen(v) > 120) snprintf(b, n, "%.120s…", v);
    else snprintf(b, n, "%s", v);
    return b;
}

static void hdr_key(const char *l, char *k, size_t n) {
    if (pfx(l, "cmdline.")) snprintf(k, n, "%.*s", (int)(strchr(l, '=') ? strcspn(l, "=") + 1 : strlen(l)), l);
    else snprintf(k, n, "%.*s", (int)strcspn(l, "="), l);
}

/* every value KEY has, in order, joined by SEP; NULL for none */
static char *hdr_vals(const snap_t *s, const char *key, char sep) {
    char k[512], *v = NULL;
    size_t vl = 0;
    for (int i = 0; i < s->nh; i++) {
        const char *l = s->hdr[i], *val;
        hdr_key(l, k, sizeof k);
        if (strcmp(k, key)) continue;
        /* a cmdline word is shown whole: "rd.break" has no value of its own */
        val = pfx(l, "cmdline.") ? l + 8 : l + strlen(k) + 1;
        if (!v) v = strdup(val);
        else {
            v = realloc(v, vl + strlen(val) + 2);
            v[vl] = sep;
            strcpy(v + vl + 1, val);
        }
        vl = strlen(v);
    }
    return v;
}

static void diff_header(ctx_t *c, const snap_t *b, const snap_t *n) {
    char k[512], k2[512], d1[160], d2[160], what[600];
    const snap_t *side[2] = { b, n };
    for (int si = 0; si < 2; si++)
        for (int i = 0; i < side[si]->nh; i++) {
            int seen = 0;
            hdr_key(side[si]->hdr[i], k, sizeof k);
            /* each key once, at its first appearance */
            for (int sj = 0; sj <= si && !seen; sj++)
                for (int j = 0; j < (sj == si ? i : side[sj]->nh) && !seen; j++) {
                    hdr_key(side[sj]->hdr[j], k2, sizeof k2);
                    seen = !strcmp(k, k2);
                }
            if (seen) continue;
            /* compared and hashed as a sequence: of repeated root= the last one is in
             * force. \x1f, not \n: the hash sorts its lines, a cmdline word may hold a space */
            char *ov = hdr_vals(b, k, '\x1f'), *nv = hdr_vals(n, k, '\x1f');
            if (!ov || !nv || strcmp(ov, nv)) {
                snprintf(what, sizeof what, "hdr %s", k);
                hash_line(c, what, ov, nv);
                for (char *q = ov; q && *q; q++) if (*q == '\x1f') *q = ' ';
                for (char *q = nv; q && *q; q++) if (*q == '\x1f') *q = ' ';
                add(c, F_CHANGE, "%s  %s → %s", k, dv(d1, sizeof d1, ov), dv(d2, sizeof d2, nv));
            }
            free(ov);
            free(nv);
        }
}

static int has_svc(const snap_t *s, const char *svc) {
    int lo = 0, hi = s->nent - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2, c = strcmp(s->ent[m].svc, svc);
        if (!c) return 1;
        if (c < 0) lo = m + 1;
        else hi = m - 1;
    }
    return 0;
}

static int nstrcmp(const char *a, const char *b) {
    if (!a || !b) return (a != NULL) - (b != NULL);
    return strcmp(a, b);
}

static int chg_cmp(const void *a, const void *b) {
    const chg_t *x = a, *y = b;
    int c = strcmp(x->key, y->key);
    if (!c) c = nstrcmp(x->old, y->old);
    if (!c) c = nstrcmp(x->new, y->new);
    if (!c) c = nstrcmp(x->src, y->src);
    return c ? c : strcmp(x->svc, y->svc);
}

/* the next offset after an snprintf at o that returned r: a cut write leaves it at the
 * terminator, so later appends write nothing */
static size_t advance(size_t o, int r, size_t sz) {
    return r < 0 || (size_t)r >= sz - o ? sz - 1 : o + (size_t)r;
}

/* one finding per run of equal (key, old, new, src): "syscall_deny  unset → default (default)   63 services: …" */
static void emit_runs(ctx_t *c, fkind_t kind, chg_t *v, int n) {
    char d1[160], d2[160], names[SHOW_NAMES * 66 + 8];
    if (n) qsort(v, (size_t)n, sizeof *v, chg_cmp);
    for (int i = 0, j; i < n; i = j) {
        size_t o = 0;
        names[0] = '\0';
        for (j = i; j < n && !strcmp(v[j].key, v[i].key) && !nstrcmp(v[j].old, v[i].old) &&
                    !nstrcmp(v[j].new, v[i].new) && !nstrcmp(v[j].src, v[i].src); j++)
            if (j - i < SHOW_NAMES) o = advance(o, snprintf(names + o, sizeof names - o, " %.63s", v[j].svc), sizeof names);
        if (j - i > SHOW_NAMES) snprintf(names + o, sizeof names - o, " …");
        if (kind == F_NOTE)
            add(c, kind, "%s  source %s → %s   %d service%s:%s", v[i].key, v[i].old ? v[i].old : "none",
                v[i].new ? v[i].new : "none", j - i, j - i > 1 ? "s" : "", names);
        else
            add(c, kind, "%s  %s → %s%s%s%s   %d service%s:%s", v[i].key, dv(d1, sizeof d1, v[i].old),
                dv(d2, sizeof d2, v[i].new), v[i].src ? " (" : "", v[i].src ? v[i].src : "", v[i].src ? ")" : "",
                j - i, j - i > 1 ? "s" : "", names);
    }
}

static int refused(const snap_t *n, const char *svc) {
    char p[128];
    snprintf(p, sizeof p, "error: %s.svc:", svc);
    for (int i = 0; i < n->ne; i++) if (pfx(n->err[i], p)) return 1;
    return 0;
}

static void diff_services(ctx_t *c, const snap_t *b, const snap_t *n) {
    chg_t *chg = NULL, *src = NULL;
    int nc = 0, ns = 0, i = 0, j = 0;
    const char *last_rm = "", *last_new = "";
    char what[300];

    while (i < b->nent || j < n->nent) {
        const ent_t *x = i < b->nent ? &b->ent[i] : NULL, *y = j < n->nent ? &n->ent[j] : NULL;
        int cmp = !x ? 1 : !y ? -1 : ent_cmp(x, y);
        if (cmp == 0) {
            if (strcmp(x->val, y->val)) {
                PUSH(chg, nc, (chg_t){ x->key, x->val, y->val, y->src, x->svc });
                snprintf(what, sizeof what, "%s.%s", x->svc, x->key);
                hash_line(c, what, x->val, y->val);
            } else if (nstrcmp(x->src, y->src)) {
                PUSH(src, ns, (chg_t){ x->key, x->src, y->src, NULL, x->svc });
            }
            i++, j++;
        } else if (cmp < 0) {
            if (has_svc(n, x->svc)) {
                PUSH(chg, nc, (chg_t){ x->key, x->val, NULL, NULL, x->svc });
                snprintf(what, sizeof what, "%s.%s", x->svc, x->key);
                hash_line(c, what, x->val, NULL);
            } else if (strcmp(last_rm, x->svc) && !refused(n, x->svc)) {
                /* a deliberate removal is a change to accept, not a failure */
                add(c, F_CHANGE, "%s  removed (.svc gone)", x->svc);
                snprintf(what, sizeof what, "%s removed", x->svc);
                hash_line(c, what, "", "");
            }
            last_rm = x->svc;
            i++;
        } else {
            if (has_svc(b, y->svc)) {
                PUSH(chg, nc, (chg_t){ y->key, NULL, y->val, y->src, y->svc });
                snprintf(what, sizeof what, "%s.%s", y->svc, y->key);
                hash_line(c, what, NULL, y->val);
            } else if (strcmp(last_new, y->svc)) {
                add(c, F_WARN, "%s  new service, not in the baseline", y->svc);
            }
            last_new = y->svc;
            j++;
        }
    }
    emit_runs(c, F_CHANGE, chg, nc);
    emit_runs(c, F_NOTE, src, ns);
    free(chg);
    free(src);

    char d1[160], d2[160];
    for (int k = 0; k < b->ngrp + n->ngrp; k++) {
        const grp_t *g = k < b->ngrp ? &b->grp[k] : &n->grp[k - b->ngrp];
        const char *ov = NULL, *nv = NULL;
        int dup = 0;
        for (int m = 0; m < b->ngrp; m++) if (!strcmp(b->grp[m].name, g->name)) ov = b->grp[m].members;
        for (int m = 0; m < n->ngrp; m++) if (!strcmp(n->grp[m].name, g->name)) nv = n->grp[m].members;
        /* a group in both is visited from the baseline side only */
        if (k >= b->ngrp) dup = ov != NULL;
        if (dup || !nstrcmp(ov, nv)) continue;
        snprintf(what, sizeof what, "grp %s", g->name);
        hash_line(c, what, ov, nv);
        add(c, F_CHANGE, "group %s members  %s → %s", g->name, dv(d1, sizeof d1, ov), dv(d2, sizeof d2, nv));
    }
}

static int line_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* sha256 of the hash lines, sorted: the same change set hashes the same whatever the order */
static void finish_hash(char *buf, char out[65]) {
    char **v = NULL, *p = buf, *l, *joined = NULL;
    int n = 0;
    size_t jl = 0;
    FILE *m;
    while ((l = strsep(&p, "\n"))) if (*l) PUSH(v, n, l);
    if (n) qsort(v, (size_t)n, sizeof *v, line_cmp);
    m = open_memstream(&joined, &jl);
    for (int i = 0; i < n; i++) fprintf(m, "%s\n", v[i]);
    fclose(m);
    sha256_hex(joined, jl, out);
    free(joined);
    free(v);
}

int diff_baseline(const char *base, const char *next, finding_t *out, int max, char changes_hash[65]) {
    int bv = version(base);
    char *hb = NULL;
    size_t hl = 0;
    snap_t b, n;
    ctx_t c = { out, max, 0, NULL };

    if (bv < 0 || bv > BASELINE_VERSION) return -1;
    c.hash = open_memstream(&hb, &hl);
    if (version(next) != BASELINE_VERSION) {
        add(&c, F_FAIL, "next boot's --check output is not a schema-verify-baseline v%d snapshot", BASELINE_VERSION);
        goto done;
    }
    parse(&b, base);
    parse(&n, next);
    diff_header(&c, &b, &n);
    for (int i = 0; i < n.ne; i++) {
        int old = 0;
        for (int k = 0; k < b.ne && !old; k++) old = !strcmp(b.err[k], n.err[i]);
        if (old) add(&c, F_WARN, "%s: already failing at the last good boot", n.err[i] + 7);
        else add(&c, F_FAIL, "%s", n.err[i] + 7);
    }
    for (int i = 0; i < n.nn; i++)
        if (strstr(n.note[i], "dropped") || strstr(n.note[i], "is not an absolute path"))
            add(&c, F_WARN, "%s", n.note[i] + 6);
    if (bv < BASELINE_VERSION) add(&c, F_WARN, "baseline from an older format; refreshed at the next good boot");
    /* a systemd entry runs no .svc files: nothing to compare them with */
    else if (!n.systemd) diff_services(&c, &b, &n);
    snap_free(&b);
    snap_free(&n);
done:
    fclose(c.hash);
    finish_hash(hb, changes_hash);
    free(hb);
    return c.n;
}

static char *read_file(const char *dir, const char *name) {
    char p[4096], *buf = NULL;
    size_t len = 0;
    ssize_t r;
    int fd;
    FILE *m;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if ((fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) < 0) return NULL;
    m = open_memstream(&buf, &len);
    char chunk[4096];
    while ((r = read(fd, chunk, sizeof chunk)) > 0) fwrite(chunk, 1, (size_t)r, m);
    fclose(m);
    close(fd);
    if (r < 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

int baseline_state(const char *dir, char **base_out) {
    char p[4096];
    int v;
    *base_out = NULL;
    snprintf(p, sizeof p, "%s/established", dir);
    if (access(p, F_OK) < 0) return BASE_NEVER;
    char *b = read_file(dir, "baseline");
    if (!b) return BASE_LOST;
    v = version(b);
    if (v < 0 || v > BASELINE_VERSION) {
        free(b);
        return BASE_LOST;
    }
    *base_out = b;
    return BASE_OK;
}

int accepted_matches(const char *dir, const char hash[65]) {
    char *a = read_file(dir, "accepted");
    int ok = a && !strncmp(a, hash, 64) && (a[64] == '\n' || !a[64]);
    free(a);
    return ok;
}

int accept_write(const char *dir, const char hash[65]) {
    char line[66];
    snprintf(line, sizeof line, "%.64s\n", hash);
    return verify_state_put(dir, "accepted", line, 65);
}
