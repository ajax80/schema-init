/* schema-ctl log [-f] [-n N] <name>: a service's log without knowing where it is. */
#include "ctl_log.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *dir_var(void) {
#ifdef SCHEMA_VERIFY_TESTHOOKS
    if (getenv("SCHEMA_CTL_LOG_VAR")) return getenv("SCHEMA_CTL_LOG_VAR");
#endif
    return "/var/log/schema-init";
}

static const char *dir_run(void) {
#ifdef SCHEMA_VERIFY_TESTHOOKS
    if (getenv("SCHEMA_CTL_LOG_RUN")) return getenv("SCHEMA_CTL_LOG_RUN");
#endif
    return "/run/log/schema-init";
}

/* PID 1 writes /var/log/schema-init, or /run/log/schema-init while /var isn't
 * writable; a box can carry both, and the one written last is the live one.
 * Opened, then judged by fstat on the fd held: nothing checked by name is used. */
int log_open(const char *name, int user, char *out, size_t n) {
    char p[2][PATH_MAX];
    int np = 0, best = -1, denied = 0;
    struct stat bs, st;
    if (!*name || strchr(name, '/') || name[0] == '.') {
        errno = ENOENT;
        return -1;
    }
    if (user) {
        const char *x = getenv("XDG_STATE_HOME"), *h = getenv("HOME");
        if (x && *x) snprintf(p[np++], PATH_MAX, "%s/schema-user/log/%s.log", x, name);
        else snprintf(p[np++], PATH_MAX, "%s/.local/state/schema-user/log/%s.log", h ? h : "", name);
    } else {
        snprintf(p[np++], PATH_MAX, "%s/%s.log", dir_var(), name);
        snprintf(p[np++], PATH_MAX, "%s/%s.log", dir_run(), name);
    }
    for (int i = 0; i < np; i++) {
        int fd = open(p[i], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            denied |= errno == EACCES || errno == EPERM;
            continue;
        }
        if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
            close(fd);
            continue;
        }
        if (best < 0 || st.st_mtim.tv_sec > bs.st_mtim.tv_sec ||
            (st.st_mtim.tv_sec == bs.st_mtim.tv_sec && st.st_mtim.tv_nsec > bs.st_mtim.tv_nsec)) {
            if (best >= 0) close(best);
            best = fd, bs = st;
            snprintf(out, n, "%s", p[i]);
        } else {
            close(fd);
        }
    }
    if (best < 0) errno = denied ? EACCES : ENOENT;
    return best;
}

static void copy_from(int fd, off_t off, FILE *out) {
    char buf[65536];
    ssize_t r;
    if (lseek(fd, off, SEEK_SET) < 0) return;
    while ((r = read(fd, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)r, out);
    fflush(out);
}

/* backwards from the end in 4 KB chunks: a 500 MB log costs no more than a small one */
int log_tail(int fd, long lines, FILE *out) {
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (lines <= 0 || st.st_size == 0) {
        copy_from(fd, 0, out);
        return 0;
    }
    char buf[4096];
    off_t end = st.st_size, pos = end, start = 0;
    long seen = 0;
    int first = 1;
    while (pos > 0 && !start) {
        size_t len = pos >= (off_t)sizeof buf ? sizeof buf : (size_t)pos;
        pos -= (off_t)len;
        if (pread(fd, buf, len, pos) != (ssize_t)len) return -1;
        for (size_t i = len; i-- > 0;) {
            /* a final newline ends the last line rather than starting an empty one */
            if (buf[i] != '\n' || (first && pos + (off_t)i == end - 1)) continue;
            if (++seen == lines) {
                start = pos + (off_t)i + 1;
                break;
            }
        }
        first = 0;
    }
    copy_from(fd, start, out);
    return 0;
}

int log_follow(const char *path, FILE *out) {
    char dir[PATH_MAX];
    const char *base = strrchr(path, '/');
    snprintf(dir, sizeof dir, "%.*s", base ? (int)(base - path) : 1, base ? path : ".");
    base = base ? base + 1 : path;
    int in = inotify_init1(IN_CLOEXEC);
    if (in < 0 || inotify_add_watch(in, dir, IN_MODIFY | IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE) < 0)
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    off_t off = fd >= 0 ? lseek(fd, 0, SEEK_END) : 0;
    char ev[sizeof(struct inotify_event) + NAME_MAX + 1];
    for (;;) {
        struct pollfd p = { in, POLLIN, 0 };
        /* a periodic look too: an event for the name can be missed across a rename */
        int r = poll(&p, 1, 1000);
        if (r > 0 && read(in, ev, sizeof ev) < 0 && errno != EINTR) return -1;
        struct stat now, cur;
        if (fd >= 0) {
            /* drain what the open file still has, renamed away or not */
            if (fstat(fd, &cur) == 0 && cur.st_size < off) off = 0;   /* truncated */
            copy_from(fd, off, out);
            off = lseek(fd, 0, SEEK_CUR);
        }
        int nfd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (nfd >= 0 && fstat(nfd, &now) == 0 &&
            (fd < 0 || fstat(fd, &cur) != 0 || now.st_ino != cur.st_ino || now.st_dev != cur.st_dev)) {
            /* a new file under the name (logrotate): switch to it, from the top */
            if (fd >= 0) close(fd);
            fd = nfd;
            copy_from(fd, 0, out);
            off = lseek(fd, 0, SEEK_CUR);
        } else if (nfd >= 0) {
            close(nfd);
        }
    }
}

int ctl_log_main(int argc, char **argv, int user) {
    long lines = 50;
    int follow = 0, i;
    const char *name = NULL;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) follow = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            char *e;
            lines = strtol(argv[++i], &e, 10);
            if (*e || lines < 0) goto usage;
        } else if (argv[i][0] == '-' || name) goto usage;
        else name = argv[i];
    }
    if (!name) goto usage;
    char path[PATH_MAX];
    int fd = log_open(name, user, path, sizeof path);
    if (fd < 0) {
        if (errno == EACCES) fprintf(stderr, "needs root: sudo schema-ctl log %s\n", name);
        else fprintf(stderr, "schema-ctl: no log for %s\n", name);
        return 1;
    }
    log_tail(fd, lines, stdout);
    close(fd);
    if (follow) log_follow(path, stdout);
    return 0;
usage:
    fprintf(stderr, "usage: schema-ctl log [-f] [-n N] <name>\n");
    return 64;
}
