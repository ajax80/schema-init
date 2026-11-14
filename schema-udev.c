/* schema-udev — native uevent -> schema rule -> action daemon (Phase 1).
 * Listens on the kernel uevent netlink (group 1), alongside systemd-udevd. */
#include <sys/un.h>
#include <sys/file.h>
#include "schema-udev.h"
#include "udev_builtins.h"
#include "udev_rules.h"
#include "udev_db.h"
#include "udev_ruleset.h"
#include "disk_links.h"
#include "uaccess.h"
#include "udev_exec.h"
#include "udev_sched.h"
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
#include <time.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <sys/ioctl.h>

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
    struct msghdr msg = { .msg_name = &sa, .msg_namelen = sizeof sa, .msg_iov = &iov, .msg_iovlen = 1,
                          .msg_control = cbuf, .msg_controllen = sizeof cbuf };
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

/* Group-2 (UDEV_MONITOR_UDEV) broadcast socket. Live mode only: once udevd is
 * retired, schema-udev is the sole source of the processed-event stream that
 * libudev monitors (kwin/libinput/WirePlumber/NetworkManager) subscribe to. */
static int g_monfd = -1;

static int monitor_open(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_pid    = 0;   /* send-only; kernel assigns pid */
    sa.nl_groups = 0;   /* we broadcast, never subscribe */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    return fd;
}

static void monitor_broadcast(const struct uevent *ev) {
    if (g_monfd < 0) return;
    char frame[8192];
    ssize_t len = libudev_frame_build(ev, frame, sizeof frame);
    if (len < 0) return;
    struct sockaddr_nl dst;
    memset(&dst, 0, sizeof dst);
    dst.nl_family = AF_NETLINK;
    dst.nl_pid    = 0;
    dst.nl_groups = 2;   /* UDEV_MONITOR_UDEV */
    struct iovec iov = { frame, (size_t)len };
    struct msghdr msg = { .msg_name = &dst, .msg_namelen = sizeof dst, .msg_iov = &iov, .msg_iovlen = 1 };
    (void)sendmsg(g_monfd, &msg, 0);
}

/* True for an env key that steers the dynamic loader, libc, or the shell —
 * one that must never originate from a uevent property. A uevent can carry an
 * arbitrary KEY=VAL, so hook children can't just inherit every property: a
 * crafted LD_PRELOAD / GCONV_PATH / BASH_ENV / IFS would hijack the loader,
 * libc, or shell. Real udev property keys are plain uppercase device fields
 * (DEVPATH, SUBSYSTEM, ACTION, MODALIAS, ...) and never match here, so
 * filtering by this predicate drops only injection attempts. */
static int env_key_unsafe(const char *k) {
    static const char *const prefixes[] = {
        "LD_", "DYLD_", "GCONV_", "GLIBC_", "BASH_", "PYTHON", "PERL5",
        "RUBY", "NODE_", "GIT_", "PS" };
    static const char *const exact[] = {
        "IFS", "PATH", "ENV", "CDPATH", "SHELLOPTS", "BASHOPTS",
        "PROMPT_COMMAND", "GETCONF_DIR", "HOSTALIASES", "RESOLV_HOST_CONF",
        "LOCALDOMAIN", "NIS_PATH", "TMPDIR", "TZDIR" };
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++)
        if (strncmp(k, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    for (size_t i = 0; i < sizeof exact / sizeof *exact; i++)
        if (strcmp(k, exact[i]) == 0) return 1;
    return 0;
}

/* Export the uevent properties as environment, dropping any injection-carrier
 * key, then force a known-good PATH and IFS. The positive PATH/IFS reset is
 * belt-and-suspenders over the filter (also normalizes an odd inherited env). */
static void export_scrubbed_env(const struct uevent *ev) {
    for (int j = 0; j < ev->n; j++)
        if (!env_key_unsafe(ev->key[j]))
            setenv(ev->key[j], ev->val[j], 1);
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("IFS", " \t\n", 1);
    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");
    unsetenv("LD_AUDIT");
}

/* Execute RUN directives fire-and-forget (udev semantics: direct
 * argv exec, not a shell; output discarded; children reaped by the SIGCHLD
 * drain). RUN{builtin} kmod load invokes modprobe for MODALIAS or explicit modules;
 * other builtins (uaccess) are handled natively elsewhere. */
/* Coldplug runs events inline with fire-and-forget RUN, as before; a live
 * worker waits for each RUN (udevd parity), bounded per helper. */
enum dispatch_mode { DISPATCH_INLINE, DISPATCH_WORKER };

/* Workers run concurrently: the claims registry, uaccess state and legacy
 * .dev symlinks are shared between devices. -1 = proceed unlocked. */
#define UDEV_LINKS_LOCK   "/run/schema-udev-links.lock"
#define UDEV_UACCESS_LOCK SCHEMA_UDEV_READY_DIR "/uaccess.lock"
static int udev_lock(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    while (flock(fd, LOCK_EX) < 0)
        if (errno != EINTR) { close(fd); return -1; }
    return fd;
}
static void udev_unlock(int fd) { if (fd >= 0) close(fd); }

static void run_ctx_runs(const struct dev_ctx *rc, enum dispatch_mode mode) {
    for (int i = 0; i < rc->nruns; i++) {
        char store[32][UE_VAL_MAX]; char *argv[33];
        int argc = udev_argv_split(rc->runs[i], store, argv, 32);
        if (argc <= 0) continue;

        if (rc->run_builtin[i]) {
            if (strcmp(argv[0], "kmod") == 0 && argc >= 2 && strcmp(argv[1], "load") == 0) {
                pid_t pid = fork();
                if (pid < 0) continue;
                if (pid == 0) {
                    sigset_t empty; sigemptyset(&empty);
                    sigprocmask(SIG_SETMASK, &empty, NULL);
                    int devnull = open("/dev/null", O_RDWR);
                    if (devnull >= 0) {
                        dup2(devnull, STDIN_FILENO);
                        dup2(devnull, STDOUT_FILENO);
                        dup2(devnull, STDERR_FILENO);
                        if (devnull > STDERR_FILENO) close(devnull);
                    }
                    if (argc > 2) {
                        char *margv[37];
                        margv[0] = "modprobe";
                        margv[1] = "-a";
                        margv[2] = "-q";
                        margv[3] = "-s";
                        margv[4] = "--";
                        for (int k = 2; k < argc; k++) margv[k + 3] = argv[k];
                        margv[argc + 3] = NULL;
                        execvp("modprobe", margv);
                        execv("/sbin/modprobe", margv);
                        execv("/usr/sbin/modprobe", margv);
                        execv("/bin/kmod", margv);
                    } else {
                        const char *alias = uevent_get(rc->ev, "MODALIAS");
                        if (alias && alias[0] && alias[0] != '-') {
                            char *margv[] = { "modprobe", "-b", "-q", "-s", "--", (char *)alias, NULL };
                            execvp("modprobe", margv);
                            execv("/sbin/modprobe", margv);
                            execv("/usr/sbin/modprobe", margv);
                            execv("/bin/kmod", margv);
                        }
                    }
                    _exit(127);
                }
                if (mode == DISPATCH_WORKER) udev_wait_child(pid, UDEV_EXEC_TIMEOUT, rc->runs[i]);
            }
            continue;
        }

        const char *direct = udev_run_direct(argc, argv);
        if (direct) { argv[0] = (char *)direct; argv[1] = NULL; }
        pid_t pid = fork();
        if (pid < 0) continue;
        if (pid == 0) {
            sigset_t empty; sigemptyset(&empty);
            sigprocmask(SIG_SETMASK, &empty, NULL);
            export_scrubbed_env(rc->ev);
            if (!strchr(argv[0], '/')) {
                char lp[512];
                snprintf(lp, sizeof lp, "/usr/lib/udev/%s", argv[0]);
                execv(lp, argv);
            }
            execvp(argv[0], argv);
            _exit(127);
        }
        if (mode == DISPATCH_WORKER) udev_wait_child(pid, UDEV_EXEC_TIMEOUT, rc->runs[i]);
    }
}

/* Live-mode sentinel: read ONCE at startup (never re-read on HUP, so a running
 * daemon never changes ownership mid-flight). Absent => dry-run (isolated
 * namespaces); present => own the real /dev, /dev/disk, and real ACLs. */
#define SCHEMA_UDEV_LIVE_FLAG "/etc/schema-init/schema-udev.live"
static int g_live = 0;

static struct ruleset g_ruleset;
static const char *const RULE_DIRS[] = {
    "/usr/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d" };

static const char *g_test_rules;   /* SCHEMA_UDEV_TEST_RULES: the only rules dir */
static void ruleset_reload(void) {
    const char *const *dirs = RULE_DIRS;
    int nd = 3;
    if (g_test_rules) { dirs = &g_test_rules; nd = 1; }
    if (ruleset_reload_swap(&g_ruleset, dirs, nd) != 0) {
        fprintf(stderr, "[schema-udev] rules reload failed or came back empty — keeping the %d loaded rule(s); "
                "restart schema-udev to load an intentionally emptied set\n", g_ruleset.n);
        return;
    }
    ug_cache_clear();
    fprintf(stderr, "[schema-udev] loaded %d native rule(s)\n", g_ruleset.n);
}

static void rules_reload(void) {
    struct dev_rule tmp[MAX_RULES];
    int n = dev_rules_load_dir(DEV_DIR, tmp, MAX_RULES);
    /* an unreadable dir reads as 0: don't let a bad reload wipe live rules */
    if (n <= 0 && g_nrules > 0) {
        fprintf(stderr, "[schema-udev] %s reload came back empty — keeping %d rule(s); "
                "restart schema-udev to load an intentionally emptied set\n", DEV_DIR, g_nrules);
        return;
    }
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
        export_scrubbed_env(ev);                 /* ACTION included here */
        execl("/bin/sh", "sh", "-c", hook, (char *)NULL);
        _exit(127);
    }
}

static void net_rename(struct uevent *ev, struct uevent *orig, const char *newname) {
    const char *sub = uevent_get(ev, "SUBSYSTEM"), *old = uevent_get(ev, "INTERFACE");
    const char *dp = uevent_get(ev, "DEVPATH");
    if (!sub || strcmp(sub, "net") || !old || !dp || !strcmp(old, newname)) return;
    if (strlen(newname) >= IFNAMSIZ || strchr(newname, '/')) {
        fprintf(stderr, "[schema-udev] net: refusing name '%s' for %s\n", newname, old);
        return;
    }
    char oldname[IFNAMSIZ];
    safe_copy(oldname, old, sizeof oldname);
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    safe_copy(ifr.ifr_name, oldname, sizeof ifr.ifr_name);
    safe_copy(ifr.ifr_newname, newname, sizeof ifr.ifr_newname);
    int r = ioctl(fd, SIOCSIFNAME, &ifr);
    int e = errno;
    close(fd);
    if (r < 0) {
        fprintf(stderr, "[schema-udev] net: rename %s -> %s failed: %s\n", oldname, newname, strerror(e));
        return;
    }
    fprintf(stderr, "[schema-udev] net: renamed %s -> %s\n", oldname, newname);
    char ndp[UE_VAL_MAX];
    const char *slash = strrchr(dp, '/');
    if ((size_t)snprintf(ndp, sizeof ndp, "%.*s/%s", (int)(slash - dp), dp, newname) >= sizeof ndp) return;
    struct uevent *evs[2] = { ev, orig };
    for (int i = 0; i < 2; i++) {
        uevent_set(evs[i], "INTERFACE", newname);
        uevent_set(evs[i], "DEVPATH", ndp);
    }
}

static int g_feed;   /* SCHEMA_UDEV_TEST_FEED in use */
static void dispatch(struct uevent *ev, enum dispatch_mode mode) {
    const char *action = uevent_get(ev, "ACTION");
    if (!action) return;
    struct uevent shadow_ev;
    int bcast = 0;
    const char *devpath = uevent_get(ev, "DEVPATH");
    int kernel_n = ev->n;
    if (devpath) {
        const char *devname = uevent_get(ev, "DEVNAME");
        char devnode[UE_VAL_MAX];
        const char *dn = NULL;
        if (devname) { snprintf(devnode, sizeof devnode, "/dev/%s", devname); dn = devnode; }
        run_builtins("/sys", devpath, dn, ev);
        if (g_live && strcmp(action, "move") == 0) {
            char id[128], p[PATH_MAX];
            struct uevent old;
            if (udev_db_filename(ev, id, sizeof id) == 0 &&
                (size_t)snprintf(p, sizeof p, "%s/%s", UDEV_DB_DIR, id) < sizeof p &&
                udev_db_read_eprops(p, &old) == 0)
                for (int i = 0; i < old.n; i++)
                    if (!uevent_get(ev, old.key[i])) uevent_set(ev, old.key[i], old.val[i]);
        }
        shadow_ev = *ev;                        /* deep copy: uevent is inline char[], no heap */
        int pre_rules_n = shadow_ev.n;
        struct dev_ctx rc;
        int ua_tagged = 0;
        if (dev_ctx_init(&rc, &shadow_ev, "/sys") == 0) {
            ruleset_apply(&g_ruleset, &rc);
            if (g_live && strcmp(action, "add") == 0 && rc.name[0]) {
                net_rename(&shadow_ev, ev, rc.name);
                devpath = uevent_get(ev, "DEVPATH");
            }
            for (int i = 0; i < rc.ntags; i++)
                if (strcmp(rc.tags[i], "uaccess") == 0) { ua_tagged = 1; break; }
            /* Live mode is the sole device-DB authority: write the full record
             * to /run/udev/data so libudev/sd-device consumers AND our own
             * IMPORT{parent}/{db} (dbroot defaults there) read our output.
             * Dry-run stays shadow-only; import there still borrows udevd. */
            (void)pre_rules_n;
            const char *db_authority = g_live ? UDEV_DB_DIR : SCHEMA_UDEV_RULES_DIR;
            const char *syms[DEVCTX_SYMLINKS_MAX];
            int nsyms = 0;
            if (strcmp(action, "remove") == 0) {
                udev_db_remove(db_authority, &shadow_ev);
            } else {
                const char *tgs[DEVCTX_TAGS_MAX];
                for (int i = 0; i < rc.nsym;  i++) syms[i] = rc.symlinks[i];
                for (int i = 0; i < rc.ntags; i++) tgs[i]  = rc.tags[i];
                nsyms = rc.nsym;
                udev_db_write_full(db_authority, &shadow_ev, kernel_n,
                                   syms, rc.nsym, tgs, rc.ntags);
            }
            /* Live mode owns real /dev: build the /dev/{char,block} farm, apply
             * node ownership/mode, run RUN{program}, then announce the processed
             * event on the udev monitor group so live consumers see hotplug. */
            if (g_live) {
                node_symlink_farm(&shadow_ev, action);
                char devid[128];
                if (udev_db_filename(&shadow_ev, devid, sizeof devid) == 0) {
                    int lk = udev_lock(UDEV_LINKS_LOCK);
                    rule_links_update("/dev", SCHEMA_UDEV_LINK_CLAIMS, devid,
                                      uevent_get(&shadow_ev, "DEVNAME"),
                                      rc.link_priority, syms, nsyms);
                    udev_unlock(lk);
                }
                if (strcmp(action, "remove") != 0) {
                    if (dn) node_apply_perms(dn, rc.owner, rc.group, rc.mode);
                    run_ctx_runs(&rc, mode);
                }
                /* announced last, once ACLs and links are in place (udevd order) */
                bcast = 1;
            } else if (g_feed && strcmp(action, "remove") != 0) {
                run_ctx_runs(&rc, mode);   /* test feed: dry-run, but RUN still exercised */
            }
        }
        run_rules("/sys", devpath, dn, ev);
        const char *sub = uevent_get(ev, "SUBSYSTEM");
        int is_block = sub && strcmp(sub, "block") == 0;
        /* disk_links' fixed trees only feed the dry-run shadow now. Live, the
         * rules' SYMLINK+= set (a superset: by-id, by-designator too) owns
         * /dev/disk through the claims registry; writing both would let the
         * last writer override the registry's pick for a shared name. */
        const char *disk_base = SCHEMA_DISK_DIR;
        int shadow_disk = is_block && !g_live;
        if (strcmp(action, "remove") == 0) {
            if (shadow_disk) disk_links_gc(disk_base, SCHEMA_UDEV_DB_DIR, ev);
            int ulk = udev_lock(UDEV_UACCESS_LOCK);
            uaccess_clear(SCHEMA_UACCESS_DIR, ev);
            if (g_live) {
                int uid = uaccess_active_uid(SEAT0_PATH);
                const char *rn = uevent_get(ev, "DEVNAME");
                if (uid >= 0 && rn && rn[0] && ua_tagged) {
                    char node[UE_VAL_MAX + 8];
                    if ((size_t)snprintf(node, sizeof node, "/dev/%s", rn) < sizeof node)
                        ua_clear_node(node, uid);
                }
            }
            udev_unlock(ulk);
            udev_db_remove(SCHEMA_UDEV_DB_DIR, ev);
        } else {
            udev_db_write(SCHEMA_UDEV_DB_DIR, ev, kernel_n);
            if (shadow_disk && (strcmp(action, "add") == 0 || strcmp(action, "change") == 0))
                disk_links_apply(disk_base, ev);
            if (strcmp(action, "add") == 0 || strcmp(action, "change") == 0) {
                int ulk = udev_lock(UDEV_UACCESS_LOCK);
                uaccess_record(SCHEMA_UACCESS_DIR, SEAT0_PATH, ev, ua_tagged);   /* audit trail */
                if (g_live) uaccess_apply(SEAT0_PATH, ev, ua_tagged);            /* real ACL */
                udev_unlock(ulk);
            }
        }
    }
    const char *dev_base = g_live ? "/dev" : SCHEMA_DEV_DIR;
    for (int i = 0; i < g_nrules; i++) {
        if (!dev_rule_match(&g_rules[i], ev)) continue;

        if (strcmp(action, "add") == 0 && g_rules[i].symlink[0]) {
            const char *dn = uevent_get(ev, "DEVNAME");
            if (dn) {
                int lk = udev_lock(UDEV_LINKS_LOCK);
                int ok = symlink_apply(dev_base, g_rules[i].symlink, dn) == 0;
                udev_unlock(lk);
                if (ok) {
                    fprintf(stderr, "[schema-udev] created symlink %s/%s -> %s\n",
                            dev_base, g_rules[i].symlink, dn);
                }
            }
        } else if (strcmp(action, "remove") == 0 && g_rules[i].symlink[0]) {
            int lk = udev_lock(UDEV_LINKS_LOCK);
            symlink_clear(dev_base, g_rules[i].symlink);
            udev_unlock(lk);
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
    if (bcast) monitor_broadcast(&shadow_ev);
}

static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

#define SUBSYS_TRIGGERED SCHEMA_UDEV_READY_DIR "/subsystems-triggered"
#define SUBSYS_DRAIN_MS  5000

static unsigned long long kernel_seqnum(void) {
    unsigned long long v = 0;
    FILE *f = fopen("/sys/kernel/uevent_seqnum", "re");
    if (f) { if (fscanf(f, "%llu", &v) != 1) v = 0; fclose(f); }
    return v;
}

/* Subsystems before devices, as systemd-udev-trigger does: device rules can
 * depend on what module adds set (module parameters, nodes from RUN+=). The
 * kernel queues the triggered events on nlfd; handle them all before the
 * device walk so "ready" means they ran. Once per boot: a live restart must
 * not replay ~1000 module/driver adds to every listener. */
static void subsystems_coldplug(int nlfd) {
    if (access(SUBSYS_TRIGGERED, F_OK) == 0) {
        fprintf(stderr, "[schema-udev] subsystem uevents already replayed this boot\n");
        return;
    }
    double t0 = mono_ms();
    int n = coldplug_trigger_subsystems("/sys");
    unsigned long long target = kernel_seqnum(), seen = 0;
    int handled = 0;
    while (n > 0 && seen < target) {
        int left = SUBSYS_DRAIN_MS - (int)(mono_ms() - t0);
        if (left <= 0) break;
        struct pollfd p = { nlfd, POLLIN, 0 };
        if (poll(&p, 1, left) <= 0) { if (errno == EINTR) continue; break; }
        for (;;) {
            char buf[8192];
            ssize_t r = netlink_recv(nlfd, buf, sizeof buf);
            if (r < 0) {
                if (errno == EINTR || errno == 0 || errno == ENOBUFS) continue;
                break;
            }
            struct uevent ev;
            if (uevent_parse(buf, (size_t)r, &ev) != 0) continue;
            const char *sq = uevent_get(&ev, "SEQNUM");
            if (sq) { unsigned long long v = strtoull(sq, NULL, 10); if (v > seen) seen = v; }
            dispatch(&ev, DISPATCH_INLINE);
            handled++;
        }
    }
    fprintf(stderr, "[schema-udev] subsystems: triggered %d, handled %d events in %.0fms%s\n",
            n, handled, mono_ms() - t0, seen < target && n > 0 ? " (TIMED OUT before the last)" : "");
    mkdir(SCHEMA_UDEV_READY_DIR, 0755);
    int fd = open(SUBSYS_TRIGGERED, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
}

static void coldplug_dispatch(struct uevent *ev) {
    double t0 = mono_ms();
    dispatch(ev, DISPATCH_INLINE);
    double dt = mono_ms() - t0;
    if (dt >= 50.0)
        fprintf(stderr, "[schema-udev] coldplug slow %.0fms %s\n", dt, uevent_get(ev, "DEVPATH"));
}

/* Live events run in forked workers (audit 3.3): the loop only queues
 * datagrams, starts a worker when the event conflicts with nothing running
 * or queued ahead of it (udev_sched.h), and kills a worker past its deadline,
 * so one stuck device never holds up the others. */
#define WQ_MAX     4096
#define WQ_WINDOW  256    /* scheduling scans this many queued events at most */
#define WORKERS_MAX 16

static struct sched_meta g_qm[WQ_MAX];
static char *g_qraw[WQ_MAX];
static size_t g_qlen[WQ_MAX];
static double g_qenq[WQ_MAX];
static int g_qhead, g_qtail;

static struct sched_meta g_rm[WORKERS_MAX];
static struct { pid_t pid; double enq_ms, deadline_ms; } g_run[WORKERS_MAX];
static int g_nrun, g_cap = 4;

static long long g_event_timeout_ms = 180000;
static long long g_slow_ms = 1000;
static int g_nlfd = -1, g_sfd = -1;
static unsigned long g_qdropped;
static double g_qdrop_logged;

static void wq_push(const char *buf, size_t len) {
    if (g_qtail == WQ_MAX && g_qhead > 0) {
        int n = g_qtail - g_qhead;
        memmove(g_qm, g_qm + g_qhead, n * sizeof g_qm[0]);
        memmove(g_qraw, g_qraw + g_qhead, n * sizeof g_qraw[0]);
        memmove(g_qlen, g_qlen + g_qhead, n * sizeof g_qlen[0]);
        memmove(g_qenq, g_qenq + g_qhead, n * sizeof g_qenq[0]);
        g_qhead = 0; g_qtail = n;
    }
    double now = mono_ms();
    char *raw = g_qtail < WQ_MAX ? malloc(len) : NULL;
    if (!raw) {
        g_qdropped++;
        if (now - g_qdrop_logged >= 1000) {
            fprintf(stderr, "[schema-udev] event queue full (%d), dropping events (%lu dropped)\n", WQ_MAX, g_qdropped);
            g_qdrop_logged = now;
            g_qdropped = 0;
        }
        return;
    }
    struct uevent ev;
    if (uevent_parse(buf, len, &ev) != 0) { free(raw); return; }
    memcpy(raw, buf, len);
    sched_meta_from(&ev, &g_qm[g_qtail]);
    g_qraw[g_qtail] = raw;
    g_qlen[g_qtail] = len;
    g_qenq[g_qtail] = now;
    g_qtail++;
}

/* drop queued entry head+i, keeping the order of the rest */
static void wq_take(int i) {
    int at = g_qhead + i;
    memmove(g_qm + g_qhead + 1, g_qm + g_qhead, i * sizeof g_qm[0]);
    memmove(g_qraw + g_qhead + 1, g_qraw + g_qhead, i * sizeof g_qraw[0]);
    memmove(g_qlen + g_qhead + 1, g_qlen + g_qhead, i * sizeof g_qlen[0]);
    memmove(g_qenq + g_qhead + 1, g_qenq + g_qhead, i * sizeof g_qenq[0]);
    (void)at;
    g_qhead++;
    if (g_qhead == g_qtail) g_qhead = g_qtail = 0;
}

static void worker_child(const char *raw, size_t len) {
    sigset_t empty;
    sigemptyset(&empty);
    signal(SIGHUP, SIG_DFL); signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL); signal(SIGCHLD, SIG_DFL);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    close(g_nlfd); close(g_sfd);
    struct uevent ev;
    if (uevent_parse(raw, len, &ev) == 0) dispatch(&ev, DISPATCH_WORKER);
    _exit(0);
}

static void run_remove(int k) {
    g_nrun--;
    if (k != g_nrun) { g_rm[k] = g_rm[g_nrun]; g_run[k] = g_run[g_nrun]; }
}

/* returns 1 when something could not start for a transient reason */
static int workers_start(void) {
    while (g_nrun < g_cap && g_qtail > g_qhead) {
        int nq = g_qtail - g_qhead;
        if (nq > WQ_WINDOW) nq = WQ_WINDOW;
        int i = sched_pick(g_qm + g_qhead, nq, g_rm, g_nrun);
        if (i < 0) return 0;
        int at = g_qhead + i;
        fflush(stderr);
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "[schema-udev] worker fork: %s\n", strerror(errno));
            return 1;
        }
        if (pid == 0) worker_child(g_qraw[at], g_qlen[at]);
        g_rm[g_nrun] = g_qm[at];
        g_run[g_nrun].pid = pid;
        g_run[g_nrun].enq_ms = g_qenq[at];
        g_run[g_nrun].deadline_ms = mono_ms() + (double)g_event_timeout_ms;
        g_nrun++;
        free(g_qraw[at]);
        wq_take(i);
    }
    return 0;
}

/* Unknown pids (coldplug's fire-and-forget RUN children, hooks, a worker
 * already given up on) are reaped silently. */
static void workers_reap(void) {
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        for (int k = 0; k < g_nrun; k++) {
            if (g_run[k].pid != pid) continue;
            double dt = mono_ms() - g_run[k].enq_ms;
            if (WIFSIGNALED(st))
                fprintf(stderr, "[schema-udev] worker for %s %s died on signal %d\n",
                        g_rm[k].action, g_rm[k].devpath, WTERMSIG(st));
            else if (WEXITSTATUS(st) != 0)
                fprintf(stderr, "[schema-udev] worker for %s %s exited %d\n",
                        g_rm[k].action, g_rm[k].devpath, WEXITSTATUS(st));
            if (dt >= (double)g_slow_ms)
                fprintf(stderr, "[schema-udev] slow %.0fms %s %s\n", dt, g_rm[k].action, g_rm[k].devpath);
            run_remove(k);
            break;
        }
    }
}

/* Never waits on the killed worker: one stuck in D state would freeze the
 * loop. Its slot is free at once; a pending SIGKILL lands before it can
 * return to user space, so it writes nothing more. */
static void workers_expire(void) {
    double now = mono_ms();
    for (int k = 0; k < g_nrun; k++) {
        if (now < g_run[k].deadline_ms) continue;
        kill(g_run[k].pid, SIGKILL);
        fprintf(stderr, "[schema-udev] event TIMED OUT after %llds %s %s\n",
                g_event_timeout_ms / 1000, g_rm[k].action, g_rm[k].devpath);
        run_remove(k);
        k--;
    }
}

static int poll_timeout(int retry) {
    if (!g_nrun) return retry ? 100 : -1;
    double next = g_run[0].deadline_ms;
    for (int k = 1; k < g_nrun; k++) if (g_run[k].deadline_ms < next) next = g_run[k].deadline_ms;
    double left = next - mono_ms();
    int ms = left <= 0 ? 0 : (int)left + 1;
    return retry && ms > 100 ? 100 : ms;
}

static void workers_shutdown(void) {
    for (int k = 0; k < g_nrun; k++) kill(g_run[k].pid, SIGTERM);
    for (int t = 0; t < 100 && g_nrun; t++) {
        struct timespec ts = { 0, 50 * 1000000L };
        nanosleep(&ts, NULL);
        workers_reap();
    }
    for (int k = 0; k < g_nrun; k++) kill(g_run[k].pid, SIGKILL);
}

static long long env_ll(const char *name, long long dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    char *end;
    long long n = strtoll(v, &end, 10);
    return *end || n < 0 ? dflt : n;
}

/* SCHEMA_UDEV_TEST_FEED: a unix datagram socket read in place of netlink,
 * for tests/test_udev_workers.py. Never as root: a test run must not touch
 * the real /dev or a live daemon's state. */
static int test_feed_open(const char *path) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) return -1;
    strcpy(sa.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    return fd;
}

int main(void) {
    const char *feed = getenv("SCHEMA_UDEV_TEST_FEED");
    g_test_rules = getenv("SCHEMA_UDEV_TEST_RULES");
    if ((feed || g_test_rules) && geteuid() == 0) {
        fprintf(stderr, "[schema-udev] SCHEMA_UDEV_TEST_* refused as root\n");
        return 1;
    }
    g_event_timeout_ms = env_ll("SCHEMA_UDEV_EVENT_TIMEOUT_MS", 180000);
    g_slow_ms = env_ll("SCHEMA_UDEV_SLOW_MS", 1000);
    long np = sysconf(_SC_NPROCESSORS_ONLN);
    g_cap = sched_cap(np > 0 ? np : 1);

    int nlfd;
    if (feed) {
        g_feed = 1;
        nlfd = test_feed_open(feed);
        if (nlfd < 0) {
            fprintf(stderr, "[schema-udev] test feed %s: %s\n", feed, strerror(errno));
            return 1;
        }
    } else {
        nlfd = netlink_open();
        if (nlfd < 0) {
            fprintf(stderr, "[schema-udev] netlink open/bind failed: %s\n", strerror(errno));
            return 1;
        }
    }
    g_nlfd = nlfd;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);  sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);  sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sfd < 0) { fprintf(stderr, "[schema-udev] signalfd: %s\n", strerror(errno)); return 1; }
    g_sfd = sfd;

    errno = 0;
    int live_rc = access(SCHEMA_UDEV_LIVE_FLAG, F_OK);
    int live_errno = errno;
    g_live = (live_rc == 0) && !g_feed;
    {
        struct stat lst;
        int st_rc = lstat(SCHEMA_UDEV_LIVE_FLAG, &lst);
        fprintf(stderr,
                "[schema-udev] flag-check: path=%s access_rc=%d errno=%d(%s) "
                "euid=%u uid=%u lstat_rc=%d%s\n",
                SCHEMA_UDEV_LIVE_FLAG, live_rc, live_errno, strerror(live_errno),
                (unsigned)geteuid(), (unsigned)getuid(), st_rc,
                st_rc == 0 ? "" : " <<FLAG ABSENT AT LAUNCH>>");
    }
    fprintf(stderr, "[schema-udev] mode=%s (%s)\n",
            g_live ? "LIVE" : "dry-run",
            g_live ? "owns real /dev, /dev/disk, ACLs" : "isolated namespaces");

    if (g_live) {
        g_monfd = monitor_open();
        if (g_monfd < 0)
            fprintf(stderr, "[schema-udev] monitor socket open failed: %s\n", strerror(errno));
        else
            fprintf(stderr, "[schema-udev] udev monitor broadcast (group 2) active\n");
    }

    if (g_live) unlink(SCHEMA_UDEV_READY);
    double rl_t0 = mono_ms();
    rules_reload();
    ruleset_reload();
    double rl_t1 = mono_ms();
    disk_links_wipe(SCHEMA_UDEV_RULES_DIR);   /* reuse the generic recursive rmdir/wipe */
    disk_links_wipe(SCHEMA_DISK_DIR);
    uaccess_wipe(SCHEMA_UACCESS_DIR);
    fprintf(stderr, "[schema-udev] rules load %.0fms, wipe %.0fms\n", rl_t1 - rl_t0, mono_ms() - rl_t1);
    if (g_feed) {
        fprintf(stderr, "[schema-udev] test feed on %s (no coldplug)\n", feed);
    } else {
        if (g_live) subsystems_coldplug(nlfd);
        fprintf(stderr, "[schema-udev] running coldplug sysfs walk...\n");
        double cp_t0 = mono_ms();
        coldplug_walk_root("/sys", coldplug_dispatch);
        fprintf(stderr, "[schema-udev] coldplug took %.0fms\n", mono_ms() - cp_t0);
    }
    if (g_live && udev_signal_ready() == 0)
        fprintf(stderr, "[schema-udev] ready marker %s written (coldplug complete)\n", SCHEMA_UDEV_READY);
    fprintf(stderr, "[schema-udev] listening on kernel uevent netlink (group 1)\n");

    fprintf(stderr, "[schema-udev] hotplug workers: up to %d, event deadline %llds\n",
            g_cap, g_event_timeout_ms / 1000);
    struct pollfd pfd[2] = { { nlfd, POLLIN, 0 }, { sfd, POLLIN, 0 } };
    int retry = 0, quit = 0;
    while (!quit) {
        if (poll(pfd, 2, poll_timeout(retry)) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if ((pfd[0].revents | pfd[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "[schema-udev] poll error on fd, exiting\n");
            break;
        }

        if (pfd[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
                if (si.ssi_signo == SIGCHLD) {
                    workers_reap();
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
                ssize_t n = g_feed ? recv(nlfd, buf, sizeof buf, MSG_DONTWAIT)
                                   : netlink_recv(nlfd, buf, sizeof buf);
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
                wq_push(buf, (size_t)n);
            }
        }

        workers_reap();
        workers_expire();
        retry = workers_start();
    }
    workers_shutdown();
    fprintf(stderr, "[schema-udev] shutting down\n");
    close(nlfd); close(sfd);
    return 0;
}
