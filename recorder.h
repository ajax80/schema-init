#ifndef SCHEMA_RECORDER_H
#define SCHEMA_RECORDER_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define REC_SOCK_PATH  "/run/schema-init/recorder.sock"
#define REC_SHM_NAME   "/schema-recorder"
#define REC_SHM_MAGIC  0x53434652u   /* "SCFR" */
#define REC_RING       128
#define REC_EVENTS_DIR "/var/lib/schema-init"

enum rec_src { REC_SRC_PID1 = 1, REC_SRC_COREDUMP, REC_SRC_SESSION, REC_SRC_EARLYOOM, REC_SRC_PSI };

typedef struct {
    uint64_t ts;
    uint8_t  src;
    uint8_t  tier;
    uint8_t  crit;
    int32_t  pid;
    int32_t  rc;
    char     kind[24];
    char     comm[32];
    char     subj[64];
    char     msg[160];
} rec_event_t;

typedef struct {
    uint8_t  state;          /* 0 none, 1 live, 2 resolved */
    uint8_t  tier;
    uint64_t event_ts;
    uint64_t resolved_ts;
    uint32_t recurrence;
    char     line[160];
} rec_arrow_t;

typedef struct {
    uint32_t    magic;
    uint32_t    seq;
    rec_arrow_t arrow;
    uint32_t    head;
    uint32_t    count;
    rec_event_t ring[REC_RING];
} rec_shm_t;

static const char *const rec_src_names[] = { "", "pid1", "coredump", "session", "earlyoom", "psi" };

static inline void rec_word(char *dst, size_t n, const char *s) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) {
        unsigned char c = (unsigned char)s[i];
        dst[i] = (c <= ' ' || c == '=' || c == 0x7f) ? '_' : (char)c;
    }
    dst[i] = '\0';
}

static inline void rec_copy(char *dst, size_t n, const char *s) {
    size_t l = strlen(s);
    if (l >= n) l = n - 1;
    memcpy(dst, s, l);
    dst[l] = '\0';
}

static inline int rec_format(char *buf, size_t n, const rec_event_t *e) {
    char kind[sizeof e->kind], comm[sizeof e->comm], subj[sizeof e->subj], msg[sizeof e->msg];
    size_t i;
    int r;
    rec_word(kind, sizeof kind, e->kind);
    rec_word(comm, sizeof comm, e->comm);
    rec_word(subj, sizeof subj, e->subj);
    snprintf(msg, sizeof msg, "%.*s", (int)sizeof e->msg - 1, e->msg);
    for (i = 0; msg[i]; i++) if (msg[i] == '\n' || msg[i] == '\r') msg[i] = ' ';
    r = snprintf(buf, n, "ts=%llu src=%s kind=%s comm=%s pid=%d rc=%d crit=%d subj=%s msg=%s",
                 (unsigned long long)e->ts,
                 e->src >= 1 && e->src <= REC_SRC_PSI ? rec_src_names[e->src] : "?",
                 kind, comm, (int)e->pid, (int)e->rc, (int)e->crit, subj, msg);
    return r < 0 || (size_t)r >= n ? -1 : r;
}

static inline int rec_parse(const char *line, rec_event_t *out) {
    int have_ts = 0, have_src = 0, have_kind = 0;
    const char *p = line;
    memset(out, 0, sizeof *out);
    while (*p && *p != '\n') {
        const char *eq, *end;
        size_t klen, vlen;
        char key[16], val[sizeof out->msg];
        while (*p == ' ') p++;
        if (!*p || *p == '\n') break;
        eq = strchr(p, '=');
        end = p;
        while (*end && *end != ' ' && *end != '\n') end++;
        if (!eq || eq >= end) { p = end; continue; }
        klen = (size_t)(eq - p);
        if (klen == 3 && !strncmp(p, "msg", 3)) {
            end = eq + 1;
            while (*end && *end != '\n') end++;
        }
        vlen = (size_t)(end - eq - 1);
        if (klen >= sizeof key) { p = end; continue; }
        if (vlen >= sizeof val) vlen = sizeof val - 1;
        memcpy(key, p, klen); key[klen] = '\0';
        memcpy(val, eq + 1, vlen); val[vlen] = '\0';
        if (!strcmp(key, "ts")) { out->ts = strtoull(val, NULL, 10); have_ts = 1; }
        else if (!strcmp(key, "src")) {
            int s;
            for (s = 1; s <= REC_SRC_PSI; s++)
                if (!strcmp(val, rec_src_names[s])) { out->src = (uint8_t)s; have_src = 1; }
        }
        else if (!strcmp(key, "kind")) { rec_copy(out->kind, sizeof out->kind, val); have_kind = 1; }
        else if (!strcmp(key, "comm")) rec_copy(out->comm, sizeof out->comm, val);
        else if (!strcmp(key, "subj")) rec_copy(out->subj, sizeof out->subj, val);
        else if (!strcmp(key, "msg")) rec_copy(out->msg, sizeof out->msg, val);
        else if (!strcmp(key, "pid")) out->pid = (int32_t)strtol(val, NULL, 10);
        else if (!strcmp(key, "rc")) out->rc = (int32_t)strtol(val, NULL, 10);
        else if (!strcmp(key, "crit")) out->crit = (uint8_t)strtoul(val, NULL, 10);
        p = end;
    }
    return have_ts && have_src && have_kind ? 0 : -1;
}

static inline void rec_send(const char *sock_path, const rec_event_t *e) {
    rec_event_t c = *e;
    char buf[640];
    struct sockaddr_un a;
    int fd, n;
    if (!c.ts) c.ts = (uint64_t)time(NULL);
    n = rec_format(buf, sizeof buf, &c);
    if (n < 0) return;
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock_path);
    (void)sendto(fd, buf, (size_t)n, MSG_DONTWAIT, (struct sockaddr *)&a, sizeof a);
    close(fd);
}

static inline const rec_shm_t *rec_shm_map(int fd) {
    struct stat st;
    const rec_shm_t *m;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof *m) return NULL;
    m = mmap(NULL, sizeof *m, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return NULL;
    if (m->magic != REC_SHM_MAGIC) { munmap((void *)m, sizeof *m); return NULL; }
    return m;
}

#endif
