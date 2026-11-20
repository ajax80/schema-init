#ifndef CHECK_H
#define CHECK_H

#include <stddef.h>

/* The service dir the next boot of root would use, root-prefixed. */
const char *check_svc_dir(const char *root, const char *cmdline, char *buf, size_t n);
int check_main(const char *root, const char *cmdline, const char *svc_dir_override);

#endif
