#ifndef UDEV_RULESET_H
#define UDEV_RULESET_H

#include "schema-udev.h"   /* safe_copy */
#include <string.h>
#include <stdlib.h>

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

#endif /* UDEV_RULESET_H */
