#ifndef UDEV_RULESET_H
#define UDEV_RULESET_H

#include "schema-udev.h"   /* safe_copy */
#include "path_id.h"   /* pi_parent, pi_sysattr, pi_subsystem, pi_base, pi_driver */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>

#define RK_KEY_MAX 32
#define RK_SUB_MAX 128
#define RK_VAL_MAX 512
#define RULE_MAX_CLAUSES 32

enum rule_op { OP_MATCH_EQ, OP_MATCH_NE, OP_ASSIGN, OP_ASSIGN_ADD, OP_ASSIGN_SUB, OP_ASSIGN_FINAL };

struct rule_clause {
    char key[RK_KEY_MAX];
    char subkey[RK_SUB_MAX];
    enum rule_op op;
    char val[RK_VAL_MAX];
};

struct rule { struct rule_clause clause[RULE_MAX_CLAUSES]; int nclause; };
struct ruleset { struct rule *rules; int n; int cap; };

/* Parse "KEY{sub}OP\"val\"" -> clause. Returns 0 / -1. */
static inline int ruleset_parse_clause(const char *s, struct rule_clause *out) {
    memset(out, 0, sizeof *out);
    while (*s == ' ' || *s == '\t') s++;
    const char *p = s;
    /* key = leading [A-Z_] run */
    while ((*p >= 'A' && *p <= 'Z') || *p == '_') p++;
    size_t klen = (size_t)(p - s);
    if (klen == 0 || klen >= RK_KEY_MAX) return -1;
    memcpy(out->key, s, klen); out->key[klen] = '\0';
    /* optional {subkey} */
    if (*p == '{') {
        const char *e = strchr(p, '}');
        if (!e) return -1;
        size_t sl = (size_t)(e - (p + 1));
        if (sl >= RK_SUB_MAX) return -1;
        memcpy(out->subkey, p + 1, sl); out->subkey[sl] = '\0';
        p = e + 1;
    }
    /* operator */
    if      (p[0] == '=' && p[1] == '=') { out->op = OP_MATCH_EQ;     p += 2; }
    else if (p[0] == '!' && p[1] == '=') { out->op = OP_MATCH_NE;     p += 2; }
    else if (p[0] == '+' && p[1] == '=') { out->op = OP_ASSIGN_ADD;   p += 2; }
    else if (p[0] == '-' && p[1] == '=') { out->op = OP_ASSIGN_SUB;   p += 2; }
    else if (p[0] == ':' && p[1] == '=') { out->op = OP_ASSIGN_FINAL; p += 2; }
    else if (p[0] == '=')                { out->op = OP_ASSIGN;       p += 1; }
    else return -1;
    /* quoted value */
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return -1;
    size_t vl = (size_t)(e - p);
    if (vl >= RK_VAL_MAX) return -1;
    memcpy(out->val, p, vl); out->val[vl] = '\0';
    return 0;
}

/* Split on top-level commas (quotes are literal), parse each as a clause. */
static inline int ruleset_parse_line(const char *line, struct rule *out) {
    out->nclause = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        /* find end of this clause: next top-level comma */
        const char *seg = p;
        int inq = 0;
        while (*p && !(*p == ',' && !inq)) { if (*p == '"') inq = !inq; p++; }
        size_t seglen = (size_t)(p - seg);
        char buf[RK_KEY_MAX + RK_SUB_MAX + RK_VAL_MAX + 8];
        if (seglen == 0 || seglen >= sizeof buf) { if (*p == ',') p++; continue; }
        memcpy(buf, seg, seglen); buf[seglen] = '\0';
        if (out->nclause >= RULE_MAX_CLAUSES) return -1;
        if (ruleset_parse_clause(buf, &out->clause[out->nclause]) != 0) return -1;
        out->nclause++;
        if (*p == ',') p++;
    }
    return out->nclause;
}

static inline int ruleset_append(struct ruleset *rs, const struct rule *r) {
    if (rs->n >= rs->cap) {
        int ncap = rs->cap ? rs->cap * 2 : 64;
        struct rule *nr = realloc(rs->rules, (size_t)ncap * sizeof *nr);
        if (!nr) return -1;
        rs->rules = nr; rs->cap = ncap;
    }
    rs->rules[rs->n++] = *r;
    return 0;
}

static inline int ruleset_load_file(const char *path, struct ruleset *rs) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char logical[4096]; size_t llen = 0; int cont = 0;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        const char *s = line;
        if (!cont) { while (*s == ' ' || *s == '\t') s++; }
        cont = 0;
        if (len && line[len-1] == '\\') { line[--len] = '\0'; cont = 1;
            /* re-trim s length after removing backslash */
            len = strlen(s); }
        else len = strlen(s);
        if (llen + strlen(s) < sizeof logical) { memcpy(logical + llen, s, strlen(s)); llen += strlen(s); logical[llen] = '\0'; }
        if (cont) continue;
        if (llen && logical[0] != '#') {
            struct rule r;
            if (ruleset_parse_line(logical, &r) > 0) ruleset_append(rs, &r);
        }
        llen = 0; logical[0] = '\0';
    }
    fclose(f);
    return 0;
}

/* Resolve *.rules basenames across dirs (later dir wins), process in lexical
 * basename order. Bounded to 1024 unique rule files. */
static inline int ruleset_load_dirs(const char *const *dirs, int ndirs, struct ruleset *rs) {
    char names[1024][64];
    char owner[1024][256];   /* full path of winning dir for that basename */
    int nn = 0;
    for (int di = 0; di < ndirs; di++) {
        DIR *d = opendir(dirs[di]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l < 7 || strcmp(e->d_name + l - 6, ".rules") != 0) continue;
            if (l >= sizeof names[0]) continue;
            int found = -1;
            for (int i = 0; i < nn; i++) if (strcmp(names[i], e->d_name) == 0) { found = i; break; }
            if (found < 0) {
                if (nn >= 1024) continue;
                found = nn++;
                safe_copy(names[found], e->d_name, sizeof names[0]);
            }
            snprintf(owner[found], sizeof owner[0], "%s/%s", dirs[di], names[found]);
        }
        closedir(d);
    }
    /* insertion sort basenames lexically, carrying owner path */
    for (int i = 1; i < nn; i++)
        for (int j = i; j > 0 && strcmp(names[j-1], names[j]) > 0; j--) {
            char tn[64]; safe_copy(tn, names[j-1], sizeof tn);
            safe_copy(names[j-1], names[j], sizeof tn); safe_copy(names[j], tn, sizeof tn);
            char to[256]; safe_copy(to, owner[j-1], sizeof to);
            safe_copy(owner[j-1], owner[j], sizeof to); safe_copy(owner[j], to, sizeof to);
        }
    for (int i = 0; i < nn; i++) ruleset_load_file(owner[i], rs);
    return 0;
}

/* udev glob: split PAT on top-level '|' (respecting [..] classes), fnmatch each. */
static inline int udev_glob(const char *pat, const char *str) {
    const char *seg = pat, *p = pat;
    int inbr = 0;
    for (;; p++) {
        if (*p == '[') inbr = 1;
        else if (*p == ']') inbr = 0;
        if (*p == '\0' || (*p == '|' && !inbr)) {
            size_t len = (size_t)(p - seg);
            char buf[RK_VAL_MAX];
            if (len < sizeof buf) {
                memcpy(buf, seg, len); buf[len] = '\0';
                if (fnmatch(buf, str, 0) == 0) return 1;
            }
            if (*p == '\0') break;
            seg = p + 1;
        }
    }
    return 0;
}

#define DEVCTX_TAGS_MAX 32
#define DEVCTX_SYMLINKS_MAX 32
#define DEVCTX_FINAL_MAX    16
#define DEVCTX_RUNS_MAX 32

struct dev_ctx {
    struct uevent *ev;                          /* properties; mutable (R3 grows) */
    const char    *sysroot;                     /* e.g. "/sys" */
    char           sysdir[PATH_MAX];            /* absolute sysfs dir of device */
    char           tags[DEVCTX_TAGS_MAX][UE_KEY_MAX];
    int            ntags;
    char           matched_parent[UE_KEY_MAX];  /* last parent-group match kname */
    char symlinks[DEVCTX_SYMLINKS_MAX][UE_VAL_MAX];
    int  nsym;
    char mode[8];
    char group[UE_KEY_MAX];
    char owner[UE_KEY_MAX];
    char name[UE_VAL_MAX];
    int  link_priority;
    int  escape;                  /* 0=none, 1=replace */
    char final_keys[DEVCTX_FINAL_MAX][RK_KEY_MAX + RK_SUB_MAX + 2];
    int  nfinal;
    int  last_rule_deferred;
    int  deferred_applies;
    char runs[DEVCTX_RUNS_MAX][UE_VAL_MAX];
    int  nruns;
    const char *dbroot;
    const char *cmdline_path;
};

static inline int dev_ctx_init(struct dev_ctx *ctx, struct uevent *ev, const char *sysroot) {
    memset(ctx, 0, sizeof *ctx);
    ctx->ev = ev;
    ctx->sysroot = sysroot;
    ctx->dbroot = "/run/udev/data";
    ctx->cmdline_path = "/proc/cmdline";
    const char *dp = uevent_get(ev, "DEVPATH");
    if (!dp) return -1;
    if ((size_t)snprintf(ctx->sysdir, sizeof ctx->sysdir, "%s%s", sysroot, dp) >= sizeof ctx->sysdir)
        return -1;
    return 0;
}

static inline void ctx_add_tag(struct dev_ctx *ctx, const char *t) {
    for (int i = 0; i < ctx->ntags; i++) if (!strcmp(ctx->tags[i], t)) return;
    if (ctx->ntags >= DEVCTX_TAGS_MAX) return;
    safe_copy(ctx->tags[ctx->ntags++], t, UE_KEY_MAX);
}

static inline void ctx_del_tag(struct dev_ctx *ctx, const char *t) {
    int w = 0;
    for (int i = 0; i < ctx->ntags; i++)
        if (strcmp(ctx->tags[i], t) != 0) {
            if (w != i) safe_copy(ctx->tags[w], ctx->tags[i], UE_KEY_MAX);
            w++;
        }
    ctx->ntags = w;
}

static inline void ctx_clear_tags(struct dev_ctx *ctx) { ctx->ntags = 0; }

static inline int udev_wl_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '#' || c == '+' || c == '-' || c == '.' ||
           c == ':' || c == '=' || c == '@' || c == '_' || c == '/';
}

static inline void udev_replace_chars(const char *in, char *out, size_t sz) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < sz; p++)
        out[o++] = udev_wl_ok(*p) ? *p : '_';
    if (sz) out[o] = '\0';
}

static inline void ctx_add_symlink(struct dev_ctx *ctx, const char *link) {
    if (!link || !*link) return;
    for (int i = 0; i < ctx->nsym; i++) if (!strcmp(ctx->symlinks[i], link)) return;
    if (ctx->nsym >= DEVCTX_SYMLINKS_MAX) return;
    safe_copy(ctx->symlinks[ctx->nsym++], link, UE_VAL_MAX);
}

static inline void ctx_del_symlink(struct dev_ctx *ctx, const char *link) {
    int w = 0;
    for (int i = 0; i < ctx->nsym; i++)
        if (strcmp(ctx->symlinks[i], link) != 0) {
            if (w != i) safe_copy(ctx->symlinks[w], ctx->symlinks[i], UE_VAL_MAX);
            w++;
        }
    ctx->nsym = w;
}

static inline void ctx_clear_symlinks(struct dev_ctx *ctx) { ctx->nsym = 0; }

static inline int uevent_set(struct uevent *ev, const char *key, const char *val) {
    for (int i = 0; i < ev->n; i++)
        if (!strcmp(ev->key[i], key)) { safe_copy(ev->val[i], val, UE_VAL_MAX); return 0; }
    if (ev->n >= UE_MAX_KEYS) return -1;
    safe_copy(ev->key[ev->n], key, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], val, UE_VAL_MAX);
    ev->n++;
    return 0;
}

static inline void rs_app(char *out, size_t sz, size_t *o, const char *s) {
    if (!s) return;
    while (*s && *o + 1 < sz) out[(*o)++] = *s++;
    if (sz) out[*o] = '\0';
}

/* Expand match-resolvable substitution tokens; deferred/unknown copied verbatim. */
static inline int ruleset_subst(const char *in, const struct dev_ctx *ctx, char *out, size_t sz) {
    size_t o = 0; if (sz) out[0] = '\0';
    const char *dp = uevent_get(ctx->ev, "DEVPATH");
    const char *kname = dp ? pi_base(dp) : "";
    for (const char *p = in; *p; ) {
        if (*p != '$' && *p != '%') { if (o + 1 < sz) { out[o++] = *p; out[o] = '\0'; } p++; continue; }
        char sig = *p;
        if (sig == '$' && p[1] == '$') { rs_app(out, sz, &o, "$"); p += 2; continue; }
        if (sig == '%' && p[1] == '%') { rs_app(out, sz, &o, "%"); p += 2; continue; }
        const char *q = p + 1;
        char name[32]; size_t nl = 0;
        if (sig == '$') { while (((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')) && nl < sizeof name - 1) name[nl++] = *q++; }
        else if (*q) { name[nl++] = *q++; }   /* % form: single letter */
        name[nl] = '\0';
        char arg[128]; arg[0] = '\0';
        if (*q == '{') { const char *e = strchr(q, '}');
            if (e) { size_t al = (size_t)(e - (q + 1)); if (al < sizeof arg) { memcpy(arg, q + 1, al); arg[al] = '\0'; } q = e + 1; } }
        char tmp[UE_VAL_MAX];
        const char *rep = NULL; int known = 1;
        if      ((sig == '$' && !strcmp(name, "kernel"))  || (sig == '%' && !strcmp(name, "k"))) rep = kname;
        else if ((sig == '$' && !strcmp(name, "number"))  || (sig == '%' && !strcmp(name, "n"))) {
            const char *d = kname + strlen(kname); while (d > kname && d[-1] >= '0' && d[-1] <= '9') d--; rep = d; }
        else if ((sig == '$' && !strcmp(name, "devpath")) || (sig == '%' && !strcmp(name, "p"))) rep = dp ? dp : "";
        else if ((sig == '$' && !strcmp(name, "id"))      || (sig == '%' && !strcmp(name, "b"))) rep = ctx->matched_parent;
        else if ((sig == '$' && !strcmp(name, "major"))   || (sig == '%' && !strcmp(name, "M"))) rep = uevent_get(ctx->ev, "MAJOR");
        else if ((sig == '$' && !strcmp(name, "minor"))   || (sig == '%' && !strcmp(name, "m"))) rep = uevent_get(ctx->ev, "MINOR");
        else if  (sig == '$' && !strcmp(name, "driver"))  rep = uevent_get(ctx->ev, "DRIVER");
        else if ((sig == '$' && !strcmp(name, "sys"))     || (sig == '%' && !strcmp(name, "S"))) rep = ctx->sysroot;
        else if ((sig == '$' && !strcmp(name, "root"))    || (sig == '%' && !strcmp(name, "r"))) rep = "/dev";
        else if ((sig == '$' && !strcmp(name, "env"))     || (sig == '%' && !strcmp(name, "E"))) rep = uevent_get(ctx->ev, arg);
        else if ((sig == '$' && !strcmp(name, "attr"))    || (sig == '%' && !strcmp(name, "s"))) {
            rep = (pi_sysattr(ctx->sysdir, arg, tmp, sizeof tmp) == 0) ? tmp : ""; }
        else known = 0;
        if (known) { rs_app(out, sz, &o, rep ? rep : ""); p = q; }
        else {
            /* deferred/unknown token: copy [p, q) verbatim */
            size_t tl = (size_t)(q - p); char tk[160];
            if (tl < sizeof tk) { memcpy(tk, p, tl); tk[tl] = '\0'; rs_app(out, sz, &o, tk); }
            p = q;
        }
    }
    return 0;
}

static inline int rk_is_match_op(enum rule_op op) { return op == OP_MATCH_EQ || op == OP_MATCH_NE; }

static inline int rk_cmp(enum rule_op op, const char *pat, const char *actual) {
    int m = (actual != NULL) && udev_glob(pat, actual);
    return (op == OP_MATCH_NE) ? !m : m;
}

/* 1 match / 0 nomatch / -1 not a device-level key */
static inline int match_dev_clause(const struct rule_clause *c, const struct dev_ctx *ctx) {
    const struct uevent *ev = ctx->ev;
    if (!strcmp(c->key, "ACTION"))    return rk_cmp(c->op, c->val, uevent_get(ev, "ACTION"));
    if (!strcmp(c->key, "DEVPATH"))   return rk_cmp(c->op, c->val, uevent_get(ev, "DEVPATH"));
    if (!strcmp(c->key, "SUBSYSTEM")) return rk_cmp(c->op, c->val, uevent_get(ev, "SUBSYSTEM"));
    if (!strcmp(c->key, "DRIVER"))    { char b[UE_VAL_MAX];
                                        const char *d = (pi_driver(ctx->sysdir, b, sizeof b) == 0)
                                                        ? b : uevent_get(ev, "DRIVER");
                                        return rk_cmp(c->op, c->val, d); }
    if (!strcmp(c->key, "KERNEL"))    { const char *dp = uevent_get(ev, "DEVPATH");
                                        return rk_cmp(c->op, c->val, dp ? pi_base(dp) : NULL); }
    if (!strcmp(c->key, "ENV"))       return rk_cmp(c->op, c->val, uevent_get(ev, c->subkey));
    if (!strcmp(c->key, "ATTR"))      { char b[UE_VAL_MAX];
                                        int ok = pi_sysattr(ctx->sysdir, c->subkey, b, sizeof b) == 0;
                                        return rk_cmp(c->op, c->val, ok ? b : NULL); }
    if (!strcmp(c->key, "TAG")) {
        int has = 0;
        for (int i = 0; i < ctx->ntags; i++) if (udev_glob(c->val, ctx->tags[i])) { has = 1; break; }
        return c->op == OP_MATCH_NE ? !has : has;
    }
    return -1;   /* parent key (SUBSYSTEMS/…) or R4 conditional (TEST/PROGRAM) */
}

static inline int rk_is_parent_key(const char *k) {
    return !strcmp(k, "SUBSYSTEMS") || !strcmp(k, "KERNELS") ||
           !strcmp(k, "DRIVERS")    || !strcmp(k, "ATTRS")   || !strcmp(k, "TAGS");
}

static inline int parent_clause_on(const struct rule_clause *c, const char *anc,
                                   const struct dev_ctx *ctx) {
    char buf[UE_VAL_MAX];
    if (!strcmp(c->key, "SUBSYSTEMS"))
        return rk_cmp(c->op, c->val, pi_subsystem(anc, buf, sizeof buf) == 0 ? buf : NULL);
    if (!strcmp(c->key, "KERNELS"))
        return rk_cmp(c->op, c->val, pi_base(anc));
    if (!strcmp(c->key, "DRIVERS"))
        return rk_cmp(c->op, c->val, pi_driver(anc, buf, sizeof buf) == 0 ? buf : NULL);
    if (!strcmp(c->key, "ATTRS"))
        return rk_cmp(c->op, c->val, pi_sysattr(anc, c->subkey, buf, sizeof buf) == 0 ? buf : NULL);
    if (!strcmp(c->key, "TAGS")) {
        int has = 0;
        for (int i = 0; i < ctx->ntags; i++) if (udev_glob(c->val, ctx->tags[i])) { has = 1; break; }
        return c->op == OP_MATCH_NE ? !has : has;
    }
    return 0;
}

/* climb from ctx->sysdir (self first) up to sysroot; match iff one ancestor
 * satisfies ALL nc clauses. Records that ancestor's basename in matched_parent. */
static inline int parent_group_match(const struct rule_clause *cl, int nc, struct dev_ctx *ctx) {
    char anc[PATH_MAX];
    safe_copy(anc, ctx->sysdir, sizeof anc);
    for (;;) {
        int all = 1;
        for (int k = 0; k < nc; k++)
            if (!parent_clause_on(&cl[k], anc, ctx)) { all = 0; break; }
        if (all) { safe_copy(ctx->matched_parent, pi_base(anc), UE_KEY_MAX); return 1; }
        if (strlen(anc) <= strlen(ctx->sysroot)) return 0;   /* don't climb above sysroot */
        if (pi_parent(anc) != 0) return 0;
    }
}

static inline int rule_match(const struct rule *r, struct dev_ctx *ctx) {
    ctx->last_rule_deferred = 0;
    for (int i = 0; i < r->nclause; i++) {
        const struct rule_clause *c = &r->clause[i];
        if (!rk_is_match_op(c->op)) {
            if (!strcmp(c->key, "PROGRAM")) ctx->last_rule_deferred = 1;  /* deferred gate (assign-op) */
            continue;    /* assignments: R3 */
        }
        int d = match_dev_clause(c, ctx);
        if (d == 0) return 0;
        if (d == 1) continue;
        /* d == -1: a parent-match group, or an R4 conditional (TEST/PROGRAM) */
        if (rk_is_parent_key(c->key)) {
            int j = i;
            while (j < r->nclause && rk_is_match_op(r->clause[j].op) &&
                   rk_is_parent_key(r->clause[j].key)) j++;
            if (!parent_group_match(&r->clause[i], j - i, ctx)) return 0;
            i = j - 1;   /* for-loop ++ advances past the group */
            continue;
        }
        ctx->last_rule_deferred = 1;   /* a deferred conditional was skipped */
        continue;   /* unknown match key (TEST/PROGRAM): deferred to R4 */
    }
    return 1;
}

static inline void ctx_final_token(const struct rule_clause *c, char *out, size_t sz) {
    if (c->subkey[0]) snprintf(out, sz, "%s{%s}", c->key, c->subkey);
    else              snprintf(out, sz, "%s", c->key);
}

static inline int ctx_key_final(const struct dev_ctx *ctx, const struct rule_clause *c) {
    char tok[RK_KEY_MAX + RK_SUB_MAX + 2]; ctx_final_token(c, tok, sizeof tok);
    for (int i = 0; i < ctx->nfinal; i++) if (!strcmp(ctx->final_keys[i], tok)) return 1;
    return 0;
}

static inline void ctx_lock_final(struct dev_ctx *ctx, const struct rule_clause *c) {
    if (ctx_key_final(ctx, c) || ctx->nfinal >= DEVCTX_FINAL_MAX) return;
    ctx_final_token(c, ctx->final_keys[ctx->nfinal++], RK_KEY_MAX + RK_SUB_MAX + 2);
}

static inline void apply_options(struct dev_ctx *ctx, const char *val) {
    const char *p = val;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - s);
        char tok[128];
        if (len == 0 || len >= sizeof tok) continue;
        memcpy(tok, s, len); tok[len] = '\0';
        if      (!strncmp(tok, "link_priority=", 14)) ctx->link_priority = atoi(tok + 14);
        else if (!strcmp(tok, "string_escape=replace")) ctx->escape = 1;
        else if (!strcmp(tok, "string_escape=none"))    ctx->escape = 0;
        /* static_node=, watch, nowatch, db_persist, ... : tracked no-ops */
    }
}

/* add each whitespace-separated symlink token (escaped) from a SYMLINK value */
static inline void apply_symlink_value(struct dev_ctx *ctx, const char *v, enum rule_op op) {
    if (op == OP_ASSIGN || op == OP_ASSIGN_FINAL) ctx_clear_symlinks(ctx);
    if (ctx->escape) {                    /* whole value -> one escaped link */
        char e[UE_VAL_MAX]; udev_replace_chars(v, e, sizeof e);
        if (op == OP_ASSIGN_SUB) ctx_del_symlink(ctx, e); else ctx_add_symlink(ctx, e);
        return;
    }
    const char *p = v;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - s);
        if (!len) continue;
        char raw[UE_VAL_MAX], e[UE_VAL_MAX];
        if (len >= sizeof raw) len = sizeof raw - 1;
        memcpy(raw, s, len); raw[len] = '\0';
        udev_replace_chars(raw, e, sizeof e);
        if (op == OP_ASSIGN_SUB) ctx_del_symlink(ctx, e); else ctx_add_symlink(ctx, e);
    }
}

static inline const char *apply_rule(const struct rule *r, struct dev_ctx *ctx) {
    for (int i = 0; i < r->nclause; i++) {
        const struct rule_clause *c = &r->clause[i];
        if (rk_is_match_op(c->op)) continue;

        if (!strcmp(c->key, "GOTO"))  return c->val;
        if (!strcmp(c->key, "LABEL")) continue;

        if (ctx_key_final(ctx, c)) continue;

        char sv[UE_VAL_MAX];
        ruleset_subst(c->val, ctx, sv, sizeof sv);

        if (!strcmp(c->key, "ENV")) {
            uevent_set(ctx->ev, c->subkey, sv);
        } else if (!strcmp(c->key, "TAG")) {
            if (c->op == OP_ASSIGN_SUB) ctx_del_tag(ctx, sv);
            else { if (c->op == OP_ASSIGN || c->op == OP_ASSIGN_FINAL) ctx_clear_tags(ctx);
                   ctx_add_tag(ctx, sv); }
        } else if (!strcmp(c->key, "SYMLINK")) {
            apply_symlink_value(ctx, sv, c->op);
        } else if (!strcmp(c->key, "OPTIONS")) {
            apply_options(ctx, sv);
        } else if (!strcmp(c->key, "MODE")) {
            safe_copy(ctx->mode, sv, sizeof ctx->mode);
        } else if (!strcmp(c->key, "GROUP")) {
            safe_copy(ctx->group, sv, sizeof ctx->group);
        } else if (!strcmp(c->key, "OWNER")) {
            safe_copy(ctx->owner, sv, sizeof ctx->owner);
        } else if (!strcmp(c->key, "NAME")) {
            safe_copy(ctx->name, sv, sizeof ctx->name);
        }
        /* IMPORT / RUN / other: R4 — ignored here */

        if (c->op == OP_ASSIGN_FINAL) ctx_lock_final(ctx, c);
    }
    return NULL;
}

/* Find the first rule at index >= from carrying LABEL=="label"; -1 if none. */
static inline int ruleset_find_label(const struct ruleset *rs, int from, const char *label) {
    for (int i = from; i < rs->n; i++)
        for (int k = 0; k < rs->rules[i].nclause; k++) {
            const struct rule_clause *c = &rs->rules[i].clause[k];
            if (!strcmp(c->key, "LABEL") && !strcmp(c->val, label)) return i;
        }
    return -1;
}

static inline int ruleset_apply(const struct ruleset *rs, struct dev_ctx *ctx) {
    for (int i = 0; i < rs->n; ) {
        if (!rule_match(&rs->rules[i], ctx)) { i++; continue; }
        if (ctx->last_rule_deferred) ctx->deferred_applies++;
        const char *goto_label = apply_rule(&rs->rules[i], ctx);
        if (goto_label) {
            int t = ruleset_find_label(rs, i + 1, goto_label);
            if (t < 0) break;      /* forward label not found: stop */
            i = t;
        } else {
            i++;
        }
    }
    return 0;
}

#endif /* UDEV_RULESET_H */
