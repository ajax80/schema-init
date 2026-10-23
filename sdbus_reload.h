#ifndef SDBUS_RELOAD_H
#define SDBUS_RELOAD_H
/* Pure helpers behind a live config reload: whether a freshly dissolved policy
   may replace the running one, and which new activation entries a live reload
   must hold back. The broker owns the reload itself (schema-dbus.c). */
#include <stdio.h>
#include <string.h>
#include "sdbus_policy.h"
#include "sdbus_activate.h"

typedef struct { const char *name; int uid; const int *gids; int n_gids; } sdbus_owner;

static inline int sdbus_policy_rule_count(const sdbus_policy *p) {
    int n = 0;
    for (int c = 0; p && c < p->n_ctxs; c++) n += p->ctxs[c].n_rules;
    return n;
}

/* A live swap has no boot-time safety net (no stock fallback, no flip
   seatbelt), so a well-formed but wrong policy must not go live: refuse one
   that loses more than half the rules, or that would deny a current owner its
   name (login1, NetworkManager, polkit... keep running either way, but their
   next RequestName and every policy check built on ownership would fail). */
static inline int sdbus_reload_policy_ok(const sdbus_policy *oldp, const sdbus_policy *newp,
                                         const sdbus_owner *owners, int n_owners,
                                         char *why, size_t why_len) {
    int on = sdbus_policy_rule_count(oldp), nn = sdbus_policy_rule_count(newp);
    if (on > 0 && nn * 2 < on) {
        snprintf(why, why_len, "rules %d -> %d", on, nn);
        return 0;
    }
    for (int i = 0; i < n_owners; i++) {
        sdbus_req req = { .op = "own", .uid = owners[i].uid, .gids = owners[i].gids,
                          .n_gids = owners[i].n_gids, .name = owners[i].name };
        if (strcmp(sdbus_policy_eval(newp, &req), SDBUS_ALLOW) != 0) {
            snprintf(why, why_len, "would deny own %s to uid %d", owners[i].name, owners[i].uid);
            return 0;
        }
    }
    return 1;
}

/* Mid-dnf a raw /usr/share entry with SystemdService= can land before
   schema-import writes its override; activating it then starts the daemon
   outside schema-init. So a live reload adds such an entry only from the
   override dir; one the running table already had stays. */
static inline int sdbus_svctab_hold_raw(sdbus_svctab *fresh, const sdbus_svctab *old,
                                        const char *override_dir) {
    int held = 0, w = 0;
    for (int i = 0; i < fresh->n; i++) {
        sdbus_svc_ent *e = &fresh->v[i];
        if (e->systemd_service && strcmp(e->dir, override_dir) && !sdbus_svctab_find(old, e->name)) {
            free(e->name); free(e->user);
            for (char **a = e->argv; a && *a; a++) free(*a);
            free(e->argv);
            held++;
            continue;
        }
        fresh->v[w++] = *e;
    }
    fresh->n = w;
    return held;
}
#endif
