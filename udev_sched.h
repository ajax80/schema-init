/* Which queued uevent a worker may take next: udevd's rule that events for
 * one device (or its ancestors/descendants) run in order, everything else
 * in parallel. Pure; schema-udev.c owns the queue and the processes. */
#ifndef UDEV_SCHED_H
#define UDEV_SCHED_H

#include <stdlib.h>
#include <string.h>

struct sched_meta {
    char devpath[UE_VAL_MAX];
    char devpath_old[UE_VAL_MAX];
    char action[16];
    char subsystem[64];
    unsigned major, minor;
    int has_devnum;
    int is_block;
    int ifindex;
};

static inline void sched_meta_from(const struct uevent *ev, struct sched_meta *m) {
    const char *v;
    memset(m, 0, sizeof *m);
    if ((v = uevent_get(ev, "DEVPATH"))) snprintf(m->devpath, sizeof m->devpath, "%s", v);
    if ((v = uevent_get(ev, "DEVPATH_OLD"))) snprintf(m->devpath_old, sizeof m->devpath_old, "%s", v);
    if ((v = uevent_get(ev, "ACTION"))) snprintf(m->action, sizeof m->action, "%s", v);
    if ((v = uevent_get(ev, "SUBSYSTEM"))) snprintf(m->subsystem, sizeof m->subsystem, "%s", v);
    const char *ma = uevent_get(ev, "MAJOR"), *mi = uevent_get(ev, "MINOR");
    if (ma && mi && *ma && *mi) {
        m->major = (unsigned)strtoul(ma, NULL, 10);
        m->minor = (unsigned)strtoul(mi, NULL, 10);
        m->has_devnum = 1;
    }
    m->is_block = strcmp(m->subsystem, "block") == 0;
    if ((v = uevent_get(ev, "IFINDEX"))) m->ifindex = atoi(v);
}

/* a == b, or one is an ancestor of the other on a '/' boundary */
static inline int sched_path_related(const char *a, const char *b) {
    if (!*a || !*b) return 0;
    size_t la = strlen(a), lb = strlen(b);
    if (la > lb) { const char *t = a; a = b; b = t; size_t tl = la; la = lb; lb = tl; }
    return strncmp(a, b, la) == 0 && (b[la] == '/' || b[la] == '\0');
}

static inline int sched_conflict(const struct sched_meta *a, const struct sched_meta *b) {
    if (!a->devpath[0] || !b->devpath[0]) return 1;
    const char *pa[2] = { a->devpath, a->devpath_old }, *pb[2] = { b->devpath, b->devpath_old };
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            if (sched_path_related(pa[i], pb[j])) return 1;
    /* db record, /dev/{block,char} link, claims and ACL node are keyed by devnum */
    if (a->has_devnum && b->has_devnum && a->is_block == b->is_block &&
        a->major == b->major && a->minor == b->minor) return 1;
    if (a->ifindex && a->ifindex == b->ifindex) return 1;
    /* NIC renames go through temporary names that can collide */
    if (!strcmp(a->subsystem, "net") && !strcmp(b->subsystem, "net")) return 1;
    return 0;
}

static inline int sched_cap(long nproc) {
    long c = 2 * nproc;
    if (c > 16) c = 16;
    if (c < 4) c = 4;
    return (int)c;
}

static inline int sched_pick(const struct sched_meta *queued, int nq,
                             const struct sched_meta *running, int nr) {
    for (int i = 0; i < nq; i++) {
        int blocked = 0;
        for (int j = 0; j < nr && !blocked; j++) blocked = sched_conflict(&queued[i], &running[j]);
        for (int j = 0; j < i && !blocked; j++) blocked = sched_conflict(&queued[i], &queued[j]);
        if (!blocked) return i;
    }
    return -1;
}

#endif
