#ifndef SDBUS_POLICY_H
#define SDBUS_POLICY_H

/* C port of tools/dbus-learn/felt_policy.py. Semantics are line-for-line with
   that module; comments pin each branch to its felt_policy.py source so the
   14,979-msg corpus conformance gate stays meaningful. Input is the *dissolved*
   policy text (dissect_policy.dissolve_tree output), never system.conf XML. */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>

typedef struct {
    const char *op;              /* "send" | "own" | "receive" | NULL */
    int uid;                     /* -1 == unknown */
    const int *gids; int n_gids;
    const char *interface, *member, *msgtype, *path;
    const char *destination;
    const char **dest_names; int n_dest_names;
    const char *name;            /* for op=="own" */
    int has_reply_serial;        /* 0/1 */
    const char *sender_name;     /* for receive_sender */
} sdbus_req;

typedef struct { char *attr; char *value; } sdbus_pred;
typedef struct { int allow; sdbus_pred *preds; int n_preds; } sdbus_rule;
typedef enum { CTX_DEFAULT, CTX_MANDATORY, CTX_USER, CTX_GROUP } sdbus_ctx_kind;
typedef struct {
    sdbus_ctx_kind kind;
    int wildcard;                /* selector absent -> applies to all */
    int selector_id;             /* resolved uid/gid, or -1 if unresolvable */
} sdbus_ctx_sel;
typedef struct {
    sdbus_ctx_kind kind;
    sdbus_ctx_sel sel;
    sdbus_rule *rules; int n_rules;
} sdbus_ctx;
struct sdbus_policy {
    sdbus_ctx *ctxs; int n_ctxs;
    char *buf;                   /* owned copy of the text, sliced by attr/value */
};
typedef struct sdbus_policy sdbus_policy;

static const char SDBUS_ALLOW[] = "allow";
static const char SDBUS_DENY[]  = "deny";

/* --- small helpers --- */
static inline int sdbus__eq(const char *a, const char *b) { return a && b && !strcmp(a, b); }

static inline char *sdbus__trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

static inline int sdbus__is_int(const char *s, long *out) {
    if (!s || !*s) return 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end) return 0;
    *out = v;
    return 1;
}

/* felt_policy _uid_of / _gid_of: name -> id, None (here -1) if unknown. */
static inline int sdbus__uid_of(const char *name) {
    struct passwd *p = getpwnam(name);
    return p ? (int)p->pw_uid : -1;
}
static inline int sdbus__gid_of(const char *name) {
    struct group *g = getgrnam(name);
    return g ? (int)g->gr_gid : -1;
}

/* Resolve a context selector once at parse time (felt_policy resolves per-eval
   with an lru_cache; static-per-boot policy lets us resolve once). */
static inline void sdbus__resolve_sel(sdbus_ctx_kind kind, const char *selector, sdbus_ctx_sel *out) {
    out->kind = kind;
    out->wildcard = 0;
    out->selector_id = -1;
    if (kind == CTX_DEFAULT || kind == CTX_MANDATORY) { out->wildcard = 1; return; }
    if (!selector || !*selector) { out->wildcard = 1; return; }  /* selector None -> applies to all */
    long v;
    if (sdbus__is_int(selector, &v)) { out->selector_id = (int)v; return; }
    out->selector_id = (kind == CTX_USER) ? sdbus__uid_of(selector) : sdbus__gid_of(selector);
}

static inline void sdbus_policy_free(sdbus_policy *pol);

/* --- parse (felt_policy.parse_policy + _parse_predicates) --- */
static inline sdbus_policy *sdbus_policy_parse(const char *text) {
    sdbus_policy *pol = calloc(1, sizeof *pol);
    if (!pol) return NULL;
    pol->buf = strdup(text ? text : "");
    if (!pol->buf) { free(pol); return NULL; }

    char *save_line = NULL;
    /* iterate lines by hand so blank lines are handled like felt_policy */
    char *line = pol->buf;
    char *nl;
    sdbus_ctx *cur = NULL;
    for (; line; line = save_line) {
        nl = strchr(line, '\n');
        if (nl) { *nl = '\0'; save_line = nl + 1; } else { save_line = NULL; }

        char *hash = strchr(line, '#');           /* line.split("#",1)[0] */
        if (hash) *hash = '\0';
        char *l = sdbus__trim(line);
        if (!*l) continue;

        char *eq = strchr(l, '=');                /* key,_,value = line.partition("=") */
        char *key, *value;
        if (eq) { *eq = '\0'; key = sdbus__trim(l); value = sdbus__trim(eq + 1); }
        else    { key = sdbus__trim(l); value = key + strlen(key); }

        if (!strcmp(key, "context")) {
            char *colon = strchr(value, ':');     /* kind,_,selector = value.partition(":") */
            char *kind_s, *sel_s;
            if (colon) { *colon = '\0'; kind_s = sdbus__trim(value); sel_s = sdbus__trim(colon + 1); }
            else       { kind_s = sdbus__trim(value); sel_s = value + strlen(value); }
            sdbus_ctx_kind kind;
            if (!strcmp(kind_s, "default")) kind = CTX_DEFAULT;
            else if (!strcmp(kind_s, "mandatory")) kind = CTX_MANDATORY;
            else if (!strcmp(kind_s, "user")) kind = CTX_USER;
            else if (!strcmp(kind_s, "group")) kind = CTX_GROUP;
            else { sdbus_policy_free(pol); return NULL; }
            pol->ctxs = realloc(pol->ctxs, (pol->n_ctxs + 1) * sizeof *pol->ctxs);
            cur = &pol->ctxs[pol->n_ctxs++];
            cur->kind = kind;
            cur->rules = NULL; cur->n_rules = 0;
            sdbus__resolve_sel(kind, (*sel_s) ? sel_s : NULL, &cur->sel);
        } else if (!strcmp(key, "allow") || !strcmp(key, "deny")) {
            if (!cur) { sdbus_policy_free(pol); return NULL; }  /* rule before any context */
            cur->rules = realloc(cur->rules, (cur->n_rules + 1) * sizeof *cur->rules);
            sdbus_rule *r = &cur->rules[cur->n_rules++];
            r->allow = (key[0] == 'a');
            r->preds = NULL; r->n_preds = 0;
            /* _parse_predicates: split on ',', partition each on ':' */
            char *p = value;
            while (*p) {
                char *comma = strchr(p, ',');
                char *part;
                if (comma) { *comma = '\0'; part = p; p = comma + 1; }
                else       { part = p; p += strlen(p); }
                char *pt = sdbus__trim(part);
                if (!*pt) continue;
                char *pc = strchr(pt, ':');
                char *attr, *val;
                if (pc) { *pc = '\0'; attr = sdbus__trim(pt); val = sdbus__trim(pc + 1); }
                else    { attr = sdbus__trim(pt); val = pt + strlen(pt); }
                r->preds = realloc(r->preds, (r->n_preds + 1) * sizeof *r->preds);
                r->preds[r->n_preds].attr = attr;
                r->preds[r->n_preds].value = val;
                r->n_preds++;
            }
        } else {
            sdbus_policy_free(pol); return NULL;   /* unknown key */
        }
    }
    return pol;
}

static inline void sdbus_policy_free(sdbus_policy *pol) {
    if (!pol) return;
    for (int i = 0; i < pol->n_ctxs; i++) {
        for (int j = 0; j < pol->ctxs[i].n_rules; j++) free(pol->ctxs[i].rules[j].preds);
        free(pol->ctxs[i].rules);
    }
    free(pol->ctxs);
    free(pol->buf);
    free(pol);
}

/* --- match (felt_policy _field_matches / _rule_matches) --- */
static inline int sdbus__field_matches(const char *field, const char *value) {
    if (!strcmp(value, "*")) return field != NULL;   /* "*" -> field is not None */
    return sdbus__eq(field, value);
}

static inline int sdbus__rule_matches(const sdbus_rule *rule, const sdbus_req *req) {
    const char *op = req->op;
    for (int i = 0; i < rule->n_preds; i++) {
        const char *attr = rule->preds[i].attr;
        const char *value = rule->preds[i].value;
        if (!strcmp(attr, "user") || !strcmp(attr, "group")) {
            if (strcmp(value, "*") != 0) return 0;   /* bare user/group gates only on "*" */
        } else if (!strcmp(attr, "own") || !strcmp(attr, "own_prefix")) {
            if (!sdbus__eq(op, "own")) return 0;
            const char *name = req->name ? req->name : "";
            if (!strcmp(attr, "own")) {
                if (strcmp(value, "*") != 0 && strcmp(value, name) != 0) return 0;
            } else {
                if (strncmp(name, value, strlen(value)) != 0) return 0;  /* startswith */
            }
        } else if (!strcmp(attr, "send_destination")) {
            if (!sdbus__eq(op, "send")) return 0;
            /* set-membership over destination's owned well-known names, with the
               raw destination as fallback (felt_policy lines 86-102). */
            int n = req->n_dest_names;
            const char *fallback = NULL;
            if (n == 0) { if (req->destination) { fallback = req->destination; n = 1; } }
            if (!strcmp(value, "*")) {
                if (n == 0) return 0;
            } else {
                int found = 0;
                for (int k = 0; k < req->n_dest_names; k++)
                    if (sdbus__eq(req->dest_names[k], value)) { found = 1; break; }
                if (!found && fallback && sdbus__eq(fallback, value)) found = 1;
                if (!found) return 0;
            }
        } else if (!strcmp(attr, "send_interface")) {
            if (!sdbus__eq(op, "send")) return 0;
            if (!sdbus__field_matches(req->interface, value)) return 0;
        } else if (!strcmp(attr, "send_member")) {
            if (!sdbus__eq(op, "send")) return 0;
            if (!sdbus__field_matches(req->member, value)) return 0;
        } else if (!strcmp(attr, "send_type")) {
            if (!sdbus__eq(op, "send")) return 0;
            if (!sdbus__field_matches(req->msgtype, value)) return 0;
        } else if (!strcmp(attr, "send_path")) {
            if (!sdbus__eq(op, "send")) return 0;
            if (!sdbus__field_matches(req->path, value)) return 0;
        } else if (!strcmp(attr, "receive_sender")) {
            if (!sdbus__eq(op, "receive")) return 0;
            if (!sdbus__field_matches(req->sender_name, value)) return 0;
        } else if (!strcmp(attr, "receive_interface")) {
            if (!sdbus__eq(op, "receive")) return 0;
            if (!sdbus__field_matches(req->interface, value)) return 0;
        } else if (!strcmp(attr, "receive_member")) {
            if (!sdbus__eq(op, "receive")) return 0;
            if (!sdbus__field_matches(req->member, value)) return 0;
        } else if (!strcmp(attr, "receive_type")) {
            if (!sdbus__eq(op, "receive")) return 0;
            if (!sdbus__field_matches(req->msgtype, value)) return 0;
        } else if (!strcmp(attr, "receive_path")) {
            if (!sdbus__eq(op, "receive")) return 0;
            if (!sdbus__field_matches(req->path, value)) return 0;
        } else {
            return 0;   /* unknown predicate never matches (felt_policy line 114) */
        }
    }
    return 1;
}

static inline int sdbus__applicable(const sdbus_ctx *ctx, const sdbus_req *req) {
    if (ctx->kind == CTX_DEFAULT || ctx->kind == CTX_MANDATORY) return 1;
    if (ctx->kind == CTX_USER) {
        if (req->uid < 0) return 0;                 /* uid None -> False */
        if (ctx->sel.wildcard) return 1;
        return ctx->sel.selector_id == req->uid;
    }
    /* CTX_GROUP */
    if (req->n_gids == 0) return 0;                 /* not gids -> False */
    if (ctx->sel.wildcard) return 1;
    for (int i = 0; i < req->n_gids; i++)
        if (req->gids[i] == ctx->sel.selector_id) return 1;
    return 0;
}

static inline int sdbus__is_requested_reply(const sdbus_req *req) {
    return sdbus__eq(req->op, "send")
        && req->has_reply_serial
        && (sdbus__eq(req->msgtype, "method_return") || sdbus__eq(req->msgtype, "error"));
}

/* felt_policy.evaluate: ordered default->group->user->mandatory, last-match-wins,
   start deny; requested-reply short-circuits to allow. */
static inline const char *sdbus_policy_eval(const sdbus_policy *pol, const sdbus_req *req) {
    if (sdbus__is_requested_reply(req)) return SDBUS_ALLOW;
    const char *verdict = SDBUS_DENY;
    static const sdbus_ctx_kind order[] = { CTX_DEFAULT, CTX_GROUP, CTX_USER, CTX_MANDATORY };
    for (int o = 0; o < 4; o++) {
        for (int c = 0; c < pol->n_ctxs; c++) {
            const sdbus_ctx *ctx = &pol->ctxs[c];
            if (ctx->kind != order[o]) continue;
            if (!sdbus__applicable(ctx, req)) continue;
            for (int r = 0; r < ctx->n_rules; r++)
                if (sdbus__rule_matches(&ctx->rules[r], req))
                    verdict = ctx->rules[r].allow ? SDBUS_ALLOW : SDBUS_DENY;
        }
    }
    return verdict;
}

#endif
