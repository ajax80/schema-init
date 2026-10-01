#include "../ns.h"
#include "../landlock.h"
#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static void write_svc(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

int main(void) {
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
                    "protect_system=0\nprotect_home=0\n");
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
    service_set_hardening_default(0);

    unlink(path);
    rmdir(dir);

    printf("all mountns-parse tests passed\n");
    return 0;
}
