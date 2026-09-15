#ifndef SYSTEMCTL_SHIM_H
#define SYSTEMCTL_SHIM_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

__attribute__((unused)) static const char *shim_state_dir(void) {
    const char *e = getenv("SCHEMA_STATE_DIR");
    return (e && *e) ? e : "/var/lib/schema-init";
}
__attribute__((unused)) static const char *shim_svc_dir(void) {
    const char *e = getenv("SCHEMA_SVC_DIR");
    return (e && *e) ? e : "/etc/schema-init/services";
}
__attribute__((unused)) static const char *shim_ctl(void) {
    const char *e = getenv("SCHEMA_CTL");
    return (e && *e) ? e : "schema-ctl";
}

__attribute__((unused)) static const char *strip_service_suffix(const char *unit, char *buf, size_t n) {
    size_t len = strlen(unit);
    const char *suf = ".service";
    size_t slen = strlen(suf);
    if (len > slen && strcmp(unit + len - slen, suf) == 0)
        len -= slen;
    if (len >= n) len = n - 1;
    memcpy(buf, unit, len);
    buf[len] = '\0';
    return buf;
}

__attribute__((unused)) static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lu = strlen(suf);
    return ls >= lu && strcmp(s + ls - lu, suf) == 0;
}

__attribute__((unused)) static int unit_supported(const char *unit) {
    static const char *bad[] = {
        ".socket", ".timer", ".path", ".target",
        ".mount", ".slice", ".scope", NULL
    };
    int i;
    if (strchr(unit, '@')) return 0;
    for (i = 0; bad[i]; i++)
        if (ends_with(unit, bad[i])) return 0;
    return 1;
}

__attribute__((unused)) static char *resolve_unit_path(const char *unit, char *buf, size_t n) {
    const char *one = getenv("SCHEMA_UNIT_DIR");
    const char *dirs[4]; int i, nd = 0;
    if (one && *one) {
        dirs[nd++] = one;
    } else {
        dirs[nd++] = "/etc/systemd/system";
        dirs[nd++] = "/usr/lib/systemd/system";
        dirs[nd++] = "/lib/systemd/system";
    }
    for (i = 0; i < nd; i++) {
        snprintf(buf, n, "%s/%s", dirs[i], unit);
        if (access(buf, F_OK) == 0) return buf;
    }
    if (!ends_with(unit, ".service")) {
        for (i = 0; i < nd; i++) {
            snprintf(buf, n, "%s/%s.service", dirs[i], unit);
            if (access(buf, F_OK) == 0) return buf;
        }
    }
    snprintf(buf, n, "%s", unit);
    return buf;
}

__attribute__((unused)) static int queue_path(char *buf, size_t n) {
    snprintf(buf, n, "%s/pending.list", shim_state_dir());
    return 0;
}

__attribute__((unused)) static int queue_contains(const char *line) {
    char qp[512], cur[512];
    queue_path(qp, sizeof qp);
    FILE *f = fopen(qp, "r");
    if (!f) return 0;
    int found = 0;
    while (fgets(cur, sizeof cur, f)) {
        cur[strcspn(cur, "\n")] = '\0';
        if (strcmp(cur, line) == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}

__attribute__((unused)) static int queue_add(const char *line) {
    if (queue_contains(line)) return 0;
    mkdir(shim_state_dir(), 0755);
    char qp[512];
    queue_path(qp, sizeof qp);
    FILE *f = fopen(qp, "a");
    if (!f) return -1;
    fprintf(f, "%s\n", line);
    fclose(f);
    return 0;
}

__attribute__((unused)) static int queue_remove(const char *line) {
    char qp[512], tmp[520], cur[512];
    queue_path(qp, sizeof qp);
    FILE *f = fopen(qp, "r");
    if (!f) return 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", qp);
    FILE *o = fopen(tmp, "w");
    if (!o) { fclose(f); return -1; }
    while (fgets(cur, sizeof cur, f)) {
        char trimmed[512];
        snprintf(trimmed, sizeof trimmed, "%s", cur);
        trimmed[strcspn(trimmed, "\n")] = '\0';
        if (strcmp(trimmed, line) != 0) fputs(cur, o);
    }
    fclose(f); fclose(o);
    rename(tmp, qp);
    return 0;
}

__attribute__((unused)) static int svc_exists(const char *name) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s.svc", shim_svc_dir(), name);
    return access(p, F_OK) == 0;
}

__attribute__((unused)) static int shim_dispatch(int argc, char **argv) {
    if (argc < 2) return 0;
    const char *verb = argv[1];
    int i;

    if (strcmp(verb, "enable") == 0 || strcmp(verb, "preset") == 0) {
        for (i = 2; i < argc; i++) {
            if (argv[i][0] == '-') continue;
            if (!unit_supported(argv[i])) continue;
            char path[512];
            resolve_unit_path(argv[i], path, sizeof path);
            queue_add(path);
        }
        return 0;
    }
    if (strcmp(verb, "disable") == 0) {
        for (i = 2; i < argc; i++) {
            if (argv[i][0] == '-') continue;
            if (!unit_supported(argv[i])) continue;
            char path[512], name[256];
            resolve_unit_path(argv[i], path, sizeof path);
            queue_remove(path);
            strip_service_suffix(argv[i], name, sizeof name);
            char svc[512];
            snprintf(svc, sizeof svc, "%s/%s.svc", shim_svc_dir(), name);
            unlink(svc);
        }
        return 0;
    }
    if (strcmp(verb, "is-enabled") == 0) {
        for (i = 2; i < argc; i++) {
            if (argv[i][0] == '-') continue;
            if (!unit_supported(argv[i])) return 1;
            char path[512], name[256];
            resolve_unit_path(argv[i], path, sizeof path);
            strip_service_suffix(argv[i], name, sizeof name);
            if (queue_contains(path) || svc_exists(name)) return 0;
            return 1;
        }
        return 1;
    }
    return 0;
}

#endif
