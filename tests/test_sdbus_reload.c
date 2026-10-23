#include "../sdbus_reload.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static int fails;
static void check(const char *name, int ok) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) fails++;
}

static char *rules(int n, const char *ctx) {
    static char buf[4096];
    int o = snprintf(buf, sizeof buf, "context = %s\n", ctx);
    for (int i = 0; i < n && o < (int)sizeof buf; i++)
        o += snprintf(buf + o, sizeof buf - o, "allow = send_destination:org.r%d\n", i);
    return buf;
}

static int ok(const char *a, const char *b, const sdbus_owner *own, int n, char *why) {
    sdbus_policy *pa = sdbus_policy_parse(a), *pb = sdbus_policy_parse(b);
    int r = sdbus_reload_policy_ok(pa, pb, own, n, why, 256);
    sdbus_policy_free(pa); sdbus_policy_free(pb);
    return r;
}

static void put(const char *dir, const char *file, const char *text) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "w");
    fputs(text, f);
    fclose(f);
}

int main(void) {
    char why[256];

    sdbus_policy *p = sdbus_policy_parse("context = default\nallow = own:a\nallow = own:b\nallow = own:c\n"
                                         "context = user:0\nallow = own:d\ndeny = own:e\n");
    check("rule_count", sdbus_policy_rule_count(p) == 5);
    sdbus_policy_free(p);

    sdbus_owner o1[] = {{ "org.x", 0, NULL, 0 }};
    const char *same = "context = default\nallow = own:*\n";
    check("ok_same_policy", ok(same, same, o1, 1, why) == 1);

    char old10[4096];
    snprintf(old10, sizeof old10, "%s", rules(10, "default"));
    why[0] = '\0';
    check("refuse_halved", ok(old10, rules(4, "default"), NULL, 0, why) == 0 && strstr(why, "10 -> 4"));
    check("allow_exactly_half", ok(old10, rules(5, "default"), NULL, 0, why) == 1);

    sdbus_owner o2[] = {{ "org.freedesktop.login1", 0, NULL, 0 }};
    why[0] = '\0';
    check("conf_removed_applies_with_note",
          ok("context = default\nallow = own:org.freedesktop.login1\nallow = send_destination:*\n",
             "context = default\nallow = send_destination:*\nallow = send_type:signal\n",
             o2, 1, why) == 1 && strstr(why, "no rules left"));
    check("refuse_own_now_denied",
          ok("context = default\nallow = own:org.freedesktop.login1\nallow = send_destination:*\n",
             "context = default\ndeny = own:org.freedesktop.login1\nallow = send_destination:*\n",
             o2, 1, why) == 0 && strstr(why, "would deny own org.freedesktop.login1"));
    check("refuse_own_wrong_user",
          ok("context = default\nallow = own:org.freedesktop.login1\nallow = send_destination:*\n",
             "context = user:999\nallow = own:org.freedesktop.login1\ncontext = default\nallow = send_destination:*\n",
             o2, 1, why) == 0);
    check("refuse_own_prefix_denied",
          ok("context = default\nallow = own:org.freedesktop.login1\nallow = send_destination:*\n",
             "context = default\ndeny = own_prefix:org.freedesktop\nallow = send_destination:*\n",
             o2, 1, why) == 0);
    why[0] = 'x';
    check("ok_clears_why", ok(same, same, o1, 1, why) == 1 && why[0] == '\0');

    check("old_empty_any_new_ok", ok("", "context = default\nallow = own:*\n", NULL, 0, why) == 1);

    char base[] = "/tmp/sdbus-reload-XXXXXX";
    if (!mkdtemp(base)) return 1;
    char ovr[300], raw[300];
    snprintf(ovr, sizeof ovr, "%s/ovr", base);
    snprintf(raw, sizeof raw, "%s/raw", base);
    mkdir(ovr, 0755); mkdir(raw, 0755);
    const char *dirs[] = { ovr, raw };
    sdbus_svctab empty = {0};

    put(raw, "a.service", "[D-BUS Service]\nName=a\nExec=/bin/a\nSystemdService=a.service\n");
    sdbus_svctab *t = sdbus_svctab_parse_dirs_masked(dirs, 2, NULL, "root");
    const sdbus_svc_ent *e = sdbus_svctab_find(t, "a");
    check("fields_set", e && e->systemd_service == 1 && e->dir == raw);
    int held = sdbus_svctab_hold_raw(t, &empty, ovr);
    check("hold_raw_new", held == 1 && !sdbus_svctab_find(t, "a"));
    sdbus_svctab_free(t);

    sdbus_svctab *old = sdbus_svctab_parse_dirs_masked(dirs, 2, NULL, "root");
    t = sdbus_svctab_parse_dirs_masked(dirs, 2, NULL, "root");
    check("keep_raw_known", sdbus_svctab_hold_raw(t, old, ovr) == 0 && sdbus_svctab_find(t, "a"));
    sdbus_svctab_free(t); sdbus_svctab_free(old);

    put(ovr, "c.service", "[D-BUS Service]\nName=c\nExec=/bin/c\nSystemdService=c.service\n");
    put(raw, "b.service", "[D-BUS Service]\nName=b\nExec=/bin/b\n");
    t = sdbus_svctab_parse_dirs_masked(dirs, 2, NULL, "root");
    sdbus_svctab_hold_raw(t, &empty, ovr);
    check("keep_override_new", sdbus_svctab_find(t, "c") != NULL);
    check("keep_plain_new", sdbus_svctab_find(t, "b") != NULL);
    e = sdbus_svctab_find(t, "b");
    check("plain_entry_flag_clear", e && e->systemd_service == 0);
    sdbus_svctab_free(t);

    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf %s", base);
    if (system(cmd)) {}

    printf(fails ? "FAIL\n" : "PASS\n");
    return fails != 0;
}
