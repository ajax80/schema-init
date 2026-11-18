#include "../service.h"
#include "../seccomp.h"
#include "../seccomp_names.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[] = "/tmp/schema-svcsc-XXXXXX";
static char path[300], dd[320], dp[360];

static void put(const char *p, const char *body) {
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static int load(const char *keys, const char *dropin, service_t *s) {
    char body[2048];
    snprintf(body, sizeof body, "name=t\nexec=/bin/true\n%s", keys);
    put(path, body);
    if (dropin) {
        mkdir(dd, 0755);
        put(dp, dropin);
    }
    memset(s, 0, sizeof *s);
    int r = service_load_one(path, s);
    if (dropin) {
        unlink(dp);
        rmdir(dd);
    }
    return r;
}

static int has(const service_t *s, const char *name) {
    for (int i = 0; i < SC_NAMES_ALL; i++)
        if (!strcmp(sc_known[i], name)) return (s->sc.set[i / 64] >> (i % 64)) & 1;
    return -1;
}

int main(void) {
    static service_t s;
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/t.svc", dir);
    snprintf(dd, sizeof dd, "%s.d", path);
    snprintf(dp, sizeof dp, "%s/10-x.conf", dd);

    assert(load("", NULL, &s) == 0 && !sc_active(&s.sc));
    /* the first line sets the mode; an allowlist loses what a ~ line names */
    assert(load("syscall_filter=@system-service\nsyscall_filter=~@privileged @resources\n", NULL, &s) == 0);
    assert(s.sc.mode == SC_ALLOW && has(&s, "openat") == 1 && has(&s, "setuid") == 0 && has(&s, "setrlimit") == 0);
    /* a denylist: a later plain line takes names back out of it */
    assert(load("syscall_filter=~@clock @mount\nsyscall_filter=mount\n", NULL, &s) == 0);
    assert(s.sc.mode == SC_DENY && has(&s, "settimeofday") == 1 && has(&s, "umount2") == 1 && has(&s, "mount") == 0);
    /* per-entry errno */
    assert(load("syscall_filter=~getuid:EACCES\n", NULL, &s) == 0 && s.sc.n_err == 1 && s.sc.err_val[0] == EACCES);
    /* an empty value clears, in a drop-in too */
    assert(load("syscall_filter=@system-service\n", "syscall_filter=\n", &s) == 0 && s.sc.mode == SC_NONE);
    assert(load("syscall_filter=@system-service\n", "syscall_filter=\nsyscall_filter=~@mount\n", &s) == 0 &&
           s.sc.mode == SC_DENY);
    /* an unknown name is skipped (newer systemd units still load); an
     * unknown group or errno refuses */
    assert(load("syscall_filter=nonesuch getpid\n", NULL, &s) == 0 && has(&s, "getpid") == 1);
    assert(load("syscall_filter=@nonesuch\n", NULL, &s) == -1);
    assert(load("syscall_filter=getuid:EBOGUS\n", NULL, &s) == -1);

    assert(load("syscall_error_number=EPERM\n", NULL, &s) == 0 && s.sc.err == EPERM);
    assert(load("syscall_error_number=13\n", NULL, &s) == 0 && s.sc.err == 13);
    assert(load("syscall_error_number=kill\n", NULL, &s) == 0 && s.sc.err == SC_KILL);
    assert(load("syscall_error_number=EBOGUS\n", NULL, &s) == -1);

    assert(load("syscall_architectures=native\n", NULL, &s) == 0 && s.sc.native && sc_active(&s.sc));
    assert(load("syscall_architectures=x86\n", NULL, &s) == -1);
    assert(load("restrict_suid_sgid=1\nlock_personality=1\nrestrict_realtime=1\nrestrict_namespaces=1\n", NULL, &s) == 0);
    assert(s.sc.suid && s.sc.persona && s.sc.rt && s.sc.ns);
    assert(load("restrict_namespaces=0\n", NULL, &s) == 0 && !s.sc.ns && !sc_active(&s.sc));
    assert(load("restrict_namespaces=cgroup ipc\n", NULL, &s) == -1);
    assert(load("lock_personality=2\n", NULL, &s) == -1);

    unlink(path);
    rmdir(dir);
    printf("all svc seccomp tests passed\n");
    return 0;
}
