#include "../seccomp.h"
#include "../seccomp_names.h"
#include "../seccomp_groups.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/filter.h>

/* audit 4.6: the BPF builder, exercised for real in forked children
 * (no_new_privs + filter needs no root). probe() returns the errno the call
 * gave (0 = it succeeded) or -signal if the child was killed. */
static char dir[] = "/tmp/schema-sc-XXXXXX";
static char path[300];

static int probe(const sc_spec_t *s, long (*fn)(void)) {
    pid_t p = fork();
    if (p == 0) {
        const char *step;
        if (s && sc_install(s, &step) != 0) _exit(250);
        errno = 0;
        long r = fn();
        _exit(r < 0 ? (errno & 0x7f) : 0);
    }
    int st;
    assert(waitpid(p, &st, 0) == p);
    if (WIFSIGNALED(st)) return -WTERMSIG(st);
    assert(WEXITSTATUS(st) != 250);
    return WEXITSTATUS(st);
}

static sc_spec_t spec(int mode, const char *entries) {
    sc_spec_t s;
    char buf[2048], *save, *t;
    memset(&s, 0, sizeof s);
    s.mode = (uint8_t)mode;
    snprintf(buf, sizeof buf, "%s", entries);
    for (t = strtok_r(buf, " ", &save); t; t = strtok_r(NULL, " ", &save)) assert(sc_add(&s, t, 0) == 0);
    return s;
}

static long c_getpid(void) { return syscall(SYS_getpid); }
static long c_getuid(void) { long r = syscall(SYS_getuid); return r < 0 ? r : 0; }
static long c_uname(void) { struct { char b[65 * 6]; } u; return syscall(SYS_uname, &u); }
static long c_chroot(void) { return syscall(SYS_chroot, "/nonexistent-dir"); }
static long c_read_bad(void) { return syscall(SYS_read, -1, NULL, 0); }   /* EBADF when allowed */
static long c_true(void) { execl("/bin/true", "true", (char *)NULL); return -1; }
static long c_x32(void) {
#ifdef __x86_64__
    return syscall(0x40000000 | SYS_getpid);
#else
    return 0;
#endif
}
static long c_int80(void) {
#ifdef __x86_64__
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(20) : "memory");   /* i386 getpid */
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
#else
    return 0;
#endif
}
static long c_chmod_suid(void) { return chmod(path, 04755); }
static long c_chmod_plain(void) { return chmod(path, 0755); }
static long c_fchmodat_sgid(void) { return syscall(SYS_fchmodat, AT_FDCWD, path, 02755); }
static long c_open_suid(void) { char p[320]; snprintf(p, sizeof p, "%s/n1", dir); return syscall(SYS_openat, AT_FDCWD, p, O_CREAT | O_WRONLY, 04755); }
static long c_open_plain(void) { char p[320]; snprintf(p, sizeof p, "%s/n2", dir); return syscall(SYS_openat, AT_FDCWD, p, O_CREAT | O_WRONLY, 0644); }
static long c_tmpfile_sgid(void) { return syscall(SYS_openat, AT_FDCWD, dir, O_TMPFILE | O_RDWR, 02755); }
static long c_openat2(void) { return syscall(SYS_openat2, AT_FDCWD, path, NULL, 0); }
static long c_mknod_suid(void) { char p[320]; snprintf(p, sizeof p, "%s/n3", dir); return syscall(SYS_mknodat, AT_FDCWD, p, S_IFREG | 04755, 0); }
static long c_pers_linux(void) { return personality(PER_LINUX) < 0 ? -1 : 0; }
static long c_pers_query(void) { return personality(0xffffffff) < 0 ? -1 : 0; }
static long c_pers_bsd(void) { return personality(PER_BSD) < 0 ? -1 : 0; }
static long c_fifo(void) { struct sched_param sp = { .sched_priority = 1 }; return sched_setscheduler(0, SCHED_FIFO, &sp); }
static long c_fifo_rof(void) { struct sched_param sp = { .sched_priority = 1 }; return sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &sp); }
static long c_other(void) { struct sched_param sp = { .sched_priority = 0 }; return sched_setscheduler(0, SCHED_OTHER, &sp); }
static long c_setattr(void) { return syscall(SYS_sched_setattr, 0, NULL, 0); }
static long c_unshare_user(void) { return unshare(CLONE_NEWUSER); }
static long c_fork(void) { pid_t p = fork(); if (p == 0) _exit(0); if (p < 0) return -1; waitpid(p, NULL, 0); return 0; }
static long c_clone_user(void) {
    long r = syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, 0, 0, 0, 0);
    if (r == 0) _exit(0);
    if (r > 0) waitpid((pid_t)r, NULL, 0);
    return r < 0 ? -1 : 0;
}
static long c_clone3(void) { return syscall(SYS_clone3, NULL, 0); }
static long c_setns(void) { return syscall(SYS_setns, -1, 0); }

int main(void) {
    sc_spec_t s;
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/f", dir);
    close(open(path, O_CREAT | O_WRONLY, 0644));

    /* allowlist: listed calls pass, the rest get EPERM (the default here);
     * @default rides along as in systemd, so the process can still exit */
    s = spec(SC_ALLOW, "getpid");
    assert(probe(&s, c_getpid) == 0 && probe(&s, c_uname) == EPERM && probe(&s, c_getuid) == 0);
    s.err = SC_KILL;
    assert(probe(&s, c_uname) == -SIGSYS);
    /* denylist: the inverse; a per-entry errno wins */
    s = spec(SC_DENY, "getuid");
    assert(probe(&s, c_getuid) == EPERM && probe(&s, c_getpid) == 0);
    s = spec(SC_DENY, "getuid:EACCES");
    assert(probe(&s, c_getuid) == EACCES);
    s = spec(SC_DENY, "getuid:13");
    assert(probe(&s, c_getuid) == 13);
    /* a whole group with an errno, and a second errno class */
    s = spec(SC_DENY, "@privileged:EACCES getuid:ENOSYS");
    assert(probe(&s, c_chroot) == EACCES && probe(&s, c_getuid) == ENOSYS && probe(&s, c_getpid) == 0);
    /* up to 4 distinct errnos; a fifth is refused (-3) */
    memset(&s, 0, sizeof s);
    s.mode = SC_DENY;
    assert(sc_add(&s, "getuid:1", 0) == 0 && sc_add(&s, "getgid:2", 0) == 0 && sc_add(&s, "getppid:3", 0) == 0 &&
           sc_add(&s, "gettid:4", 0) == 0 && sc_add(&s, "uname:5", 0) == -3);
    /* a group, removal from it, and execve always allowed */
    s = spec(SC_ALLOW, "@system-service");
    assert(probe(&s, c_true) == 0 && probe(&s, c_getpid) == 0);
    /* a ~ line takes names out; @default's members always stay */
    assert(sc_add(&s, "uname", 1) == 0 && sc_add(&s, "getpid", 1) == 0);
    assert(probe(&s, c_uname) == EPERM && probe(&s, c_getpid) == 0);
    /* an unknown name is skipped (-2: a unit from a newer systemd still
     * loads, as systemd itself ignores them); an unknown group or errno is a
     * typo and refuses */
    assert(sc_add(&s, "@nonesuch", 0) == -1 && sc_add(&s, "nonesuch", 0) == -2 && sc_add(&s, "getpid:EBOGUS", 0) == -1);

    /* a big allowlist (trampolines in the tree): both ends and just outside */
    {
        int lo = 1 << 30, hi = -1;
        s = spec(SC_ALLOW, "@system-service");
        for (int k = 0; k < SC_GROUPS; k++) {
            if (strcmp(sc_groups[k].name, "@system-service")) continue;
            for (const unsigned short *p = sc_groups[k].idx; *p != 0xffff; p++)
                if (sc_nr[*p] >= 0) { if (sc_nr[*p] < lo) lo = sc_nr[*p]; if (sc_nr[*p] > hi) hi = sc_nr[*p]; }
        }
        struct sock_filter prog[SC_MAX_INSNS];
        int n = sc_build(&s, prog, SC_MAX_INSNS);
        assert(n > 255 && n <= SC_MAX_INSNS);
        assert(probe(&s, c_read_bad) == EBADF);
        /* hi + 1 is above every member, so it can't be allowed */
        int above_nr = hi + 1;
        assert(lo >= 0);
        pid_t p = fork();
        if (p == 0) {
            const char *step;
            if (sc_install(&s, &step) != 0) _exit(250);
            long r = syscall(above_nr, 0, 0, 0, 0, 0, 0);
            _exit(r < 0 ? errno & 0x7f : 0);
        }
        int st;
        waitpid(p, &st, 0);
        assert(WIFEXITED(st) && WEXITSTATUS(st) == EPERM);
    }
    /* a filter as large as the names allow still fits the kernel's limit */
    {
        sc_spec_t all;
        memset(&all, 0, sizeof all);
        all.mode = SC_ALLOW;
        for (int i = 0; i < SC_NAMES_ALL; i++) all.set[i / 64] |= 1ull << (i % 64);
        struct sock_filter prog[SC_MAX_INSNS];
        int n = sc_build(&all, prog, SC_MAX_INSNS);
        assert(n > 0 && n <= SC_MAX_INSNS);
    }

#ifdef __x86_64__
    /* foreign ABIs: x32 and i386 compat never get past a filter */
    s = spec(SC_DENY, "getuid");
    assert(probe(&s, c_x32) == -SIGSYS);
    if (probe(NULL, c_int80) == 0) {
        s = spec(SC_ALLOW, "@system-service");
        assert(probe(&s, c_int80) == -SIGSYS);
    }
#endif

    /* restrict_suid_sgid */
    memset(&s, 0, sizeof s);
    s.suid = 1;
    assert(probe(&s, c_chmod_suid) == EPERM && probe(&s, c_chmod_plain) == 0);
    assert(probe(&s, c_fchmodat_sgid) == EPERM);
    assert(probe(&s, c_open_suid) == EPERM && probe(&s, c_open_plain) == 0);
    if (probe(NULL, c_tmpfile_sgid) == 0) assert(probe(&s, c_tmpfile_sgid) == EPERM);
    assert(probe(&s, c_openat2) == ENOSYS);
    assert(probe(&s, c_mknod_suid) == EPERM);
    /* lock_personality */
    memset(&s, 0, sizeof s);
    s.persona = 1;
    assert(probe(&s, c_pers_linux) == 0 && probe(&s, c_pers_query) == 0 && probe(&s, c_pers_bsd) == EPERM);
    /* its allowed values still meet syscall_filter (no early ALLOW) */
    s = spec(SC_DENY, "personality");
    s.persona = 1;
    assert(probe(&s, c_pers_linux) == EPERM);
    /* restrict_realtime (this account may set FIFO: RLIMIT_RTPRIO) */
    memset(&s, 0, sizeof s);
    s.rt = 1;
    assert(probe(&s, c_fifo) == EPERM && probe(&s, c_fifo_rof) == EPERM && probe(&s, c_other) == 0);
    /* systemd's RestrictRealtime filters only sched_setscheduler */
    assert(probe(&s, c_setattr) != EPERM);
    /* restrict_namespaces: plain fork still works */
    memset(&s, 0, sizeof s);
    s.ns = 1;
    assert(probe(&s, c_unshare_user) == EPERM && probe(&s, c_fork) == 0);
    assert(probe(&s, c_clone_user) == EPERM);
    assert(probe(&s, c_clone3) == ENOSYS && probe(&s, c_setns) == EPERM);
    assert(probe(NULL, c_setns) == EBADF);

    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    assert(system(cmd) == 0);
    printf("all seccomp tests passed\n");
    return 0;
}
