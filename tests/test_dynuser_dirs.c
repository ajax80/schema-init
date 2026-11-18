#include "../service.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* audit 4.10: dynamic_user's private state dirs. Root only. The engine's
 * state/cache/logs bases are re-rooted under a temp dir (not under /tmp: the
 * service's private_tmp would hide it, so the test turns that off). */
static char root[] = "/run/schema-dyndirs-XXXXXX";
static char path[300], out[320];

static void p(char *b, size_t n, const char *rel) { snprintf(b, n, "%s/%s", root, rel); }

static int spawn(const char *keys, char *got, size_t n) {
    static service_t s;
    int st;
    FILE *f = fopen(path, "w");
    fprintf(f, "name=dyn\nexec=/bin/sh\nargs=-c\nargs=%s\noneshot=1\ndynamic_user=1\nprivate_tmp=0\n", keys);
    fclose(f);
    unlink(out);
    memset(&s, 0, sizeof s);
    assert(service_load_one(path, &s) == 0);
    s.dyn_uid = 64000;
    s.run_uid = s.run_gid = 64000;
    assert(service_spawn(&s) == 0);
    assert(waitpid(s.child_pid, &st, 0) == s.child_pid && WIFEXITED(st));
    got[0] = 0;
    if ((f = fopen(out, "r"))) {
        size_t r = fread(got, 1, n - 1, f);
        got[r] = 0;
        fclose(f);
    }
    return WEXITSTATUS(st);
}

int main(void) {
    char a[400], b[400], keys[1200], got[1024];
    struct stat st;
    if (geteuid() != 0) { printf("test_dynuser_dirs: skipped (needs root)\n"); return 0; }
    assert(mkdtemp(root));
    service_exec_dir_root = root;
    snprintf(path, sizeof path, "%s/t.svc", root);
    snprintf(out, sizeof out, "%s/out", root);
    snprintf(a, sizeof a, "mkdir -p %s/var/lib %s/var/cache %s/var/log && chmod 0777 %s", root, root, root, root);
    assert(system(a) == 0);

    /* a plain dir left from before dynamic_user moves into private/, keeps its
     * contents, and becomes a symlink */
    p(a, sizeof a, "var/lib/foo"); assert(mkdir(a, 0755) == 0);
    p(a, sizeof a, "var/lib/foo/old"); { FILE *f = fopen(a, "w"); fputs("kept\n", f); fclose(f); }
    snprintf(keys, sizeof keys, "cat $STATE_DIRECTORY/old > %s; id -u >> %s\nstate_dir=foo", out, out);
    assert(spawn(keys, got, sizeof got) == 0);
    assert(!strcmp(got, "kept\n64000\n"));
    p(a, sizeof a, "var/lib/foo");
    assert(lstat(a, &st) == 0 && S_ISLNK(st.st_mode));
    b[readlink(a, b, sizeof b - 1)] = 0;
    assert(!strcmp(b, "private/foo"));
    p(a, sizeof a, "var/lib/private");
    assert(stat(a, &st) == 0 && (st.st_mode & 07777) == 0700 && st.st_uid == 0);
    p(a, sizeof a, "var/lib/private/foo/old");
    assert(stat(a, &st) == 0 && st.st_uid == 64000);

    /* a dir owned by an old uid is re-owned, never through a planted symlink */
    p(a, sizeof a, "outside"); { FILE *f = fopen(a, "w"); fclose(f); chown(a, 0, 0); }
    p(a, sizeof a, "var/lib/private/bar"); assert(mkdir(a, 0755) == 0); chown(a, 1234, 1234);
    p(a, sizeof a, "var/lib/private/bar/f"); { FILE *f = fopen(a, "w"); fclose(f); chown(a, 1234, 1234); }
    p(a, sizeof a, "var/lib/private/bar/sub"); assert(mkdir(a, 0755) == 0); chown(a, 1234, 1234);
    p(a, sizeof a, "var/lib/private/bar/evil"); p(b, sizeof b, "outside"); assert(symlink(b, a) == 0);
    snprintf(keys, sizeof keys, "touch $STATE_DIRECTORY/sub/new && echo ok > %s\nstate_dir=bar", out);
    assert(spawn(keys, got, sizeof got) == 0 && !strcmp(got, "ok\n"));
    p(a, sizeof a, "var/lib/private/bar/f"); assert(stat(a, &st) == 0 && st.st_uid == 64000);
    p(a, sizeof a, "outside"); assert(stat(a, &st) == 0 && st.st_uid == 0);

    /* inside the service only its own private dirs exist */
    p(a, sizeof a, "var/lib/private/other"); assert(mkdir(a, 0700) == 0);
    p(a, sizeof a, "var/lib/other"); assert(symlink("private/other", a) == 0);
    snprintf(keys, sizeof keys, "ls %s/var/lib/private > %s; ls %s/var/lib/other/ >/dev/null 2>&1 || echo hidden >> %s\n"
             "state_dir=mine\ncache_dir=mine", root, out, root, out);
    assert(spawn(keys, got, sizeof got) == 0);
    if (strcmp(got, "mine\nhidden\n")) { fprintf(stderr, "ns view: %s\n", got); return 1; }
    p(a, sizeof a, "var/cache/mine"); assert(lstat(a, &st) == 0 && S_ISLNK(st.st_mode));

    /* the stop runner reaches the private dir too (same binds as the service) */
    {
        static service_t r;
        int ws;
        FILE *f = fopen(path, "w");
        fprintf(f, "name=dyn\nexec=/bin/true\ndynamic_user=1\nprivate_tmp=0\nstate_dir=mine\n"
                   "exec_stop=/bin/sh -c \"echo stopped > $STATE_DIRECTORY/s\"\n");
        fclose(f);
        memset(&r, 0, sizeof r);
        assert(service_load_one(path, &r) == 0);
        r.dyn_uid = 64000;
        r.run_uid = r.run_gid = 64000;
        pid_t pid = service_stop_spawn(&r, 1);
        assert(pid > 0 && waitpid(pid, &ws, 0) == pid && WIFEXITED(ws));
        p(a, sizeof a, "var/lib/private/mine/s");
        if (WEXITSTATUS(ws) != 0 || stat(a, &st) != 0) { fprintf(stderr, "stop runner could not write its state dir\n"); return 1; }
    }

    /* a plain dir and a non-empty private dir of the same name: the start
     * fails, it does not run against a half-set-up dir */
    p(a, sizeof a, "var/lib/clash"); assert(mkdir(a, 0755) == 0);
    p(a, sizeof a, "var/lib/clash/x"); { FILE *f = fopen(a, "w"); fclose(f); }
    p(a, sizeof a, "var/lib/private/clash"); assert(mkdir(a, 0755) == 0);
    p(a, sizeof a, "var/lib/private/clash/y"); { FILE *f = fopen(a, "w"); fclose(f); }
    snprintf(keys, sizeof keys, "echo ran > %s\nstate_dir=clash", out);
    assert(spawn(keys, got, sizeof got) == 126 && !got[0]);

    /* a re-own that can't finish leaves the top dir on the old uid, so the
     * next start tries again (a too-deep tree here) */
    p(a, sizeof a, "var/lib/private/deep"); assert(mkdir(a, 0755) == 0); chown(a, 1234, 1234);
    {
        int fd = open(a, O_RDONLY | O_DIRECTORY);
        for (int i = 0; i < 300; i++) {
            assert(mkdirat(fd, "d", 0755) == 0);
            int c = openat(fd, "d", O_RDONLY | O_DIRECTORY);
            fchown(c, 1234, 1234);
            close(fd);
            fd = c;
        }
        close(fd);
    }
    snprintf(keys, sizeof keys, "echo ran > %s\nstate_dir=deep", out);
    assert(spawn(keys, got, sizeof got) == 126);
    p(a, sizeof a, "var/lib/private/deep"); assert(stat(a, &st) == 0 && st.st_uid == 1234);

    snprintf(a, sizeof a, "rm -rf %s", root);
    assert(system(a) == 0);
    printf("all dynuser dirs tests passed\n");
    return 0;
}
