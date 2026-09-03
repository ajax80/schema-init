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

static inline sdbus_matchset *sdbus_match_new(void) { return calloc(1, sizeof(sdbus_matchset)); }

static inline void sdbus__rule_clear(sdbus_match_rule *r) {
    free(r->type); free(r->interface); free(r->member);
    free(r->path); free(r->path_namespace); free(r->sender); free(r->raw);
    memset(r, 0, sizeof *r);
}

/* parse "key='value',key='value'" into r; returns 0 ok, -1 malformed. */
static inline int sdbus__parse_rule(const char *rule, sdbus_match_rule *r) {
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

static inline int sdbus_match_add(sdbus_matchset *m, const char *rule) {
    sdbus_match_rule r;
    if (sdbus__parse_rule(rule, &r) != 0) return -1;
    m->rules = realloc(m->rules, (m->n + 1) * sizeof *m->rules);
    m->rules[m->n++] = r;
    return 0;
}

static inline int sdbus_match_remove(sdbus_matchset *m, const char *rule) {
    for (int i = 0; i < m->n; i++) {
        if (!strcmp(m->rules[i].raw, rule)) {
            sdbus__rule_clear(&m->rules[i]);
            m->rules[i] = m->rules[--m->n];
            return 0;
        }
    }
    return -1;                                  /* not found */
}

static inline int sdbus__eqornull(const char *constraint, const char *field) {
    if (!constraint) return 1;                  /* absent constraint = wildcard */
    return field && !strcmp(constraint, field);
}

/* path_namespace matches path itself or any path under it. */
static inline int sdbus__ns_match(const char *ns, const char *path) {
    if (!ns) return 1;
    if (!path) return 0;
    size_t nl = strlen(ns);
    if (strncmp(ns, path, nl) != 0) return 0;
    return path[nl] == '\0' || path[nl] == '/' || (nl == 1 && ns[0] == '/');
}

/* A rule's sender= may name the emitter by its unique name (":1.N") or by any
   well-known name it owns; on the wire a signal's sender is always the unique
   name, so match the constraint against the unique name AND the sender's owned
   well-known set. Without this, a sender='well.known.Name' subscription (what
   gvfs/Solid/gdbus register) never matches a service that emits as ":1.N". */
static inline int sdbus__sender_match(const char *constraint, const char *uniq,
                                      const char **owned, int n_owned) {
    if (!constraint) return 1;                  /* absent = wildcard */
    if (uniq && !strcmp(constraint, uniq)) return 1;
    for (int i = 0; i < n_owned; i++)
        if (owned[i] && !strcmp(constraint, owned[i])) return 1;
    return 0;
}

/* 1 if any stored rule accepts this signal. sender_uniq is the emitter's unique
   name; sender_owned/n_owned are the well-known names it owns (may be NULL/0). */
static inline int sdbus_match_signal(sdbus_matchset *m, const char *interface,
                              const char *member, const char *path,
                              const char *sender_uniq, const char **sender_owned,
                              int n_owned) {
    for (int i = 0; i < m->n; i++) {
        sdbus_match_rule *r = &m->rules[i];
        if (r->type && strcmp(r->type, "signal") != 0) continue;
        if (!sdbus__eqornull(r->interface, interface)) continue;
        if (!sdbus__eqornull(r->member, member)) continue;
        if (!sdbus__eqornull(r->path, path)) continue;
        if (!sdbus__sender_match(r->sender, sender_uniq, sender_owned, n_owned)) continue;
        if (!sdbus__ns_match(r->path_namespace, path)) continue;
        return 1;
    }
    return 0;
}

static inline void sdbus_match_free(sdbus_matchset *m) {
    if (!m) return;
    for (int i = 0; i < m->n; i++) sdbus__rule_clear(&m->rules[i]);
    free(m->rules); free(m);
}

#endif
