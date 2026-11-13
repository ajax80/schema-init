/* Loader checks from the 2026-11-12 audit, Tier 0: a malformed key refuses
 * the service instead of loading it as something else. */
#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static char dir[64];

static void write_file(const char *name, const char *body) {
    char p[256];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

/* load NAME.svc holding BODY; 0 loaded, -1 refused */
static int load(const char *body, service_t *svc) {
    char p[256];
    write_file("t.svc", body);
    snprintf(p, sizeof p, "%s/t.svc", dir);
    return service_load_one(p, svc);
}

static int load_key(const char *kv, service_t *svc) {
    char body[512];
    snprintf(body, sizeof body, "name=t\nexec=/bin/true\n%s\n", kv);
    return load(body, svc);
}

static void test_on_calendar(void) {
    service_t s;
    assert(load_key("on_calendar=25:00", &s) == -1);
    assert(load_key("on_calendar=Mon", &s) == -1);
    assert(load_key("on_calendar=", &s) == 0 && !(s.flags & SVC_TIMER));
    assert(load_key("on_calendar=03:30", &s) == 0);
    assert((s.flags & SVC_TIMER) && (s.flags & SVC_TIMER_CALENDAR));
}

static void test_names(void) {
    service_t s;
    assert(load("name=a b\nexec=/bin/true\n", &s) == -1);
    assert(load("name=../x\nexec=/bin/true\n", &s) == -1);
    assert(load("name=.hidden\nexec=/bin/true\n", &s) == -1);
    assert(load("name=a/b\nexec=/bin/true\n", &s) == -1);
    assert(load("name=app-org.kde.x:1+y_z\nexec=/bin/true\n", &s) == 0);
    assert(load("name=systemd-fsck@dev-disk-by\\x2duuid-1\nexec=/bin/true\n", &s) == 0);
    write_file("motor@12.svc", "exec=/bin/true\n");
    char p[256];
    snprintf(p, sizeof p, "%s/motor@12.svc", dir);
    assert(service_load_one(p, &s) == 0 && !strcmp(s.name, "motor@12"));
    unlink(p);
}

static void test_limits(void) {
    service_t s;
    assert(load_key("cpu_limit=abc", &s) == -1);
    assert(load_key("cpu_limit=20000", &s) == -1);
    assert(load_key("cpu_limit=50%", &s) == -1);
    assert(load_key("cpu_limit=150", &s) == 0 && s.cpu_limit_pct == 150);
    assert(load_key("cpu_limit=0", &s) == 0 && s.cpu_limit_pct == 0);
    assert(load_key("cpu_limit=", &s) == 0 && s.cpu_limit_pct == 0);

    assert(load_key("mem_limit=1G", &s) == 0 && s.mem_limit_mb == 1024);
    assert(load_key("mem_limit=512M", &s) == 0 && s.mem_limit_mb == 512);
    assert(load_key("mem_limit=64", &s) == 0 && s.mem_limit_mb == 64);
    assert(load_key("mem_limit=", &s) == 0 && s.mem_limit_mb == 0);
    assert(load_key("mem_limit=abc", &s) == -1);
    assert(load_key("mem_limit=1T", &s) == -1);
    assert(load_key("mem_limit=-5", &s) == -1);
    assert(load_key("mem_limit=1GB", &s) == -1);
    if (sizeof(long) > 4) {
        assert(load_key("mem_limit=1099511627777", &s) == -1);
        assert(load_key("mem_limit=1099511627776", &s) == 0 && mem_high_bytes(s.mem_limit_mb) > 0);
    } else {   /* 32-bit long: strtol's own range is the cap */
        assert(load_key("mem_limit=2147483648", &s) == -1);
        assert(load_key("mem_limit=2147483647", &s) == 0 && mem_high_bytes(s.mem_limit_mb) > 0);
        assert(load_key("mem_limit=2097152G", &s) == -1);   /* the G multiply's own guard */
        assert(load_key("mem_limit=2097151G", &s) == 0);
    }

    assert(load_key("watchdog_sec=-1", &s) == -1);
    assert(load_key("watchdog_sec=x", &s) == -1);
    assert(load_key("watchdog_sec=30", &s) == 0 && s.watchdog_sec == 30);
    assert(load_key("watchdog_sec=0", &s) == 0 && s.watchdog_sec == 0);
}

/* sorted load order, and a second file taking a used name= is refused */
static void test_load_order_and_dups(void) {
    service_t *tab = calloc(MAX_SERVICES, sizeof *tab);
    assert(tab);
    char p[256];
    snprintf(p, sizeof p, "%s/t.svc", dir);
    unlink(p);
    write_file("c.svc", "name=c\nexec=/bin/true\n");
    write_file("b.svc", "name=x\nexec=/bin/false\n");
    write_file("a.svc", "name=x\nexec=/bin/true\n");
    write_file("d.svc", "name=d\nexec=/bin/true\n");
    int n = services_load(dir, tab, MAX_SERVICES);
    assert(n == 3);
    assert(!strcmp(tab[0].name, "x") && !strcmp(tab[0].exec, "/bin/true"));
    assert(!strcmp(tab[1].name, "c"));
    assert(!strcmp(tab[2].name, "d"));
    /* `systemctl mask`: an empty <name>.svc.masked twin keeps it out (audit 1.6) */
    write_file("d.svc.masked", "");
    n = services_load(dir, tab, MAX_SERVICES);
    assert(n == 2 && strcmp(tab[0].name, "d") && strcmp(tab[1].name, "d"));
    free(tab);
}

int main(void) {
    snprintf(dir, sizeof dir, "/tmp/schema-svc-audit0-XXXXXX");
    assert(mkdtemp(dir));
    test_on_calendar();
    test_names();
    test_limits();
    test_load_order_and_dups();
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    assert(system(cmd) == 0);
    printf("test_svc_audit0: ok\n");
    return 0;
}
