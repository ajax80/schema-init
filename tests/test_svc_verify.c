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

    /* the kernel rounds memory.max down to a page: still a match */
    snprintf(svc.verify_memory_max, sizeof svc.verify_memory_max, "100000000");
    put(pl, "memory.max", "99999744\n");
    assert(verify_limits_match(svcd, &svc, why, sizeof why) == 1);
    snprintf(svc.verify_memory_max, sizeof svc.verify_memory_max, "67108864");

    /* a matching cgroup with nothing in it does not count */
    put(pl, "memory.max", "67108864\n");
    put(pl, "cgroup.events", "populated 0\nfrozen 0\n");
    assert(verify_limits_match(svcd, &svc, why, sizeof why) == 0);

    /* parse: the keys load, and a service without them has nothing to verify */
    char p[300];
    snprintf(p, sizeof p, "%s/v.svc", root);
    put(root, "v.svc", "name=v\nexec=/bin/true\nverify_memory_max=67108864\nverify_cpu_max=150000 100000\nverify_pids_max=100\n");
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0);
    assert(!strcmp(svc.verify_memory_max, "67108864") && !strcmp(svc.verify_cpu_max, "150000 100000") &&
           !strcmp(svc.verify_pids_max, "100"));
    assert(service_verifies_limits(&svc));
    put(root, "v.svc", "name=v\nexec=/bin/true\n");
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0 && !service_verifies_limits(&svc));

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
    printf("test_svc_verify: PASS\n");
    return 0;
}
