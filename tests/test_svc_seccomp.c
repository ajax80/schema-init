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

static int err_of(const service_t *s, const char *name) {
    for (int i = 0; i < SC_NAMES_ALL; i++)
        if (!strcmp(sc_known[i], name))
            for (int k = 0; k < s->sc.n_err; k++)
                if ((s->sc.err_set[k][i / 64] >> (i % 64)) & 1) return s->sc.err_val[k];
    return 0;
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

    /* syscall_deny=: opt-in only. Unset is nothing even with the hardening
     * default on: a host .svc written before the knob existed (mount-home)
     * must not lose mount(2) on upgrade — blakbox 11-18 */
    assert(load("", NULL, &s) == 0 && s.sc.mode == SC_NONE);
    service_set_hardening_default(1);
    assert(load("", NULL, &s) == 0 && s.sc.mode == SC_NONE && !(s.hard_default & HARD_SD));
    assert(load("syscall_deny=default\n", NULL, &s) == 0 && s.sc.mode == SC_DENY && has(&s, "mount") == 1 &&
           has(&s, "init_module") == 1 && has(&s, "settimeofday") == 1 && has(&s, "bpf") == 1 && has(&s, "openat") == 0);
    assert(load("syscall_deny=0\n", NULL, &s) == 0 && s.sc.mode == SC_NONE && (s.hard_set & HARD_SD));
    assert(load("syscall_deny=@clock\n", NULL, &s) == 0 && s.sc.mode == SC_DENY && has(&s, "settimeofday") == 1 &&
           has(&s, "mount") == 0);
    service_set_hardening_default(0);
    assert(load("syscall_deny=default\n", NULL, &s) == 0 && s.sc.mode == SC_DENY && has(&s, "mount") == 1);
    /* merged into the one filter: a denylist adds, an allowlist loses them */
    assert(load("syscall_filter=~getuid\nsyscall_deny=@clock\n", NULL, &s) == 0 && s.sc.mode == SC_DENY &&
           has(&s, "getuid") == 1 && has(&s, "settimeofday") == 1);
    assert(load("syscall_filter=@system-service @clock\nsyscall_deny=@clock\n", NULL, &s) == 0 && s.sc.mode == SC_ALLOW &&
           has(&s, "openat") == 1 && has(&s, "settimeofday") == 0);
    assert(load("syscall_deny=@nonesuch\n", NULL, &s) == -1);
    assert(load("syscall_deny=2\n", NULL, &s) == -1);
    /* review of #546: the unit's own filter wins where they overlap */
    assert(load("syscall_filter=~mount:EROFS\nsyscall_deny=@mount\n", NULL, &s) == 0 &&
           err_of(&s, "mount") == EROFS && has(&s, "umount2") == 1 && err_of(&s, "umount2") == 0);
    service_set_hardening_default(1);
    assert(load("syscall_filter=~@privileged\nsyscall_filter=mount\n", NULL, &s) == 0 && has(&s, "mount") == 0 &&
           !(s.hard_default & HARD_SD));
    assert(load("user=nobody\nno_new_privs=0\n", NULL, &s) == 0 && s.sc.mode == SC_NONE && !(s.hard_default & HARD_SD));
    assert(load("no_new_privs=0\n", NULL, &s) == 0 && (s.hard_default & HARD_SD) == (geteuid() == 0 ? HARD_SD : 0));
    assert(load("syscall_deny=no\n", NULL, &s) == 0 && s.sc.mode == SC_NONE);
    service_set_hardening_default(0);
    assert(load("syscall_deny=yes\n", NULL, &s) == 0 && has(&s, "mount") == 1);
    assert(load("syscall_deny=~mount\n", NULL, &s) == -1);
    assert(load("syscall_deny=default bpf\n", NULL, &s) == -1);
    {
        char big[600] = "syscall_deny=";
        while (strlen(big) < 450) strcat(big, "mount ");
        strcat(big, "\n");
        assert(load(big, NULL, &s) == -1);
    }

    unlink(path);
    rmdir(dir);
    printf("all svc seccomp tests passed\n");
    return 0;
}
