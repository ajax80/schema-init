#ifndef SDBUS_MATCH_H
#define SDBUS_MATCH_H

/* AddMatch rule parsing + signal matching. Grammar: comma-separated
   key='value' pairs (D-Bus match rules). Only the keys the broker needs for
   signal delivery are interpreted; unknown keys are accepted but ignored for
   matching (they only narrow, and we have no data for them). Escaped quotes in
   values are not supported (interface/member/path names never contain them). */

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *type, *interface, *member, *path, *path_namespace, *sender;
    char *raw;                 /* verbatim rule, for exact-string removal */
} sdbus_match_rule;

struct sdbus_matchset { sdbus_match_rule *rules; int n; };
typedef struct sdbus_matchset sdbus_matchset;

static sdbus_matchset *sdbus_match_new(void) { return calloc(1, sizeof(sdbus_matchset)); }

static void sdbus__rule_clear(sdbus_match_rule *r) {
    free(r->type); free(r->interface); free(r->member);
    free(r->path); free(r->path_namespace); free(r->sender); free(r->raw);
    memset(r, 0, sizeof *r);
}

/* parse "key='value',key='value'" into r; returns 0 ok, -1 malformed. */
static int sdbus__parse_rule(const char *rule, sdbus_match_rule *r) {
    memset(r, 0, sizeof *r);
    const char *p = rule;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *k = p;
        while (*p && *p != '=' && *p != ',') p++;
        if (*p != '=') { sdbus__rule_clear(r); return -1; }
        size_t klen = (size_t)(p - k);
        p++;                                   /* skip '=' */
        if (*p != '\'') { sdbus__rule_clear(r); return -1; }
        p++;                                   /* skip opening quote */
        const char *v = p;
        while (*p && *p != '\'') p++;
        if (*p != '\'') { sdbus__rule_clear(r); return -1; }
        size_t vlen = (size_t)(p - v);
        p++;                                   /* skip closing quote */

        char *val = malloc(vlen + 1);
        memcpy(val, v, vlen); val[vlen] = '\0';
        char **slot = NULL;
        if (klen == 4 && !strncmp(k, "type", 4)) slot = &r->type;
        else if (klen == 9 && !strncmp(k, "interface", 9)) slot = &r->interface;
        else if (klen == 6 && !strncmp(k, "member", 6)) slot = &r->member;
        else if (klen == 4 && !strncmp(k, "path", 4)) slot = &r->path;
        else if (klen == 14 && !strncmp(k, "path_namespace", 14)) slot = &r->path_namespace;
        else if (klen == 6 && !strncmp(k, "sender", 6)) slot = &r->sender;
        if (slot) { free(*slot); *slot = val; } else free(val);   /* unknown key ignored */
    }
    r->raw = strdup(rule);
    return 0;
}

static int sdbus_match_add(sdbus_matchset *m, const char *rule) {
    sdbus_match_rule r;
    if (sdbus__parse_rule(rule, &r) != 0) return -1;
    m->rules = realloc(m->rules, (m->n + 1) * sizeof *m->rules);
    m->rules[m->n++] = r;
    return 0;
}

static int sdbus_match_remove(sdbus_matchset *m, const char *rule) {
    for (int i = 0; i < m->n; i++) {
        if (!strcmp(m->rules[i].raw, rule)) {
            sdbus__rule_clear(&m->rules[i]);
            m->rules[i] = m->rules[--m->n];
            return 0;
        }
    }
    return -1;                                  /* not found */
}

static int sdbus__eqornull(const char *constraint, const char *field) {
    if (!constraint) return 1;                  /* absent constraint = wildcard */
    return field && !strcmp(constraint, field);
}

/* path_namespace matches path itself or any path under it. */
static int sdbus__ns_match(const char *ns, const char *path) {
    if (!ns) return 1;
    if (!path) return 0;
    size_t nl = strlen(ns);
    if (strncmp(ns, path, nl) != 0) return 0;
    return path[nl] == '\0' || path[nl] == '/' || (nl == 1 && ns[0] == '/');
}

/* 1 if any stored rule accepts this signal. */
static int sdbus_match_signal(sdbus_matchset *m, const char *interface,
                              const char *member, const char *path, const char *sender) {
    for (int i = 0; i < m->n; i++) {
        sdbus_match_rule *r = &m->rules[i];
        if (r->type && strcmp(r->type, "signal") != 0) continue;
        if (!sdbus__eqornull(r->interface, interface)) continue;
        if (!sdbus__eqornull(r->member, member)) continue;
        if (!sdbus__eqornull(r->path, path)) continue;
        if (!sdbus__eqornull(r->sender, sender)) continue;
        if (!sdbus__ns_match(r->path_namespace, path)) continue;
        return 1;
    }
    return 0;
}

static void sdbus_match_free(sdbus_matchset *m) {
    if (!m) return;
    for (int i = 0; i < m->n; i++) sdbus__rule_clear(&m->rules[i]);
    free(m->rules); free(m);
}

#endif
