#ifndef SDBUS_ACTIVATE_H
#define SDBUS_ACTIVATE_H

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static inline sdbus_svctab *sdbus_svctab_parse_dir(const char *dir) {
    sdbus_svctab *t = calloc(1, sizeof *t);
    DIR *d = opendir(dir);
    if (!d) return t;                       /* empty table, not NULL */
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
        sdbus__svc_add(t, name, exec, user);
    }
    closedir(d);
    return t;
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

#endif
