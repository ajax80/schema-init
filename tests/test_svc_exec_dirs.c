#include "../service.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sched.h>

static char path[300];

static int load(const char *body, service_t *svc) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
    memset(svc, 0, sizeof *svc);
    return service_load_one(path, svc);
}

static int is_dir(const char *p, mode_t mode) {
    struct stat st;
    return lstat(p, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 07777) == mode;
}

static void write_file(const char *p, const char *v) {
    int fd = open(p, O_WRONLY);
    if (fd >= 0) { if (write(fd, v, strlen(v)) < 0) {} close(fd); }
}

/* a mount the service left below its runtime dir is never descended into;
 * 0 = held, 1 = no unprivileged namespaces here (skipped) */
static int mount_guard(const char *base) {
    pid_t pid = fork();
    if (pid == 0) {
        char m[600], f[650], map[64];
        uid_t u = getuid();
        gid_t g = getgid();
        if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) _exit(1);
        snprintf(map, sizeof map, "0 %u 1", (unsigned)u);
        write_file("/proc/self/setgroups", "deny");
        write_file("/proc/self/uid_map", map);
        snprintf(map, sizeof map, "0 %u 1", (unsigned)g);
        write_file("/proc/self/gid_map", map);
        snprintf(m, sizeof m, "%s/mt/mnt", base);
        if (mkdir(m, 0755) != 0 && errno != EEXIST) _exit(2);
        if (mount("none", m, "tmpfs", 0, NULL) != 0) _exit(1);
        snprintf(f, sizeof f, "%s/precious", m);
        fclose(fopen(f, "w"));
        service_remove_tree(base, "mt");
        _exit(access(f, F_OK) == 0 ? 0 : 3);
    }
    int st;
    waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && (WEXITSTATUS(st) == 0 || WEXITSTATUS(st) == 1));
    return WEXITSTATUS(st);
}

int main(void) {
    char dir[] = "/tmp/schema-execdirs-XXXXXX";
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/d.svc", dir);
    service_t s;

    assert(load("name=d\nexec=/bin/true\n", &s) == 0);
    for (int k = 0; k < EXEC_DIR_KINDS; k++) assert(s.exec_dir_count[k] == 0 && !(s.exec_dir_mode_set & (1 << k)));
    assert(load("name=d\nexec=/bin/true\nruntime_dir=sshd  foo/bar\nruntime_dir=baz/\n"
                "state_dir=d\ncache_dir=c\nlogs_dir=l\nstate_dir_mode=0700\nlogs_dir_mode=750\n", &s) == 0);
    assert(s.exec_dir_count[EXEC_DIR_RUNTIME] == 3);
    assert(!strcmp(s.exec_dir[EXEC_DIR_RUNTIME][0], "sshd") && !strcmp(s.exec_dir[EXEC_DIR_RUNTIME][1], "foo/bar") &&
           !strcmp(s.exec_dir[EXEC_DIR_RUNTIME][2], "baz"));
    assert(s.exec_dir_count[EXEC_DIR_STATE] == 1 && s.exec_dir_count[EXEC_DIR_CACHE] == 1 &&
           s.exec_dir_count[EXEC_DIR_LOGS] == 1);
    assert(s.exec_dir_mode[EXEC_DIR_STATE] == 0700 && s.exec_dir_mode[EXEC_DIR_LOGS] == 0750 &&
           s.exec_dir_mode_set == ((1 << EXEC_DIR_STATE) | (1 << EXEC_DIR_LOGS)));
    /* a repeated entry is kept once */
    assert(load("name=d\nexec=/bin/true\nruntime_dir=a a\nruntime_dir=a\n", &s) == 0 &&
           s.exec_dir_count[EXEC_DIR_RUNTIME] == 1);
    assert(load("name=d\nexec=/bin/true\nruntime_dir=/abs\n", &s) == -1);
    assert(load("name=d\nexec=/bin/true\nruntime_dir=a/../b\n", &s) == -1);
    assert(load("name=d\nexec=/bin/true\nruntime_dir=..\n", &s) == -1);
    assert(load("name=d\nexec=/bin/true\nruntime_dir=a/./b\n", &s) == -1);
    assert(load("name=d\nexec=/bin/true\nruntime_dir=a//b\n", &s) == -1);
    assert(load("name=d\nexec=/bin/true\nstate_dir=a:b\n", &s) == -1);
    assert(load("name=d\nexec=/bin/true\ncache_dir_mode=0899\n", &s) == -1);
    assert(load("name=d\nexec=/bin/true\ncache_dir_mode=17777\n", &s) == -1);
    assert(load("name=d\nexec=/bin/true\ncache_dir_mode=\n", &s) == 0 && !(s.exec_dir_mode_set & (1 << EXEC_DIR_CACHE)));
    assert(load("name=d\nexec=/bin/true\ncache_dir_mode=0000\n", &s) == 0 && s.exec_dir_mode[EXEC_DIR_CACHE] == 0 &&
           (s.exec_dir_mode_set & (1 << EXEC_DIR_CACHE)));
    assert(load("name=d\nexec=/bin/true\nruntime_dir=1 2 3 4 5 6 7 8\n", &s) == 0 &&
           s.exec_dir_count[EXEC_DIR_RUNTIME] == 8);
    assert(load("name=d\nexec=/bin/true\nruntime_dir=1 2 3 4 5 6 7 8 9\n", &s) == -1);
    {
        char dd[400], dp[450];
        snprintf(dd, sizeof dd, "%s.d", path);
        assert(mkdir(dd, 0755) == 0);
        snprintf(dp, sizeof dp, "%s/10.conf", dd);
        FILE *f = fopen(dp, "w");
        fputs("runtime_dir=\nstate_dir_mode=\nstate_dir=new\n", f);
        fclose(f);
        assert(load("name=d\nexec=/bin/true\nruntime_dir=a b\nstate_dir=old\nstate_dir_mode=0700\n", &s) == 0);
        assert(s.exec_dir_count[EXEC_DIR_RUNTIME] == 0 && !(s.exec_dir_mode_set & (1 << EXEC_DIR_STATE)));
        assert(s.exec_dir_count[EXEC_DIR_STATE] == 2 && !strcmp(s.exec_dir[EXEC_DIR_STATE][1], "new"));
        unlink(dp);
        rmdir(dd);
    }

    /* creation: walks from the base, intermediates 0755, leaf gets the mode */
    char base[400], p[600];
    snprintf(base, sizeof base, "%s/base", dir);
    assert(mkdir(base, 0755) == 0);
    const char *step = "";
    umask(022);
    assert(service_make_dir(base, "a/b/c", 0700, (uid_t)-1, (gid_t)-1, &step) == 0);
    snprintf(p, sizeof p, "%s/a", base);
    assert(is_dir(p, 0755));
    snprintf(p, sizeof p, "%s/a/b/c", base);
    assert(is_dir(p, 0700));
    /* an existing leaf takes the mode */
    assert(service_make_dir(base, "a/b/c", 0750, (uid_t)-1, (gid_t)-1, &step) == 0 && is_dir(p, 0750));
    /* a symlink in a directory someone else can write is never followed
     * (0777 stands in for a parent the service user owns) */
    char out[400], pub[450];
    snprintf(out, sizeof out, "%s/outside", dir);
    assert(mkdir(out, 0755) == 0);
    snprintf(pub, sizeof pub, "%s/pub", base);
    assert(mkdir(pub, 0755) == 0 && chmod(pub, 0777) == 0);
    snprintf(p, sizeof p, "%s/link", pub);
    assert(symlink(out, p) == 0);
    errno = 0;
    assert(service_make_dir(base, "pub/link/x", 0755, (uid_t)-1, (gid_t)-1, &step) == -1 && (errno == ELOOP || errno == ENOTDIR));
    snprintf(p, sizeof p, "%s/outside/x", dir);
    assert(access(p, F_OK) != 0);
    /* ...nor is a symlink leaf chmodded through */
    assert(service_make_dir(base, "pub/link", 0700, (uid_t)-1, (gid_t)-1, &step) == -1);
    assert(is_dir(out, 0755));
    /* an admin's link in a directory only we can write is followed
     * (/var/lib/mysql -> /data/mysql) */
    char adm[450], tgt[450];
    snprintf(tgt, sizeof tgt, "%s/data", dir);
    assert(mkdir(tgt, 0755) == 0);
    snprintf(adm, sizeof adm, "%s/adm", base);
    assert(symlink(tgt, adm) == 0);
    assert(service_make_dir(base, "adm", 0750, (uid_t)-1, (gid_t)-1, &step) == 0 && is_dir(tgt, 0750));
    assert(service_make_dir(base, "adm/sub", 0700, (uid_t)-1, (gid_t)-1, &step) == 0);
    snprintf(p, sizeof p, "%s/sub", tgt);
    assert(is_dir(p, 0700));
    snprintf(p, sizeof p, "%s/link", base);
    assert(symlink(out, p) == 0);
    /* a file in the way fails */
    snprintf(p, sizeof p, "%s/file", base);
    fclose(fopen(p, "w"));
    assert(service_make_dir(base, "file", 0755, (uid_t)-1, (gid_t)-1, &step) == -1 && errno == ENOTDIR);

    /* removal: the whole tree, never through a symlink */
    snprintf(p, sizeof p, "%s/a/b/c/f", base);
    fclose(fopen(p, "w"));
    snprintf(p, sizeof p, "%s/a/b/c/l", base);
    assert(symlink(out, p) == 0);
    snprintf(p, sizeof p, "%s/outside/keep", dir);
    fclose(fopen(p, "w"));
    assert(service_remove_tree(base, "a/b") == 0);
    snprintf(p, sizeof p, "%s/a/b", base);
    assert(access(p, F_OK) != 0);
    snprintf(p, sizeof p, "%s/a", base);
    assert(is_dir(p, 0755));
    snprintf(p, sizeof p, "%s/outside/keep", dir);
    assert(access(p, F_OK) == 0);
    /* removing through a symlinked parent touches nothing */
    assert(service_remove_tree(base, "link/keep") == -1);
    assert(access(p, F_OK) == 0);
    assert(service_remove_tree(base, "gone") == 0);
    snprintf(p, sizeof p, "%s/mt", base);
    assert(mkdir(p, 0755) == 0);
    if (mount_guard(base)) puts("test_svc_exec_dirs: mount guard skipped (no user namespaces)");

    /* the environment: full paths, sorted, ':'-joined */
    assert(load("name=d\nexec=/bin/true\nruntime_dir=zz aa\n", &s) == 0);
    char env[1024];
    assert(service_exec_dir_env(&s, EXEC_DIR_RUNTIME, "/run", env, sizeof env) == 0);
    assert(!strcmp(env, "/run/aa:/run/zz"));
    {
        /* 8 entries of 200 under a long base: the size the caller must allot */
        char body[3000] = "name=d\nexec=/bin/true\n", lb[1024], *big;
        for (int i = 0; i < 8; i++) {
            char e[220] = "logs_dir=";
            memset(e + 9, 'a' + i, 200);
            strcpy(e + 209, "\n");
            strcat(body, e);
        }
        assert(load(body, &s) == 0 && s.exec_dir_count[EXEC_DIR_LOGS] == 8);
        memset(lb, 'b', 1000); lb[0] = '/'; lb[1000] = '\0';
        size_t need = service_exec_dir_env_size(&s, EXEC_DIR_LOGS, lb);
        assert(need > 8 * 1200 && (big = malloc(need)));
        assert(service_exec_dir_env(&s, EXEC_DIR_LOGS, lb, big, need) == 0 && strlen(big) < need && strlen(big) > 8 * 1200);
        free(big);
    }

    char cmd[500];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    assert(system(cmd) == 0);
    printf("test_svc_exec_dirs: ok\n");
    return 0;
}
