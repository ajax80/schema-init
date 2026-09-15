#ifndef SYSTEMCTL_SHIM_H
#define SYSTEMCTL_SHIM_H

#include <stdlib.h>
#include <string.h>

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

__attribute__((unused)) static int shim_dispatch(int argc, char **argv) {
    (void)argc; (void)argv;
    return 0;
}

#endif
