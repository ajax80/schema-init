#ifndef SCHEMA_UDEV_EXEC_H
#define SCHEMA_UDEV_EXEC_H
#include "schema-udev.h"
#include <string.h>

static inline int udev_argv_split(const char *cmd, char store[][UE_VAL_MAX],
                                  char *argv[], int max) {
    int argc = 0;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argc >= max) return -1;
        char *o = store[argc]; size_t n = 0;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '\'') { p++; while (*p && *p != '\'') { if (n + 1 < UE_VAL_MAX) o[n++] = *p; p++; }
                              if (*p == '\'') p++; }
            else { if (n + 1 < UE_VAL_MAX) o[n++] = *p; p++; }
        }
        o[n] = '\0'; argv[argc] = store[argc]; argc++;
    }
    argv[argc] = NULL;
    return argc;
}
#endif
