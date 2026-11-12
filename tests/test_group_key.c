#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* group= through the real exec path. Run as uid 0 of a user namespace
 * (tests/test_group_key.sh wraps this): setgid is allowed there, the fleet's
 * real gids are mapped, and nothing touches the host. */

static void write_svc(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
    chmod(path, 0600);
}

static int run(service_t *svc) {
    int st;
    if (service_spawn(svc) != 0) return -1;
    assert(waitpid(svc->child_pid, &st, 0) == svc->child_pid && WIFEXITED(st));
    return WEXITSTATUS(st);
}

static void ids(const char *out, char *buf, size_t n) {
    FILE *f = fopen(out, "r");
    assert(f);
    size_t r = fread(buf, 1, n - 1, f);
    buf[r] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    fclose(f);
}

int main(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "test_group_key: needs uid 0 (a user namespace is fine)\n");
        return 1;
    }
    umask(022);
    char tmpl[] = "/tmp/schema-grpXXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir);
    chmod(dir, 01777);   /* uid 1 writes its own output files here */
    char p[256], out[256], body[512], got[64];
    service_t svc;

    snprintf(out, sizeof out, "%s/a.out", dir);
    /* group= alone: root with that gid (systemd Group= with no User=) */
    snprintf(p, sizeof p, "%s/a.svc", dir);
    snprintf(body, sizeof body,
        "name=a\nexec=/bin/sh\nargs=-c\nargs=echo \"$(id -u) $(id -g)\" > %s\ngroup=5\n", out);
    write_svc(p, body);
    assert(service_load_one(p, &svc) == 0);
    assert(run(&svc) == 0);
    ids(out, got, sizeof got);
    assert(strcmp(got, "0 5") == 0);

    snprintf(out, sizeof out, "%s/b.out", dir);
    /* group= before user=: file order must not decide the gid */
    snprintf(p, sizeof p, "%s/b.svc", dir);
    snprintf(body, sizeof body,
        "name=b\nexec=/bin/sh\nargs=-c\nargs=echo \"$(id -u) $(id -g)\" > %s\ngroup=5\nuser=1\n", out);
    write_svc(p, body);
    assert(service_load_one(p, &svc) == 0);
    assert(run(&svc) == 0);
    ids(out, got, sizeof got);
    assert(strcmp(got, "1 5") == 0);

    snprintf(out, sizeof out, "%s/c.out", dir);
    /* user= alone keeps the primary group (regression) */
    snprintf(p, sizeof p, "%s/c.svc", dir);
    snprintf(body, sizeof body,
        "name=c\nexec=/bin/sh\nargs=-c\nargs=echo \"$(id -u) $(id -g)\" > %s\nuser=1\n", out);
    write_svc(p, body);
    assert(service_load_one(p, &svc) == 0);
    assert(run(&svc) == 0);
    ids(out, got, sizeof got);
    assert(strcmp(got, "1 1") == 0);

    snprintf(out, sizeof out, "%s/d.out", dir);
    /* a group that never resolves fails the start, not silently root:root */
    snprintf(p, sizeof p, "%s/d.svc", dir);
    snprintf(body, sizeof body,
        "name=d\nexec=/bin/sh\nargs=-c\nargs=echo \"$(id -u) $(id -g)\" > %s\ngroup=no-such-group-xyz\n", out);
    write_svc(p, body);
    unlink(out);
    assert(service_load_one(p, &svc) == 0);   /* parse defers, like user= */
    assert(run(&svc) == 126);
    assert(access(out, F_OK) != 0);

    printf("test_group_key: ALL OK\n");
    return 0;
}
