# udev parity/shadow harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone read-only tool that diffs schema-udev's synthesized device properties against real systemd-udevd's `/run/udev/data`, producing a ranked "what udev adds that we lack" worklist.

**Architecture:** Pure, unit-tested helpers in a new header `udev-parity.h` (includes `schema-udev.h`); a thin orchestration tool `tools/udev-parity.c` that walks `/sys` via the existing `coldplug_walk_root`, reads each device's udev db entry, aggregates the gap, and prints a report. Nothing is written; `schema-udev.c`/`schema-udev.h` are unchanged.

**Tech Stack:** C99, gcc `-Wall -Wextra -D_GNU_SOURCE`, reuses `struct uevent`, `uevent_get`, `safe_copy`, `coldplug_walk_root`, `udev_db_filename` from `schema-udev.h`.

## Global Constraints

- `schema-udev.c` and `schema-udev.h` are **not modified** — the harness only consumes the header's existing inline functions.
- Read-only: no netlink, no `/sys` writes, no writes to `/run/udev`. Only reads `/sys` and `/run/udev/data`.
- Real udev db keys to parse: lines beginning `E:` (`E:KEY=value`). Ignore `V:`/`I:`/`G:`/`Q:`/`S:`/`L:`/`W:`.
- `UDEV_DB_DIR` = `/run/udev/data`.
- New pure logic is `static inline` in `udev-parity.h`; follow `schema-udev.h` style (no comments unless a non-obvious invariant).
- Every task ends green under `make test`, `-Wall -Wextra` clean.

---

### Task 1: pure parity helpers + unit tests

**Files:**
- Create: `udev-parity.h`
- Create: `tests/test_parity.c`

**Interfaces:**
- Consumes: `struct uevent`, `UE_KEY_MAX`, `UE_VAL_MAX`, `UE_MAX_KEYS`, `safe_copy`, `uevent_get` (from `schema-udev.h`).
- Produces:
  - `int udev_db_read_eprops(const char *path, struct uevent *out)` — parse `E:` lines of a db file into `out`; 0 ok, -1 unreadable.
  - `const char *parity_builtin_hint(const char *key)` — builtin/source name for a key, or `""`.
  - `struct keycount { char key[UE_KEY_MAX]; int count; }`
  - `void keycount_add(struct keycount *tab, int *n, int max, const char *key)`
  - `void keycount_sort_desc(struct keycount *tab, int n)`

- [ ] **Step 1: Write the failing test** — create `tests/test_parity.c`:

```c
#include "../udev-parity.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    /* udev_db_read_eprops: only E: lines captured, others ignored */
    char tmpl[] = "/tmp/parity-XXXXXX";
    char *dir = mkdtemp(tmpl); assert(dir);
    char p[256]; snprintf(p, sizeof p, "%s/c1:3", dir);
    FILE *f = fopen(p, "w");
    fputs("V:1\n", f);
    fputs("I:12345\n", f);
    fputs("E:ID_SERIAL=Foo_Bar_123\n", f);
    fputs("E:ID_INPUT=1\n", f);
    fputs("G:systemd\n", f);
    fputs("S:serial/by-id/xyz\n", f);
    fclose(f);

    struct uevent ev;
    assert(udev_db_read_eprops(p, &ev) == 0);
    assert(ev.n == 2);
    assert(strcmp(uevent_get(&ev, "ID_SERIAL"), "Foo_Bar_123") == 0);
    assert(strcmp(uevent_get(&ev, "ID_INPUT"), "1") == 0);
    assert(uevent_get(&ev, "V") == NULL && uevent_get(&ev, "G") == NULL);

    assert(udev_db_read_eprops("/no/such/file", &ev) == -1);

    /* parity_builtin_hint mapping (order-sensitive: _FROM_DATABASE wins) */
    assert(strcmp(parity_builtin_hint("ID_INPUT_KEYBOARD"), "input_id") == 0);
    assert(strcmp(parity_builtin_hint("ID_NET_NAME_PATH"), "net_id") == 0);
    assert(strcmp(parity_builtin_hint("ID_FS_UUID"), "blkid") == 0);
    assert(strcmp(parity_builtin_hint("ID_SERIAL"), "usb_id") == 0);
    assert(strcmp(parity_builtin_hint("ID_VENDOR_FROM_DATABASE"), "hwdb") == 0);
    assert(strcmp(parity_builtin_hint("ID_PATH"), "path_id") == 0);
    assert(strcmp(parity_builtin_hint("DEVNAME"), "") == 0);

    /* keycount add + sort */
    struct keycount tab[8]; int n = 0;
    keycount_add(tab, &n, 8, "ID_INPUT");
    keycount_add(tab, &n, 8, "ID_SERIAL");
    keycount_add(tab, &n, 8, "ID_INPUT");
    assert(n == 2);
    keycount_sort_desc(tab, n);
    assert(strcmp(tab[0].key, "ID_INPUT") == 0 && tab[0].count == 2);
    assert(strcmp(tab[1].key, "ID_SERIAL") == 0 && tab[1].count == 1);

    unlink(p); rmdir(dir);
    printf("test_parity: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run it, confirm it fails to compile** (header missing):

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_parity.c -o /tmp/t && /tmp/t`
Expected: FAIL — `udev-parity.h` not found / functions undefined.

- [ ] **Step 3: Create `udev-parity.h`:**

```c
#ifndef UDEV_PARITY_H
#define UDEV_PARITY_H

#include "schema-udev.h"
#include <string.h>
#include <stdio.h>

#define UDEV_DB_DIR "/run/udev/data"

static inline int udev_db_read_eprops(const char *path, struct uevent *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(out, 0, sizeof *out);
    char line[1024];
    while (fgets(line, sizeof line, f) && out->n < UE_MAX_KEYS) {
        if (line[0] != 'E' || line[1] != ':') continue;
        char *kv = line + 2;
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';
        safe_copy(out->key[out->n], kv, UE_KEY_MAX);
        safe_copy(out->val[out->n], val, UE_VAL_MAX);
        out->n++;
    }
    fclose(f);
    return 0;
}

static inline const char *parity_builtin_hint(const char *key) {
    if (strstr(key, "_FROM_DATABASE")) return "hwdb";
    if (strncmp(key, "ID_INPUT", 8) == 0) return "input_id";
    if (strncmp(key, "ID_NET", 6) == 0) return "net_id";
    if (strncmp(key, "ID_FS", 5) == 0 || strncmp(key, "ID_PART", 7) == 0) return "blkid";
    if (strncmp(key, "ID_PATH", 7) == 0) return "path_id";
    if (strncmp(key, "ID_V4L", 6) == 0 || strncmp(key, "ID_VIDEO", 8) == 0) return "v4l_id";
    if (strncmp(key, "ID_SERIAL", 9) == 0 || strncmp(key, "ID_USB", 6) == 0 ||
        strncmp(key, "ID_MODEL", 8) == 0 || strncmp(key, "ID_VENDOR", 9) == 0) return "usb_id";
    return "";
}

struct keycount { char key[UE_KEY_MAX]; int count; };

static inline void keycount_add(struct keycount *tab, int *n, int max, const char *key) {
    int i;
    for (i = 0; i < *n; i++)
        if (strcmp(tab[i].key, key) == 0) { tab[i].count++; return; }
    if (*n < max) {
        safe_copy(tab[*n].key, key, UE_KEY_MAX);
        tab[*n].count = 1;
        (*n)++;
    }
}

static inline void keycount_sort_desc(struct keycount *tab, int n) {
    int i, j;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (tab[j].count > tab[i].count) {
                struct keycount t = tab[i]; tab[i] = tab[j]; tab[j] = t;
            }
}

#endif /* UDEV_PARITY_H */
```

- [ ] **Step 4: Run the test, confirm pass**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_parity.c -o /tmp/t && /tmp/t`
Expected: `test_parity: OK`

- [ ] **Step 5: Commit**

```bash
git add udev-parity.h tests/test_parity.c
git commit -m "feat(udev-parity): pure db-eprops parser, builtin hints, keycount helpers"
```

---

### Task 2: the udev-parity tool + build wiring

**Files:**
- Create: `tools/udev-parity.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: everything from `udev-parity.h` + `coldplug_walk_root`, `uevent_from_sysfs` (indirectly), `udev_db_filename`, `uevent_get`.
- Produces: an executable `./udev-parity` that prints the parity report.

- [ ] **Step 1: Create `tools/udev-parity.c`:**

```c
/* Read-only parity harness: diff schema-udev's synthesized properties against
 * real systemd-udevd's /run/udev/data. Writes nothing. */
#include "../udev-parity.h"
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

static void collect(const struct uevent *ev) {
    const char *sub = uevent_get(ev, "SUBSYSTEM");
    if (!sub) sub = "(none)";
    struct subrow *row = sub_row(sub);
    g_total++;
    if (row) row->devices++;

    char key[128];
    if (udev_db_filename(ev, key, sizeof key) != 0) return;
    char path[256];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", UDEV_DB_DIR, key) >= sizeof path) return;

    struct uevent dbev;
    if (udev_db_read_eprops(path, &dbev) != 0) return;   /* no db entry for this device */
    g_total_db++;
    if (row) row->with_db++;

    int j;
    for (j = 0; j < dbev.n; j++) {
        if (row) row->ekeys++;
        const char *have = uevent_get(ev, dbev.key[j]);
        if (have) {
            if (row) row->reproduced++;
            if (strcmp(have, dbev.val[j]) != 0) g_mismatch++;
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
```

- [ ] **Step 2: Wire the Makefile.** Add `test_parity` to the `test` target (match the existing per-test recipe pattern):

```make
	$(CC) $(CFLAGS) tests/test_parity.c -o /tmp/schema-test-parity && /tmp/schema-test-parity
```

Add a standalone `parity` target (near other build targets):

```make
parity:
	$(CC) $(CFLAGS) tools/udev-parity.c -o udev-parity
```

- [ ] **Step 3: Build the tool clean**

Run: `make parity`
Expected: compiles `-Wall -Wextra` clean → `./udev-parity` produced.

- [ ] **Step 4: Run the full test suite**

Run: `make clean && make test`
Expected: all existing tests plus `test_parity: OK`, no warnings.

- [ ] **Step 5: Live smoke (report sanity)**

Run: `sudo ./udev-parity | head -40`
Expected: a per-subsystem table + a ranked TOP MISSING PROPERTIES list with builtin hints. Spot-check one line against `sudo cat /run/udev/data/<some-id>` to confirm an `E:` key it reports as missing really is present in udev's db and absent from the kernel uevent.

- [ ] **Step 6: Commit**

```bash
git add tools/udev-parity.c Makefile
git commit -m "feat(udev-parity): standalone parity harness tool + build wiring"
```

---

## Self-Review

**1. Spec coverage:** standalone tool ✓ (Task 2); pure helpers `udev_db_read_eprops`/`parity_builtin_hint`/`keycount_*` ✓ (Task 1); property/db parity only, frame parity excluded ✓; ranked missing-key report with builtin hints ✓ (Task 2 Step 1); per-subsystem summary ✓; `schema-udev.c`/`.h` untouched ✓ (Global Constraints; harness only includes the header); unit tests for parser/hints/aggregation ✓ (Task 1); live smoke ✓ (Task 2 Step 5).

**2. Placeholder scan:** none — every code step is complete.

**3. Type consistency:** `udev_db_read_eprops`→`int`; `parity_builtin_hint`→`const char *` (never NULL, `""` sentinel, so `hint[0]` is safe); `keycount_add`/`keycount_sort_desc`→`void`; `struct keycount` fields match between header, test, and tool. `udev_db_filename` signature matches its 3a definition (`(ev, out, size)` → 0/-1). Report uses `g_missing`/`g_subs` consistently.
