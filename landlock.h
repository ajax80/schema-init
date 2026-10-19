#ifndef LANDLOCK_H
#define LANDLOCK_H

#include <stdint.h>

#define MAX_LANDLOCK 16

/* 1 if path is p or lies beneath it. */
int landlock_beneath(const char *path, const char *p);

/* Confine the calling process's filesystem access to paths: bit i of rw_mask
 * set grants paths[i] every right, clear grants read + execute. Paths that do
 * not exist are skipped; n == 0 leaves the filesystem alone. tcp_n[0] / tcp_n[1]
 * (-1 = unrestricted) are how many of tcp[0] / tcp[1] the process may bind /
 * connect to over TCP; anything else is refused (Landlock ABI 4, kernel 6.7).
 * Needs no_new_privs or CAP_SYS_ADMIN. Returns 0, or -1 with errno set and
 * *step naming the failing operation. */
int apply_landlock(char *const *paths, uint32_t rw_mask, int n,
                   const int tcp_n[2], const uint16_t tcp[2][MAX_LANDLOCK], const char **step);

#endif
