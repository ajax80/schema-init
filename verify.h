#ifndef SCHEMA_VERIFY_H
#define SCHEMA_VERIFY_H

#include "verify_boot.h"
#include <stdio.h>

/* bls_dir NULL: grubenv's blsdir=; cmdline_txt NULL: the Pi's usual places;
 * inspect: a stand-in for root_inspect's mount+chroot child (tests only) */
typedef struct {
    int quiet, accept;
    const char *state_dir, *grubenv, *bls_dir, *grubcfg, *fstab, *kmsg_path, *devdir;
    const char *esp, *cmdline_txt, *pi_init, *inspect;
} verify_opts_t;

/* rpmvercmp's rule; bls_sort orders entries as blscfg's menu, newest first */
int bls_vercmp(const char *a, const char *b);
void bls_sort(bls_entry_t *ents, int n);
/* the real paths; a -DSCHEMA_VERIFY_TESTHOOKS build takes each from its SCHEMA_VERIFY_* variable */
void verify_opts_env(verify_opts_t *o);
/* 0 pass, 1 FAIL, 2 unaccepted CHANGE or lost baseline, 3 nothing worse than UNRESOLVED */
int verify_run(const verify_opts_t *o, FILE *out);

#endif
