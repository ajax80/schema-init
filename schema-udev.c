/* schema-udev — native uevent -> schema rule -> action daemon (Phase 1).
 * Listens on the kernel uevent netlink (group 1), alongside systemd-udevd. */
#include "schema-udev.h"
#include "udev_builtins.h"
#include "udev_rules.h"
#include "udev_db.h"
#include "udev_ruleset.h"
#include "disk_links.h"
#include "uaccess.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <linux/netlink.h>

#define DEV_DIR "/etc/schema-init/dev"

static int netlink_open(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof one);
    int rcv = 16 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcv, sizeof rcv);
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;   /* kernel uevents only; NEVER group 2 (udev/libudev) */
    sa.nl_pid    = 0;   /* kernel auto-assigns a unique pid */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    return fd;
}

/* Receive one datagram, verifying it came from the kernel.
 * Returns length (>=0) on a verified event; -1 to skip (spoofed / would-block /
 * ENOBUFS — check errno: EAGAIN/EWOULDBLOCK means drain complete). */
static ssize_t netlink_recv(int fd, char *buf, size_t bufsz) {
    struct iovec iov = { buf, bufsz };
    struct sockaddr_nl sa;
    char cbuf[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr msg = { &sa, sizeof sa, &iov, 1, cbuf, sizeof cbuf, 0 };
    ssize_t n = recvmsg(fd, &msg, 0);
    if (n < 0) return -1;                 /* errno set by recvmsg */
    if (sa.nl_pid != 0) { errno = 0; return -1; }   /* not from kernel */
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    if (!cm || cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_CREDENTIALS ||
        cm->cmsg_len < CMSG_LEN(sizeof(struct ucred))) { errno = 0; return -1; }
    struct ucred *uc = (struct ucred *)CMSG_DATA(cm);
    if (uc->uid != 0) { errno = 0; return -1; }     /* not root/kernel */
    return n;
}

static struct dev_rule g_rules[MAX_RULES];
static int g_nrules = 0;

/* Live-mode sentinel: read ONCE at startup (never re-read on HUP, so a running
 * daemon never changes ownership mid-flight). Absent => dry-run (isolated
 * namespaces); present => own the real /dev, /dev/disk, and real ACLs. */
#define SCHEMA_UDEV_LIVE_FLAG "/etc/schema-init/schema-udev.live"
static int g_live = 0;

static struct ruleset g_ruleset;
static const char *const RULE_DIRS[] = {
    "/usr/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d" };

static void ruleset_reload(void) {
    free(g_ruleset.rules);
    memset(&g_ruleset, 0, sizeof g_ruleset);
    ruleset_load_dirs(RULE_DIRS, 3, &g_ruleset);
    fprintf(stderr, "[schema-udev] loaded %d native rule(s)\n", g_ruleset.n);
}

static void rules_reload(void) {
    struct dev_rule tmp[MAX_RULES];
    int n = dev_rules_load_dir(DEV_DIR, tmp, MAX_RULES);
    memcpy(g_rules, tmp, sizeof(struct dev_rule) * (n < 0 ? 0 : n));
    g_nrules = n < 0 ? 0 : n;
    fprintf(stderr, "[schema-udev] loaded %d rule(s) from %s\n", g_nrules, DEV_DIR);
}

/* Fork a hook, exporting the full uevent (incl. ACTION) as environment.
 * Parent does not block; children are reaped by the SIGCHLD drain in main(). */
static void run_hook(const char *hook, const struct uevent *ev) {
    pid_t pid = fork();
    if (pid < 0) { fprintf(stderr, "[schema-udev] fork: %s\n", strerror(errno)); return; }
    if (pid == 0) {
        sigset_t empty; sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        for (int j = 0; j < ev->n; j++)
            setenv(ev->key[j], ev->val[j], 1);   /* ACTION included here */
        execl("/bin/sh", "sh", "-c", hook, (char *)NULL);
        _exit(127);
    }
}

static void dispatch(struct uevent *ev) {
    const char *action = uevent_get(ev, "ACTION");
    if (!action) return;
    const char *devpath = uevent_get(ev, "DEVPATH");
    int kernel_n = ev->n;
    if (devpath) {
        const char *devname = uevent_get(ev, "DEVNAME");
        char devnode[UE_VAL_MAX];
        const char *dn = NULL;
        if (devname) { snprintf(devnode, sizeof devnode, "/dev/%s", devname); dn = devnode; }
        run_builtins("/sys", devpath, dn, ev);
        struct uevent shadow_ev = *ev;          /* deep copy: uevent is inline char[], no heap */
        int pre_rules_n = shadow_ev.n;
        struct dev_ctx rc;
        if (dev_ctx_init(&rc, &shadow_ev, "/sys") == 0) {
            ruleset_apply(&g_ruleset, &rc);
            if (strcmp(action, "remove") == 0) {
                udev_db_remove(SCHEMA_UDEV_RULES_DIR, &shadow_ev);
            } else if (rc.nsym > 0 || rc.ntags > 0 || shadow_ev.n > pre_rules_n) {
                const char *syms[DEVCTX_SYMLINKS_MAX];
                const char *tgs[DEVCTX_TAGS_MAX];
                for (int i = 0; i < rc.nsym;  i++) syms[i] = rc.symlinks[i];
                for (int i = 0; i < rc.ntags; i++) tgs[i]  = rc.tags[i];
                udev_db_write_full(SCHEMA_UDEV_RULES_DIR, &shadow_ev, kernel_n,
                                   syms, rc.nsym, tgs, rc.ntags);
            }
        }
        run_rules("/sys", devpath, dn, ev);
        const char *sub = uevent_get(ev, "SUBSYSTEM");
        int is_block = sub && strcmp(sub, "block") == 0;
        /* Live mode owns the real trees; dry-run stays isolated. The udev db is
         * always shadow-only for now (real /run/udev/data write is deferred to
         * E3: its record format needs S:/G:/Q:/V:/I: lines, not just E:). */
        const char *disk_base = g_live ? "/dev/disk" : SCHEMA_DISK_DIR;
        if (strcmp(action, "remove") == 0) {
            if (is_block) disk_links_gc(disk_base, SCHEMA_UDEV_DB_DIR, ev);
            uaccess_clear(SCHEMA_UACCESS_DIR, ev);
            if (g_live) {
                int uid = uaccess_active_uid(SEAT0_PATH);
                const char *rn = uevent_get(ev, "DEVNAME");
                if (uid >= 0 && rn && rn[0] && uaccess_eligible(ev)) {
                    char node[UE_VAL_MAX + 8];
                    if ((size_t)snprintf(node, sizeof node, "/dev/%s", rn) < sizeof node)
                        ua_clear_node(node, uid);
                }
            }
            udev_db_remove(SCHEMA_UDEV_DB_DIR, ev);
        } else {
            udev_db_write(SCHEMA_UDEV_DB_DIR, ev, kernel_n);
            if (is_block && (strcmp(action, "add") == 0 || strcmp(action, "change") == 0))
                disk_links_apply(disk_base, ev);
            if (strcmp(action, "add") == 0 || strcmp(action, "change") == 0) {
                uaccess_record(SCHEMA_UACCESS_DIR, SEAT0_PATH, ev);   /* audit trail */
                if (g_live) uaccess_apply(SEAT0_PATH, ev);            /* real ACL */
            }
        }
    }
    const char *dev_base = g_live ? "/dev" : SCHEMA_DEV_DIR;
    for (int i = 0; i < g_nrules; i++) {
        if (!dev_rule_match(&g_rules[i], ev)) continue;

        if (strcmp(action, "add") == 0 && g_rules[i].symlink[0]) {
            const char *dn = uevent_get(ev, "DEVNAME");
            if (dn) {
                if (symlink_apply(dev_base, g_rules[i].symlink, dn) == 0) {
                    fprintf(stderr, "[schema-udev] created symlink %s/%s -> %s\n",
                            dev_base, g_rules[i].symlink, dn);
                }
            }
        } else if (strcmp(action, "remove") == 0 && g_rules[i].symlink[0]) {
            symlink_clear(dev_base, g_rules[i].symlink);
            fprintf(stderr, "[schema-udev] removed symlink %s/%s\n",
                    dev_base, g_rules[i].symlink);
        }

        const char *hook = NULL;
        if (strcmp(action, "add") == 0 && g_rules[i].on_add[0])    hook = g_rules[i].on_add;
        else if (strcmp(action, "remove") == 0 && g_rules[i].on_remove[0]) hook = g_rules[i].on_remove;
        if (hook) {
            fprintf(stderr, "[schema-udev] matched %s %s %s -> %s\n",
                    g_rules[i].name, action,
                    uevent_get(ev, "DEVNAME") ? uevent_get(ev, "DEVNAME") : uevent_get(ev, "DEVPATH"),
                    hook);
            run_hook(hook, ev);
        }
    }
}

int main(void) {
    int nlfd = netlink_open();
    if (nlfd < 0) {
        fprintf(stderr, "[schema-udev] netlink open/bind failed: %s\n", strerror(errno));
        return 1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);  sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);  sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sfd < 0) { fprintf(stderr, "[schema-udev] signalfd: %s\n", strerror(errno)); return 1; }

    g_live = (access(SCHEMA_UDEV_LIVE_FLAG, F_OK) == 0);
    fprintf(stderr, "[schema-udev] mode=%s (%s)\n",
            g_live ? "LIVE" : "dry-run",
            g_live ? "owns real /dev, /dev/disk, ACLs" : "isolated namespaces");

    rules_reload();
    ruleset_reload();
    disk_links_wipe(SCHEMA_UDEV_RULES_DIR);   /* reuse the generic recursive rmdir/wipe */
    disk_links_wipe(SCHEMA_DISK_DIR);
    uaccess_wipe(SCHEMA_UACCESS_DIR);
    fprintf(stderr, "[schema-udev] running coldplug sysfs walk...\n");
    coldplug_walk_root("/sys", dispatch);
    fprintf(stderr, "[schema-udev] listening on kernel uevent netlink (group 1)\n");

    struct pollfd pfd[2] = { { nlfd, POLLIN, 0 }, { sfd, POLLIN, 0 } };
    for (;;) {
        if (poll(pfd, 2, -1) < 0) { if (errno == EINTR) continue; break; }

        if ((pfd[0].revents | pfd[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "[schema-udev] poll error on fd, exiting\n");
            break;
        }

        if (pfd[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            int quit = 0;
            while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
                if (si.ssi_signo == SIGCHLD) {
                    while (waitpid(-1, NULL, WNOHANG) > 0) ;   /* drain all */
                } else if (si.ssi_signo == SIGHUP) {
                    rules_reload();
                    ruleset_reload();
                } else {
                    quit = 1;   /* TERM / INT */
                }
            }
            if (quit) break;
        }

        if (pfd[0].revents & POLLIN) {
            for (;;) {
                char buf[8192];
                ssize_t n = netlink_recv(nlfd, buf, sizeof buf);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == ENOBUFS) {
                        fprintf(stderr, "[schema-udev] kernel dropped events (ENOBUFS)\n");
                        continue;
                    }
                    if (errno == EINTR) continue;
                    if (errno == 0) continue;   /* spoofed datagram skipped */
                    break;
                }
                struct uevent ev;
                if (uevent_parse(buf, (size_t)n, &ev) != 0) continue;
                dispatch(&ev);
            }
        }
    }
    fprintf(stderr, "[schema-udev] shutting down\n");
    close(nlfd); close(sfd);
    return 0;
}
