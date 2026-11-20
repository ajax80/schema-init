#include "seccomp.h"
#include "seccomp_names.h"
#include "seccomp_groups.h"
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

_Static_assert(SC_NAMES_ALL <= SC_WORDS * 64, "SC_WORDS too small for sc_known");

#if defined(__x86_64__)
#define SC_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define SC_ARCH AUDIT_ARCH_AARCH64
#elif defined(__arm__)
#define SC_ARCH AUDIT_ARCH_ARM
#elif defined(__i386__)
#define SC_ARCH AUDIT_ARCH_I386
#elif defined(__riscv) && __riscv_xlen == 64
#define SC_ARCH AUDIT_ARCH_RISCV64
#else
#error "seccomp.c: no AUDIT_ARCH for this architecture"
#endif

/* the low 32 bits of a 64-bit argument: modes and flags all live there */
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define ARG_LO(i) (uint32_t)(offsetof(struct seccomp_data, args) + 8 * (i) + 4)
#else
#define ARG_LO(i) (uint32_t)(offsetof(struct seccomp_data, args) + 8 * (i))
#endif
#define NR_OFF   (uint32_t)offsetof(struct seccomp_data, nr)
#define ARCH_OFF (uint32_t)offsetof(struct seccomp_data, arch)
#define X32_BIT  0x40000000u
#define SUIDSGID (S_ISUID | S_ISGID)
#define NEW_NS   (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | \
                  CLONE_NEWPID | CLONE_NEWNET)
#ifndef CLONE_NEWTIME
#define CLONE_NEWTIME 0x80
#endif

typedef struct { struct sock_filter *p; int n, max; } em_t;

static void emit(em_t *e, uint16_t code, uint8_t jt, uint8_t jf, uint32_t k) {
    if (e->n < e->max) e->p[e->n] = (struct sock_filter){ code, jt, jf, k };
    e->n++;
}
#define LD(e, off)  emit(e, BPF_LD | BPF_W | BPF_ABS, 0, 0, off)
#define RET(e, v)   emit(e, BPF_RET | BPF_K, 0, 0, v)
#define ALLOW       SECCOMP_RET_ALLOW
#define ERR(x)      (SECCOMP_RET_ERRNO | (x))

static uint32_t act(uint16_t err) {
    return err == SC_KILL ? SECCOMP_RET_KILL_PROCESS : ERR(err ? err : EPERM);
}

int sc_active(const sc_spec_t *s) {
    return s->mode || s->native || s->suid || s->persona || s->rt || s->ns;
}

static const struct { const char *n; int v; } errnos[] = {
    { "EPERM", EPERM }, { "EACCES", EACCES }, { "ENOSYS", ENOSYS }, { "EINVAL", EINVAL },
    { "ENOENT", ENOENT }, { "EIO", EIO }, { "EBADF", EBADF }, { "EAGAIN", EAGAIN },
    { "ENOMEM", ENOMEM }, { "EFAULT", EFAULT }, { "EBUSY", EBUSY }, { "EEXIST", EEXIST },
    { "ENODEV", ENODEV }, { "EOPNOTSUPP", EOPNOTSUPP }, { "ENOTSUP", ENOTSUP }, { "EROFS", EROFS },
    { "ENOTTY", ENOTTY }, { "ESRCH", ESRCH }, { "EINTR", EINTR }, { "ENOTDIR", ENOTDIR },
};

int sc_errno_parse(const char *v, uint16_t *out) {
    char *end;
    if (!strcasecmp(v, "kill")) { *out = SC_KILL; return 0; }
    if (*v >= '0' && *v <= '9') {
        unsigned long n = strtoul(v, &end, 10);
        if (*end || n > 4095) return -1;
        *out = (uint16_t)n;
        return 0;
    }
    for (size_t i = 0; i < sizeof errnos / sizeof errnos[0]; i++)
        if (!strcmp(v, errnos[i].n)) { *out = (uint16_t)errnos[i].v; return 0; }
    return -1;
}

static int name_idx(const char *n, size_t len) {
    int lo = 0, hi = SC_NAMES_ALL - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strncmp(sc_known[mid], n, len);
        if (!c && sc_known[mid][len]) c = 1;
        if (!c) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

/* an errno is one of up to SC_MAX_ERRS classes, each a bitset, so a whole
 * group can carry one (@privileged:EPERM is 55 names); -3 past that */
static int mark(sc_spec_t *s, int idx, int remove, int has_err, uint16_t err) {
    uint64_t bit = 1ull << (idx % 64);
    for (int k = 0; k < s->n_err; k++) s->err_set[k][idx / 64] &= ~bit;
    if (remove) {
        s->set[idx / 64] &= ~bit;
        return 0;
    }
    s->set[idx / 64] |= bit;
    if (!has_err || s->mode == SC_ALLOW) return 0;   /* an allowlist entry is never blocked */
    int k;
    for (k = 0; k < s->n_err && s->err_val[k] != err; k++) {}
    if (k == s->n_err) {
        if (s->n_err >= SC_MAX_ERRS) return -3;
        s->err_val[s->n_err++] = err;
    }
    s->err_set[k][idx / 64] |= bit;
    return 0;
}

/* name or @group, optionally :ERRNO; added to the set, or removed from it.
 * -1: unknown group or errno (a typo); -2: unknown syscall name, which the
 * caller skips, as systemd does, so a unit from a newer systemd still loads;
 * -3: more distinct errnos than SC_MAX_ERRS */
int sc_add(sc_spec_t *s, const char *entry, int remove) {
    const char *colon = strchr(entry, ':');
    size_t len = colon ? (size_t)(colon - entry) : strlen(entry);
    uint16_t err = 0;
    int has_err = colon != NULL;
    if (has_err && sc_errno_parse(colon + 1, &err) != 0) return -1;
    if (!len) return -1;
    if (entry[0] == '@') {
        for (int g = 0; g < SC_GROUPS; g++) {
            if (strlen(sc_groups[g].name) != len || strncmp(sc_groups[g].name, entry, len)) continue;
            for (const unsigned short *p = sc_groups[g].idx; *p != 0xffff; p++)
                { int r = mark(s, *p, remove, has_err, err); if (r) return r; }
            return 0;
        }
        return -1;
    }
    int idx = name_idx(entry, len);
    return idx < 0 ? -2 : mark(s, idx, remove, has_err, err);
}

int sc_count(const sc_spec_t *s) {
    int n = 0;
    for (int i = 0; i < SC_NAMES_ALL; i++) n += (s->set[i / 64] >> (i % 64)) & 1 && sc_nr[i] >= 0;
    return n;
}

static int nr_cmp(const void *a, const void *b) {
    return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}

int sc_nrs(const uint64_t *set, int *out) {
    int n = 0, u = 0;
    for (int i = 0; i < SC_NAMES_ALL; i++)
        if ((set[i / 64] >> (i % 64)) & 1 && sc_nr[i] >= 0) out[n++] = sc_nr[i];
    qsort(out, (size_t)n, sizeof *out, nr_cmp);
    /* two names can share a number on one arch */
    for (int i = 0; i < n; i++)
        if (!u || out[u - 1] != out[i]) out[u++] = out[i];
    return u;
}

/* "if nr == X: <block>"; the block either returns or falls through */
static int open_if(em_t *e, int nr) {
    LD(e, NR_OFF);
    emit(e, BPF_JMP | BPF_JEQ | BPF_K, 0, 0, (uint32_t)nr);
    return e->n - 1;
}
static void close_if(em_t *e, int at) {
    if (at < e->max) e->p[at].jf = (uint8_t)(e->n - at - 1);
}
static void ret_if(em_t *e, int nr, uint32_t v) {
    int at = open_if(e, nr);
    RET(e, v);
    close_if(e, at);
}
/* a mode argument with setuid/setgid bits: EPERM */
static void suid_mode(em_t *e, int nr, int marg) {
    int at = open_if(e, nr);
    LD(e, ARG_LO(marg));
    emit(e, BPF_JMP | BPF_JSET | BPF_K, 0, 1, SUIDSGID);
    RET(e, ERR(EPERM));
    close_if(e, at);
}
/* open family: only when creating (O_CREAT, or O_TMPFILE, which takes a mode too) */
static void suid_open(em_t *e, int nr, int farg, int marg) {
    int at = open_if(e, nr);
    LD(e, ARG_LO(farg));
    emit(e, BPF_JMP | BPF_JSET | BPF_K, 2, 0, O_CREAT);
    emit(e, BPF_ALU | BPF_AND | BPF_K, 0, 0, O_TMPFILE);
    emit(e, BPF_JMP | BPF_JEQ | BPF_K, 0, 3, O_TMPFILE);
    LD(e, ARG_LO(marg));
    emit(e, BPF_JMP | BPF_JSET | BPF_K, 0, 1, SUIDSGID);
    RET(e, ERR(EPERM));
    close_if(e, at);
}
static void flags_deny(em_t *e, int nr, int arg, uint32_t mask) {
    int at = open_if(e, nr);
    LD(e, ARG_LO(arg));
    emit(e, BPF_JMP | BPF_JSET | BPF_K, 0, 1, mask);
    RET(e, ERR(EPERM));
    close_if(e, at);
}

static void siblings(em_t *e, const sc_spec_t *s) {
    if (s->persona) {
        /* PER_LINUX, or the query value, and nothing else */
        /* allowed values fall through: syscall_filter still decides */
        int at = open_if(e, __NR_personality);
        LD(e, ARG_LO(0));
        emit(e, BPF_JMP | BPF_JEQ | BPF_K, 2, 0, 0);
        emit(e, BPF_JMP | BPF_JEQ | BPF_K, 1, 0, 0xffffffffu);
        RET(e, ERR(EPERM));
        close_if(e, at);
    }
    if (s->rt) {
        int at = open_if(e, __NR_sched_setscheduler);
        LD(e, ARG_LO(1));
        emit(e, BPF_ALU | BPF_AND | BPF_K, 0, 0, ~(uint32_t)SCHED_RESET_ON_FORK);
        emit(e, BPF_JMP | BPF_JEQ | BPF_K, 3, 0, SCHED_FIFO);
        emit(e, BPF_JMP | BPF_JEQ | BPF_K, 2, 0, SCHED_RR);
        emit(e, BPF_JMP | BPF_JEQ | BPF_K, 1, 0, 6 /* SCHED_DEADLINE */);
        emit(e, BPF_JMP | BPF_JA, 0, 0, 1);
        RET(e, ERR(EPERM));
        close_if(e, at);
        /* sched_setattr is left alone, as systemd's RestrictRealtime does */
    }
    if (s->ns) {
        /* clone's low byte is the exit signal, so no CLONE_NEWTIME there */
        flags_deny(e, __NR_clone, 0, NEW_NS);
        flags_deny(e, __NR_unshare, 0, NEW_NS | CLONE_NEWTIME);
        ret_if(e, __NR_setns, ERR(EPERM));
#ifdef __NR_clone3
        /* flags behind a pointer: ENOSYS makes libc fall back to clone */
        ret_if(e, __NR_clone3, ERR(ENOSYS));
#endif
    }
    if (s->suid) {
#ifdef __NR_chmod
        suid_mode(e, __NR_chmod, 1);
#endif
        suid_mode(e, __NR_fchmod, 1);
        suid_mode(e, __NR_fchmodat, 2);
#ifdef __NR_fchmodat2
        suid_mode(e, __NR_fchmodat2, 2);
#endif
#ifdef __NR_mknod
        suid_mode(e, __NR_mknod, 1);
#endif
        suid_mode(e, __NR_mknodat, 2);
#ifdef __NR_creat
        suid_mode(e, __NR_creat, 1);
#endif
#ifdef __NR_open
        suid_open(e, __NR_open, 1, 2);
#endif
        suid_open(e, __NR_openat, 2, 3);
#ifdef __NR_openat2
        ret_if(e, __NR_openat2, ERR(ENOSYS));
#endif
    }
}

typedef struct { uint32_t nr, act; } leaf_t;

static int cmp_leaf(const void *a, const void *b) {
    uint32_t x = ((const leaf_t *)a)->nr, y = ((const leaf_t *)b)->nr;
    return x < y ? -1 : x > y;
}

/* instruction count of the subtree over [lo, hi): small ranges are jeq
 * chains, larger ones split on the middle with one compare (two when the
 * left half is too far for an 8-bit jump and needs a BPF_JA trampoline) */
static int tree_size(int lo, int hi) {
    if (hi - lo <= 4) return 2 * (hi - lo) + 1;
    int mid = (lo + hi) / 2, l = tree_size(lo, mid);
    return (l > 255 ? 2 : 1) + l + tree_size(mid, hi);
}

static void tree(em_t *e, const leaf_t *v, int lo, int hi, uint32_t dflt) {
    if (hi - lo <= 4) {
        for (int i = lo; i < hi; i++) {
            emit(e, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, v[i].nr);
            RET(e, v[i].act);
        }
        RET(e, dflt);
        return;
    }
    int mid = (lo + hi) / 2, l = tree_size(lo, mid);
    if (l <= 255) {
        emit(e, BPF_JMP | BPF_JGE | BPF_K, (uint8_t)l, 0, v[mid].nr);
    } else {
        emit(e, BPF_JMP | BPF_JGE | BPF_K, 0, 1, v[mid].nr);
        emit(e, BPF_JMP | BPF_JA, 0, 0, (uint32_t)l);
    }
    tree(e, v, lo, mid, dflt);
    tree(e, v, mid, hi, dflt);
}

int sc_build(const sc_spec_t *s, struct sock_filter *out, int max) {
    static leaf_t v[SC_NAMES_ALL];
    em_t e = { out, 0, max };
    int n = 0;
    /* any other ABI never gets past a filter: a 32-bit compat call would
     * otherwise bypass every check below, written for native numbers */
    LD(&e, ARCH_OFF);
    emit(&e, BPF_JMP | BPF_JEQ | BPF_K, 1, 0, SC_ARCH);
    RET(&e, SECCOMP_RET_KILL_PROCESS);
#ifdef __x86_64__
    LD(&e, NR_OFF);
    emit(&e, BPF_JMP | BPF_JGE | BPF_K, 0, 1, X32_BIT);
    RET(&e, SECCOMP_RET_KILL_PROCESS);
#endif
    /* the exec of the service itself can never trip its own filter */
    ret_if(&e, __NR_execve, ALLOW);
#ifdef __NR_execveat
    ret_if(&e, __NR_execveat, ALLOW);
#endif
    siblings(&e, s);
    if (s->mode == SC_NONE) {
        RET(&e, ALLOW);
        return e.n <= max ? e.n : -1;
    }
    uint64_t set[SC_WORDS];
    memcpy(set, s->set, sizeof set);
    /* an allowlist always carries @default, as in systemd: without
     * exit_group/rt_sigreturn a process can't even exit or return from a signal */
    for (int g = 0; s->mode == SC_ALLOW && g < SC_GROUPS; g++)
        if (!strcmp(sc_groups[g].name, "@default"))
            for (const unsigned short *p = sc_groups[g].idx; *p != 0xffff; p++) set[*p / 64] |= 1ull << (*p % 64);
    for (int i = 0; i < SC_NAMES_ALL; i++) {
        if (!((set[i / 64] >> (i % 64)) & 1) || sc_nr[i] < 0) continue;
        uint32_t a = s->mode == SC_ALLOW ? ALLOW : act(s->err);
        for (int k = 0; s->mode == SC_DENY && k < s->n_err; k++)
            if ((s->err_set[k][i / 64] >> (i % 64)) & 1) a = act(s->err_val[k]);
        v[n].nr = (uint32_t)sc_nr[i];
        v[n++].act = a;
    }
    qsort(v, (size_t)n, sizeof v[0], cmp_leaf);
    LD(&e, NR_OFF);
    tree(&e, v, 0, n, s->mode == SC_ALLOW ? act(s->err) : ALLOW);
    return e.n <= max ? e.n : -1;
}

/* the last step before execve. Without CAP_SYS_ADMIN the kernel wants
 * no_new_privs first (systemd's rule), so that is set only when needed */
int sc_install(const sc_spec_t *s, const char **step) {
    static struct sock_filter prog[SC_MAX_INSNS];
    *step = "build";
    int n = sc_build(s, prog, SC_MAX_INSNS);
    if (n < 0) { errno = E2BIG; return -1; }
    struct sock_fprog fp = { (unsigned short)n, prog };
    *step = "install";
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fp) == 0) return 0;
    if (errno != EACCES) return -1;
    *step = "no_new_privs";
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
    *step = "install";
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fp);
}
