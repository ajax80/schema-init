#include "../service.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static pid_t sleeper(void) {
    pid_t p = fork();
    assert(p >= 0);
    if (p == 0) { pause(); _exit(0); }
    return p;
}

static void put(const char *dir, const char *file, const char *body) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

int main(void) {
    char tmpl[] = "/tmp/schema-cgkillXXXXXX";
    char *dir = mkdtemp(tmpl);
    char buf[64], path[256];
    int st;
    assert(dir);

    assert(cgroup_kill_path(dir) == 0);

    pid_t a = sleeper();
    snprintf(buf, sizeof buf, "%d\n", (int)a);
    put(dir, "cgroup.procs", buf);
    assert(cgroup_kill_path(dir) == 1);
    assert(waitpid(a, &st, 0) == a && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);

    pid_t b = sleeper();
    snprintf(buf, sizeof buf, "%d\n", (int)b);
    put(dir, "cgroup.procs", buf);
    put(dir, "cgroup.kill", "");
    assert(cgroup_kill_path(dir) == 1);
    snprintf(path, sizeof path, "%s/cgroup.kill", dir);
    FILE *f = fopen(path, "r");
    assert(f && fgets(buf, sizeof buf, f) && strcmp(buf, "1") == 0);
    fclose(f);
    assert(waitpid(b, &st, WNOHANG) == 0);
    kill(b, SIGKILL);
    waitpid(b, &st, 0);

    unlink(path);
    snprintf(path, sizeof path, "%s/cgroup.procs", dir);
    unlink(path);
    rmdir(dir);
    printf("test_cgroup_kill: ok\n");
    return 0;
}
