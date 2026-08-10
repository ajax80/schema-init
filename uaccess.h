#ifndef UACCESS_H
#define UACCESS_H

#include "schema-udev.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>

#define SCHEMA_UACCESS_DIR "/run/schema-udev/uaccess"
#define SEAT0_PATH         "/run/systemd/seats/seat0"

static inline int uaccess_active_uid(const char *seat_path) {
    FILE *f = fopen(seat_path, "r");
    if (!f) return -1;
    char line[256];
    int uid = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "ACTIVE_UID=", 11) == 0) { uid = atoi(line + 11); break; }
    }
    fclose(f);
    return uid;
}

static inline int uaccess_eligible(const struct uevent *ev) {
    const char *sub = uevent_get(ev, "SUBSYSTEM");
    if (!sub) return 0;
    return strcmp(sub, "sound") == 0
        || strcmp(sub, "video4linux") == 0
        || strcmp(sub, "media") == 0;
}

static inline int ua_keyname(const struct uevent *ev, char *out, size_t outsz) {
    const char *maj = uevent_get(ev, "MAJOR");
    const char *min = uevent_get(ev, "MINOR");
    if (!maj || !min) return -1;
    if ((size_t)snprintf(out, outsz, "c%s:%s", maj, min) >= outsz) return -1;
    return 0;
}

static inline int uaccess_clear(const char *dir, const struct uevent *ev) {
    char key[64];
    if (ua_keyname(ev, key, sizeof key) != 0) return -1;
    char path[512];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, key) >= sizeof path) return -1;
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}

static inline int ua_mkdir_p(const char *path) {
    char tmp[512];
    safe_copy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1; *p = '/'; }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static inline int uaccess_record(const char *dir, const char *seat_path,
                                 const struct uevent *ev) {
    if (!uaccess_eligible(ev)) return uaccess_clear(dir, ev);
    int uid = uaccess_active_uid(seat_path);
    if (uid < 0) return uaccess_clear(dir, ev);
    const char *devname = uevent_get(ev, "DEVNAME");
    if (!devname || !devname[0]) return -1;

    char key[64];
    if (ua_keyname(ev, key, sizeof key) != 0) return -1;
    if (ua_mkdir_p(dir) != 0) return -1;

    char final[512], tmp[512];
    if ((size_t)snprintf(final, sizeof final, "%s/%s", dir, key) >= sizeof final) return -1;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s/.%s.tmp.%d", dir, key, (int)getpid()) >= sizeof tmp) return -1;

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "DEVNODE=/dev/%s\nGRANT_UID=%d\nACL=user:%d:rw\n", devname, uid, uid);
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, final) != 0) { unlink(tmp); return -1; }
    return 0;
}

static inline void uaccess_wipe(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) < sizeof path)
            unlink(path);
    }
    closedir(d);
}

#endif /* UACCESS_H */
