/*
 * schema-journal-sink — Track B journald compatibility shim.
 *
 * Wears journald's face without its machinery: we provide journald's three
 * ingestion sockets (plus the /run/systemd/journal/ dir), drain them, and
 * write plain text to schema-init's log dir. No binary journal, no journalctl.
 *
 * Opt-in, non-critical. schema-init itself never needs this to boot (it dup2s
 * each service's own log fd); this exists only so foreign libsystemd / syslog(3)
 * software finds a journald-shaped endpoint. See docs/journal-sink-design.md.
 *
 * This replaces rsyslog as well: it owns /dev/log, unlinking and rebinding it
 * at start. An rsyslogd beside it is left on a dead socket, silently getting
 * nothing. An empty /var/log/messages is by design, not breakage. If
 * facility-split files are wanted, fan out here.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "fdclose.h"

#define DEV_LOG       "/dev/log"
#define JOURNAL_DIR   "/run/systemd/journal"
#define NATIVE_SOCK   JOURNAL_DIR "/socket"
#define STDOUT_SOCK   JOURNAL_DIR "/stdout"

#define DEFAULT_OUT   "/var/log/schema-init/journal.log"
#define FALLBACK_OUT  "/run/log/schema-init/journal.log"

#define LINE_MAX_     8192
#define MAX_STREAM_CONNS 64
#define MAX_FD_BYTES  (1u << 20)   /* cap on a single SCM_RIGHTS-passed entry */

static const char *prio[] = {
    "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"
};

static int   out_fd  = -1;
static int   kmsg_fd = -1;
static off_t out_sz  = 0;
static off_t out_cap = 0;       /* JOURNAL_SINK_MAXBYTES, 0 = unbounded */

/* ---- output ------------------------------------------------------------- */

static void iso_now(char *out, size_t n) {
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

static void emit(const char *src, int sev, const char *tag,
                 const char *msg, size_t mlen) {
    char ts[32], line[LINE_MAX_ + 256], sane[LINE_MAX_];
    const char *pn = (sev >= 0 && sev < 8) ? prio[sev] : "-";
    if (!tag || !*tag) tag = "-";
    if (mlen > LINE_MAX_) mlen = LINE_MAX_;

    /* one message = one physical line: fold embedded CR/LF to spaces */
    for (size_t i = 0; i < mlen; i++) {
        char ch = msg[i];
        sane[i] = (ch == '\n' || ch == '\r') ? ' ' : ch;
    }

    iso_now(ts, sizeof ts);
    int n = snprintf(line, sizeof line, "%s %s %s %s: %.*s\n",
                     ts, src, pn, tag, (int)mlen, sane);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = sizeof line - 1;

    /* size cap: single-file truncate-and-restart (no journald-grade vacuum) */
    if (out_cap > 0 && out_sz + n > out_cap) {
        if (ftruncate(out_fd, 0) == 0) {
            lseek(out_fd, 0, SEEK_SET);
            out_sz = 0;
        }
    }
    write_all(out_fd, line, (size_t)n);
    out_sz += n;

    if (kmsg_fd >= 0) {
        char kline[LINE_MAX_ + 64];
        int s = (sev >= 0 && sev < 8) ? sev : 6;   /* LOG_USER facility (1) */
        int kn = snprintf(kline, sizeof kline, "<%d>%s: %.*s\n",
                          8 + s, tag, (int)mlen, sane);
        if (kn > 0) write_all(kmsg_fd, kline, (size_t)kn);
    }
}

/* ---- native protocol (/run/systemd/journal/socket) ---------------------- */

/* Entry = newline-separated FIELD=value lines; a binary field is
 * FIELD\n <u64 LE length> <raw bytes> \n. We keep only MESSAGE, plus
 * PRIORITY and SYSLOG_IDENTIFIER for the output line. */
static void parse_native(const char *buf, size_t len) {
    const char *p = buf, *end = buf + len;
    const char *msg = NULL, *tag = NULL;
    size_t msglen = 0, taglen = 0;
    int sev = -1;
    char tagbuf[256];

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        const char *eq = memchr(p, '=', (size_t)(nl - p));
        if (eq) {
            size_t flen = (size_t)(eq - p);
            const char *v = eq + 1;
            size_t vlen = (size_t)(nl - v);
            if (flen == 7 && !memcmp(p, "MESSAGE", 7)) {
                msg = v; msglen = vlen;
            } else if (flen == 8 && !memcmp(p, "PRIORITY", 8)) {
                if (vlen) sev = atoi(v);
            } else if (flen == 17 && !memcmp(p, "SYSLOG_IDENTIFIER", 17)) {
                tag = v; taglen = vlen;
            }
            p = nl + 1;
        } else {
            /* binary field: FIELD \n <u64 LE> <raw> \n */
            size_t flen = (size_t)(nl - p);
            const char *lp = nl + 1;
            if (lp + 8 > end) break;
            unsigned long long blen = 0;
            for (int i = 0; i < 8; i++)
                blen |= (unsigned long long)(unsigned char)lp[i] << (8 * i);
            const char *raw = lp + 8;
            /* compare as integers: raw + blen can overflow the pointer (UB) and
             * wrap past `end`, slipping a hostile length through. raw <= end
             * here, so end - raw is a safe non-negative bound. */
            if (blen > (unsigned long long)(end - raw)) break;
            if (flen == 7 && !memcmp(p, "MESSAGE", 7)) {
                msg = raw; msglen = (size_t)blen;
            }
            p = raw + blen;
            if (p < end && *p == '\n') p++;
        }
    }

    if (tag && taglen < sizeof tagbuf) {
        memcpy(tagbuf, tag, taglen);
        tagbuf[taglen] = '\0';
    } else {
        tagbuf[0] = '\0';
    }
    if (msg) emit("native", sev, tagbuf, msg, msglen);
}

/* 1 if fd lives on a tmpfs mount. Asked of /proc, never of the fd: fstat,
 * fstatfs and read on a FUSE fd go to its daemon and can block. */
static int fd_on_tmpfs(int fd) {
    char p[64], line[1024];
    int mnt = -1, found = 0;
    snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(p, "re");
    if (!f) return 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "mnt_id: %d", &mnt) == 1) break;
    fclose(f);
    if (mnt < 0 || !(f = fopen("/proc/self/mountinfo", "re"))) return 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *sep = strstr(line, " - ");
        char fstype[32];
        if (atoi(line) == mnt && sep && sscanf(sep + 3, "%31s", fstype) == 1)
            found = strcmp(fstype, "tmpfs") == 0 ? 1 : -1;
    }
    fclose(f);
    return found == 1;
}

/* Parse a SCM_RIGHTS-passed entry. The socket is 0666 and the sink is single
 * threaded: a pipe with a live writer, or a FUSE file, would block it forever
 * and take all logging with it. So, as journald: a sealed memfd, or a regular
 * file on tmpfs, read with pread up to its size. Nothing touches the fd until
 * one of those is established (F_GET_SEALS is answered by the kernel). */
static void drain_passed_fd(int fd) {
    static char fbuf[MAX_FD_BYTES];
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    int sealed = seals >= 0 && (seals & (F_SEAL_WRITE | F_SEAL_SHRINK)) == (F_SEAL_WRITE | F_SEAL_SHRINK);
    if (!sealed && !fd_on_tmpfs(fd)) return;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) return;
    size_t want = (size_t)st.st_size < sizeof fbuf ? (size_t)st.st_size : sizeof fbuf;
    size_t off = 0;
    while (off < want) {
        ssize_t r = pread(fd, fbuf + off, want - off, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        off += (size_t)r;
    }
    if (off) parse_native(fbuf, off);
}

static void handle_native(int sock) {
    char buf[LINE_MAX_];
    /* room for SCM_MAX_FD: a truncated message has the kernel close the
     * rest in this process, and that close can block like any other */
    char ctl[CMSG_SPACE(sizeof(int) * FDCLOSE_MSG_FDS)];
    for (;;) {
        struct iovec iov = { buf, sizeof buf };
        struct msghdr mh = { 0 };
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = ctl;
        mh.msg_controllen = sizeof ctl;

        ssize_t r = recvmsg(sock, &mh, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;                 /* EAGAIN: drained */
        }

        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int *fds = (int *)CMSG_DATA(c);
                int nf = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                for (int i = 0; i < nf; i++)
                    drain_passed_fd(fds[i]);
                fd_close_detached(fds, nf);     /* close() can block too: a FUSE flush */
                if (fdclose_full()) return;     /* the next message may not fit the park */
            }
        }
        if (r > 0) parse_native(buf, (size_t)r);
    }
}

/* ---- /dev/log (classic syslog datagrams) -------------------------------- */

/* Best-effort: strip leading <PRI>, lift a "tag:" prefix if present. */
static void handle_devlog(int sock) {
    char buf[LINE_MAX_];
    for (;;) {
        ssize_t r = recv(sock, buf, sizeof buf, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;
        size_t len = (size_t)r;
        while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\0')) len--;

        const char *p = buf;
        const char *pend = buf + len;
        int sev = -1;
        if (p < pend && *p == '<') {
            const char *gt = memchr(p, '>', (size_t)(pend - p));
            if (gt) {
                int pri = atoi(p + 1);
                sev = pri & 7;
                p = gt + 1;
            }
        }
        /* optional RFC3164 timestamp "Mmm DD HH:MM:SS " prepended by logger(1) */
        if (pend - p >= 16 && p[3] == ' ' && p[6] == ' ' &&
            p[9] == ':' && p[12] == ':' && p[15] == ' ')
            p += 16;
        /* optional "tag: message" or "tag[pid]: message" */
        char tag[256]; tag[0] = '\0';
        const char *colon = memchr(p, ':', (size_t)(pend - p));
        if (colon && colon - p < (ptrdiff_t)sizeof tag - 1) {
            const char *sp = memchr(p, ' ', (size_t)(colon - p));
            if (!sp) {
                size_t tl = (size_t)(colon - p);
                const char *br = memchr(p, '[', tl);
                if (br) tl = (size_t)(br - p);
                memcpy(tag, p, tl);
                tag[tl] = '\0';
                p = colon + 1;
                while (p < pend && *p == ' ') p++;
            }
        }
        emit("devlog", sev, tag, p, (size_t)(pend - p));
    }
}

/* ---- stdout stream (/run/systemd/journal/stdout) ------------------------ */

struct sconn {
    int    fd;
    int    hdr_left;            /* header lines remaining (starts at 7) */
    char   ident[256];
    char   buf[LINE_MAX_];
    size_t buf_len;
};

static void sconn_line(struct sconn *c, const char *line, size_t len) {
    if (c->hdr_left > 0) {
        if (c->hdr_left == 7) {            /* line 1: identifier (tag) */
            size_t n = len < sizeof c->ident - 1 ? len : sizeof c->ident - 1;
            memcpy(c->ident, line, n);
            c->ident[n] = '\0';
        }
        c->hdr_left--;                     /* lines 2-7: unit/pri/flags */
        return;
    }
    int sev = -1;                          /* level-prefix form: <N>payload */
    if (len >= 3 && line[0] == '<' && line[2] == '>') {
        sev = line[1] - '0';
        line += 3; len -= 3;
    }
    emit("stdout", sev, c->ident, line, len);
}

static void sconn_read(struct sconn *c) {
    for (;;) {
        ssize_t r = read(c->fd, c->buf + c->buf_len, sizeof c->buf - c->buf_len);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            c->fd = -1; return;
        }
        if (r == 0) { c->fd = -1; return; }   /* EOF */
        c->buf_len += (size_t)r;

        size_t start = 0;
        for (size_t i = 0; i < c->buf_len; i++) {
            if (c->buf[i] == '\n') {
                sconn_line(c, c->buf + start, i - start);
                start = i + 1;
            }
        }
        if (start) {
            memmove(c->buf, c->buf + start, c->buf_len - start);
            c->buf_len -= start;
        }
        if (c->buf_len == sizeof c->buf) {     /* line too long: flush + mark */
            sconn_line(c, c->buf, c->buf_len);
            c->buf_len = 0;
        }
    }
}

/* ---- socket setup ------------------------------------------------------- */

static int make_unix(const char *path, int type) {
    int fd = socket(AF_UNIX, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { perror("schema-journal-sink: socket"); return -1; }

    struct sockaddr_un sa = { 0 };
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) {
        fprintf(stderr, "schema-journal-sink: %s: path too long for a socket\n", path);
        close(fd);
        return -1;
    }
    strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "schema-journal-sink: bind %s: %s\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path, 0666);
    if (type == SOCK_STREAM && listen(fd, 16) < 0) {
        perror("schema-journal-sink: listen");
        close(fd);
        return -1;
    }
    return fd;
}

static int open_out(void) {
    const char *path = getenv("JOURNAL_SINK_FILE");
    int fd;
    if (path) {
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    } else {
        fd = open(DEFAULT_OUT, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
        if (fd < 0) fd = open(FALLBACK_OUT,
                              O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    }
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0) out_sz = st.st_size;
    }
    return fd;
}

int main(void) {
    /* JOURNAL_SINK_ROOT prefixes every socket path: tests only */
    const char *root = getenv("JOURNAL_SINK_ROOT");
    char p_run[256], p_dir[256], p_devlog[256], p_native[256], p_stdout[256];
    if (!root) root = "";
    snprintf(p_run, sizeof p_run, "%s/run/systemd", root);
    snprintf(p_dir, sizeof p_dir, "%s%s", root, JOURNAL_DIR);
    snprintf(p_devlog, sizeof p_devlog, "%s%s", root, DEV_LOG);
    snprintf(p_native, sizeof p_native, "%s%s", root, NATIVE_SOCK);
    snprintf(p_stdout, sizeof p_stdout, "%s%s", root, STDOUT_SOCK);

    /* dir first: code that stat()s JOURNAL_DIR for "journald is home" */
    mkdir(p_run, 0755);
    mkdir(p_dir, 0755);

    out_fd = open_out();
    if (out_fd < 0) {
        perror("schema-journal-sink: cannot open output log");
        return 1;
    }
    if (getenv("JOURNAL_SINK_MAXBYTES"))
        out_cap = (off_t)strtoll(getenv("JOURNAL_SINK_MAXBYTES"), NULL, 10);
    if (getenv("JOURNAL_SINK_KMSG"))
        kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

    int dev_fd    = make_unix(p_devlog, SOCK_DGRAM);
    int native_fd = make_unix(p_native, SOCK_DGRAM);
    fdclose_sock = native_fd;   /* fd passing shuts while the closers are full */
    int stdout_fd = make_unix(p_stdout, SOCK_STREAM);
    if (native_fd < 0 || stdout_fd < 0) {
        fprintf(stderr, "schema-journal-sink: required socket missing\n");
        return 1;
    }

    struct sconn conns[MAX_STREAM_CONNS];
    for (int i = 0; i < MAX_STREAM_CONNS; i++) conns[i].fd = -1;

    struct pollfd pfd[3 + MAX_STREAM_CONNS];

    for (;;) {
        int n = 0;
        pfd[n].fd = dev_fd;    pfd[n].events = POLLIN; n++;   /* 0 */
        /* closers full: leave fd-bearing messages queued until one exits */
        pfd[n].fd = fdclose_full() ? -1 : native_fd; pfd[n].events = POLLIN; n++;   /* 1 */
        pfd[n].fd = stdout_fd; pfd[n].events = POLLIN; n++;   /* 2 */
        for (int i = 0; i < MAX_STREAM_CONNS; i++) {
            if (conns[i].fd >= 0) {
                pfd[n].fd = conns[i].fd;
                pfd[n].events = POLLIN;
                n++;
            }
        }

        if (poll(pfd, n, fdclose_n ? 100 : -1) < 0) {
            if (errno == EINTR) continue;
            perror("schema-journal-sink: poll");
            break;
        }

        if (dev_fd >= 0 && (pfd[0].revents & POLLIN)) handle_devlog(dev_fd);
        for (pid_t cp; (cp = waitpid(-1, NULL, WNOHANG)) > 0; )
            fdclose_reaped(cp);
        if (pfd[1].revents & POLLIN) handle_native(native_fd);

        if (pfd[2].revents & POLLIN) {
            for (;;) {
                int c = accept4(stdout_fd, NULL, NULL,
                                SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (c < 0) break;
                int slot = -1;
                for (int i = 0; i < MAX_STREAM_CONNS; i++)
                    if (conns[i].fd < 0) { slot = i; break; }
                if (slot < 0) { close(c); continue; }   /* at cap: refuse */
                conns[slot].fd = c;
                conns[slot].hdr_left = 7;
                conns[slot].ident[0] = '\0';
                conns[slot].buf_len = 0;
            }
        }

        /* stream conns occupy pfd[3..n) in conns-array order */
        int idx = 3;
        for (int i = 0; i < MAX_STREAM_CONNS; i++) {
            if (conns[i].fd < 0) continue;
            if (pfd[idx].revents & (POLLIN | POLLHUP | POLLERR))
                sconn_read(&conns[i]);
            if (conns[i].fd < 0) close(pfd[idx].fd);  /* EOF/err: reap slot */
            idx++;
        }
    }
    return 0;
}
