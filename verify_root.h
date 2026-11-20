#ifndef SCHEMA_VERIFY_ROOT_H
#define SCHEMA_VERIFY_ROOT_H

#include "verify_boot.h"
#include <stddef.h>
#include <sys/stat.h>

/* fail/reasons: FAIL lines. unres: UNRESOLVED lines, never PASS and never FAIL;
 * unresolved points at unres[0] when there is any. check_out is malloc'd, the
 * caller frees it (NUL-terminated); check_rc is -1 when --check gave no report. */
typedef struct {
    int fail;
    char reasons[16][256];
    int nreasons;
    const char *unresolved;
    char unres[8][256];
    int nunres;
    char *check_out;
    size_t check_len;
    int check_rc;
    char init_path[512]; /* init= resolved inside the target */
    int is_schema_init;
    int bootok_exec; /* schema-bootok's exec= in the target: 0 not seen, 1 executable, 2 not */
} root_report_t;

/* devdir NULL: /dev ($SCHEMA_VERIFY_DEVDIR in a -DSCHEMA_VERIFY_TESTHOOKS build). 0 when spec names a present node. */
int dev_resolve(const char *spec, const char *devdir, char *out, size_t n);
/* BLS paths are relative to GRUB's root: the btrfs top level, or a separate /boot */
int bls_path_map(const char *grub_root_mnt, const char *bls_path, char *out, size_t n);
/* stat a BLS path inside root, symlinks resolved against root (nosym: refused);
 * 0 found, -1 absent, -2 refused or unreadable */
int stat_in_root(const char *root, const char *path, int nosym, struct stat *st);
/* 1 when mountinfo has a mount on /boot: then that partition is GRUB's root */
int boot_is_separate(const char *mountinfo_path);
/* GRUB expands $var/${var} in options at boot; 0 ok, -1 with the unknown name in missing */
int opts_expand(const char *opts, char (*env)[2][256], int nenv, char *out, size_t n, char *missing, size_t msz);
/* the entry checks that need no mount; 0 when root_inspect may go on to mount */
int entry_precheck(const bls_entry_t *e, char (*env)[2][256], int nenv, char *opts, size_t optsz, root_report_t *r);
/* the child's records (F/U fail/unresolved, I init path, S schema-init, C --check rc,
 * E its first stderr line, B schema-bootok's exec= 1 ok / 2 not executable) into r; takes out, the --check stdout */
void root_report_fill(char *ctl, size_t clen, char *out, size_t olen, root_report_t *r);
/* entry_precheck, then mount root= read-only in a private namespace and check the
 * target; one 30 s deadline covers the whole inspection */
int root_inspect(const bls_entry_t *e, char (*env)[2][256], int nenv, const char *check_args_extra, root_report_t *r);

#endif
