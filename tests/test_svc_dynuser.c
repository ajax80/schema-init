#include "../service.h"
#include "../ns.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[] = "/tmp/schema-dynuser-XXXXXX";
static char path[300], dd[320], dp[360];

static void put(const char *p, const char *body) {
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static int load(const char *keys, const char *dropin, service_t *s) {
    char body[2048];
    snprintf(body, sizeof body, "name=t\nexec=/bin/true\n%s", keys);
    put(path, body);
    if (dropin) {
        mkdir(dd, 0755);
        put(dp, dropin);
    }
    memset(s, 0, sizeof *s);
    int r = service_load_one(path, s);
    if (dropin) {
        unlink(dp);
        rmdir(dd);
    }
    return r;
}

int main(void) {
    static service_t s;
    char keys[2048];
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/t.svc", dir);
    snprintf(dd, sizeof dd, "%s.d", path);
    snprintf(dp, sizeof dp, "%s/10-x.conf", dd);

    assert(load("", NULL, &s) == 0 && !s.supp_count && !s.dynamic_user);
    assert(load("supplementary_groups=wheel 10\n", NULL, &s) == 0 && s.supp_count == 2 &&
           !strcmp(s.supp_groups[0], "wheel") && !strcmp(s.supp_groups[1], "10"));
    assert(load("supplementary_groups=a\nsupplementary_groups=b c\n", NULL, &s) == 0 && s.supp_count == 3);
    keys[0] = 0;
    strcat(keys, "supplementary_groups=");
    for (int i = 0; i < 17; i++) snprintf(keys + strlen(keys), sizeof keys - strlen(keys), "g%d ", i);
    strcat(keys, "\n");
    assert(load(keys, NULL, &s) == -1);
    /* a drop-in's empty value clears, as for the other lists */
    assert(load("supplementary_groups=a b\n", "supplementary_groups=\n", &s) == 0 && !s.supp_count);
    assert(load("supplementary_groups=bad:name\n", NULL, &s) == -1);

    /* dynamic_user implies the sandbox, unless the unit says otherwise */
    assert(load("dynamic_user=1\n", NULL, &s) == 0 && s.dynamic_user == 1);
    assert((s.flags & SVC_NO_NEW_PRIVS) && s.ns_private_tmp == 1 && s.ns_protect_system == PROTECT_SYSTEM_FULL &&
           s.ns_protect_home == 1);
    assert(s.hard_dyn == (HARD_NNP | HARD_PT | HARD_PS | HARD_PH));
    assert(load("dynamic_user=1\nprivate_tmp=0\nprotect_system=1\n", NULL, &s) == 0 && s.ns_private_tmp == 0 &&
           s.ns_protect_system == PROTECT_SYSTEM_BASE && !(s.hard_dyn & (HARD_PT | HARD_PS)) && (s.hard_dyn & HARD_PH));
    assert(load("dynamic_user=0\n", NULL, &s) == 0 && !s.dynamic_user && !s.hard_dyn);
    assert(load("dynamic_user=2\n", NULL, &s) == -1);
    /* schema-user ignores dynamic_user=1, and so what it implies */
    service_dynamic_users = 0;
    assert(load("dynamic_user=1\n", NULL, &s) == 0 && !s.hard_dyn && !s.ns_protect_home);
    service_dynamic_users = 1;
    /* <base>/<d> is a symlink into private/: one level only */
    assert(load("dynamic_user=1\nstate_dir=a/b\n", NULL, &s) == -1);
    assert(load("dynamic_user=1\nruntime_dir=a/b\nstate_dir=a\n", NULL, &s) == 0);
    assert(load("state_dir=a/b\n", NULL, &s) == 0);

    unlink(path);
    rmdir(dir);
    printf("all svc dynuser tests passed\n");
    return 0;
}
