#ifndef SCHEMA_SECCOMP_H
#define SCHEMA_SECCOMP_H

#include <stdint.h>

struct sock_filter;

#define SC_WORDS 10            /* bits over sc_known[]; seccomp.c asserts it is enough */
#define SC_KILL 0xFFFF         /* "errno" meaning: kill the process */
#define SC_MAX_INSNS 4096      /* the kernel's BPF_MAXINSNS */
#define SC_MAX_ERRS 4          /* distinct per-entry errnos */
enum { SC_NONE, SC_ALLOW, SC_DENY };

typedef struct {
    uint8_t  mode;             /* SC_NONE / SC_ALLOW / SC_DENY */
    uint64_t set[SC_WORDS];    /* bit i = sc_known[i] */
    uint16_t err;              /* blocked calls: 0 = EPERM, SC_KILL = kill */
    uint8_t  n_err;            /* per-entry :ERRNO classes (denylists) */
    uint16_t err_val[SC_MAX_ERRS];
    uint64_t err_set[SC_MAX_ERRS][SC_WORDS];
    uint8_t  native, suid, persona, rt, ns;
} sc_spec_t;

int sc_active(const sc_spec_t *s);
int sc_add(sc_spec_t *s, const char *entry, int remove);
int sc_errno_parse(const char *v, uint16_t *out);
int sc_build(const sc_spec_t *s, struct sock_filter *out, int max);
int sc_install(const sc_spec_t *s, const char **step);
int sc_count(const sc_spec_t *s);

#endif
