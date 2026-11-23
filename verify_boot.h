#ifndef SCHEMA_VERIFY_BOOT_H
#define SCHEMA_VERIFY_BOOT_H

#include "karg.h"
#include <stddef.h>

/* the env block is 1 KB, so a value can be nearly that long; one longer than this is
 * kept as GRUBENV_CUT, never as a shorter value */
#define GRUBENV_SZ 1024
#define GRUBENV_CUT "\x01"
/* the option namespace's value for a name grubenv and grub.cfg both set, differently */
#define GRUBENV_BOTH "\x02"
/* the option namespace's value for a name grub.cfg sets past the room verify has */
#define GRUBENV_DROPPED "\x03"
typedef char grubenv_t[2][GRUBENV_SZ];

typedef struct {
    char id[256];
    char linux_path[512];
    char initrd[8][512];
    int n_initrd;
    int initrd_vars; /* initrd words that are GRUB variables such as $tuned_initrd */
    char options[4096];
    int blank_before_last;
    int parse_err;
    int extra_linux, extra_options; /* a second linux/options line: UNRESOLVED */
} bls_entry_t;

typedef struct { const char *actual, *normal, *fallback; int unresolved; } nextboot_t;

int grubenv_read(const char *path, grubenv_t *kv, int max);
/* grubenv's env_block=OFF+LEN (512-byte sectors on dev), which GRUB loads after the file;
 * -1 with errno when it can't be read or is no env block */
int grubenv_block_read(const char *dev, const char *spec, grubenv_t *kv, int max);
int bls_parse(const char *path, bls_entry_t *e);
/* 0 ok; -1 FAIL with why */
int karg_check(char (*args)[512], int n, char *why, size_t whysz);
/* 0 resolved; 1 unresolved; 2 FAIL: schema_fallback_id names no entry.
 * An id that could not be resolved is NULL (fallback keeps the unknown id).
 * dflt: grub.cfg's header default (grubcfg_default), NULL when not known.
 * ents must be in blscfg's sort order; a menu position resolves only when none was cut. */
int nextboot_resolve(grubenv_t *env, int nenv, const bls_entry_t *ents, int nents, const char *dflt, int cut,
                     nextboot_t *nb);
int grubcfg_live_fallback(const char *cfg_text);
/* the value grub.cfg's header gives default= when there is no next_entry ("${saved_entry}",
 * a position, a title); 0, or -1 when it can't be told */
int grubcfg_default(const char *cfg_text, char *out, size_t n);
/* grub.cfg's top-level literal set NAME=value lines (not under if, menuentry or function),
 * the last one per name; the count */
int grubcfg_vars(const char *cfg_text, grubenv_t *kv, int max);

#endif
