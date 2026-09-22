#ifndef SDBUS_ACTIVATE_H
#define SDBUS_ACTIVATE_H

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct { char *name; char **argv; char *user; } sdbus_svc_ent;
typedef struct { sdbus_svc_ent *v; int n; } sdbus_svctab;

/* split "a b c" into a NULL-terminated argv (whitespace-delimited, no quoting —
   dbus service Exec lines are plain paths + flags). */
static inline char **sdbus__split_argv(const char *s) {
    int cap = 4, n = 0;
    char **a = malloc(cap * sizeof *a);
    char *dup = strdup(s), *save = NULL;
    for (char *tok = strtok_r(dup, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        if (n + 1 >= cap) { cap *= 2; a = realloc(a, cap * sizeof *a); }
        a[n++] = strdup(tok);
    }
    a[n] = NULL;
    free(dup);
    return a;
}

static inline void sdbus__svc_add(sdbus_svctab *t, const char *name,
                                  const char *exec, const char *user) {
    t->v = realloc(t->v, (t->n + 1) * sizeof *t->v);
    sdbus_svc_ent *e = &t->v[t->n++];
    e->name = strdup(name);
    e->argv = sdbus__split_argv(exec);
    e->user = strdup(user && *user ? user : "root");
}

/* Names schema-init runs as first-class managed services; the broker must never
   bus-activate a second copy (which would race the managed one for the well-known
   name). Loaded from a newline-delimited file (# comments, blank lines and
   surrounding whitespace ignored) that lives in /etc, so a distro package update
   of the daemon's own .service file cannot silently un-mask it. */
typedef struct { char **v; int n; } sdbus_strset;

static inline sdbus_strset *sdbus_masklist_load(const char *path) {
    sdbus_strset *s = calloc(1, sizeof *s);
    if (!path) return s;
    FILE *f = fopen(path, "r");
    if (!f) return s;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || !*p) continue;
        char *e = p + strlen(p);
        while (e > p && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
        s->v = realloc(s->v, (s->n + 1) * sizeof *s->v);
        s->v[s->n++] = strdup(p);
    }
    fclose(f);
    return s;
}

static inline int sdbus_masklist_has(const sdbus_strset *s, const char *name) {
    if (!s || !name) return 0;
    for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i], name)) return 1;
    return 0;
}

static inline void sdbus_masklist_free(sdbus_strset *s) {
    if (!s) return;
    for (int i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v); free(s);
}

static inline sdbus_svctab *sdbus_svctab_parse_dir_masked(const char *dir, const char *maskfile) {
    sdbus_svctab *t = calloc(1, sizeof *t);
    sdbus_strset *mask = sdbus_masklist_load(maskfile);
    DIR *d = opendir(dir);
    if (!d) { sdbus_masklist_free(mask); return t; }   /* empty table, not NULL */
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l < 9 || strcmp(de->d_name + l - 8, ".service")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[2048], name[2048] = "", exec[2048] = "", user[2048] = "";
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (!strncmp(line, "Name=", 5))      snprintf(name, sizeof name, "%s", line + 5);
            else if (!strncmp(line, "Exec=", 5)) snprintf(exec, sizeof exec, "%s", line + 5);
            else if (!strncmp(line, "User=", 5)) snprintf(user, sizeof user, "%s", line + 5);
        }
        fclose(f);
        if (!name[0] || !exec[0]) continue;                 /* need both */
        /* skip Exec=/bin/false (systemd-only activatables) */
        if (!strcmp(exec, "/bin/false") || !strcmp(exec, "/usr/bin/false")) continue;
        if (sdbus_masklist_has(mask, name)) continue;       /* schema-managed: never bus-activate */
        sdbus__svc_add(t, name, exec, user);
    }
    closedir(d);
    sdbus_masklist_free(mask);
    return t;
}

static inline sdbus_svctab *sdbus_svctab_parse_dir(const char *dir) {
    return sdbus_svctab_parse_dir_masked(dir, NULL);
}

static inline const sdbus_svc_ent *sdbus_svctab_find(sdbus_svctab *t, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->v[i].name, name)) return &t->v[i];
    return NULL;
}

static inline void sdbus_svctab_free(sdbus_svctab *t) {
    if (!t) return;
    for (int i = 0; i < t->n; i++) {
        free(t->v[i].name); free(t->v[i].user);
        for (char **a = t->v[i].argv; a && *a; a++) free(*a);
        free(t->v[i].argv);
    }
    free(t->v); free(t);
}

/* Fix 1a (SP4 design doc): whether spawn_service should attempt to drop
   privileges before exec. System bus: drop whenever the resolved target
   isn't already root, matching stock dbus-daemon's system-activation
   behavior (unchanged from before this function existed). Session bus:
   NEVER drop -- the broker is already running, unprivileged, as the
   only user in play. Attempting it there calls initgroups()/setgid()/
   setuid() without CAP_SETGID and _exit(127)s the child before exec,
   even when the target uid matches the broker's own uid exactly. */
static inline int sdbus_activate_should_drop_privs(int system_bus, uid_t target_uid) {
    return system_bus && target_uid != 0;
}

/* Fix 2 (SP4 design doc): build the env array for an activated child.
   System bus: the pre-existing minimal clean env (PATH + DBUS_STARTER_*
   only) -- byte-identical to what spawn_service built inline before this
   function existed. Session bus: pass through the broker's own
   `inherited` environment (Wayland/XDG/HOME/etc -- session-activated
   apps like the KDE portals need these to function at all) with
   DBUS_STARTER_ADDRESS/DBUS_STARTER_BUS_TYPE=session PREPENDED so they
   are found first by a front-to-back getenv() scan even in the
   (unlikely) case `inherited` already carries stale DBUS_STARTER_*
   entries. Returns a malloc'd NULL-terminated array of malloc'd
   strings -- free with sdbus_activate_free_env(). `inherited` may be
   NULL (treated as empty) and is never modified. */
static inline char **sdbus_activate_build_env(int system_bus, const char *bus_addr,
                                              char **inherited) {
    char starter[320];
    snprintf(starter, sizeof starter, "DBUS_STARTER_ADDRESS=%s", bus_addr);

    if (system_bus) {
        char **env = malloc(4 * sizeof *env);
        env[0] = strdup("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin");
        env[1] = strdup(starter);
        env[2] = strdup("DBUS_STARTER_BUS_TYPE=system");
        env[3] = NULL;
        return env;
    }

    int n = 0;
    for (char **p = inherited; p && *p; p++) n++;
    char **env = malloc((n + 3) * sizeof *env);
    int i = 0;
    env[i++] = strdup(starter);
    env[i++] = strdup("DBUS_STARTER_BUS_TYPE=session");
    for (char **p = inherited; p && *p; p++) env[i++] = strdup(*p);
    env[i] = NULL;
    return env;
}

static inline void sdbus_activate_free_env(char **env) {
    if (!env) return;
    for (char **p = env; *p; p++) free(*p);
    free(env);
}

typedef enum { SDBUS_HELD_IMPLICIT, SDBUS_HELD_EXPLICIT } sdbus_held_kind;
typedef struct {
    unsigned char *bytes; int len;      /* captured wire message (implicit) */
    int *fds; int nfds;                 /* its passed fds (implicit) */
    int caller_id; uint32_t serial; int expects_reply;
    sdbus_held_kind kind;
} sdbus_held_msg;
typedef struct {
    char *name; int child_pid; long deadline_ms;
    sdbus_held_msg *held; int n_held;
} sdbus_pending_act;
typedef struct { sdbus_pending_act *v; int n; } sdbus_acts;

static inline sdbus_acts *sdbus_acts_new(void) { return calloc(1, sizeof(sdbus_acts)); }

static inline sdbus_pending_act *sdbus_acts_find(sdbus_acts *a, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < a->n; i++)
        if (!strcmp(a->v[i].name, name)) return &a->v[i];
    return NULL;
}

static inline sdbus_pending_act *sdbus_acts_by_pid(sdbus_acts *a, int pid) {
    for (int i = 0; i < a->n; i++) if (a->v[i].child_pid == pid) return &a->v[i];
    return NULL;
}

static inline sdbus_pending_act *sdbus_acts_begin(sdbus_acts *a, const char *name,
                                                  int pid, long deadline_ms) {
    a->v = realloc(a->v, (a->n + 1) * sizeof *a->v);
    sdbus_pending_act *e = &a->v[a->n++];
    memset(e, 0, sizeof *e);
    e->name = strdup(name); e->child_pid = pid; e->deadline_ms = deadline_ms;
    return e;
}

static inline void sdbus_acts_hold(sdbus_pending_act *e, const sdbus_held_msg *m) {
    e->held = realloc(e->held, (e->n_held + 1) * sizeof *e->held);
    sdbus_held_msg *d = &e->held[e->n_held++];
    *d = *m;
    if (m->len > 0 && m->bytes) { d->bytes = malloc(m->len); memcpy(d->bytes, m->bytes, m->len); }
    else { d->bytes = NULL; d->len = 0; }
    if (m->nfds > 0 && m->fds) { d->fds = malloc(m->nfds * sizeof(int)); memcpy(d->fds, m->fds, m->nfds * sizeof(int)); }
    else { d->fds = NULL; d->nfds = 0; }
}

/* remove entry at index i, handing its held[] array to *out (ownership transfers). */
static inline void sdbus__acts_pop(sdbus_acts *a, int i, sdbus_held_msg **out, int *n) {
    *out = a->v[i].held; *n = a->v[i].n_held;
    free(a->v[i].name);
    a->v[i] = a->v[--a->n];             /* swap-remove */
}

static inline int sdbus_acts_take(sdbus_acts *a, const char *name,
                                  sdbus_held_msg **out, int *n) {
    for (int i = 0; i < a->n; i++)
        if (!strcmp(a->v[i].name, name)) { sdbus__acts_pop(a, i, out, n); return 1; }
    *out = NULL; *n = 0; return 0;
}

static inline long sdbus_acts_next_deadline(sdbus_acts *a) {
    long best = -1;
    for (int i = 0; i < a->n; i++)
        if (best < 0 || a->v[i].deadline_ms < best) best = a->v[i].deadline_ms;
    return best;
}

static inline int sdbus_acts_reap_expired(sdbus_acts *a, long now_ms,
                                          sdbus_held_msg **out, int *n) {
    for (int i = 0; i < a->n; i++)
        if (a->v[i].deadline_ms <= now_ms) { sdbus__acts_pop(a, i, out, n); return 1; }
    *out = NULL; *n = 0; return 0;
}

static inline void sdbus_acts_free(sdbus_acts *a) {
    if (!a) return;
    for (int i = 0; i < a->n; i++) {
        free(a->v[i].name);
        for (int j = 0; j < a->v[i].n_held; j++) { free(a->v[i].held[j].bytes); free(a->v[i].held[j].fds); }
        free(a->v[i].held);
    }
    free(a->v); free(a);
}

#endif
