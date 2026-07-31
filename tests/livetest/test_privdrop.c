/* test_privdrop — livetest helper. Replays chronyd's real self-privdrop under
 * whatever capability set schema-init hands us, so the vmtest cannot false-green
 * the way the Phase-1 /bin/sleep test-chrony.svc did. Static, no libcap, no
 * /etc/passwd (numeric uid). Exits 0-path by idling in pause() so the harness
 * can inspect /proc/<pid>/status while it is still alive in its cgroup. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <grp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <netinet/in.h>
#include <linux/capability.h>

#define CHRONY_UID 996
#define CHRONY_GID 996
#define RUNDIR     "/run/chrony-test"

static void fail(const char *step) {
    dprintf(2, "PRIVDROP_FAIL step=%s errno=%d\n", step, errno);
    _exit(1);
}

/* Lower permitted to exactly CAP_SYS_TIME and raise it into effective. After a
 * setuid() to non-root with PR_SET_KEEPCAPS, permitted survives but effective is
 * cleared; this restores CAP_SYS_TIME the way chronyd's libcap call does. */
static int retain_sys_time(void) {
    struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct data[2];
    unsigned long long mask = 1ULL << CAP_SYS_TIME;
    memset(data, 0, sizeof(data));
    data[0].effective = data[0].permitted = data[0].inheritable =
        (unsigned)(mask & 0xffffffffu);
    data[1].effective = data[1].permitted = data[1].inheritable =
        (unsigned)(mask >> 32);
    return syscall(SYS_capset, &hdr, data);
}

int main(void) {
    if (geteuid() != 0) fail("not-root");

    /* 1. bind a privileged NTP port -> CAP_NET_BIND_SERVICE. EADDRINUSE means
     *    the permission check already passed (real chronyd may hold :123). */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) fail("socket");
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(123);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 &&
        (errno == EACCES || errno == EPERM))
        fail("bind123");

    /* 2. create + chown the runtime dir -> CAP_CHOWN. */
    mkdir(RUNDIR, 0750);                 /* EEXIST is fine */
    if (chown(RUNDIR, CHRONY_UID, CHRONY_GID) != 0) fail("chown");

    /* 3. write the pidfile AS ROOT into the chrony-owned 0750 dir, exactly as
     *    chronyd does before it drops privileges. Root is not the dir owner and
     *    0750 grants "other" nothing, so this open REQUIRES CAP_DAC_OVERRIDE --
     *    the cap whose absence crash-looped the first hardened boot. Without it
     *    this returns EACCES, the helper _exit(1)s, its cgroup empties, and
     *    every /proc assertion in the harness MISSes -> RED. */
    int pf = open(RUNDIR "/chronyd.pid", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pf < 0) fail("pidfile-root");
    close(pf);

    /* 4. drop to the chrony user, keeping caps across the transition. */
    if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) != 0) fail("keepcaps");
    if (setgroups(0, NULL) != 0) fail("setgroups");   /* CAP_SETGID */
    if (setgid(CHRONY_GID) != 0) fail("setgid");      /* CAP_SETGID */
    if (setuid(CHRONY_UID) != 0) fail("setuid");      /* CAP_SETUID */

    /* 5. restore CAP_SYS_TIME into effective, now that we are non-root. */
    if (retain_sys_time() != 0) fail("capset");

    /* 6. prove CAP_SYS_TIME is effective post-drop: a zero-offset clock set is
     *    cap-gated but harmless. adjtimex returns clock state (>=0) or -1. */
    struct timex tx;
    memset(&tx, 0, sizeof(tx));
    tx.modes = ADJ_OFFSET;
    tx.offset = 0;
    if (adjtimex(&tx) == -1 && errno == EPERM) fail("adjtimex");

    /* 7. sentinel written as uid 996 into the dir we now own -> proves we ran
     *    the whole chain to the end. The harness greps this out of serial. */
    int fd = open(RUNDIR "/ok", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) fail("sentinel-open");
    if (write(fd, "PRIVDROP_OK\n", 12) != 12) fail("sentinel-write");
    close(fd);

    for (;;) pause();   /* stay alive in the cgroup for /proc inspection */
    return 0;
}
