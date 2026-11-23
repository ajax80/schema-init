#ifndef CTL_LOG_H
#define CTL_LOG_H
#include <stddef.h>
#include <stdio.h>

int log_pick(const char *name, int user, char *out, size_t n);
int log_tail(int fd, long lines, FILE *out);
int log_follow(const char *path, FILE *out);
int ctl_log_main(int argc, char **argv, int user);
#endif
