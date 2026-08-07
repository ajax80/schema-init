#ifndef UDEV_PARITY_H
#define UDEV_PARITY_H

#include "schema-udev.h"
#include <string.h>
#include <stdio.h>

#define UDEV_DB_DIR "/run/udev/data"

static inline int udev_db_read_eprops(const char *path, struct uevent *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(out, 0, sizeof *out);
    char line[1024];
    while (fgets(line, sizeof line, f) && out->n < UE_MAX_KEYS) {
        if (line[0] != 'E' || line[1] != ':') continue;
        char *kv = line + 2;
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';
        safe_copy(out->key[out->n], kv, UE_KEY_MAX);
        safe_copy(out->val[out->n], val, UE_VAL_MAX);
        out->n++;
    }
    fclose(f);
    return 0;
}

static inline const char *parity_builtin_hint(const char *key) {
    if (strstr(key, "_FROM_DATABASE")) return "hwdb";
    if (strncmp(key, "ID_INPUT", 8) == 0) return "input_id";
    if (strncmp(key, "ID_NET_NAME_P", 13) == 0 || strncmp(key, "ID_NET_NAME_M", 13) == 0 ||
        strncmp(key, "ID_NET_NAME_S", 13) == 0 || strncmp(key, "ID_NET_NAME_O", 13) == 0 ||
        strncmp(key, "ID_NET_NAMING_", 14) == 0) return "net_id";
    if (strncmp(key, "ID_FS_", 6) == 0 || strncmp(key, "ID_PART_", 8) == 0) return "blkid";
    if (strcmp(key, "ID_PATH") == 0 || strcmp(key, "ID_PATH_TAG") == 0) return "path_id";
    if (strncmp(key, "ID_V4L", 6) == 0 || strncmp(key, "ID_VIDEO", 8) == 0) return "v4l_id";
    if (strncmp(key, "ID_USB", 6) == 0 || strcmp(key, "ID_SERIAL") == 0) return "usb_id";
    return "";
}

struct keycount { char key[UE_KEY_MAX]; int count; };

static inline void keycount_add(struct keycount *tab, int *n, int max, const char *key) {
    int i;
    for (i = 0; i < *n; i++)
        if (strcmp(tab[i].key, key) == 0) { tab[i].count++; return; }
    if (*n < max) {
        safe_copy(tab[*n].key, key, UE_KEY_MAX);
        tab[*n].count = 1;
        (*n)++;
    }
}

static inline void keycount_sort_desc(struct keycount *tab, int n) {
    int i, j;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (tab[j].count > tab[i].count) {
                struct keycount t = tab[i]; tab[i] = tab[j]; tab[j] = t;
            }
}

#endif /* UDEV_PARITY_H */
