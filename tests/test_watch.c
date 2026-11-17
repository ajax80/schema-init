#include "../watch.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>

static char T[] = "/tmp/schema-watch-XXXXXX";
static int fd;
static int ev_fires;   /* watch_event calls that returned FIRE in the last drain */

static const char *p(const char *rel) {
    static char b[4][300];
    static int i;
    i = (i + 1) % 4;
    snprintf(b[i], sizeof b[i], "%s/%s", T, rel);
    return b[i];
}

static void put(const char *rel, const char *s) {
    int f = open(p(rel), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(f >= 0);
    assert(write(f, s, strlen(s)) == (ssize_t)strlen(s));
    close(f);
}

/* every event so far, through watch_event, re-arming on REARM */
static int drain(watch_t *w) {
    char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n;
    int r = 0;
    ev_fires = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (char *q = buf; q < buf + n;) {
            struct inotify_event *ev = (struct inotify_event *)q;
            int e = watch_event(w, ev);
            if (e & WATCH_EV_FIRE) ev_fires++;
            if (e & WATCH_EV_REARM) e |= watch_rearm(fd, w);
            r |= e;
            q += sizeof *ev + ev->len;
        }
    }
    return r;
}

static void setw(watch_t *w, const char *kind, const char *rel) {
    char v[320];
    snprintf(v, sizeof v, "%s:%s", kind, p(rel));
    assert(watch_parse(v, w) == 0);
    watch_arm(fd, w);
    drain(w);
}

static void test_parse(void) {
    watch_t w;
    char big[320];
    assert(watch_parse("exists:/a", &w) == 0 && w.kind == WATCH_EXISTS && !strcmp(w.path, "/a") &&
           w.wd_parent == -1 && w.wd_leaf == -1);
    assert(watch_parse("exists_glob:/a/*.q", &w) == 0 && w.kind == WATCH_EXISTS_GLOB);
    assert(watch_parse("dir_not_empty:/a", &w) == 0 && w.kind == WATCH_DIR_NOT_EMPTY);
    assert(watch_parse("changed:/a", &w) == 0 && w.kind == WATCH_CHANGED);
    assert(watch_parse("modified:/a", &w) == 0 && w.kind == WATCH_MODIFIED);
    assert(!strcmp(watch_kind_name(WATCH_DIR_NOT_EMPTY), "dir_not_empty"));
    assert(watch_parse("exists:rel", &w) == -1);
    assert(watch_parse("bogus:/a", &w) == -1);
    assert(watch_parse("exists:", &w) == -1);
    assert(watch_parse("/a", &w) == -1);
    assert(watch_parse("exists_glob:/a*/b", &w) == -1);
    assert(watch_parse("exists:/a/", &w) == -1);
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    memcpy(big, "exists:/", 8);
    assert(watch_parse(big, &w) == -1);
}

static void test_changed_once(void) {
    watch_t w;
    put("f", "0");
    setw(&w, "changed", "f");
    put("f", "1");
    assert(drain(&w) & WATCH_EV_FIRE);
    assert(ev_fires == 1);
}

static void test_modified_write(void) {
    watch_t w;
    put("m", "0");
    setw(&w, "modified", "m");
    int f = open(p("m"), O_WRONLY);
    assert(write(f, "x", 1) == 1);
    assert(drain(&w) & WATCH_EV_FIRE);
    close(f);
    drain(&w);
    /* changed ignores a write until the close */
    setw(&w, "changed", "m");
    f = open(p("m"), O_WRONLY);
    assert(write(f, "y", 1) == 1);
    assert(!(drain(&w) & WATCH_EV_FIRE));
    close(f);
    assert(drain(&w) & WATCH_EV_FIRE);
}

static void test_rename_over(void) {
    watch_t w;
    put("r", "0");
    setw(&w, "changed", "r");
    put("r.new", "1");
    assert(rename(p("r.new"), p("r")) == 0);
    assert(drain(&w) & WATCH_EV_FIRE);
    put("r", "2");
    assert(drain(&w) & WATCH_EV_FIRE);
}

static void test_symlink_leaf(void) {
    watch_t w;
    put("tgt", "0");
    assert(symlink(p("tgt"), p("lnk")) == 0);
    setw(&w, "changed", "lnk");
    put("tgt", "1");
    assert(!(drain(&w) & WATCH_EV_FIRE));
    put("tgt2", "0");
    assert(symlink(p("tgt2"), p("lnk.tmp")) == 0);
    assert(rename(p("lnk.tmp"), p("lnk")) == 0);
    assert(drain(&w) & WATCH_EV_FIRE);
}

static void test_deep_parent(void) {
    watch_t w;
    setw(&w, "changed", "a/b/c/file");
    assert(mkdir(p("a"), 0755) == 0);
    assert(!(drain(&w) & WATCH_EV_FIRE));
    assert(mkdir(p("a/b"), 0755) == 0);
    assert(!(drain(&w) & WATCH_EV_FIRE));
    assert(mkdir(p("a/b/c"), 0755) == 0);
    assert(!(drain(&w) & WATCH_EV_FIRE));
    put("a/b/c/file", "x");
    assert(drain(&w) & WATCH_EV_FIRE);
}

static void test_symlink_ancestor(void) {
    watch_t w;
    assert(mkdir(p("real"), 0755) == 0);
    assert(symlink(p("real"), p("link")) == 0);
    setw(&w, "exists", "link/x");
    assert(!watch_level_true(&w));
    put("real/x", "");
    assert(drain(&w) & WATCH_EV_FIRE);
}

static void test_level(void) {
    watch_t w;
    setw(&w, "exists", "lv");
    assert(!watch_level_true(&w) && watch_is_level(&w));
    put("lv", "");
    assert(drain(&w) & WATCH_EV_FIRE);
    assert(watch_level_true(&w));

    assert(mkdir(p("d"), 0755) == 0);
    setw(&w, "exists_glob", "d/*.q");
    assert(!watch_level_true(&w));
    put("d/a.txt", "");
    assert(!(drain(&w) & WATCH_EV_FIRE));
    put("d/a.q", "");
    assert(drain(&w) & WATCH_EV_FIRE);
    assert(watch_level_true(&w));

    assert(mkdir(p("e"), 0755) == 0);
    setw(&w, "dir_not_empty", "e");
    assert(!watch_level_true(&w));
    put("e/x", "");
    assert(drain(&w) & WATCH_EV_FIRE);
    assert(watch_level_true(&w));

    setw(&w, "changed", "lv");
    assert(!watch_is_level(&w));
}

static void test_dir_recreated(void) {
    watch_t w;
    assert(mkdir(p("rc"), 0755) == 0);
    setw(&w, "dir_not_empty", "rc");
    assert(rmdir(p("rc")) == 0);
    assert(!(drain(&w) & WATCH_EV_FIRE));
    assert(mkdir(p("rc"), 0755) == 0);
    assert(!(drain(&w) & WATCH_EV_FIRE));
    put("rc/x", "");
    assert(drain(&w) & WATCH_EV_FIRE);
}

static void test_sibling_noise(void) {
    watch_t w;
    put("quiet", "0");
    setw(&w, "changed", "quiet");
    for (int i = 0; i < 100; i++) {
        put("noisy", "x");
        assert(drain(&w) == WATCH_EV_NONE);
    }
}

/* the mask inotify holds for path's inode on fd (fdinfo), -1 if unwatched;
 * *count gets the number of watches on fd */
static long watched_mask(int f, const char *path, int *count) {
    char fi[64], line[512];
    struct stat st;
    long m = -1;
    if (count) *count = 0;
    snprintf(fi, sizeof fi, "/proc/self/fdinfo/%d", f);
    FILE *fp = fopen(fi, "r");
    assert(fp);
    int have = path && stat(path, &st) == 0;
    while (fgets(line, sizeof line, fp)) {
        unsigned long ino, mask;
        if (sscanf(line, "inotify wd:%*d ino:%lx sdev:%*x mask:%lx", &ino, &mask) != 2) continue;
        if (count) (*count)++;
        if (have && ino == (unsigned long)st.st_ino) m = (long)mask;
    }
    fclose(fp);
    return m;
}

static void test_masks(void) {
    watch_t w;
    assert(mkdir(p("mk"), 0755) == 0);
    put("mk/f", "0");
    setw(&w, "changed", "mk/f");
    long pm = watched_mask(fd, p("mk"), NULL), lm = watched_mask(fd, p("mk/f"), NULL);
    /* the parent sees only structural events: a busy dir doesn't wake us */
    assert(pm >= 0 && !(pm & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB)) && (pm & IN_CREATE));
    assert(lm >= 0 && (lm & IN_CLOSE_WRITE) && !(lm & IN_MODIFY));
    watch_t m;
    setw(&m, "modified", "mk/f");
    assert(watched_mask(fd, p("mk/f"), NULL) & IN_MODIFY);
    /* a narrower later watch never narrows a shared inode's mask */
    setw(&w, "changed", "mk/f");
    assert(watched_mask(fd, p("mk/f"), NULL) & IN_MODIFY);
}

static void test_gc(void) {
    int g = inotify_init1(IN_NONBLOCK | IN_CLOEXEC), n;
    watch_t w, *live[1] = { &w };
    char v[320];
    assert(g >= 0);
    snprintf(v, sizeof v, "changed:%s", p("gc/a/b/f"));
    assert(watch_parse(v, &w) == 0);
    watch_arm(g, &w);
    assert(mkdir(p("gc"), 0755) == 0 || errno == EEXIST);
    assert(mkdir(p("gc/a"), 0755) == 0 && mkdir(p("gc/a/b"), 0755) == 0);
    put("gc/a/b/f", "x");
    watch_rearm(g, &w);
    watched_mask(g, NULL, &n);
    assert(n >= 3);   /* T (or gc), gc/a/b, f: the old ancestor lingers */
    watch_gc(g, live, 1);
    watched_mask(g, NULL, &n);
    assert(n == 2 && watched_mask(g, p("gc/a/b"), NULL) >= 0 && watched_mask(g, p("gc/a/b/f"), NULL) >= 0);
    /* a watch that is gone takes its wds with it */
    watch_gc(g, live, 0);
    watched_mask(g, NULL, &n);
    assert(n == 0);
    close(g);
}

static void test_make_dir(void) {
    watch_t w;
    struct stat st;
    char v[320];
    snprintf(v, sizeof v, "dir_not_empty:%s", p("m1/n"));
    assert(watch_parse(v, &w) == 0 && watch_make_dir(&w, 0770) == 0);
    assert(stat(p("m1/n"), &st) == 0 && (st.st_mode & 07777) == 0770);
    assert(stat(p("m1"), &st) == 0 && (st.st_mode & 07777) == 0755);
    /* systemd's MakeDirectory=: PATH itself for dir_not_empty, changed and
     * modified; nothing for exists and exists_glob */
    snprintf(v, sizeof v, "changed:%s", p("c1/f"));
    assert(watch_parse(v, &w) == 0 && watch_make_dir(&w, 0755) == 0);
    assert(stat(p("c1/f"), &st) == 0 && S_ISDIR(st.st_mode));
    snprintf(v, sizeof v, "modified:%s", p("c2"));
    assert(watch_parse(v, &w) == 0 && watch_make_dir(&w, 0755) == 0);
    assert(stat(p("c2"), &st) == 0 && S_ISDIR(st.st_mode));
    snprintf(v, sizeof v, "exists_glob:%s", p("g/*.q"));
    assert(watch_parse(v, &w) == 0 && watch_make_dir(&w, 0755) == 0);
    assert(stat(p("g"), &st) != 0);
    snprintf(v, sizeof v, "exists:%s", p("e1/x"));
    assert(watch_parse(v, &w) == 0 && watch_make_dir(&w, 0755) == 0);
    assert(stat(p("e1"), &st) != 0);
}

int main(void) {
    umask(022);
    assert(mkdtemp(T));
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    assert(fd >= 0);
    test_parse();
    test_changed_once();
    test_modified_write();
    test_rename_over();
    test_symlink_leaf();
    test_deep_parent();
    test_symlink_ancestor();
    test_level();
    test_dir_recreated();
    test_sibling_noise();
    test_make_dir();
    test_masks();
    test_gc();
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf %s", T);
    assert(system(cmd) == 0);
    printf("all watch tests passed\n");
    return 0;
}
