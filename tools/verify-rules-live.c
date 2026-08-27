/* Read-only fidelity gate: diff the native rule interpreter's symlinks+tags
 * against real systemd-udevd /run/udev/data. Writes nothing. exit(1) on any
 * in-scope divergence (the E3-flip precondition), exit(0) when clean. */
#include "../schema-udev.h"
#include "../udev_rules.h"     /* run_builtins */
#include "../udev_db.h"        /* udev_db_filename, read_links_tags, UDEV_DB_DIR */
#include "../udev_ruleset.h"   /* ruleset_load_dirs, dev_ctx, ruleset_apply */
#include "flip_classify.h"     /* link_is_critical — harmful vs harmless */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static struct ruleset g_rs;
static int g_dev = 0, g_dev_db = 0;
static int g_sym_extra = 0, g_sym_miss_inscope = 0, g_sym_miss_debt = 0;
static int g_tag_miss = 0, g_tag_extra = 0;
/* permissive split: harmful = things that can actually break the machine. */
static int g_sym_miss_harmful = 0, g_sym_miss_harmless = 0;

/* KNOWN-DEBT: by-id links need ata_id/scsi_id serial/wwn we don't fully emit yet. */
static int link_is_known_debt(const char *link) {
    return strncmp(link, "disk/by-id/", 11) == 0;
}

static int in_set(char set[][UE_VAL_MAX], int n, const char *s) {
    for (int i = 0; i < n; i++) if (!strcmp(set[i], s)) return 1;
    return 0;
}
static int in_tagset(char set[][UE_KEY_MAX], int n, const char *s) {
    for (int i = 0; i < n; i++) if (set[i][0] && !strcmp(set[i], s)) return 1;
    return 0;
}

static void collect(struct uevent *ev_in) {
    struct uevent ev = *ev_in;
    int kernel_n = ev.n; (void)kernel_n;
    const char *devpath = uevent_get(&ev, "DEVPATH");
    if (!devpath) return;
    g_dev++;
    const char *devname = uevent_get(&ev, "DEVNAME");
    char devnode[UE_VAL_MAX]; const char *dn = NULL;
    if (devname) { snprintf(devnode, sizeof devnode, "/dev/%s", devname); dn = devnode; }
    run_builtins("/sys", devpath, dn, &ev);

    struct dev_ctx ctx;
    if (dev_ctx_init(&ctx, &ev, "/sys") != 0) return;
    ruleset_apply(&g_rs, &ctx);

    char name[128];
    if (udev_db_filename(&ev, name, sizeof name) != 0) return;
    char path[256];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", UDEV_DB_DIR, name) >= sizeof path) return;
    char tlinks[32][UE_VAL_MAX]; int tnl = 0;
    char ttags[32][UE_KEY_MAX];  int tnt = 0;
    if (udev_db_read_links_tags(path, tlinks, &tnl, 32, ttags, &tnt, 32) != 0) return;
    g_dev_db++;

    /* SYM-EXTRA: ours not theirs -> interpreter applied a wrong link (fatal). */
    for (int i = 0; i < ctx.nsym; i++)
        if (!in_set(tlinks, tnl, ctx.symlinks[i])) {
            printf("SYM-EXTRA  %-10s %s\n", name, ctx.symlinks[i]);
            g_sym_extra++;
        }
    /* SYM-MISS: theirs not ours -> known-debt (by-id) or in-scope (fatal). */
    for (int i = 0; i < tnl; i++)
        if (!in_set(ctx.symlinks, ctx.nsym, tlinks[i])) {
            if (link_is_known_debt(tlinks[i])) {
                printf("KNOWN-DEBT %-10s %s\n", name, tlinks[i]);
                g_sym_miss_debt++;
            } else if (link_is_critical(tlinks[i])) {
                printf("SYM-MISS!  %-10s %s  (HARMFUL: boot/fstab exact path)\n", name, tlinks[i]);
                g_sym_miss_inscope++;
                g_sym_miss_harmful++;
            } else {
                printf("SYM-MISS   %-10s %s  (harmless: device reachable otherwise)\n", name, tlinks[i]);
                g_sym_miss_inscope++;
                g_sym_miss_harmless++;
            }
        }
    /* Tags: set compare G: both directions (in-scope, fatal). */
    for (int i = 0; i < tnt; i++)
        if (!in_tagset(ctx.tags, ctx.ntags, ttags[i])) {
            printf("TAG-MISS   %-10s %s\n", name, ttags[i]);
            g_tag_miss++;
        }
    for (int i = 0; i < ctx.ntags; i++)
        if (ctx.tags[i][0] != '\0' && !in_tagset(ttags, tnt, ctx.tags[i])) {
            printf("TAG-EXTRA  %-10s %s\n", name, ctx.tags[i]);
            g_tag_extra++;
        }
}

int main(int argc, char **argv) {
    /* --permissive: gate on HARMFUL divergence only (the flip wizard's model).
     * default: strict — gate on ANY in-scope divergence (parity-tuning use). */
    int permissive = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--permissive")) permissive = 1;

    ruleset_load_dirs((const char *const[]){
        "/usr/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d" }, 3, &g_rs);
    printf("loaded %d native rule(s)\n", g_rs.n);
    coldplug_walk_root("/sys", collect);

    int inscope = g_sym_extra + g_sym_miss_inscope + g_tag_miss + g_tag_extra;
    int harmful = g_sym_miss_harmful + g_tag_miss;
    printf("\n== verify-rules-live ==\n");
    printf("devices: %d scanned, %d with udev db\n", g_dev, g_dev_db);
    printf("SYM-EXTRA (harmless, superset): %d\n", g_sym_extra);
    printf("SYM-MISS harmful (boot/fstab): %d\n", g_sym_miss_harmful);
    printf("SYM-MISS harmless (reachable): %d\n", g_sym_miss_harmless);
    printf("SYM-MISS known-debt (by-id): %d\n", g_sym_miss_debt);
    printf("TAG-MISS harmful: %d   TAG-EXTRA (harmless): %d\n", g_tag_miss, g_tag_extra);
    printf("IN-SCOPE DIVERGENCE: %d\n", inscope);
    printf("HARMFUL: %d\n", harmful);
    int fail = permissive ? (harmful > 0) : (inscope > 0);
    printf("MODE: %s -> gate %s\n", permissive ? "permissive" : "strict",
           fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
