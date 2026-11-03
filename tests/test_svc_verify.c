#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static void put(const char *dir, const char *name, const char *val) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(val, f);
    fclose(f);
}

static void mkcg(const char *dir, const char *mem, const char *cpu, int populated) {
    mkdir(dir, 0755);
    put(dir, "memory.max", mem);
    put(dir, "cpu.max", cpu);
    put(dir, "pids.max", "max\n");
    put(dir, "cgroup.events", populated ? "populated 1\nfrozen 0\n" : "populated 0\nfrozen 0\n");
}

int main(void) {
    char root[] = "/tmp/schema-verify-XXXXXX", svcd[256], rt[300], pl[300], why[160];
    service_t svc;
    assert(mkdtemp(root));
    snprintf(svcd, sizeof svcd, "%s/svc", root);
    snprintf(rt, sizeof rt, "%s/runtime", svcd);
    snprintf(pl, sizeof pl, "%s/libpod-payload-abc", svcd);
    mkcg(svcd, "max\n", "max 100000\n", 1);
    mkcg(rt, "max\n", "max 100000\n", 1);
    mkcg(pl, "67108864\n", "150000 100000\n", 1);

    memset(&svc, 0, sizeof svc);
    snprintf(svc.verify_memory_max, sizeof svc.verify_memory_max, "67108864");
    snprintf(svc.verify_cpu_max, sizeof svc.verify_cpu_max, "150000 100000");
    assert(verify_limits_match(svcd, &svc, why, sizeof why) == 1);

    /* the limit never landed: payload unlimited */
    put(pl, "memory.max", "max\n");
    assert(verify_limits_match(svcd, &svc, why, sizeof why) == 0);
    assert(strstr(why, "memory.max") && strstr(why, "want=67108864") && strstr(why, "got=max"));

    /* why= names each populated cgroup's first mismatch, sorted by name:
     * runtime/'s "max" can't hide the payload's wrong limit, nor the reverse */
    put(pl, "memory.max", "33554432\n");
    assert(verify_limits_match(svcd, &svc, why, sizeof why) == 0);
    assert(!strcmp(why, "libpod-payload-abc: memory.max want=67108864 got=33554432; "
                        "runtime: memory.max want=67108864 got=max"));
    /* the common failure: payload unlimited, beside a sibling with some other real limit */
    put(pl, "memory.max", "max\n");
    put(rt, "memory.max", "1048576\n");
    assert(verify_limits_match(svcd, &svc, why, sizeof why) == 0);
    assert(!strcmp(why, "libpod-payload-abc: memory.max want=67108864 got=max; "
                        "runtime: memory.max want=67108864 got=1048576"));
    put(rt, "memory.max", "max\n");
    /* a long payload id is cut to 40 chars; want/got survive */
    {
        char lp[400];
        snprintf(lp, sizeof lp, "%s/libpod-payload-0a2f4e7f2eccbad7cecf5b71c520dd3b930f442c0765f69b1fd8b6ababfd1e86", svcd);
        mkcg(lp, "33554432\n", "150000 100000\n", 1);
        put(pl, "cgroup.events", "populated 0\nfrozen 0\n");
        assert(verify_limits_match(svcd, &svc, why, sizeof why) == 0);
        assert(strstr(why, "libpod-payload-0a2f4e7f2eccbad7cecf5b71c: memory.max want=67108864 got=33554432"));
        put(lp, "cgroup.events", "populated 0\nfrozen 0\n");
        put(pl, "cgroup.events", "populated 1\nfrozen 0\n");
    }
    /* a missing or empty memory.max is never within a page of a small want */
    snprintf(svc.verify_memory_max, sizeof svc.verify_memory_max, "1000");
    {
        char f[400];
        snprintf(f, sizeof f, "%s/memory.max", pl);
        unlink(f);
        assert(verify_limits_match(svcd, &svc, why, sizeof why) == 0);
        assert(strstr(why, "libpod-payload-abc: memory.max want=1000 got=missing"));
        put(pl, "memory.max", "");
        assert(verify_limits_match(svcd, &svc, why, sizeof why) == 0);
    }
    snprintf(svc.verify_memory_max, sizeof svc.verify_memory_max, "67108864");
    put(pl, "memory.max", "max\n");

    /* the kernel rounds memory.max down to a page: still a match */
    snprintf(svc.verify_memory_max, sizeof svc.verify_memory_max, "100000000");
    put(pl, "memory.max", "99999744\n");
    assert(verify_limits_match(svcd, &svc, why, sizeof why) == 1);
    snprintf(svc.verify_memory_max, sizeof svc.verify_memory_max, "67108864");

    /* a matching cgroup with nothing in it does not count */
    put(pl, "memory.max", "67108864\n");
    put(pl, "cgroup.events", "populated 0\nfrozen 0\n");
    assert(verify_limits_match(svcd, &svc, why, sizeof why) == 0);

    /* no populated cgroup at all: the reason says so, not "no cgroup" */
    put(pl, "cgroup.events", "populated 0\nfrozen 0\n");
    put(rt, "cgroup.events", "populated 0\nfrozen 0\n");
    why[0] = '\0';
    assert(verify_limits_match(svcd, &svc, why, sizeof why) == 0);
    assert(strstr(why, "no populated cgroup"));
    put(pl, "cgroup.events", "populated 1\nfrozen 0\n");
    put(rt, "cgroup.events", "populated 1\nfrozen 0\n");

    /* verify_* needs notify=1: refused, like main_pid_notify */
    char q[300];
    snprintf(q, sizeof q, "%s/n.svc", root);
    put(root, "n.svc", "name=n\nexec=/bin/true\nverify_memory_max=67108864\n");
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(q, &svc) != 0);

    /* bind=: one per .svc, also a dep (not counted twice), survives a drop-in's dep= reset */
    {
        char b[300];
        snprintf(b, sizeof b, "%s/b.svc", root);
        put(root, "b.svc", "name=b\nexec=/bin/true\ndep=pod\nbind=pod\ndep=net\n");
        memset(&svc, 0, sizeof svc);
        assert(service_load_one(b, &svc) == 0 && !strcmp(svc.bind_name, "pod"));
        int n = 0, have = 0;
        for (int k = 0; k < MAX_DEPS && svc.dep_name[k][0]; k++) { n++; have |= !strcmp(svc.dep_name[k], "pod"); }
        assert(have && n == 2);
        put(root, "b.svc", "name=b\nexec=/bin/true\nbind=pod\n");
        memset(&svc, 0, sizeof svc);
        assert(service_load_one(b, &svc) == 0 && !strcmp(svc.dep_name[0], "pod"));
        put(root, "b.svc", "name=b\nexec=/bin/true\nbind=pod\nbind=other\n");
        memset(&svc, 0, sizeof svc);
        assert(service_load_one(b, &svc) != 0);
        /* a held timer would come back with a stale timer_next: refused */
        put(root, "b.svc", "name=b\nexec=/bin/true\non_boot_sec=5\nbind=pod\n");
        memset(&svc, 0, sizeof svc);
        assert(service_load_one(b, &svc) != 0);
        put(root, "b.svc", "name=b\nexec=/bin/true\nno_restart=1\n");
        memset(&svc, 0, sizeof svc);
        assert(service_load_one(b, &svc) == 0 && svc.no_restart_cfg == 1);
    }

    /* parse: the keys load, and a service without them has nothing to verify */
    char p[300];
    snprintf(p, sizeof p, "%s/v.svc", root);
    put(root, "v.svc", "name=v\nexec=/bin/true\nnotify=1\nverify_memory_max=67108864\nverify_cpu_max=150000 100000\nverify_pids_max=100\n");
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0);
    assert(!strcmp(svc.verify_memory_max, "67108864") && !strcmp(svc.verify_cpu_max, "150000 100000") &&
           !strcmp(svc.verify_pids_max, "100"));
    assert(service_verifies_limits(&svc));
    put(root, "v.svc", "name=v\nexec=/bin/true\n");
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0 && !service_verifies_limits(&svc));

    /* I1: no cgroup to check against: the spawn fails, nothing is left running */
    snprintf(q, sizeof q, "%s/c.svc", root);
    put(root, "c.svc", "name=c\nexec=/bin/sleep\nargs=30\nnotify=1\nverify_memory_max=67108864\n");
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(q, &svc) == 0);
    service_cgroup_root = "";
    assert(service_spawn(&svc) < 0 && svc.child_pid <= 0);
    put(root, "c.svc", "name=c\nexec=/bin/sleep\nargs=30\nnotify=1\nmain_pid_notify=1\n");
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(q, &svc) == 0);
    assert(service_spawn(&svc) < 0 && svc.child_pid <= 0);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
    printf("test_svc_verify: PASS\n");
    return 0;
}
