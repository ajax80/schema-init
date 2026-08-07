/* Read-only parity harness: diff schema-udev's synthesized properties against
 * real systemd-udevd's /run/udev/data. Writes nothing. */
#include "../udev-parity.h"
#include "../udev_rules.h"   /* run_builtins + run_rules */
#include <stdio.h>
#include <string.h>

#define MAX_MISSING 512
#define MAX_SUBS    64

static struct keycount g_missing[MAX_MISSING];
static int g_nmissing = 0;

struct subrow { char sub[UE_KEY_MAX]; int devices, with_db, ekeys, reproduced; };
static struct subrow g_subs[MAX_SUBS];
static int g_nsubs = 0;
static int g_total = 0, g_total_db = 0, g_mismatch = 0;

static struct subrow *sub_row(const char *sub) {
    int i;
    for (i = 0; i < g_nsubs; i++)
        if (strcmp(g_subs[i].sub, sub) == 0) return &g_subs[i];
    if (g_nsubs < MAX_SUBS) {
        safe_copy(g_subs[g_nsubs].sub, sub, UE_KEY_MAX);
        g_subs[g_nsubs].devices = g_subs[g_nsubs].with_db = 0;
        g_subs[g_nsubs].ekeys = g_subs[g_nsubs].reproduced = 0;
        return &g_subs[g_nsubs++];
    }
    return NULL;
}

static void collect(struct uevent *ev_in) {
    struct uevent ev = *ev_in;   /* mutable copy: run builtins + rules on it */
    const char *devpath = uevent_get(&ev, "DEVPATH");
    if (devpath) {
        const char *devname = uevent_get(&ev, "DEVNAME");
        char devnode[UE_VAL_MAX]; const char *dn = NULL;
        if (devname) { snprintf(devnode, sizeof devnode, "/dev/%s", devname); dn = devnode; }
        run_builtins("/sys", devpath, dn, &ev);
        run_rules("/sys", devpath, dn, &ev);
    }
    const char *sub = uevent_get(&ev, "SUBSYSTEM");
    if (!sub) sub = "(none)";
    struct subrow *row = sub_row(sub);
    g_total++;
    if (row) row->devices++;

    char key[128];
    if (udev_db_filename(&ev, key, sizeof key) != 0) return;
    char path[256];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", UDEV_DB_DIR, key) >= sizeof path) return;

    struct uevent dbev;
    if (udev_db_read_eprops(path, &dbev) != 0) return;   /* no db entry for this device */
    g_total_db++;
    if (row) row->with_db++;

    int j;
    for (j = 0; j < dbev.n; j++) {
        if (row) row->ekeys++;
        const char *have = uevent_get(&ev, dbev.key[j]);
        if (have) {
            if (row) row->reproduced++;
            if (strcmp(have, dbev.val[j]) != 0) {
                printf("VALMIS %s %s: ours='%s' theirs='%s'\n", key, dbev.key[j], have, dbev.val[j]);
                g_mismatch++;
            }
        } else {
            keycount_add(g_missing, &g_nmissing, MAX_MISSING, dbev.key[j]);
        }
    }
}

int main(void) {
    coldplug_walk_root("/sys", collect);
    keycount_sort_desc(g_missing, g_nmissing);

    printf("== schema-udev vs %s parity ==\n", UDEV_DB_DIR);
    printf("Scanned %d devices, %d with a udev db entry, across %d subsystems.\n\n",
           g_total, g_total_db, g_nsubs);

    printf("Per subsystem (devices / with-db / E: keys / reproduced by schema-udev):\n");
    {
        int i;
        for (i = 0; i < g_nsubs; i++)
            printf("  %-12s %4d / %4d / %4d / %d\n", g_subs[i].sub,
                   g_subs[i].devices, g_subs[i].with_db, g_subs[i].ekeys, g_subs[i].reproduced);
    }

    printf("\nTOP MISSING PROPERTIES (udev E: keys, by device count):\n");
    {
        int i;
        for (i = 0; i < g_nmissing; i++) {
            const char *hint = parity_builtin_hint(g_missing[i].key);
            if (hint[0])
                printf("  %-28s %4d   [%s]\n", g_missing[i].key, g_missing[i].count, hint);
            else
                printf("  %-28s %4d\n", g_missing[i].key, g_missing[i].count);
        }
    }

    printf("\nVALUE MISMATCHES (keys in both, differing value): %d\n", g_mismatch);
    return 0;
}
