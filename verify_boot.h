#ifndef SCHEMA_VERIFY_BOOT_H
#define SCHEMA_VERIFY_BOOT_H

#include "karg.h"
#include <stddef.h>

typedef struct {
    char id[256];
    char linux_path[512];
    char initrd[8][512];
    int n_initrd;
    char options[4096];
    int blank_before_last;
    int parse_err;
} bls_entry_t;

typedef struct { const char *actual, *normal, *fallback; int unresolved; } nextboot_t;

int grubenv_read(const char *path, char (*kv)[2][256], int max);
int bls_parse(const char *path, bls_entry_t *e);
/* 0 ok; -1 FAIL with why */
int karg_check(char (*args)[512], int n, char *why, size_t whysz);
/* 0 resolved; 1 unresolved; 2 FAIL: schema_fallback_id names no entry.
 * An id that could not be resolved is NULL (fallback keeps the unknown id). */
int nextboot_resolve(char (*env)[2][256], int nenv, const bls_entry_t *ents, int nents, nextboot_t *nb);
int grubcfg_live_fallback(const char *cfg_text);

#endif
