#include "../ns.h"
#include "../landlock.h"
#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <ifaddrs.h>
#include <sched.h>
#include <signal.h>
#include <fcntl.h>
#include <linux/capability.h>

/* the bounding set service_apply_hardening leaves, read in a forked child
 * (a user namespace when not root); -1 when no userns is available */
/* a user namespace mapped to root when not root; -1 when none is allowed */
static int become_root(void) {
    char m[32];
    int fd;
    uid_t u = getuid();
    gid_t g = getgid();
    if (geteuid() == 0) return 0;
    if (unshare(CLONE_NEWUSER) != 0) return -1;
    if ((fd = open("/proc/self/setgroups", O_WRONLY)) >= 0) { (void)!write(fd, "deny", 4); close(fd); }
    snprintf(m, sizeof m, "0 %u 1", (unsigned)u);
    if ((fd = open("/proc/self/uid_map", O_WRONLY)) < 0 || write(fd, m, strlen(m)) < 0) return -1;
    close(fd);
    snprintf(m, sizeof m, "0 %u 1", (unsigned)g);
    if ((fd = open("/proc/self/gid_map", O_WRONLY)) < 0 || write(fd, m, strlen(m)) < 0) return -1;
    close(fd);
    return 0;
}

static int mdwe_ok(void) {
    int st;
    pid_t pid = fork();
    if (pid == 0) _exit(apply_mdwe() == 0 ? 0 : 1);
    return waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* a stop runner of a private_network service runs in the main process's
 * netns (or a fresh one once it's gone), never the host's. Returns 1 ok,
 * 0 skipped (no userns), asserts otherwise */
static int runner_netns_check(const char *dir) {
    int st;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        char p[300], out[300], body[1024], host[64], got[64] = "", mainns[64] = "";
        static service_t s;
        ssize_t n;
        if (become_root() != 0) _exit(99);
        n = readlink("/proc/self/ns/net", host, sizeof host - 1);
        if (n < 0) _exit(2);
        host[n] = 0;
        snprintf(p, sizeof p, "%s/net.svc", dir);
        snprintf(out, sizeof out, "%s/net.out", dir);
        snprintf(body, sizeof body, "name=net\nexec=/bin/sleep\nargs=30\nprivate_network=1\n"
                 "exec_stop=/bin/sh -c \"readlink /proc/self/ns/net > %s\"\n", out);
        FILE *f = fopen(p, "w");
        fputs(body, f);
        fclose(f);
        if (service_load_one(p, &s) != 0 || service_spawn(&s) != 0) _exit(3);
        for (int i = 0; i < 200 && !mainns[0]; i++) {
            char l[64];
            snprintf(l, sizeof l, "/proc/%d/ns/net", (int)s.child_pid);
            n = readlink(l, mainns, sizeof mainns - 1);
            if (n > 0) { mainns[n] = 0; if (!strcmp(mainns, host)) mainns[0] = 0; }
            if (!mainns[0]) usleep(10000);
        }
        if (!mainns[0]) _exit(4);
        pid_t r = service_stop_spawn(&s, 1);
        if (r <= 0 || waitpid(r, &st, 0) != r || !WIFEXITED(st) || WEXITSTATUS(st)) _exit(5);
        f = fopen(out, "r");
        if (!f || !fgets(got, sizeof got, f)) _exit(6);
        fclose(f);
        got[strcspn(got, "\n")] = 0;
        if (strcmp(got, mainns)) _exit(7);
        kill(s.child_pid, SIGKILL);
        waitpid(s.child_pid, &st, 0);
        s.child_pid = 0;
        /* main gone: a fresh netns, still not the host's */
        r = service_stop_spawn(&s, 1);
        if (r <= 0 || waitpid(r, &st, 0) != r || !WIFEXITED(st) || WEXITSTATUS(st)) _exit(8);
        f = fopen(out, "r");
        if (!f || !fgets(got, sizeof got, f)) _exit(9);
        fclose(f);
        got[strcspn(got, "\n")] = 0;
        if (!strcmp(got, host)) _exit(10);
        unlink(out);
        unlink(p);
        _exit(0);
    }
    assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st));
    if (WEXITSTATUS(st) == 99) return 0;
    if (WEXITSTATUS(st)) fprintf(stderr, "runner_netns_check: child exit %d\n", WEXITSTATUS(st));
    assert(WEXITSTATUS(st) == 0);
    return 1;
}

static unsigned probe_flags;  /* set by hardened_bounding: what the child saw */
#define PROBE_RWX_REFUSED 1
#define PROBE_ONLY_LO     2
static long long hardened_bounding(const service_t *svc) {
    int pfd[2], st;
    unsigned long long caps = 0, out[2];
    assert(pipe(pfd) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(pfd[0]);
        if (become_root() != 0) _exit(99);
        if (service_apply_hardening(svc) != 0) _exit(1);
        for (int c = 0; c < 64; c++)
            if (prctl(PR_CAPBSET_READ, c) == 1) caps |= 1ULL << c;
        out[0] = caps;
        out[1] = 0;
        if (mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
            out[1] |= PROBE_RWX_REFUSED;
        {
            struct ifaddrs *ifa, *i;
            int lo_only = getifaddrs(&ifa) == 0;
            for (i = lo_only ? ifa : NULL; i; i = i->ifa_next)
                if (strcmp(i->ifa_name, "lo")) lo_only = 0;
            if (lo_only) { freeifaddrs(ifa); out[1] |= PROBE_ONLY_LO; }
        }
        (void)!write(pfd[1], out, sizeof out);
        _exit(0);
    }
    close(pfd[1]);
    assert(waitpid(pid, &st, 0) == pid);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 99) { close(pfd[0]); return -1; }
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    assert(read(pfd[0], out, sizeof out) == sizeof out);
    close(pfd[0]);
    probe_flags = (unsigned)out[1];
    return (long long)out[0];
}

static void write_svc(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

int main(void) {
    umask(022);
    uint8_t v;
    static service_t one, table[4];
    char dir[] = "/tmp/schema-test-mountns-XXXXXX";
    char path[256];

    assert(parse_ns_bool("0", &v) == 0 && v == 0);
    assert(parse_ns_bool("1", &v) == 0 && v == 1);
    assert(parse_ns_bool("yes", &v) == -1);
    assert(parse_ns_bool("", &v) == -1);
    assert(parse_protect_system("0", &v) == 0 && v == PROTECT_SYSTEM_NONE);
    assert(parse_protect_system("1", &v) == 0 && v == PROTECT_SYSTEM_BASE);
    assert(parse_protect_system("full", &v) == 0 && v == PROTECT_SYSTEM_FULL);
    assert(parse_protect_system("strict", &v) == -1);

    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/sandboxed.svc", dir);
    write_svc(path, "name=sandboxed\nexec=/usr/bin/true\n"
                    "private_tmp=1\nprotect_system=full\nprotect_home=1\n"
                    "no_new_privs=1\nkeep_caps=CAP_NET_BIND_SERVICE\n");

    assert(service_load_one(path, &one) == 0);
    assert(one.ns_private_tmp == 1);
    assert(one.ns_protect_system == PROTECT_SYSTEM_FULL);
    assert(one.ns_protect_home == 1);

    assert(services_load(dir, table, 4) == 1);
    assert(table[0].ns_private_tmp == one.ns_private_tmp);
    assert(table[0].ns_protect_system == one.ns_protect_system);
    assert(table[0].ns_protect_home == one.ns_protect_home);
    assert((table[0].flags & SVC_NO_NEW_PRIVS) == (one.flags & SVC_NO_NEW_PRIVS));
    assert(table[0].cap_restrict == one.cap_restrict);
    assert(table[0].cap_keep_mask == one.cap_keep_mask);
    unlink(path);

    struct { const char *body; } bad[] = {
        { "name=b\nexec=/usr/bin/true\nprotect_system=yes\n" },
        { "name=b\nexec=/usr/bin/true\nprivate_tmp=1\nready_path=/tmp/b.ready\n" },
        { "name=b\nexec=/usr/bin/true\nprivate_tmp=1\nready_path=/var/tmp/b.ready\n" },
        { "name=b\nexec=/tmp/b.sh\nprivate_tmp=1\n" },
        { "name=b\nexec=/usr/bin/true\nprotect_home=1\nready_path=/run/user/1000/b\n" },
        { "name=b\nexec=/home/me/b.sh\nprotect_home=1\n" },
        { "name=b\nexec=/root/b.sh\nprotect_home=1\n" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        snprintf(path, sizeof(path), "%s/b.svc", dir);
        write_svc(path, bad[i].body);
        assert(service_load_one(path, &one) == -1);
        assert(services_load(dir, table, 4) == 0);
        unlink(path);
    }

    snprintf(path, sizeof(path), "%s/ok.svc", dir);
    write_svc(path, "name=ok\nexec=/usr/bin/true\nprivate_tmp=0\nprotect_home=1\n"
                    "ready_path=/run/ok.ready\n");
    assert(service_load_one(path, &one) == 0);
    assert(one.ns_private_tmp == 0 && one.ns_protect_home == 1);
    unlink(path);

    assert(landlock_beneath("/usr/bin/x", "/usr"));
    assert(landlock_beneath("/usr/bin/x", "/usr/"));
    assert(landlock_beneath("/usr", "/usr"));
    assert(landlock_beneath("/usr/bin/x", "/"));
    assert(!landlock_beneath("/usrx/bin", "/usr"));
    assert(!landlock_beneath("/us", "/usr"));

    snprintf(path, sizeof(path), "%s/ll.svc", dir);
    write_svc(path, "name=ll\nexec=/usr/bin/true\nlandlock_ro=/usr\nlandlock_rw=/var/lib/ll\n"
                    "landlock_ro=/etc/ll.conf\n");
    assert(service_load_one(path, &one) == 0);
    assert(one.landlock_count == 3 && one.landlock_rw == 2);
    assert(strcmp(one.landlock[2], "/etc/ll.conf") == 0);
    assert(services_load(dir, table, 4) == 1);
    assert(table[0].landlock_count == 3 && table[0].landlock_rw == 2);
    assert(strcmp(table[0].landlock[1], "/var/lib/ll") == 0);
    unlink(path);

    struct { const char *body; } llbad[] = {
        { "name=b\nexec=/usr/bin/true\nlandlock_ro=/etc\n" },
        { "name=b\nexec=/usr/bin/true\nlandlock_ro=usr\n" },
        { "name=b\nexec=/usr/bin/true\nlandlock_ro=/usr \n" },
        { "name=b\nexec=/usr/bin/true\nlandlock_ro=/usrx\n" },
        { "name=b\nexec=/usr/bin/true\nlandlock_ro=/usr\nkeep_caps=CAP_NET_BIND_SERVICE\n" },
    };
    for (size_t i = 0; i < sizeof(llbad) / sizeof(llbad[0]); i++) {
        snprintf(path, sizeof(path), "%s/b.svc", dir);
        write_svc(path, llbad[i].body);
        assert(service_load_one(path, &one) == -1);
        assert(services_load(dir, table, 4) == 0);
        unlink(path);
    }
    snprintf(path, sizeof(path), "%s/ok.svc", dir);
    write_svc(path, "name=ok\nexec=/usr/bin/true\nlandlock_ro=/usr\nno_new_privs=1\n"
                    "keep_caps=CAP_NET_BIND_SERVICE\n");
    assert(service_load_one(path, &one) == 0);
    write_svc(path, "name=ok\nexec=/usr/bin/true\nlandlock_ro=/usr\n"
                    "keep_caps=CAP_SYS_ADMIN\n");
    assert(service_load_one(path, &one) == 0);
    unlink(path);

    snprintf(path, sizeof(path), "%s/rw.svc", dir);
    write_svc(path, "name=rw\nexec=/usr/bin/true\nprotect_system=full\n"
                    "rw_path= /etc/pihole/ \nrw_path=/var/lib/my data\t\n");
    assert(service_load_one(path, &one) == 0);
    assert(one.rw_path_count == 2);
    assert(strcmp(one.rw_path[0], "/etc/pihole") == 0);
    assert(strcmp(one.rw_path[1], "/var/lib/my data") == 0);
    assert(services_load(dir, table, 4) == 1);
    assert(table[0].rw_path_count == 2 && strcmp(table[0].rw_path[0], "/etc/pihole") == 0);
    unlink(path);

    struct { const char *body; } rwbad[] = {
        { "name=b\nexec=/usr/bin/true\nrw_path=etc/x\n" },
        { "name=b\nexec=/usr/bin/true\nrw_path=\n" },
        { "name=b\nexec=/usr/bin/true\nrw_path=/etc/../root\n" },
        { "name=b\nexec=/usr/bin/true\nrw_path=/etc/x/..\n" },
        { "name=b\nexec=/usr/bin/true\nrw_path=/\n" },
        { "name=b\nexec=/usr/bin/true\nrw_path=/a\nrw_path=/b\nrw_path=/c\nrw_path=/d\n"
          "rw_path=/e\nrw_path=/f\nrw_path=/g\nrw_path=/h\nrw_path=/i\n" },
    };
    for (size_t i = 0; i < sizeof(rwbad) / sizeof(rwbad[0]); i++) {
        snprintf(path, sizeof(path), "%s/b.svc", dir);
        write_svc(path, rwbad[i].body);
        assert(service_load_one(path, &one) == -1);
        assert(services_load(dir, table, 4) == 0);
        unlink(path);
    }
    snprintf(path, sizeof(path), "%s/ok.svc", dir);
    write_svc(path, "name=ok\nexec=/usr/bin/true\nrw_path=/etc/..x\n");
    assert(service_load_one(path, &one) == 0 && strcmp(one.rw_path[0], "/etc/..x") == 0);
    unlink(path);

    struct { const char *key; uint8_t bit; } kn[] = {
        { "protect_kernel_tunables", NS_K_TUNABLES }, { "protect_kernel_modules", NS_K_MODULES },
        { "protect_control_groups", NS_K_CGROUPS }, { "private_devices", NS_K_DEVICES },
        { "protect_hostname", NS_K_HOSTNAME }, { "private_network", NS_K_NETWORK },
        { "memory_deny_write_execute", NS_K_MDWE },
    };
    snprintf(path, sizeof(path), "%s/k.svc", dir);
    for (size_t i = 0; i < sizeof(kn) / sizeof(kn[0]); i++)
        assert(strcmp(ns_kernel_names[__builtin_ctz(kn[i].bit)], kn[i].key) == 0);
    write_svc(path, "name=k\nexec=/usr/bin/true\n");
    assert(service_load_one(path, &one) == 0 && one.umask_val == -1);
    write_svc(path, "name=k\nexec=/usr/bin/true\numask=0027\n");
    assert(service_load_one(path, &one) == 0 && one.umask_val == 027);
    write_svc(path, "name=k\nexec=/usr/bin/true\numask=0\n");
    assert(service_load_one(path, &one) == 0 && one.umask_val == 0);
    write_svc(path, "name=k\nexec=/usr/bin/true\numask=077\n");
    assert(service_load_one(path, &one) == 0 && one.umask_val == 077);
    write_svc(path, "name=k\nexec=/usr/bin/true\numask=077\numask=\n");
    assert(service_load_one(path, &one) == 0 && one.umask_val == -1);
    {
        const char *ubad[] = { "0888", "1777", "abc", "00777x", "07777" };
        for (size_t i = 0; i < sizeof(ubad) / sizeof(ubad[0]); i++) {
            char body[96];
            snprintf(body, sizeof body, "name=k\nexec=/usr/bin/true\numask=%s\n", ubad[i]);
            write_svc(path, body);
            assert(service_load_one(path, &one) == -1);
        }
    }
    for (size_t i = 0; i < sizeof(kn) / sizeof(kn[0]); i++) {
        char body[160];
        snprintf(body, sizeof body, "name=k\nexec=/usr/bin/true\n%s=1\n", kn[i].key);
        write_svc(path, body);
        assert(service_load_one(path, &one) == 0 && one.ns_kernel == kn[i].bit);
        snprintf(body, sizeof body, "name=k\nexec=/usr/bin/true\n%s=1\n%s=0\n", kn[i].key, kn[i].key);
        write_svc(path, body);
        assert(service_load_one(path, &one) == 0 && one.ns_kernel == 0);
    }
    write_svc(path, "name=k\nexec=/usr/bin/true\nprotect_hostname=1\nprivate_devices=1\n");
    assert(service_load_one(path, &one) == 0 && one.ns_kernel == (NS_K_HOSTNAME | NS_K_DEVICES));
    assert(services_load(dir, table, 4) == 1 && table[0].ns_kernel == one.ns_kernel);
    unlink(path);
    struct { const char *body; } kbad[] = {
        { "name=b\nexec=/usr/bin/true\nprotect_hostname=2\n" },
        { "name=b\nexec=/usr/bin/true\nprivate_devices=yes\n" },
        { "name=b\nexec=/usr/lib/modules/x/helper\nprotect_kernel_modules=1\n" },
        { "name=b\nexec=/lib/modules/x/helper\nprotect_kernel_modules=1\n" },
        { "name=b\nexec=/usr/bin/true\nprivate_devices=1\nready_path=/dev/schema-ready\n" },
        { "name=b\nexec=/dev/b.sh\nprivate_devices=1\nready_path=/dev/shm/b.ready\n" },
        { "name=b\nexec=/usr/bin/true\nprotect_kernel_modules=1\nambient_caps=CAP_SYS_MODULE\n"
          "user=nobody\n" },
        { "name=b\nexec=/usr/bin/true\nprivate_devices=1\nambient_caps=CAP_MKNOD\nuser=nobody\n" },
    };
    for (size_t i = 0; i < sizeof(kbad) / sizeof(kbad[0]); i++) {
        snprintf(path, sizeof(path), "%s/b.svc", dir);
        write_svc(path, kbad[i].body);
        assert(service_load_one(path, &one) == -1);
        assert(services_load(dir, table, 4) == 0);
        unlink(path);
    }
    snprintf(path, sizeof(path), "%s/ok.svc", dir);
    write_svc(path, "name=ok\nexec=/usr/bin/true\nprivate_devices=1\nready_path=/run/x\n");
    assert(service_load_one(path, &one) == 0);
    write_svc(path, "name=ok\nexec=/usr/bin/true\nprivate_devices=1\nready_path=/dev/shm/ok.ready\n");
    assert(service_load_one(path, &one) == 0);
    write_svc(path, "name=ok\nexec=/usr/bin/true\nprivate_devices=1\nready_path=/dev/mqueue/ok\n");
    assert(service_load_one(path, &one) == 0);
    write_svc(path, "name=ok\nexec=/usr/bin/true\nprotect_kernel_modules=1\n"
                    "keep_caps=CAP_SYS_MODULE,CAP_NET_ADMIN\n");
    assert(service_load_one(path, &one) == 0);
    long long bs = hardened_bounding(&one);
    if (bs >= 0) {
        assert(!(bs & (1LL << CAP_SYS_MODULE)) && (bs & (1LL << CAP_NET_ADMIN)));
        write_svc(path, "name=ok\nexec=/usr/bin/true\nprivate_devices=1\n");
        assert(service_load_one(path, &one) == 0 && !one.cap_restrict);
        bs = hardened_bounding(&one);
        assert(!(bs & (1LL << CAP_MKNOD)) && !(bs & (1LL << CAP_SYS_RAWIO)));
        assert((bs & (1LL << CAP_SYS_MODULE)) && (bs & (1LL << CAP_NET_ADMIN)));
        assert(!(probe_flags & PROBE_RWX_REFUSED));
        write_svc(path, "name=ok\nexec=/usr/bin/true\nprivate_network=1\n");
        assert(service_load_one(path, &one) == 0);
        bs = hardened_bounding(&one);
        assert(probe_flags & PROBE_ONLY_LO);
        if (mdwe_ok()) {
            write_svc(path, "name=ok\nexec=/usr/bin/true\nmemory_deny_write_execute=1\n");
            assert(service_load_one(path, &one) == 0);
            bs = hardened_bounding(&one);
            assert(probe_flags & PROBE_RWX_REFUSED);
        } else
            printf("SKIP  memory_deny_write_execute (kernel has no PR_SET_MDWE)\n");
        assert(runner_netns_check(dir) == 1);
    } else
        printf("SKIP  kernel cap drops (no userns)\n");
    unlink(path);

    assert(hardening_default_resolve(NULL, NULL) == 0);
    assert(hardening_default_resolve(NULL, "on") == 1);
    assert(hardening_default_resolve(NULL, " on \n") == 1);
    assert(hardening_default_resolve(NULL, "ON") == 0);
    assert(hardening_default_resolve(NULL, "1") == 0);
    assert(hardening_default_resolve(NULL, "one") == 0);
    assert(hardening_default_resolve(NULL, "") == 0);
    assert(hardening_default_resolve("quiet schema.hardening_default=0 rhgb\n", "on") == 0);
    assert(hardening_default_resolve("schema.hardening_default=1\n", NULL) == 1);
    assert(hardening_default_resolve("xschema.hardening_default=1", NULL) == 0);
    assert(hardening_default_resolve("schema.hardening_default=10", "on") == 1);
    assert(hardening_default_resolve("schema.hardening_default=2", NULL) == 0);

    snprintf(path, sizeof(path), "%s/plain.svc", dir);
    write_svc(path, "name=plain\nexec=/usr/bin/true\n");
    assert(service_load_one(path, &one) == 0);
    assert(!(one.flags & SVC_NO_NEW_PRIVS) && !one.ns_private_tmp &&
           !one.ns_protect_system && !one.ns_protect_home && !one.hard_default);

    service_set_hardening_default(1);
    assert(service_load_one(path, &one) == 0);
    assert((one.flags & SVC_NO_NEW_PRIVS) && one.ns_private_tmp == 1 &&
           one.ns_protect_system == PROTECT_SYSTEM_BASE && one.ns_protect_home == 1);
    assert(one.hard_default == (HARD_NNP | HARD_PT | HARD_PS | HARD_PH) && !one.hard_set);
    assert(services_load(dir, table, 4) == 1);
    assert(table[0].hard_default == one.hard_default && table[0].flags == one.flags);

    write_svc(path, "name=plain\nexec=/usr/bin/true\nno_new_privs=0\nprivate_tmp=0\n"
                    "protect_system=0\nprotect_home=0\nsyscall_deny=0\n");
    assert(service_load_one(path, &one) == 0);
    assert(!(one.flags & SVC_NO_NEW_PRIVS) && !one.ns_private_tmp &&
           !one.ns_protect_system && !one.ns_protect_home && !one.hard_default);
    assert(services_load(dir, table, 4) == 1);
    assert(!(table[0].flags & SVC_NO_NEW_PRIVS) && !table[0].hard_default);

    write_svc(path, "name=plain\nexec=/home/me/run.sh\nready_path=/tmp/plain.ready\n");
    assert(service_load_one(path, &one) == 0);
    assert(!one.ns_protect_home && !one.ns_private_tmp);
    assert(one.hard_dropped == (HARD_PT | HARD_PH));
    assert(one.ns_protect_system == PROTECT_SYSTEM_BASE && (one.flags & SVC_NO_NEW_PRIVS));
    assert(services_load(dir, table, 4) == 1 && table[0].hard_dropped == one.hard_dropped);

    write_svc(path, "name=plain\nexec=/home/me/run.sh\nprotect_home=1\n");
    assert(service_load_one(path, &one) == -1);
    assert(services_load(dir, table, 4) == 0);

    /* working_dir: a defaulted knob that hides it is dropped, an explicit one kept */
    write_svc(path, "name=plain\nexec=/usr/bin/true\nworking_dir=-~\n");
    assert(service_load_one(path, &one) == 0);
    assert(one.hard_dropped == HARD_PH && !one.ns_protect_home && one.ns_private_tmp == 1);
    write_svc(path, "name=plain\nexec=/usr/bin/true\nworking_dir=/tmp\n");
    assert(service_load_one(path, &one) == 0);
    assert(one.hard_dropped == HARD_PT && !one.ns_private_tmp);
    write_svc(path, "name=plain\nexec=/usr/bin/true\nworking_dir=/tmp/w\nprivate_tmp=1\n");
    assert(service_load_one(path, &one) == 0);
    assert(one.ns_private_tmp == 1 && !(one.hard_dropped & HARD_PT));
    write_svc(path, "name=plain\nexec=/usr/bin/true\nworking_dir=/srv/w\n");
    assert(service_load_one(path, &one) == 0 && !one.hard_dropped);
    service_set_hardening_default(0);

    unlink(path);
    rmdir(dir);

    printf("all mountns-parse tests passed\n");
    return 0;
}
