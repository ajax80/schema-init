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
#include <sys/acl.h>
#include <acl/libacl.h>

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

/* Grant user:uid rw on the real device node's access ACL, preserving all other
 * entries and recomputing the mask so the grant is effective. Idempotent. */
static inline int ua_apply_node(const char *node, int uid) {
    acl_t acl = acl_get_file(node, ACL_TYPE_ACCESS);
    if (!acl) return -1;
    acl_entry_t ent;
    int found = 0;
    for (int r = acl_get_entry(acl, ACL_FIRST_ENTRY, &ent);
         r == 1; r = acl_get_entry(acl, ACL_NEXT_ENTRY, &ent)) {
        acl_tag_t tag;
        if (acl_get_tag_type(ent, &tag) != 0 || tag != ACL_USER) continue;
        uid_t *q = acl_get_qualifier(ent);
        if (!q) continue;
        int match = ((int)*q == uid);
        acl_free(q);
        if (match) { found = 1; break; }
    }
    if (!found) {
        if (acl_create_entry(&acl, &ent) != 0) { acl_free(acl); return -1; }
        if (acl_set_tag_type(ent, ACL_USER) != 0) { acl_free(acl); return -1; }
        uid_t u = (uid_t)uid;
        if (acl_set_qualifier(ent, &u) != 0) { acl_free(acl); return -1; }
    }
    acl_permset_t ps;
    if (acl_get_permset(ent, &ps) != 0) { acl_free(acl); return -1; }
    acl_clear_perms(ps);
    acl_add_perm(ps, ACL_READ);
    acl_add_perm(ps, ACL_WRITE);
    if (acl_set_permset(ent, ps) != 0) { acl_free(acl); return -1; }
    if (acl_calc_mask(&acl) != 0) { acl_free(acl); return -1; }
    int rc = acl_set_file(node, ACL_TYPE_ACCESS, acl);
    acl_free(acl);
    return rc;
}

/* Remove the user:uid entry from the node's access ACL. A missing node or a
 * missing entry is a no-op success (the device/grant is already gone). */
static inline int ua_clear_node(const char *node, int uid) {
    acl_t acl = acl_get_file(node, ACL_TYPE_ACCESS);
    if (!acl) return errno == ENOENT ? 0 : -1;
    acl_entry_t ent;
    int changed = 0;
    for (int r = acl_get_entry(acl, ACL_FIRST_ENTRY, &ent);
         r == 1; r = acl_get_entry(acl, ACL_NEXT_ENTRY, &ent)) {
        acl_tag_t tag;
        if (acl_get_tag_type(ent, &tag) != 0 || tag != ACL_USER) continue;
        uid_t *q = acl_get_qualifier(ent);
        if (!q) continue;
        int match = ((int)*q == uid);
        acl_free(q);
        if (match) {
            if (acl_delete_entry(acl, ent) != 0) { acl_free(acl); return -1; }
            changed = 1;
            break;
        }
    }
    int rc = 0;
    if (changed) {
        if (acl_calc_mask(&acl) != 0) { acl_free(acl); return -1; }
        rc = acl_set_file(node, ACL_TYPE_ACCESS, acl);
    }
    acl_free(acl);
    return rc;
}

/* Live counterpart of uaccess_record: apply/clear the real ACL on /dev/DEVNAME
 * for the seat's active uid. Ineligible or no active uid -> nothing granted. */
static inline int uaccess_apply(const char *seat_path, const struct uevent *ev) {
    const char *devname = uevent_get(ev, "DEVNAME");
    if (!devname || !devname[0]) return -1;
    char node[UE_VAL_MAX + 8];
    if ((size_t)snprintf(node, sizeof node, "/dev/%s", devname) >= sizeof node) return -1;
    int uid = uaccess_active_uid(seat_path);
    if (uid < 0) return 0;
    if (!uaccess_eligible(ev)) return ua_clear_node(node, uid);
    return ua_apply_node(node, uid);
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
