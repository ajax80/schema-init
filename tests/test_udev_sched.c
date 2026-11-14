#include "../udev_ruleset.h"
#include "../udev_sched.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct sched_meta meta(const char *devpath, const char *subsystem,
                              unsigned major, unsigned minor, int ifindex) {
    struct uevent ev;
    char b[16];
    memset(&ev, 0, sizeof ev);
    uevent_set(&ev, "ACTION", "add");
    if (devpath) uevent_set(&ev, "DEVPATH", devpath);
    if (subsystem) uevent_set(&ev, "SUBSYSTEM", subsystem);
    if (major || minor) {
        snprintf(b, sizeof b, "%u", major); uevent_set(&ev, "MAJOR", b);
        snprintf(b, sizeof b, "%u", minor); uevent_set(&ev, "MINOR", b);
    }
    if (ifindex) { snprintf(b, sizeof b, "%d", ifindex); uevent_set(&ev, "IFINDEX", b); }
    struct sched_meta m;
    sched_meta_from(&ev, &m);
    return m;
}

#define P(x) meta(x, "block", 0, 0, 0)

static void prefix_boundary(void) {
    struct sched_meta a = P("/devices/b/sda"), c = P("/devices/b/sda/sda1"),
                      d = P("/devices/b/sdab"), e = P("/devices/b/sdb");
    assert(sched_conflict(&a, &c) && sched_conflict(&c, &a));
    assert(!sched_conflict(&a, &d));
    assert(sched_conflict(&a, &a));
    assert(!sched_conflict(&a, &e));
}

static void replug_same_devnum(void) {
    struct sched_meta a = meta("/devices/usb1/1-1/host6/target6:0:0/6:0:0:0/block/sdb", "block", 8, 16, 0);
    struct sched_meta b = meta("/devices/usb2/2-3/host7/target7:0:0/7:0:0:0/block/sdb", "block", 8, 16, 0);
    assert(sched_conflict(&a, &b));
}

static void block_vs_char_same_numbers(void) {
    struct sched_meta a = meta("/devices/x/blk", "block", 8, 16, 0);
    struct sched_meta b = meta("/devices/y/chr", "misc", 8, 16, 0);
    assert(!sched_conflict(&a, &b));
}

static void same_ifindex(void) {
    struct sched_meta a = meta("/devices/x/one", "foo", 0, 0, 3), b = meta("/devices/y/two", "bar", 0, 0, 3);
    assert(sched_conflict(&a, &b));
    struct sched_meta n1 = meta("/devices/n/eth0", "net", 0, 0, 2), n2 = meta("/devices/m/eth1", "net", 0, 0, 4);
    assert(sched_conflict(&n1, &n2));
    struct sched_meta blk = meta("/devices/z/sdz", "block", 8, 0, 0);
    assert(!sched_conflict(&n1, &blk));
}

/* only net add/move can rename (temp names collide); a veth remove burst
 * must not queue a keyboard behind it */
static void net_remove_not_serialised(void) {
    struct sched_meta r1 = meta("/devices/virtual/net/veth1", "net", 0, 0, 11);
    struct sched_meta r2 = meta("/devices/virtual/net/veth2", "net", 0, 0, 12);
    snprintf(r1.action, sizeof r1.action, "remove");
    snprintf(r2.action, sizeof r2.action, "remove");
    assert(!sched_conflict(&r1, &r2));
    struct sched_meta a2 = meta("/devices/virtual/net/veth3", "net", 0, 0, 13);
    assert(!sched_conflict(&r1, &a2));
    struct sched_meta a3 = meta("/devices/virtual/net/veth4", "net", 0, 0, 14);
    assert(sched_conflict(&a2, &a3));
}

static void move_old_path(void) {
    struct sched_meta a = meta("/devices/x/wlan0", "net", 0, 0, 0);
    snprintf(a.devpath_old, sizeof a.devpath_old, "%s", "/devices/x/eth0");
    snprintf(a.subsystem, sizeof a.subsystem, "%s", "foo");
    struct sched_meta b = meta("/devices/x/eth0", "bar", 0, 0, 0);
    assert(sched_conflict(&a, &b) && sched_conflict(&b, &a));
}

static void no_devpath(void) {
    struct sched_meta a = meta(NULL, "block", 0, 0, 0), b = P("/devices/q");
    assert(sched_conflict(&a, &b) && sched_conflict(&b, &a));
}

static void queue_order_same_dev(void) {
    struct sched_meta q[3] = { P("/devices/b/sda"), P("/devices/b/sda"), P("/devices/b/sdb") };
    assert(sched_pick(q, 3, NULL, 0) == 0);
    struct sched_meta r[1] = { P("/devices/b/sda") };
    assert(sched_pick(q + 1, 2, r, 1) == 1);
}

static void held_behind_queued(void) {
    struct sched_meta r[1] = { P("/devices/b/sda/sda1") };
    struct sched_meta q[2] = { P("/devices/b/sda"), P("/devices/b/sdc") };
    assert(sched_pick(q, 2, r, 1) == 1);
    /* sda1 waits behind the queued sda even with nothing running */
    struct sched_meta q2[2] = { P("/devices/b/sda"), P("/devices/b/sda/sda1") };
    struct sched_meta r2[1] = { P("/devices/b/sda") };
    assert(sched_pick(q2 + 0, 2, r2, 1) == -1);
}

static void cap(void) {
    assert(sched_cap(1) == 4);
    assert(sched_cap(4) == 8);
    assert(sched_cap(32) == 16);
}

static void meta_from(void) {
    struct sched_meta m = meta("/devices/b/sda", "block", 8, 16, 0);
    assert(m.has_devnum == 1 && m.is_block == 1 && m.ifindex == 0);
    assert(m.major == 8 && m.minor == 16);
    assert(!strcmp(m.action, "add") && !strcmp(m.devpath, "/devices/b/sda"));
    struct sched_meta n = meta("/devices/v/x", "misc", 0, 0, 0);
    assert(n.has_devnum == 0 && n.is_block == 0);
}

int main(void) {
    prefix_boundary();
    replug_same_devnum();
    block_vs_char_same_numbers();
    same_ifindex();
    net_remove_not_serialised();
    move_old_path();
    no_devpath();
    queue_order_same_dev();
    held_behind_queued();
    cap();
    meta_from();
    printf("test_udev_sched: ok\n");
    return 0;
}
