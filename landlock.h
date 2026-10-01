#ifndef LANDLOCK_H
#define LANDLOCK_H

#include <stdint.h>

#define MAX_LANDLOCK 16

/* 1 if path is p or lies beneath it. */
int landlock_beneath(const char *path, const char *p);

/* Confine the calling process's filesystem access to paths: bit i of rw_mask
 * set grants paths[i] every right, clear grants read + execute. Paths that do
 * not exist are skipped. Needs no_new_privs or CAP_SYS_ADMIN. Returns 0, or -1
 * with errno set and *step naming the failing operation. */
int apply_landlock(char *const *paths, uint32_t rw_mask, int n, const char **step);

#endif
