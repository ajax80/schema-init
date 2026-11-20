#ifndef SCHEMA_KARG_H
#define SCHEMA_KARG_H

/* Kernel cmdline words as the kernel splits them (lib/cmdline.c next_arg):
 * double quotes group, no backslash escapes, a leading quote on the word or
 * on its value is dropped with its closing one. -1 past max words. */
int karg_split(const char *opts, char (*out)[512], int max);

#endif
