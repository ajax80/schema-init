#ifndef UDEV_RULESET_H
#define UDEV_RULESET_H

#include "schema-udev.h"   /* safe_copy */
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

#endif /* UDEV_RULESET_H */
