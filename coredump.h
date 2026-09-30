#ifndef COREDUMP_H
#define COREDUMP_H

/* Crash capture. Fedora's initrd sets kernel.core_pattern to pipe into
 * systemd-coredump, which only forwards to a socket-activated
 * systemd-coredump.socket that never exists under schema-init: every crash is
 * logged as "Failed to connect to coredump service" and the core is lost.
 * PID 1 takes that pattern (and only that one: apport, abrt or a hand-set
 * pattern is left alone) for schema-coredump, which stores a capped zstd core
 * plus a metadata file per crash. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>

#define COREDUMP_HELPER      "/usr/bin/schema-coredump"
#define COREDUMP_PATTERN     "|" COREDUMP_HELPER " %P %u %g %s %t %c %d"
#define COREDUMP_PATTERN_SYS "/proc/sys/kernel/core_pattern"
#define COREDUMP_SYSTEMD     "|/usr/lib/systemd/systemd-coredump"
#define COREDUMP_DIR         "/var/lib/schema-coredump"
#define COREDUMP_RAW_MAX     (2ULL << 30)   /* uncompressed bytes read per crash */
#define COREDUMP_DIR_MAX     (4ULL << 30)   /* compressed cores kept in total */
#define COREDUMP_FREE_PCT    10             /* never fill the filesystem past this */
#define COREDUMP_KEEP        200            /* metadata records kept */
#define COREDUMP_RATE_SECS   60             /* one stored core per comm per window */

/* 1: CUR is the pattern to replace; 2: it is ours already; 0: leave it. */
static inline int coredump_pattern_classify(const char *cur) {
    size_t l = strcspn(cur, "\n");
    size_t sl = strlen(COREDUMP_SYSTEMD);
    if (l == strlen(COREDUMP_PATTERN) && !memcmp(cur, COREDUMP_PATTERN, l)) return 2;
    if (l >= sl && !memcmp(cur, COREDUMP_SYSTEMD, sl) && (l == sl || cur[sl] == ' ')) return 1;
    return 0;
}

/* Returns 1 when the kernel pattern is ours afterwards, 0 when it belongs to
 * someone else, -1 on error. */
static inline int coredump_take_pattern(void) {
    char cur[256];
    int fd = open(COREDUMP_PATTERN_SYS, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, cur, sizeof cur - 1);
    close(fd);
    if (n < 0) return -1;
    cur[n] = '\0';
    int c = coredump_pattern_classify(cur);
    if (c != 1) return c == 2;
    if (access(COREDUMP_HELPER, X_OK) != 0) return 0;
    fd = open(COREDUMP_PATTERN_SYS, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t w = write(fd, COREDUMP_PATTERN, strlen(COREDUMP_PATTERN));
    close(fd);
    return w == (ssize_t)strlen(COREDUMP_PATTERN) ? 1 : -1;
}

/* Soft RLIMIT_CORE up to the hard limit, but only while the pattern is ours:
 * a host whose initrd never set one boots with the kernel's "core", and
 * sysctl.svc hands it to schema-coredump later, so each spawn checks. */
static inline void coredump_raise_if_ours(void) {
    char cur[256];
    struct rlimit rl;
    int fd = open(COREDUMP_PATTERN_SYS, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n = read(fd, cur, sizeof cur - 1);
    close(fd);
    if (n < 0) return;
    cur[n] = '\0';
    if (coredump_pattern_classify(cur) != 2) return;
    if (getrlimit(RLIMIT_CORE, &rl) != 0 || rl.rlim_cur >= rl.rlim_max) return;
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_CORE, &rl);
}

/* "0::/schema-init/<svc>[/...]" -> <svc>; empty when not a service. */
static inline void coredump_service_from_cgroup(const char *text, char *out, size_t sz) {
    const char *pfx = "0::/schema-init/";
    out[0] = '\0';
    const char *p = strstr(text, pfx);
    if (!p || (p != text && p[-1] != '\n') || sz == 0) return;
    p += strlen(pfx);
    size_t l = strcspn(p, "/\n");
    if (l >= sz) l = sz - 1;
    memcpy(out, p, l);
    out[l] = '\0';
}

/* A value for a KEY=VALUE line: no control characters. */
static inline void coredump_clean_value(char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f) *s = ' ';
}

/* A file-name component: [A-Za-z0-9._+-], no leading dot, never empty. */
static inline void coredump_clean_name(char *s, size_t sz) {
    if (sz == 0) return;
    if (!*s) { snprintf(s, sz, "unknown"); return; }
    for (char *c = s; *c; c++) {
        unsigned char u = (unsigned char)*c;
        int ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
                 u == '.' || u == '_' || u == '+' || u == '-';
        if (!ok) *c = '_';
    }
    if (s[0] == '.') s[0] = '_';
}

/* core.<comm>.<uid>.<time>.<pid>.<ext> -> <comm>, which may itself hold dots. */
static inline void coredump_stem_comm(const char *name, char *out, size_t sz) {
    out[0] = '\0';
    if (strncmp(name, "core.", 5) || sz == 0) return;
    const char *s = name + 5, *e = s + strlen(s);
    for (int dots = 0; e > s && dots < 4; ) if (*--e == '.') dots++;
    if (e <= s || *e != '.') return;
    size_t l = (size_t)(e - s);
    if (l >= sz) l = sz - 1;
    memcpy(out, s, l);
    out[l] = '\0';
}

#endif /* COREDUMP_H */
