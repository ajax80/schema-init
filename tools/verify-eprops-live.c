/* Read-only E: property-fidelity gate: diff the native builtins+rule interpreter's
 * derived properties against real systemd-udevd /run/udev/data E: lines. Writes
 * nothing. This is the Part-B oracle the symlink/tag gate (verify-rules-live) is
 * blind to: post-flip our E: set is what libudev/sd-device consumers read.
 *
 * Categories per (key):
 *   E-MISS  theirs has key, ours doesn't                       (in-scope unless debt)
 *   E-DIFF  both have key, values differ                       (in-scope unless debt)
 *   E-EXTRA ours has derived key, theirs doesn't               (in-scope unless debt)
 * Prints a key-frequency histogram so the burn-down can be prioritized.
 * exit(1) on any in-scope divergence, exit(0) when clean. */
#include "../schema-udev.h"
#include "../udev_rules.h"     /* run_builtins */
#include "../udev_db.h"        /* udev_db_filename, udev_db_read_eprops, UDEV_DB_DIR */
#include "../udev_ruleset.h"   /* ruleset_load_dirs, dev_ctx, ruleset_apply */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static struct ruleset g_rs;
static int g_dev = 0, g_dev_db = 0;
static int g_miss = 0, g_diff = 0, g_extra = 0, g_debt = 0;

/* KNOWN-DEBT keys: structurally not yet emitted, tracked separately so the
 * in-scope number reflects what actually blocks the flip. Start empty-ish;
 * populate from the first run's histogram after triage with the architect. */
static int key_is_known_debt(const char *k) {
    /* by-id / persistent-serial family — same root cause as the by-id symlink
     * known-debt already accepted by verify-rules-live. */
    if (!strcmp(k, "ID_SERIAL") || !strcmp(k, "ID_SERIAL_SHORT") || !strcmp(k, "ID_WWN")) return 1;
    /* systemd-internal, not applicable to a schema-native device manager. */
    if (!strncmp(k, "SYSTEMD_", 8)) return 1;
    return 0;
}

/* key-frequency histogram over divergent keys */
#define HK 512
static char h_key[HK][UE_KEY_MAX];
static int  h_cnt[HK];
static int  h_n = 0;
static void hist_bump(const char *k) {
    for (int i = 0; i < h_n; i++) if (!strcmp(h_key[i], k)) { h_cnt[i]++; return; }
    if (h_n < HK) { safe_copy(h_key[h_n], k, UE_KEY_MAX); h_cnt[h_n] = 1; h_n++; }
}
static int hist_cmp(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    return h_cnt[ib] - h_cnt[ia];
}

static void collect(struct uevent *ev_in) {
    struct uevent ev = *ev_in;
    int kernel_n = ev.n;
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
    struct uevent theirs;
    if (udev_db_read_eprops(path, &theirs) != 0) return;
    g_dev_db++;

    /* theirs -> ours: MISS (absent) or DIFF (value mismatch) */
    for (int i = 0; i < theirs.n; i++) {
        const char *k = theirs.key[i], *tv = theirs.val[i];
        const char *ov = uevent_get(&ev, k);
        int debt = key_is_known_debt(k);
        if (!ov) {
            if (debt) { g_debt++; }
            else { printf("E-MISS   %-12s %s=%s\n", name, k, tv); g_miss++; hist_bump(k); }
        } else if (strcmp(ov, tv) != 0) {
            if (debt) { g_debt++; }
            else { printf("E-DIFF   %-12s %s  ours=%s  theirs=%s\n", name, k, ov, tv); g_diff++; hist_bump(k); }
        }
    }
    /* ours (derived only, >= kernel_n) -> theirs: EXTRA (theirs absent) */
    for (int i = kernel_n; i < ev.n; i++) {
        const char *k = ev.key[i];
        if (!k[0] || !ev.val[i][0]) continue;
        if (k[0] == '.') continue;                 /* private prop: never persisted by either side */
        if (uevent_get(&theirs, k)) continue;      /* present in theirs -> covered above */
        if (key_is_known_debt(k)) { g_debt++; continue; }
        printf("E-EXTRA  %-12s %s=%s\n", name, k, ev.val[i]);
        g_extra++; hist_bump(k);
    }
}

int main(void) {
    ruleset_load_dirs((const char *const[]){
        "/usr/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d" }, 3, &g_rs);
    printf("loaded %d native rule(s)\n", g_rs.n);
    coldplug_walk_root("/sys", collect);

    int inscope = g_miss + g_diff + g_extra;
    printf("\n== verify-eprops-live ==\n");
    printf("devices: %d scanned, %d with udev db\n", g_dev, g_dev_db);
    printf("E-MISS: %d   E-DIFF: %d   E-EXTRA: %d   (known-debt suppressed: %d)\n",
           g_miss, g_diff, g_extra, g_debt);

    if (h_n > 0) {
        int idx[HK]; for (int i = 0; i < h_n; i++) idx[i] = i;
        qsort(idx, h_n, sizeof idx[0], hist_cmp);
        printf("\n-- divergent-key histogram (top 30) --\n");
        for (int i = 0; i < h_n && i < 30; i++)
            printf("  %4d  %s\n", h_cnt[idx[i]], h_key[idx[i]]);
    }
    printf("\nIN-SCOPE E-DIVERGENCE: %d -> gate %s\n", inscope, inscope ? "FAIL" : "PASS");
    return inscope ? 1 : 0;
}
