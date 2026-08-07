# schema-udev rules engine (sub-project B, slice 1: property completeness) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After `run_builtins()` runs per device, add udev's two remaining property-derivation mechanisms — `IMPORT{parent}` inheritance and composite/constructed hwdb keys — so schema-udev's full per-device property set matches real udevd's `/run/udev/data` `E:` lines for all devices, 0 missing + 0 mismatch (scoped to the six reimplemented builtins).

**Architecture:** One new header `udev_rules.h` exposing `run_rules(sysroot, devpath, devnode, ev)`, called once in `dispatch()` immediately after `run_builtins()`. Pass 1 walks the ancestor chain (nearest-first) and inherits a bounded `ID_*` key-set the child lacks. Pass 2 constructs the synthetic modalias udev builds per device class, queries hwdb, and merges `*_FROM_DATABASE` keys. All additions are first-writer-wins via A's `ub_add`. Fully inert: computes/attaches properties, fires nothing. The measurement instrument and acceptance engine is `tools/udev-parity.c`, fixed to run the builtins+rules before diffing vs the live db.

**Tech Stack:** C99, header-only inline modules (matches A). Reuses `udev_builtins.h` (`run_builtins`, `ub_add`, `ub_absorb`), `hwdb.h` (`hwdb_open`/`hwdb_query`/`hwdb_close`), `path_id.h` (`pi_parent`/`pi_sysattr`), `schema-udev.h` (`uevent_get`/`uevent_from_sysfs`/`safe_copy`).

## Global Constraints

- **Boundary discipline:** `schema-udev.c` gains exactly ONE new call site (`run_rules` after `run_builtins`). `schema-udev.h` is untouched. Keep all new logic in `udev_rules.h`.
- **Inertness:** no new symlink, hook, or broadcast may fire. B only computes and attaches properties.
- **Merge:** every addition goes through `ub_add()` (first-writer-wins). Never overwrite the kernel payload, a builtin's output, or a nearer ancestor's value.
- **Empirical scope:** the inheritable-key set (Task 3) and composite classes (Task 4) are bounded by the measured gap (Task 2). Do not inherit all `ID_*` blindly; do not add composite classes with no local device.
- **Honest gate:** the live gate compares every in-scope device and key; a shrinking device count is a hollow-gate smell (this is how A's fake gate was caught). Exclusions (`ata_id`/`scsi_id`/`cdrom_id`/`v4l_id`/`mtd`, runtime keys) are documented in the script, not silent.
- **Compiler:** `-Wall -Wextra` clean under the existing `$(CFLAGS)`.
- **hwdb path:** trie lives at `/etc/udev/hwdb.bin` then `/usr/lib/udev/hwdb.bin` (same fallback order as `hwdb_build`).

---

### Task 1: Scaffold `udev_rules.h` and wire it into dispatch

**Files:**
- Create: `udev_rules.h`
- Modify: `schema-udev.c:4` (include) and `schema-udev.c:90` (call site)
- Create: `tests/test_udev_rules.c`
- Modify: `Makefile:110` (add test line after `test_udev_builtins`)

**Interfaces:**
- Produces: `int run_rules(const char *sysroot, const char *devpath, const char *devnode, struct uevent *ev)` — returns count of keys added; skeleton returns 0.

- [ ] **Step 1: Write the failing test** — `tests/test_udev_rules.c`:

```c
#include "udev_rules.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- synthetic sysfs builder (same pattern as test_udev_builtins.c) --- */
static char ROOT[64];
static void root_make(void) { strcpy(ROOT, "/tmp/urtestXXXXXX"); assert(mkdtemp(ROOT)); }
static void mkdirs(const char *rel) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s%s", ROOT, rel);
    for (char *s = p + strlen(ROOT) + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}
static void writef(const char *rel, const char *body) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s%s", ROOT, rel);
    FILE *f = fopen(p, "w"); assert(f); fputs(body, f); fclose(f);
}
static void set_subsystem(const char *devrel, const char *name) {
    char busrel[PATH_MAX]; snprintf(busrel, sizeof busrel, "/bus/%s", name); mkdirs(busrel);
    char linkp[PATH_MAX], target[PATH_MAX];
    snprintf(linkp, sizeof linkp, "%s%s/subsystem", ROOT, devrel);
    snprintf(target, sizeof target, "%s/bus/%s", ROOT, name);
    unlink(linkp); assert(symlink(target, linkp) == 0);
}
static const char *getval(const struct uevent *ev, const char *k) { return uevent_get(ev, k); }

static void test_inert_on_childless(void) {
    root_make();
    /* a lone device with a uevent file and no interesting ancestors/modalias */
    mkdirs("/devices/virtual/mem/null");
    writef("/devices/virtual/mem/null/uevent", "DEVTYPE=\nMAJOR=1\nMINOR=3\n");
    set_subsystem("/devices/virtual/mem/null", "mem");
    struct uevent ev; ev.n = 0;
    strcpy(ev.key[ev.n], "SUBSYSTEM"); strcpy(ev.val[ev.n], "mem"); ev.n++;
    strcpy(ev.key[ev.n], "DEVPATH");   strcpy(ev.val[ev.n], "/devices/virtual/mem/null"); ev.n++;
    int before = ev.n;
    int added = run_rules(ROOT, "/devices/virtual/mem/null", "/dev/null", &ev);
    assert(added == 0);
    assert(ev.n == before);
    (void)getval;
    printf("test_udev_rules inert: OK\n");
}

int main(void) {
    test_inert_on_childless();
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_rules.c -o /tmp/tur && /tmp/tur`
Expected: FAIL to compile — `udev_rules.h: No such file or directory`.

- [ ] **Step 3: Create the skeleton `udev_rules.h`**

```c
#ifndef UDEV_RULES_H
#define UDEV_RULES_H

#include "udev_builtins.h"   /* run_builtins, ub_add, ub_absorb, uevent_* , pi_* */
#include <string.h>
#include <limits.h>
#include <stdio.h>

/* Post-builtin property derivation: IMPORT{parent} inheritance + composite hwdb.
 * Additive, first-writer-wins; never overwrites existing keys. Returns count added. */
static inline int run_rules(const char *sysroot, const char *devpath,
                            const char *devnode, struct uevent *ev) {
    (void)sysroot; (void)devpath; (void)devnode; (void)ev;
    return 0;
}

#endif /* UDEV_RULES_H */
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_rules.c -o /tmp/tur && /tmp/tur`
Expected: PASS — `test_udev_rules inert: OK`.

- [ ] **Step 5: Wire into dispatch and Makefile**

In `schema-udev.c`, add the include after line 4 (`#include "udev_builtins.h"`):

```c
#include "udev_rules.h"
```

In `schema-udev.c` `dispatch()`, immediately after the existing `run_builtins("/sys", devpath, dn, ev);` (line 90), add:

```c
            run_rules("/sys", devpath, dn, ev);
```

In `Makefile`, after line 110 (`test_udev_builtins`), add:

```makefile
	$(CC) $(CFLAGS) tests/test_udev_rules.c -o /tmp/schema-test-ur && /tmp/schema-test-ur
```

- [ ] **Step 6: Build the daemon and run the suite**

Run: `make schema-udev && make test`
Expected: builds clean; suite green incl. `test_udev_rules inert: OK`.

- [ ] **Step 7: Commit**

```bash
git add udev_rules.h schema-udev.c tests/test_udev_rules.c Makefile
git commit -m "feat(rules-engine): scaffold run_rules + wire into dispatch (inert)"
```

---

### Task 2: Make `tools/udev-parity.c` measure the real post-A gap

**Files:**
- Modify: `tools/udev-parity.c` (`collect()` at lines 31-59; add include)

**Interfaces:**
- Consumes: `run_builtins`, `run_rules` (Task 1).
- Produces: a working `tools/udev-parity` binary whose output (per-subsystem reproduced counts + TOP MISSING PROPERTIES) is the empirical input to Tasks 3 and 4.

- [ ] **Step 1: Add the builtins+rules pass to `collect()`**

At the top of `tools/udev-parity.c`, change the include block to also pull the rules engine (it already includes `../udev-parity.h`, which includes `schema-udev.h`). Add after the existing `#include "../udev-parity.h"`:

```c
#include "../udev_rules.h"   /* run_builtins + run_rules */
```

In `collect()`, before the db comparison, enrich a mutable copy of `ev`. Replace the opening of `collect()`:

```c
static void collect(const struct uevent *ev) {
    const char *sub = uevent_get(ev, "SUBSYSTEM");
    if (!sub) sub = "(none)";
    struct subrow *row = sub_row(sub);
    g_total++;
    if (row) row->devices++;
```

with:

```c
static void collect(const struct uevent *ev_in) {
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
```

Then in the rest of `collect()`, every remaining `ev` reference must point at the local copy. The remaining references are `udev_db_filename(ev, ...)` and `uevent_get(ev, dbev.key[j])`. Change both to `&ev`:

```c
    char key[128];
    if (udev_db_filename(&ev, key, sizeof key) != 0) return;
```

```c
        const char *have = uevent_get(&ev, dbev.key[j]);
```

- [ ] **Step 2: Build the tool**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tools/udev-parity.c -o /tmp/udev-parity`
Expected: compiles clean.

- [ ] **Step 3: Measure the gap and record it**

Run: `sudo /tmp/udev-parity | tee /tmp/parity-baseline.txt`
Expected: prints per-subsystem `devices / with-db / E: keys / reproduced` and a `TOP MISSING PROPERTIES` list annotated with `[builtin]` hints, plus a `VALUE MISMATCHES` count.

Record, from `/tmp/parity-baseline.txt`, into the ledger/progress notes:
- **Inheritable-key set (for Task 3):** the missing keys whose `[hint]` is `path_id`, `usb_id`, or `hwdb` that appear on *child* devices whose own builtin does not emit them (expected: `ID_PATH`, `ID_PATH_TAG`, `ID_USB_*`, `*_FROM_DATABASE`).
- **Composite classes (for Task 4):** the missing `*_FROM_DATABASE` keys that persist on devices whose OWN `modalias` is empty or is a constructed form — grouped by subsystem (expected: usb, net/OUI, acpi, pci). If a class shows no missing key on this hardware, it is out of scope for this slice.

This task has no unit test (it is a diagnostic tool); its deliverable is the recorded gap. Do not proceed to Task 3 until the baseline is captured.

- [ ] **Step 4: Commit**

```bash
git add tools/udev-parity.c
git commit -m "feat(rules-engine): parity tool runs builtins+rules before diffing db"
```

---

### Task 3: Pass 1 — `IMPORT{parent}` inheritance

**Files:**
- Modify: `udev_rules.h`
- Modify: `tests/test_udev_rules.c`

**Interfaces:**
- Consumes: the inheritable-key set recorded in Task 2.
- Produces: `run_rules()` now inherits selected `ID_*` keys from ancestors.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_udev_rules.c` and call from `main`:

```c
/* child (hidraw) under a usb_device parent that carries ID_USB_VENDOR-style props.
 * We seed the PARENT with a modalias so hwdb/usb-derived keys exist to inherit,
 * and assert the child inherits ID_PATH from the ancestor chain. */
static void test_inherit_id_path(void) {
    root_make();
    mkdirs("/devices/pci0000:00/0000:00:14.0/usb1/1-2");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/uevent",
           "DEVTYPE=usb_device\n");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/modalias", "usb:v1234p5678\n");
    set_subsystem("/devices/pci0000:00/0000:00:14.0", "pci");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1", "usb");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1/1-2", "usb");
    /* child hidraw device */
    mkdirs("/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/hidraw/hidraw5");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/hidraw/hidraw5/uevent",
           "DEVTYPE=\n");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/hidraw/hidraw5", "hidraw");

    struct uevent ev; ev.n = 0;
    strcpy(ev.key[ev.n], "SUBSYSTEM"); strcpy(ev.val[ev.n], "hidraw"); ev.n++;
    strcpy(ev.key[ev.n], "DEVPATH");
    strcpy(ev.val[ev.n], "/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/hidraw/hidraw5");
    ev.n++;
    run_rules(ROOT, ev.val[1], NULL, &ev);
    const char *idp = uevent_get(&ev, "ID_PATH");
    assert(idp != NULL);            /* inherited from the usb ancestor's path_id */
    printf("test_udev_rules inherit ID_PATH: OK\n");
}

/* first-writer-wins: a child that already has its own ID_PATH keeps it */
static void test_inherit_first_writer_wins(void) {
    root_make();
    mkdirs("/devices/pci0000:00/0000:00:14.0/usb1/1-2");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/uevent", "DEVTYPE=usb_device\n");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/modalias", "usb:v1234p5678\n");
    set_subsystem("/devices/pci0000:00/0000:00:14.0", "pci");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1", "usb");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1/1-2", "usb");
    mkdirs("/devices/pci0000:00/0000:00:14.0/usb1/1-2/child");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/child/uevent", "DEVTYPE=\n");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1/1-2/child", "hidraw");

    struct uevent ev; ev.n = 0;
    strcpy(ev.key[ev.n], "SUBSYSTEM"); strcpy(ev.val[ev.n], "hidraw"); ev.n++;
    strcpy(ev.key[ev.n], "DEVPATH");
    strcpy(ev.val[ev.n], "/devices/pci0000:00/0000:00:14.0/usb1/1-2/child"); ev.n++;
    strcpy(ev.key[ev.n], "ID_PATH"); strcpy(ev.val[ev.n], "MINE-DO-NOT-REPLACE"); ev.n++;
    run_rules(ROOT, ev.val[1], NULL, &ev);
    assert(strcmp(uevent_get(&ev, "ID_PATH"), "MINE-DO-NOT-REPLACE") == 0);
    printf("test_udev_rules first-writer-wins: OK\n");
}

/* a non-inheritable key on the ancestor is NOT copied down */
static void test_inherit_bounded_set(void) {
    root_make();
    mkdirs("/devices/pci0000:00/0000:00:14.0/usb1/1-2");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/uevent",
           "DEVTYPE=usb_device\nID_NONINHERIT=nope\n");
    set_subsystem("/devices/pci0000:00/0000:00:14.0", "pci");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1", "usb");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1/1-2", "usb");
    mkdirs("/devices/pci0000:00/0000:00:14.0/usb1/1-2/child");
    writef("/devices/pci0000:00/0000:00:14.0/usb1/1-2/child/uevent", "DEVTYPE=\n");
    set_subsystem("/devices/pci0000:00/0000:00:14.0/usb1/1-2/child", "hidraw");

    struct uevent ev; ev.n = 0;
    strcpy(ev.key[ev.n], "SUBSYSTEM"); strcpy(ev.val[ev.n], "hidraw"); ev.n++;
    strcpy(ev.key[ev.n], "DEVPATH");
    strcpy(ev.val[ev.n], "/devices/pci0000:00/0000:00:14.0/usb1/1-2/child"); ev.n++;
    run_rules(ROOT, ev.val[1], NULL, &ev);
    assert(uevent_get(&ev, "ID_NONINHERIT") == NULL);
    printf("test_udev_rules bounded-set: OK\n");
}
```

Add to `main()` before `return 0;`:

```c
    test_inherit_id_path();
    test_inherit_first_writer_wins();
    test_inherit_bounded_set();
```

- [ ] **Step 2: Run to verify failure**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_rules.c -o /tmp/tur && /tmp/tur`
Expected: FAIL — `inherit ID_PATH` assertion (`idp != NULL`) fails; skeleton `run_rules` adds nothing.

- [ ] **Step 3: Implement Pass 1 in `udev_rules.h`**

Insert before `run_rules`, and fill `run_rules`:

```c
/* Keys udev propagates from an ancestor to a child (IMPORT{parent}).
 * Bounded set — confirmed by the Task 2 parity measurement. */
static inline int rules_inheritable(const char *key) {
    return strcmp(key, "ID_PATH") == 0 ||
           strcmp(key, "ID_PATH_TAG") == 0 ||
           strncmp(key, "ID_USB_", 7) == 0 ||
           strstr(key, "_FROM_DATABASE") != NULL;
}

/* Walk ancestors nearest-first; for each real ancestor device, compute its
 * builtin properties and inherit the bounded ID_* keys the child lacks. */
static inline int rules_import_parent(const char *sysroot, const char *devpath,
                                      struct uevent *ev) {
    int before = ev->n;
    char cur[PATH_MAX];
    if ((size_t)snprintf(cur, sizeof cur, "%s", devpath) >= sizeof cur) return 0;
    while (pi_parent(cur) == 0) {
        /* stop once we've climbed above /devices */
        if (strncmp(cur, "/devices", 8) != 0) break;
        char sysdir[PATH_MAX];
        if ((size_t)snprintf(sysdir, sizeof sysdir, "%s%s", sysroot, cur) >= sizeof sysdir) break;
        struct uevent anc;
        if (uevent_from_sysfs(sysroot, sysdir, &anc) != 0) continue;  /* not a device dir */
        run_builtins(sysroot, cur, NULL, &anc);
        for (int i = 0; i < anc.n; i++)
            if (rules_inheritable(anc.key[i]))
                ub_add(ev, anc.key[i], anc.val[i]);   /* first-writer-wins */
    }
    return ev->n - before;
}
```

And replace the body of `run_rules`:

```c
static inline int run_rules(const char *sysroot, const char *devpath,
                            const char *devnode, struct uevent *ev) {
    (void)devnode;
    int before = ev->n;
    rules_import_parent(sysroot, devpath, ev);
    return ev->n - before;
}
```

- [ ] **Step 4: Run unit tests to verify pass**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_rules.c -o /tmp/tur && /tmp/tur`
Expected: PASS — `inherit ID_PATH`, `first-writer-wins`, `bounded-set` all OK.

- [ ] **Step 5: Re-measure parity**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tools/udev-parity.c -o /tmp/udev-parity && sudo /tmp/udev-parity | tee /tmp/parity-after-t3.txt`
Expected: `path_id`/`usb_id`-hinted MISSING keys on child devices drop sharply vs `/tmp/parity-baseline.txt`. Remaining missing should be dominated by `*_FROM_DATABASE` (composite hwdb = Task 4) and out-of-scope builtins. Record the residual.

If the inheritable set proved too narrow/broad (real gap differs from expectation), adjust `rules_inheritable()` to exactly the keys the measurement shows udev inherits, and re-run Steps 4-5.

- [ ] **Step 6: Commit**

```bash
git add udev_rules.h tests/test_udev_rules.c
git commit -m "feat(rules-engine): IMPORT{parent} inheritance (bounded ID_* set)"
```

---

### Task 4: Pass 2 — composite/constructed hwdb keys

**Files:**
- Modify: `udev_rules.h`
- Modify: `tests/test_udev_rules.c`

**Interfaces:**
- Consumes: the composite-class list recorded in Task 2; `hwdb_open`/`hwdb_query`/`hwdb_close` from `hwdb.h`.
- Produces: `run_rules()` now attaches `*_FROM_DATABASE` keys from constructed modaliases.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_udev_rules.c` and call from `main`:

```c
/* composite usb modalias is built as usb:vVVVVpPPPP from idVendor/idProduct */
static void test_composite_usb_modalias(void) {
    char out[128];
    assert(rules_usb_modalias("1d6b", "0002", NULL, out, sizeof out) == 0);
    assert(strcmp(out, "usb:v1D6Bp0002") == 0);
    /* with bcdDevice -> appends dVVVV */
    assert(rules_usb_modalias("1d6b", "0002", "0410", out, sizeof out) == 0);
    assert(strcmp(out, "usb:v1D6Bp0002d0410") == 0);
    printf("test_udev_rules composite usb modalias: OK\n");
}

/* OUI lookup key from a MAC address is OUI:XXXXXX (first 3 octets, upper, no colons) */
static void test_composite_oui_key(void) {
    char out[32];
    assert(rules_oui_key("00:1a:2b:3c:4d:5e", out, sizeof out) == 0);
    assert(strcmp(out, "OUI:001A2B") == 0);
    printf("test_udev_rules composite OUI key: OK\n");
}
```

Add to `main()` before `return 0;`:

```c
    test_composite_usb_modalias();
    test_composite_oui_key();
```

- [ ] **Step 2: Run to verify failure**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_rules.c -o /tmp/tur && /tmp/tur`
Expected: FAIL to compile — `rules_usb_modalias`/`rules_oui_key` undefined.

- [ ] **Step 3: Implement the composite helpers + Pass 2 in `udev_rules.h`**

Add `#include <ctype.h>` to the includes. Insert these helpers before `run_rules` (implement exactly the classes the Task 2 gap shows; the code below covers usb + OUI + acpi + pci — drop any class the measurement proves absent on this hardware):

```c
/* Build usb:vVVVVpPPPP[dDDDD] from idVendor/idProduct[/bcdDevice], hex upper. */
static inline int rules_usb_modalias(const char *vend, const char *prod,
                                     const char *bcd, char *out, size_t outsz) {
    if (!vend || !prod) return -1;
    int w = snprintf(out, outsz, "usb:v%04Xp%04X",
                     (unsigned)strtoul(vend, NULL, 16), (unsigned)strtoul(prod, NULL, 16));
    if (w < 0 || (size_t)w >= outsz) return -1;
    if (bcd && bcd[0]) {
        int w2 = snprintf(out + w, outsz - w, "d%04X", (unsigned)strtoul(bcd, NULL, 16));
        if (w2 < 0 || (size_t)(w + w2) >= outsz) return -1;
    }
    return 0;
}

/* Build OUI:XXXXXX from the first three octets of a MAC (upper, no separators). */
static inline int rules_oui_key(const char *mac, char *out, size_t outsz) {
    if (!mac) return -1;
    char hex[7]; int h = 0;
    for (const char *p = mac; *p && h < 6; p++)
        if (isxdigit((unsigned char)*p)) hex[h++] = (char)toupper((unsigned char)*p);
    if (h < 6) return -1;
    hex[6] = '\0';
    int w = snprintf(out, outsz, "OUI:%s", hex);
    return (w > 0 && (size_t)w < outsz) ? 0 : -1;
}

/* Query the hwdb trie with an arbitrary constructed key, merge results (first-writer-wins). */
static inline void rules_hwdb_lookup(const char *key, struct uevent *ev) {
    struct hwdb h;
    if (hwdb_open("/etc/udev/hwdb.bin", &h) != 0 &&
        hwdb_open("/usr/lib/udev/hwdb.bin", &h) != 0) return;
    struct uevent tmp; tmp.n = 0;
    hwdb_query(&h, key, &tmp);
    hwdb_close(&h);
    ub_absorb(ev, &tmp);
}

/* Construct the per-class synthetic modalias and merge its hwdb result. */
static inline int rules_composite_hwdb(const char *sysroot, const char *devpath,
                                       struct uevent *ev) {
    int before = ev->n;
    const char *sub = uevent_get(ev, "SUBSYSTEM");
    char devdir[PATH_MAX];
    if ((size_t)snprintf(devdir, sizeof devdir, "%s%s", sysroot, devpath) >= sizeof devdir)
        return 0;
    char a[256], b[256], c[256], key[512];

    if (sub && strcmp(sub, "usb") == 0 &&
        pi_sysattr(devdir, "idVendor", a, sizeof a) == 0 &&
        pi_sysattr(devdir, "idProduct", b, sizeof b) == 0) {
        const char *bcd = (pi_sysattr(devdir, "bcdDevice", c, sizeof c) == 0) ? c : NULL;
        if (rules_usb_modalias(a, b, bcd, key, sizeof key) == 0) rules_hwdb_lookup(key, ev);
    }
    if (sub && strcmp(sub, "net") == 0 &&
        pi_sysattr(devdir, "address", a, sizeof a) == 0 &&
        rules_oui_key(a, key, sizeof key) == 0) {
        rules_hwdb_lookup(key, ev);
    }
    return ev->n - before;
}
```

Then extend `run_rules` to call Pass 2 after Pass 1:

```c
static inline int run_rules(const char *sysroot, const char *devpath,
                            const char *devnode, struct uevent *ev) {
    (void)devnode;
    int before = ev->n;
    rules_import_parent(sysroot, devpath, ev);
    rules_composite_hwdb(sysroot, devpath, ev);
    return ev->n - before;
}
```

- [ ] **Step 4: Run unit tests to verify pass**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_rules.c -o /tmp/tur && /tmp/tur`
Expected: PASS — composite usb modalias + OUI key OK, all prior tests still OK.

- [ ] **Step 5: Re-measure parity**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tools/udev-parity.c -o /tmp/udev-parity && sudo /tmp/udev-parity | tee /tmp/parity-after-t4.txt`
Expected: `*_FROM_DATABASE` MISSING keys drop toward 0; residual missing should be only out-of-scope builtins (`ata_id`/`scsi_id`/`cdrom_id`/`v4l_id`/`mtd`) and runtime keys. `VALUE MISMATCHES: 0`.

If a measured composite class from Task 2 is still missing (e.g. acpi/pci), add its constructor to `rules_composite_hwdb` following the same pattern (read the class's identifying sysattrs, build the modalias string udev uses, `rules_hwdb_lookup`), and re-run Steps 4-5. If a class in the code has no local device, leave it — it is inert.

- [ ] **Step 6: Commit**

```bash
git add udev_rules.h tests/test_udev_rules.c
git commit -m "feat(rules-engine): composite/constructed hwdb keys (measured classes)"
```

---

### Task 5: Honest full-device live gate + green suite + vmtest

**Files:**
- Create: `tests/verify_rules_live.sh`
- Modify: `Makefile` (no change needed if the gate is run standalone; do NOT add sudo to `make test`)

**Interfaces:**
- Consumes: `tools/udev-parity` (Tasks 2-4).

- [ ] **Step 1: Write the gate** — `tests/verify_rules_live.sh`:

```sh
#!/bin/sh
# Full-device parity gate for the rules engine (sub-project B slice 1).
#
# Proves run_builtins()+run_rules() reproduces every /run/udev/data E: key that
# is OWNED BY THE SIX REIMPLEMENTED BUILTINS (and their inheritance/composites),
# on ALL devices with a udev db entry, 0 missing + 0 mismatch.
#
# HONEST SCOPE — excluded (documented, not hidden):
#   - keys owned by builtins not yet reimplemented: ata_id/scsi_id/cdrom_id
#     (ID_ATA_*, and ID_SERIAL/ID_MODEL/ID_VENDOR on non-usb block/optical),
#     v4l_id (ID_V4L_*/ID_VIDEO_*), mtd_probe;
#   - pure-runtime/db keys: USEC_INITIALIZED, tags, seat bookkeeping.
# The tool's parity_builtin_hint() attributes each E: key to its owning builtin;
# this gate asserts 0 missing/0 mismatch for in-scope keys and prints the device
# count so a hollow (shrunk) comparison is visible. sudo: blkid reads raw block.
set -eu
cd "$(dirname "$0")/.."

gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tools/udev-parity.c -o /tmp/udev-parity
out=$(sudo /tmp/udev-parity)
echo "$out"

# device-count smell: a real run scans hundreds of /sys devices
scanned=$(echo "$out" | sed -n 's/^Scanned \([0-9]*\) devices.*/\1/p')
[ "${scanned:-0}" -ge 50 ] || { echo "HOLLOW GATE: only $scanned devices scanned"; exit 1; }

# value mismatches must be zero
mm=$(echo "$out" | sed -n 's/^VALUE MISMATCHES.*: \([0-9]*\)$/\1/p')
[ "${mm:-1}" -eq 0 ] || { echo "FAIL: $mm value mismatch(es)"; exit 1; }

# in-scope missing must be zero: any TOP MISSING line whose [hint] is one of the
# six reimplemented builtins is a real gap. Lines with no hint, or hints for
# not-yet-reimplemented builtins (v4l_id), are out of scope.
inscope_missing=$(echo "$out" | grep -E '\[(path_id|usb_id|input_id|net_id|blkid|hwdb)\]' || true)
if [ -n "$inscope_missing" ]; then
    echo "FAIL: in-scope missing keys:"; echo "$inscope_missing"; exit 1
fi
echo "PASS: full-device parity, 0 in-scope missing, 0 mismatch across $scanned devices"
```

Make it executable: `chmod +x tests/verify_rules_live.sh`.

- [ ] **Step 2: Run the gate**

Run: `tests/verify_rules_live.sh`
Expected: ends `PASS: full-device parity, 0 in-scope missing, 0 mismatch across <N> devices` with N ≥ 50.

If it fails on an in-scope key, that key is a genuine gap — return to Task 3 (if it's a `path_id`/`usb_id`/`hwdb` inherited key) or Task 4 (if it's a composite `*_FROM_DATABASE`), fix, and re-run. Do NOT narrow the gate to make it pass.

- [ ] **Step 3: Full suite green**

Run: `make test`
Expected: entire suite green including `test_udev_rules`, `-Wall -Wextra` clean.

- [ ] **Step 4: vmtest rail unchanged**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS` (schema-udev is not PID 1; the boot rail must be unaffected by this change).

- [ ] **Step 5: Commit**

```bash
git add tests/verify_rules_live.sh
git commit -m "test(rules-engine): honest full-device parity live gate"
```

---

## Self-Review

**Spec coverage:**
- `IMPORT{parent}` inheritance → Task 3. ✓
- Composite/constructed hwdb → Task 4. ✓
- Fix `udev-parity.c` to run builtins+rules → Task 2. ✓
- New `udev_rules.h`, one call site in `dispatch()`, `schema-udev.h` untouched → Task 1. ✓
- Inertness (no symlink/hook/broadcast) → nothing in any task fires an action; `dispatch()`'s existing action path is unchanged. ✓
- First-writer-wins merge → `ub_add`/`ub_absorb` used throughout Tasks 3-4. ✓
- Honest full-device gate with documented exclusions + hollow-gate guard → Task 5. ✓
- `make test` green + vmtest PASS → Task 5. ✓

**Placeholder scan:** the empirical steps (Task 2 measurement; Task 3/4 "adjust to the measured set") are concrete actions with concrete commands and a concrete derivation rule, not TODOs. Starting sets are pre-seeded from the A findings so no task begins from a blank. ✓

**Type consistency:** `run_rules(const char*, const char*, const char*, struct uevent*)` identical in Tasks 1/2/3/4. `rules_inheritable(const char*)`, `rules_import_parent(...)`, `rules_usb_modalias(vend,prod,bcd,out,outsz)`, `rules_oui_key(mac,out,outsz)`, `rules_hwdb_lookup(key,ev)`, `rules_composite_hwdb(sysroot,devpath,ev)` — each defined once and used consistently. `ub_add`/`ub_absorb`/`uevent_get`/`pi_sysattr`/`pi_parent`/`uevent_from_sysfs`/`hwdb_open`/`hwdb_query`/`hwdb_close` match their real signatures in the tree. ✓
