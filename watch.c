#include "watch.h"
#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *const kinds[] = { "exists", "exists_glob", "dir_not_empty", "changed", "modified" };

#define STRUCTURAL (IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM)
#define GONE       (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)

const char *watch_kind_name(uint8_t kind) {
    return kind < sizeof kinds / sizeof kinds[0] ? kinds[kind] : "?";
}

int watch_is_level(const watch_t *w) {
    return w->kind == WATCH_EXISTS || w->kind == WATCH_EXISTS_GLOB || w->kind == WATCH_DIR_NOT_EMPTY;
}

int watch_parse(const char *val, watch_t *w) {
    const char *c = strchr(val, ':');
    size_t n, k;
    memset(w, 0, sizeof *w);
    w->wd_parent = w->wd_leaf = -1;
    if (!c) return -1;
    for (k = 0; k < sizeof kinds / sizeof kinds[0]; k++)
        if (strlen(kinds[k]) == (size_t)(c - val) && !strncmp(val, kinds[k], (size_t)(c - val))) break;
    if (k == sizeof kinds / sizeof kinds[0]) return -1;
    c++;
    n = strlen(c);
    if (c[0] != '/' || n >= sizeof w->path || (n > 1 && c[n - 1] == '/')) return -1;
    if (k == WATCH_EXISTS_GLOB) {
        const char *last = strrchr(c, '/');
        for (const char *q = c; q < last; q++)
            if (strchr("*?[", *q)) return -1;
    }
    w->kind = (uint8_t)k;
    memcpy(w->path, c, n + 1);
    return 0;
}

/* the directory whose contents decide the watch: the glob's dir, else PATH */
static void target(const watch_t *w, char *out, size_t n) {
    snprintf(out, n, "%s", w->path);
    if (w->kind == WATCH_EXISTS_GLOB) {
        char *s = strrchr(out, '/');
        if (s == out) s[1] = 0;
        else *s = 0;
    }
}

static int ncomp(const char *path) {
    int n = 0;
    for (const char *q = path; *q;) {
        while (*q == '/') q++;
        if (!*q) break;
        n++;
        while (*q && *q != '/') q++;
    }
    return n;
}

/* component idx (0-based) of path */
static int comp(const char *path, int idx, const char **s, size_t *len) {
    const char *q = path;
    for (int i = 0; *q; i++) {
        while (*q == '/') q++;
        if (!*q) break;
        const char *e = q;
        while (*e && *e != '/') e++;
        if (i == idx) { *s = q; *len = (size_t)(e - q); return 0; }
        q = e;
    }
    return -1;
}

int watch_level_true(const watch_t *w) {
    if (w->kind == WATCH_EXISTS) return access(w->path, F_OK) == 0;
    if (w->kind == WATCH_EXISTS_GLOB) {
        glob_t g;
        int r = glob(w->path, GLOB_NOSORT, NULL, &g) == 0 && g.gl_pathc > 0;
        if (r || g.gl_pathv) globfree(&g);
        return r;
    }
    if (w->kind == WATCH_DIR_NOT_EMPTY) {
        DIR *d = opendir(w->path);
        struct dirent *e;
        int r = 0;
        if (!d) return 0;
        while (!r && (e = readdir(d)))
            r = strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
        closedir(d);
        return r;
    }
    return 0;
}

/* Ancestors follow symlinks (/var/run is watched as the real /run); the leaf
 * of changed/modified is the link itself. Old wds are left to the kernel:
 * another watch may share the inode. */
int watch_rearm(int ino_fd, watch_t *w) {
    char tgt[sizeof w->path], cur[sizeof w->path] = "/";
    int glob_k = w->kind == WATCH_EXISTS_GLOB, edge = !watch_is_level(w);
    int total, dirs, i, seen;
    struct stat st;
    target(w, tgt, sizeof tgt);
    total = ncomp(tgt);
    dirs = glob_k ? total : total - 1;   /* components that must be directories */
    for (i = 0; i < dirs; i++) {
        const char *s = NULL;
        size_t len = 0;
        char next[sizeof cur];
        if (comp(tgt, i, &s, &len) != 0) break;
        snprintf(next, sizeof next, "%s%s%.*s", cur, strcmp(cur, "/") ? "/" : "", (int)len, s);
        if (stat(next, &st) != 0 || !S_ISDIR(st.st_mode)) break;
        memcpy(cur, next, sizeof cur);
    }
    w->depth = total - i;
    w->wd_parent = inotify_add_watch(ino_fd, cur, WATCH_INO_MASK | IN_ONLYDIR);
    w->wd_leaf = -1;
    seen = !glob_k && lstat(w->path, &st) == 0;
    if (seen && w->depth == 1) {
        if (edge)
            w->wd_leaf = inotify_add_watch(ino_fd, w->path, WATCH_INO_MASK | IN_DONT_FOLLOW);
        else if (w->kind == WATCH_DIR_NOT_EMPTY)
            w->wd_leaf = inotify_add_watch(ino_fd, w->path, WATCH_INO_MASK | IN_ONLYDIR);
    }
    int fire = edge ? (seen && !w->leaf_seen) : watch_level_true(w);
    w->leaf_seen = (uint8_t)seen;
    return fire ? WATCH_EV_FIRE : WATCH_EV_NONE;
}

void watch_arm(int ino_fd, watch_t *w) {
    w->leaf_seen = 1;   /* the first arm never fires an edge kind */
    (void)watch_rearm(ino_fd, w);
}

static int level_fire(const watch_t *w) {
    return watch_level_true(w) ? WATCH_EV_FIRE : WATCH_EV_NONE;
}

int watch_event(const watch_t *w, const struct inotify_event *ev) {
    int edge = !watch_is_level(w);
    if (ev->mask & IN_Q_OVERFLOW)
        return WATCH_EV_REARM | (edge ? WATCH_EV_FIRE : level_fire(w));
    if (ev->wd == w->wd_leaf && w->wd_leaf >= 0) {
        if (ev->mask & GONE) return WATCH_EV_REARM | (edge ? WATCH_EV_FIRE : 0);
        if (w->kind == WATCH_DIR_NOT_EMPTY)
            return (ev->mask & (IN_CREATE | IN_MOVED_TO)) ? level_fire(w) : WATCH_EV_NONE;
        uint32_t m = IN_CLOSE_WRITE | IN_ATTRIB | (ev->len ? STRUCTURAL : 0) |
                     (w->kind == WATCH_MODIFIED ? IN_MODIFY : 0);
        return (ev->mask & m) ? WATCH_EV_FIRE : WATCH_EV_NONE;
    }
    if (ev->wd != w->wd_parent || w->wd_parent < 0) return WATCH_EV_NONE;
    if (ev->mask & GONE) return WATCH_EV_REARM;
    if (!(ev->mask & STRUCTURAL)) return WATCH_EV_NONE;
    if (w->depth == 0)   /* exists_glob with its dir present: any entry may match */
        return level_fire(w);
    char tgt[sizeof w->path];
    const char *s;
    size_t len;
    target(w, tgt, sizeof tgt);
    if (comp(tgt, ncomp(tgt) - w->depth, &s, &len) != 0 || !ev->len ||
        strlen(ev->name) != len || strncmp(ev->name, s, len))
        return WATCH_EV_NONE;
    if (w->depth > 1) return WATCH_EV_REARM;
    return WATCH_EV_REARM | (edge ? WATCH_EV_FIRE : level_fire(w));
}

int watch_make_dir(const watch_t *w, mode_t mode) {
    char tgt[sizeof w->path];
    if (w->kind != WATCH_DIR_NOT_EMPTY && w->kind != WATCH_EXISTS_GLOB) return 0;
    target(w, tgt, sizeof tgt);
    for (char *q = tgt + 1; *q; q++) {
        if (*q != '/') continue;
        *q = 0;
        if (mkdir(tgt, 0755) != 0 && errno != EEXIST) return -1;
        *q = '/';
    }
    if (mkdir(tgt, mode) != 0) return errno == EEXIST ? 0 : -1;
    return chmod(tgt, mode);   /* mkdir's mode went through the umask */
}
