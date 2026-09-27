#include "../../sdbus_match.h"

static const char *field(const char **p, const char *end) {
    if (*p >= end) return NULL;
    const char *s = *p;
    while (*p < end && **p) (*p)++;
    if (*p >= end) return NULL;
    (*p)++;
    return s;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    if (size > 1 << 16) return 0;
    char *buf = malloc(size + 1);
    memcpy(buf, data, size); buf[size] = 0;
    const char *p = buf, *end = buf + size + 1;
    sdbus_matchset *ms = sdbus_match_new();
    const char *rule;
    int nrules = 0;
    while ((rule = field(&p, end)) && *rule != '#' && nrules++ < 8) sdbus_match_add(ms, rule);
    const char *type = field(&p, end), *iface = field(&p, end), *member = field(&p, end);
    const char *path = field(&p, end), *arg0 = field(&p, end), *sender = field(&p, end);
    const char *dest = field(&p, end), *owned = field(&p, end);
    const char *ownv[1] = { owned };
    sdbus_match_signal(ms, iface, member, path, sender, ownv, owned ? 1 : 0, arg0);
    sdbus_match_message(ms, type, iface, member, path, arg0, sender, ownv, owned ? 1 : 0, dest, ownv, owned ? 1 : 0);
    if (ms->n) sdbus_match_remove(ms, ms->rules[0].raw);
    sdbus_match_free(ms);
    free(buf);
    return 0;
}
