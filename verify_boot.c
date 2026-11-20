#include "verify_boot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int grubenv_read(const char *path, char (*kv)[2][256], int max) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        snprintf(kv[n][0], 256, "%.*s", (int)(eq - line), line);
        snprintf(kv[n][1], 256, "%s", eq + 1);
        n++;
    }
    fclose(f);
    return n;
}

static int put(char *dst, size_t sz, const char *src) {
    return snprintf(dst, sz, "%s", src) >= (int)sz;
}

int bls_parse(const char *path, bls_entry_t *e) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(e, 0, sizeof *e);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t bl = strlen(base);
    if (bl > 5 && !strcmp(base + bl - 5, ".conf")) bl -= 5;
    e->parse_err |= snprintf(e->id, sizeof e->id, "%.*s", (int)bl, base) >= (int)sizeof e->id;

    char line[8192];
    int bad_seen = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *k = line + strspn(line, " \t");
        if (*k == '#') continue;
        size_t kl = strcspn(k, " \t");
        char *v = k + kl;
        v += strspn(v, " \t");
        /* blscfg stops the entry at a line with no value; whatever follows is dropped */
        if (!kl || !*v) {
            bad_seen = 1;
            continue;
        }
        if (bad_seen) e->blank_before_last = 1;
        char *end = v + strlen(v);
        while (end > v && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        /* blscfg reads the first linux and options line; a second is reported, not guessed */
        if (kl == 5 && !strncmp(k, "linux", 5)) {
            if (e->linux_path[0]) e->extra_linux = 1;
            else e->parse_err |= put(e->linux_path, sizeof e->linux_path, v);
        } else if (kl == 6 && !strncmp(k, "initrd", 6)) {
            for (char *t = strtok(v, " \t"); t; t = strtok(NULL, " \t")) {
                if (*t == '$') continue; /* a GRUB variable such as $tuned_initrd, not a file */
                if (e->n_initrd == 8) { e->parse_err = 1; break; }
                e->parse_err |= put(e->initrd[e->n_initrd++], sizeof e->initrd[0], t);
            }
        } else if (kl == 7 && !strncmp(k, "options", 7)) {
            if (e->options[0]) e->extra_options = 1;
            else e->parse_err |= put(e->options, sizeof e->options, v);
        }
    }
    fclose(f);
    return 0;
}

int karg_check(char (*args)[512], int n, char *why, size_t whysz) {
    static const char *const keys[] = { "root=", "rootflags=", "init=" };
    for (size_t k = 0; k < sizeof keys / sizeof *keys; k++) {
        size_t kl = strlen(keys[k]);
        const char *first = NULL;
        for (int i = 0; i < n && strcmp(args[i], "--"); i++) {
            if (strncmp(args[i], keys[k], kl)) continue;
            if (!first) { first = args[i]; continue; }
            if (strcmp(first, args[i]))
                snprintf(why, whysz, "conflicting %s: %s and %s", keys[k], first, args[i]);
            else
                snprintf(why, whysz, "duplicate %s", args[i]);
            return -1;
        }
        if (!first && k == 0) {
            snprintf(why, whysz, "no root= in the kernel arguments");
            return -1;
        }
    }
    return 0;
}

static const char *env_get(char (*env)[2][256], int n, const char *k) {
    for (int i = n - 1; i >= 0; i--)
        if (!strcmp(env[i][0], k)) return env[i][1][0] ? env[i][1] : NULL;
    return NULL;
}

static const char *ent_find(const bls_entry_t *ents, int n, const char *id) {
    for (int i = 0; i < n; i++)
        if (!strcmp(ents[i].id, id)) return ents[i].id;
    return NULL;
}

int nextboot_resolve(char (*env)[2][256], int nenv, const bls_entry_t *ents, int nents, nextboot_t *nb) {
    const char *saved = env_get(env, nenv, "saved_entry"), *next = env_get(env, nenv, "next_entry");
    const char *fb = env_get(env, nenv, "schema_fallback_id");
    const char *cnt = env_get(env, nenv, "boot_counter"), *ok = env_get(env, nenv, "boot_success");
    memset(nb, 0, sizeof *nb);

    if (saved) nb->normal = ent_find(ents, nents, saved);
    else if (nents) nb->normal = ents[0].id;
    nb->actual = next ? ent_find(ents, nents, next) : nb->normal;
    if (!nb->normal || (next && !nb->actual)) nb->unresolved = 1;

    if (fb) {
        nb->fallback = ent_find(ents, nents, fb);
    } else {
        /* what schema-snapshot arm would pick: newest schema-snap-*, else newest schema-good* */
        static const char *const pre[] = { "schema-snap-", "schema-good" };
        for (int k = 0; k < 2 && !nb->fallback; k++)
            for (int i = 0; i < nents; i++)
                if (!strncmp(ents[i].id, pre[k], strlen(pre[k])) &&
                    (!nb->fallback || strcmp(ents[i].id, nb->fallback) > 0))
                    nb->fallback = ents[i].id;
    }

    /* 08_fallback_counting runs first: 0/-1 become -1, anything else is decremented,
     * so a 1 in grubenv reads 0 by the time 09_schema_fallback tests it */
    if (cnt && ok && !strcmp(ok, "0")) {
        int spent = !strcmp(cnt, "0") || !strcmp(cnt, "-1");
        if (fb && (spent || !strcmp(cnt, "1"))) {
            nb->actual = nb->fallback;
        } else if (spent) {
            /* 08's stock default=1 is a menu position, which blscfg's sort decides */
            nb->actual = NULL;
            nb->unresolved = 1;
        }
    }

    if (fb && !nb->fallback) {
        nb->fallback = fb;
        nb->unresolved = 1;
        return 2;
    }
    return nb->unresolved;
}

static int has_word(const char *s, const char *w) {
    size_t n = strlen(w);
    return !strncmp(s, w, n) && (!s[n] || s[n] == ' ' || s[n] == '\t' || s[n] == ';');
}

int grubcfg_live_fallback(const char *cfg) {
    /* per open if: 1 while in the then-branch of the boot_counter/boot_success test */
    char guard[64] = { 0 };
    int depth = 0, braces = 0, live = 0;
    for (const char *p = cfg; *p;) {
        size_t ll = strcspn(p, "\n");
        char line[1024];
        snprintf(line, sizeof line, "%.*s", (int)ll, p);
        p += ll + (p[ll] == '\n');
        char *s = line + strspn(line, " \t");
        char *end = s + strlen(s);
        while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = '\0';
        if (!*s || *s == '#') continue;

        if (has_word(s, "if")) {
            if (depth < (int)sizeof guard)
                guard[depth] = strstr(s, "${boot_counter}") && strstr(s, "\"${boot_success}\" = \"0\"");
            depth++;
        } else if ((has_word(s, "elif") || has_word(s, "else")) && depth > 0) {
            if (depth <= (int)sizeof guard) guard[depth - 1] = 0;
        } else if (has_word(s, "fi") && depth > 0) {
            depth--;
        } else if (end > s && end[-1] == '{') {
            braces++;
        } else if (!strcmp(s, "}")) {
            braces--;
        } else if (!strncmp(s, "set default=", 12) && !braces) {
            int guarded = 0;
            for (int i = 0; i < depth && i < (int)sizeof guard; i++) guarded |= guard[i];
            /* any later top-level set default= overrides an earlier fallback line */
            live = guarded && !strcmp(s, "set default=\"${schema_fallback_id}\"");
        }
    }
    return live;
}
