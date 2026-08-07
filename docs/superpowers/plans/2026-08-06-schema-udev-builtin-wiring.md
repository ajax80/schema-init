# schema-udev builtin wiring (endgame sub-project A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** schema-udev runs the six merged builtins against each device with udev-faithful dispatch guards, merging their output into the event's property set (the `IMPORT{builtin}` equivalent), verified by 0-mismatch aggregate parity vs real udev across all `/sys` devices.

**Architecture:** A new header `udev_builtins.h` includes all six builtin headers and exposes two functions: `ub_select()` — a pure guard function returning a bitmask of which builtins apply to a device — and `run_builtins()` — thin dispatch that calls each selected builtin (each already appends into the `struct uevent`). `schema-udev.c` gains exactly one `run_builtins()` call site in `dispatch()`, before the rule-match loop. Compute-only/inert: no symlink/hook logic changes.

**Tech Stack:** C99, single-header builtins, libc `fnmatch`/`mmap`/`pread`. Tests are freestanding C compiled per the Makefile `test:` target; the live gate is a POSIX shell script driving `udevadm`.

## Global Constraints

- Boundary: `schema-udev.c` changes ONLY by the single `run_builtins()` call site; `schema-udev.h` changes ONLY by `UE_MAX_KEYS` 32→64. `git diff master` on those two files must show nothing else.
- Compute-only/inert: no new symlink creation or hook firing. Builtins only populate `ev`'s property set.
- Dispatch order is fixed: hwdb, path_id, usb_id, input_id, net_id, blkid (matches udev rule precedence).
- Builtins APPEND into `struct uevent *out` (they own their key namespaces); no cross-builtin key collisions occur on the fleet; `uevent_get` returns the first match. No separate merge/dedup code.
- Compile flags: `-O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I.` — must be warning-clean.
- Live gate: run `run_builtins` over every `/sys` device, diff the union of the six builtins' owned key-subsets vs `udevadm info -q property`, BOTH directions, under `sudo` (blkid raw reads). Exclude `ID_OUI_FROM_DATABASE`, `ID_NET_DRIVER`, `ID_FS_SIZE`, `ID_FS_BLOCKSIZE`, `ID_FS_LASTBLOCK`. Expect 0 mismatches.
- `vmtest.sh` must still print `RESULT: PASS` (schema-udev is not PID 1; the boot rail is unaffected).

### Builtin entry points (all `static inline`, all append into `out`)

```c
ssize_t path_id_build (const char *sysroot, const char *devpath, struct uevent *out);
int     usb_id_build  (const char *sysroot, const char *devpath, struct uevent *out);
int     input_id_build(const char *sysroot, const char *devpath, struct uevent *out);
int     net_id_build  (const char *sysroot, const char *devpath, struct uevent *out);
int     blkid_pt_build(const char *sysroot, const char *devpath, const char *devnode, struct uevent *out);
int     blkid_fs_build(const char *sysroot, const char *devpath, const char *devnode, struct uevent *out);
int     hwdb_build    (const char *sysroot, const char *devpath, struct uevent *out);
```

### Shared primitives (from `path_id.h`, already on master)

```c
int pi_subsystem(const char *devdir, char *out, size_t outsz);          /* basename of <devdir>/subsystem symlink target; 0 on success */
int pi_sysattr  (const char *devdir, const char *attr, char *out, size_t outsz); /* single-line sysfs read, strips \n; 0 on success */
int pi_parent   (char *cur);                                            /* truncate cur at last '/'; 0 on success */
const char *pi_base(const char *dir);                                   /* basename pointer */
```

### uevent API (from `schema-udev.h`)

```c
#define UE_MAX_KEYS 64   /* raised from 32 in Task 1 */
#define UE_KEY_MAX  64
#define UE_VAL_MAX  512
struct uevent { char key[UE_MAX_KEYS][UE_KEY_MAX]; char val[UE_MAX_KEYS][UE_VAL_MAX]; int n; };
const char *uevent_get(const struct uevent *ev, const char *key);       /* first match or NULL */
```

---

## File Structure

- Create `udev_builtins.h` — the orchestrator: `ub_select()` (guard bitmask) + `run_builtins()` (dispatch) + leaf helpers.
- Create `tests/test_udev_builtins.c` — unit tests for the leaf helpers and `ub_select()` on synthetic sysfs trees.
- Create `tests/verify_builtins_live.sh` — the aggregate live parity gate.
- Modify `schema-udev.h` — `UE_MAX_KEYS` 32→64 (one line).
- Modify `schema-udev.c` — one `run_builtins()` call site in `dispatch()`.
- Modify `Makefile` — one line in the `test:` target to build+run `tests/test_udev_builtins.c`.

---

## Task 1: Cap bump + leaf guard helpers

**Files:**
- Modify: `schema-udev.h` (the `#define UE_MAX_KEYS` line)
- Create: `udev_builtins.h`
- Create: `tests/test_udev_builtins.c`
- Modify: `Makefile` (`test:` target)

**Interfaces:**
- Consumes: `pi_subsystem`, `pi_sysattr`, `pi_parent` from `path_id.h`; `struct uevent`, `uevent_get` from `schema-udev.h`.
- Produces: `ub_kernel_name(const char *devpath) -> const char *`; `ub_in(const char *s, const char *const *set) -> int`; `ub_has_modalias(const char *sysroot, const char *devpath) -> int`; `ub_ancestor_in(const char *sysroot, const char *devpath, const char *const *subs) -> int`.

- [ ] **Step 1: Bump the key cap**

In `schema-udev.h`, change the one line:

```c
#define UE_MAX_KEYS 64
```

- [ ] **Step 2: Create `udev_builtins.h` with includes and leaf helpers**

```c
#ifndef UDEV_BUILTINS_H
#define UDEV_BUILTINS_H

#include "schema-udev.h"
#include "path_id.h"
#include "usb_id.h"
#include "input_id.h"
#include "net_id.h"
#include "blkid_fs.h"   /* pulls in blkid_pt.h */
#include "hwdb.h"

#include <string.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>

/* kernel kobject name = basename of the devpath */
static inline const char *ub_kernel_name(const char *devpath) {
    const char *b = strrchr(devpath, '/');
    return b ? b + 1 : devpath;
}

/* is s (may be NULL) one of the NULL-terminated set? */
static inline int ub_in(const char *s, const char *const *set) {
    if (!s) return 0;
    for (int i = 0; set[i]; i++) if (strcmp(s, set[i]) == 0) return 1;
    return 0;
}

/* does the device have a non-empty modalias sysattr? (udev MODALIAS!="") */
static inline int ub_has_modalias(const char *sysroot, const char *devpath) {
    char devdir[PATH_MAX], buf[UE_VAL_MAX];
    snprintf(devdir, sizeof devdir, "%s%s", sysroot, devpath);
    return pi_sysattr(devdir, "modalias", buf, sizeof buf) == 0 && buf[0] != '\0';
}

/* is this device's OR any ancestor's subsystem in the set? (udev SUBSYSTEMS==) */
static inline int ub_ancestor_in(const char *sysroot, const char *devpath,
                                 const char *const *subs) {
    char cur[PATH_MAX], sub[64];
    snprintf(cur, sizeof cur, "%s%s", sysroot, devpath);
    for (;;) {
        if (pi_subsystem(cur, sub, sizeof sub) == 0 && ub_in(sub, subs)) return 1;
        if (strcmp(cur, sysroot) == 0) break;
        if (pi_parent(cur) != 0) break;
    }
    return 0;
}

#endif /* UDEV_BUILTINS_H */
```

- [ ] **Step 3: Write the failing helper tests**

Create `tests/test_udev_builtins.c`:

```c
#include "udev_builtins.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- synthetic sysfs builder rooted at a mkdtemp dir --- */
static char ROOT[64];
static void root_make(void) { strcpy(ROOT, "/tmp/ubtestXXXXXX"); assert(mkdtemp(ROOT)); }
static void mkdirs(const char *rel) {                 /* mkdir -p ROOT/rel */
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s%s", ROOT, rel);
    for (char *s = p + strlen(ROOT) + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}
static void writef(const char *rel, const char *body) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s%s", ROOT, rel);
    FILE *f = fopen(p, "w"); assert(f); fputs(body, f); fclose(f);
}
/* create ROOT/<devrel>/subsystem -> a dir whose basename is `name` */
static void set_subsystem(const char *devrel, const char *name) {
    char busrel[PATH_MAX]; snprintf(busrel, sizeof busrel, "/bus/%s", name); mkdirs(busrel);
    char linkp[PATH_MAX], target[PATH_MAX];
    snprintf(linkp, sizeof linkp, "%s%s/subsystem", ROOT, devrel);
    snprintf(target, sizeof target, "%s/bus/%s", ROOT, name);
    unlink(linkp); assert(symlink(target, linkp) == 0);
}

int main(void) {
    root_make();

    /* ub_kernel_name */
    assert(strcmp(ub_kernel_name("/devices/pci0000:00/0000:00:01.0/net/enp6s0"), "enp6s0") == 0);
    assert(strcmp(ub_kernel_name("sda"), "sda") == 0);

    /* ub_in */
    static const char *const set[] = { "pci", "usb", "platform", NULL };
    assert(ub_in("usb", set) == 1);
    assert(ub_in("acpi", set) == 0);
    assert(ub_in(NULL, set) == 0);

    /* ub_has_modalias */
    mkdirs("/devices/dev_ma");
    writef("/devices/dev_ma/modalias", "pci:v00001022d\n");
    assert(ub_has_modalias(ROOT, "/devices/dev_ma") == 1);
    mkdirs("/devices/dev_noma");
    assert(ub_has_modalias(ROOT, "/devices/dev_noma") == 0);

    /* ub_ancestor_in: leaf 'net', parent 'pci' */
    mkdirs("/devices/pci0000:00/0000:00:01.0/net/eth0");
    set_subsystem("/devices/pci0000:00/0000:00:01.0", "pci");
    set_subsystem("/devices/pci0000:00/0000:00:01.0/net/eth0", "net");
    static const char *const busset[] = { "pci", "usb", "platform", "acpi", NULL };
    assert(ub_ancestor_in(ROOT, "/devices/pci0000:00/0000:00:01.0/net/eth0", busset) == 1);
    /* virtual device: no pci/usb/platform/acpi ancestor */
    mkdirs("/devices/virtual/block/zram0");
    set_subsystem("/devices/virtual/block/zram0", "block");
    assert(ub_ancestor_in(ROOT, "/devices/virtual/block/zram0", busset) == 0);

    printf("test_udev_builtins helpers: OK\n");
    return 0;
}
```

- [ ] **Step 4: Add the Makefile line and run the test to verify it fails to compile (functions/macros not yet complete)**

In `Makefile`, inside the `test:` target, after the `test_hwdb.c` line add:

```make
	$(CC) $(CFLAGS) tests/test_udev_builtins.c -o /tmp/schema-test-ub && /tmp/schema-test-ub
```

Run: `make test`
Expected: fails at the new line only if `udev_builtins.h` is incomplete. With Steps 1–2 done it should COMPILE; this step confirms the test is wired into `make test`. If Step 2 was skipped it fails with a missing-header/undeclared error.

- [ ] **Step 5: Run the test to verify it passes**

Run: `make test`
Expected: `test_udev_builtins helpers: OK`, whole suite green, no `-Wall -Wextra` warnings.

- [ ] **Step 6: Commit**

```bash
git add schema-udev.h udev_builtins.h tests/test_udev_builtins.c Makefile
git commit -m "feat(builtin-wiring): key-cap bump + guard leaf helpers (ub_kernel_name/in/has_modalias/ancestor_in)"
```

---

## Task 2: `ub_select` guard bitmask + `run_builtins` dispatch

**Files:**
- Modify: `udev_builtins.h` (add `ub_select` + `run_builtins`)
- Modify: `tests/test_udev_builtins.c` (add `ub_select` cases)

**Interfaces:**
- Consumes: the leaf helpers from Task 1; `uevent_get`; the seven builtin entry points.
- Produces: `enum { UB_HWDB=1, UB_PATH=2, UB_USB=4, UB_INPUT=8, UB_NET=16, UB_BLKID=32 }`; `int ub_select(const char *sysroot, const char *devpath, const char *devnode, const struct uevent *ev)`; `int run_builtins(const char *sysroot, const char *devpath, const char *devnode, struct uevent *ev)`.

- [ ] **Step 1: Write the failing `ub_select` tests**

Add to `tests/test_udev_builtins.c` a helper that fabricates an event and a set of assertions, then call it from `main` before the final `printf`:

```c
/* fabricate an event with the given SUBSYSTEM/DEVTYPE/DEVPATH (any may be NULL) */
static void ev_set(struct uevent *ev, const char *sub, const char *dt, const char *dp) {
    ev->n = 0;
    if (sub) { strcpy(ev->key[ev->n], "SUBSYSTEM"); strcpy(ev->val[ev->n], sub); ev->n++; }
    if (dt)  { strcpy(ev->key[ev->n], "DEVTYPE");   strcpy(ev->val[ev->n], dt);  ev->n++; }
    if (dp)  { strcpy(ev->key[ev->n], "DEVPATH");   strcpy(ev->val[ev->n], dp);  ev->n++; }
}

static void test_select(void) {
    struct uevent ev;

    /* usb_device with modalias, parent usb -> HWDB|USB|PATH */
    mkdirs("/devices/pci0/usb1/1-2");
    writef("/devices/pci0/usb1/1-2/modalias", "usb:v1234p5678\n");
    set_subsystem("/devices/pci0", "pci");
    set_subsystem("/devices/pci0/usb1", "usb");
    set_subsystem("/devices/pci0/usb1/1-2", "usb");
    ev_set(&ev, "usb", "usb_device", "/devices/pci0/usb1/1-2");
    int s = ub_select(ROOT, "/devices/pci0/usb1/1-2", "/dev/bus/usb/001/002", &ev);
    assert(s & UB_USB); assert(s & UB_HWDB); assert(s & UB_PATH);
    assert(!(s & UB_BLKID)); assert(!(s & UB_NET)); assert(!(s & UB_INPUT));

    /* virtual block disk (zram0): BLKID only, PATH suppressed by /virtual/ + no bus ancestor */
    mkdirs("/devices/virtual/block/zram0");
    set_subsystem("/devices/virtual/block/zram0", "block");
    ev_set(&ev, "block", "disk", "/devices/virtual/block/zram0");
    s = ub_select(ROOT, "/devices/virtual/block/zram0", "/dev/zram0", &ev);
    assert(s & UB_BLKID); assert(!(s & UB_PATH));

    /* real disk (nvme): BLKID + PATH */
    mkdirs("/devices/pci0/nvme/nvme0/nvme0n1");
    set_subsystem("/devices/pci0/nvme/nvme0/nvme0n1", "block");
    ev_set(&ev, "block", "disk", "/devices/pci0/nvme/nvme0/nvme0n1");
    s = ub_select(ROOT, "/devices/pci0/nvme/nvme0/nvme0n1", "/dev/nvme0n1", &ev);
    assert(s & UB_BLKID); assert(s & UB_PATH);

    /* sr0 optical: BLKID suppressed by KERNEL sr* */
    mkdirs("/devices/pci0/ata/sr0");
    set_subsystem("/devices/pci0/ata/sr0", "block");
    ev_set(&ev, "block", "disk", "/devices/pci0/ata/sr0");
    s = ub_select(ROOT, "/devices/pci0/ata/sr0", "/dev/sr0", &ev);
    assert(!(s & UB_BLKID));

    /* mmcblk0boot0: BLKID suppressed */
    mkdirs("/devices/mmc/mmcblk0boot0");
    set_subsystem("/devices/mmc/mmcblk0boot0", "block");
    ev_set(&ev, "block", "disk", "/devices/mmc/mmcblk0boot0");
    s = ub_select(ROOT, "/devices/mmc/mmcblk0boot0", "/dev/mmcblk0boot0", &ev);
    assert(!(s & UB_BLKID));

    /* net device: NET only (no modalias here) */
    mkdirs("/devices/pci0/net/eth0");
    set_subsystem("/devices/pci0/net/eth0", "net");
    ev_set(&ev, "net", NULL, "/devices/pci0/net/eth0");
    s = ub_select(ROOT, "/devices/pci0/net/eth0", NULL, &ev);
    assert(s & UB_NET); assert(!(s & UB_BLKID)); assert(!(s & UB_USB));

    /* input device: INPUT (+PATH via pci ancestor) */
    mkdirs("/devices/pci0/input/input5");
    set_subsystem("/devices/pci0/input/input5", "input");
    ev_set(&ev, "input", NULL, "/devices/pci0/input/input5");
    s = ub_select(ROOT, "/devices/pci0/input/input5", NULL, &ev);
    assert(s & UB_INPUT);

    /* usb interface (DEVTYPE=usb_interface, NOT usb_device): USB suppressed */
    mkdirs("/devices/pci0/usb1/1-2/1-2:1.0");
    set_subsystem("/devices/pci0/usb1/1-2/1-2:1.0", "usb");
    ev_set(&ev, "usb", "usb_interface", "/devices/pci0/usb1/1-2/1-2:1.0");
    s = ub_select(ROOT, "/devices/pci0/usb1/1-2/1-2:1.0", NULL, &ev);
    assert(!(s & UB_USB));

    printf("test_udev_builtins ub_select: OK\n");
}
```

Call `test_select();` from `main` just before the final `printf`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — `ub_select`, `UB_*`, and `run_builtins` are undeclared.

- [ ] **Step 3: Implement `ub_select` and `run_builtins`**

Add to `udev_builtins.h` before `#endif`:

```c
enum { UB_HWDB = 1, UB_PATH = 2, UB_USB = 4, UB_INPUT = 8, UB_NET = 16, UB_BLKID = 32 };

/* Pure guard logic: which builtins apply to this device? Mirrors the IMPORT{builtin}
 * conditions in systemd's shipped /usr/lib/udev/rules.d. Order of the bits is
 * irrelevant; run_builtins imposes the dispatch order. */
static inline int ub_select(const char *sysroot, const char *devpath,
                            const char *devnode, const struct uevent *ev) {
    static const char *const bus3[]  = { "pci", "usb", "platform", NULL };
    static const char *const bus4[]  = { "pci", "usb", "platform", "acpi", NULL };
    const char *subsystem = uevent_get(ev, "SUBSYSTEM");
    const char *devtype   = uevent_get(ev, "DEVTYPE");
    const char *kname     = ub_kernel_name(devpath);
    int sel = 0;

    if (ub_has_modalias(sysroot, devpath)) sel |= UB_HWDB;

    if (ub_in(subsystem, bus3)) sel |= UB_PATH;
    else if (subsystem && strcmp(subsystem, "block") == 0 &&
             devtype && strcmp(devtype, "disk") == 0 &&
             strstr(devpath, "/virtual/") == NULL) sel |= UB_PATH;
    else if (ub_ancestor_in(sysroot, devpath, bus4)) sel |= UB_PATH;

    if (subsystem && strcmp(subsystem, "usb") == 0 &&
        devtype && strcmp(devtype, "usb_device") == 0) sel |= UB_USB;

    if (subsystem && strcmp(subsystem, "input") == 0) sel |= UB_INPUT;

    if (subsystem && strcmp(subsystem, "net") == 0) sel |= UB_NET;

    if (subsystem && strcmp(subsystem, "block") == 0 && devnode &&
        devtype && (strcmp(devtype, "disk") == 0 || strcmp(devtype, "partition") == 0) &&
        fnmatch("sr*", kname, 0) != 0 && fnmatch("mmcblk*boot*", kname, 0) != 0)
        sel |= UB_BLKID;

    return sel;
}

/* Dispatch: run each selected builtin in fixed udev precedence order. Each builtin
 * appends its properties into ev. Returns the number of properties added. */
static inline int run_builtins(const char *sysroot, const char *devpath,
                               const char *devnode, struct uevent *ev) {
    int before = ev->n;
    int sel = ub_select(sysroot, devpath, devnode, ev);
    if (sel & UB_HWDB)  hwdb_build(sysroot, devpath, ev);
    if (sel & UB_PATH)  path_id_build(sysroot, devpath, ev);
    if (sel & UB_USB)   usb_id_build(sysroot, devpath, ev);
    if (sel & UB_INPUT) input_id_build(sysroot, devpath, ev);
    if (sel & UB_NET)   net_id_build(sysroot, devpath, ev);
    if (sel & UB_BLKID) { blkid_pt_build(sysroot, devpath, devnode, ev);
                          blkid_fs_build(sysroot, devpath, devnode, ev); }
    return ev->n - before;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: `test_udev_builtins ub_select: OK`, whole suite green, `-Wall -Wextra` clean.

- [ ] **Step 5: Commit**

```bash
git add udev_builtins.h tests/test_udev_builtins.c
git commit -m "feat(builtin-wiring): ub_select guard bitmask + run_builtins dispatch"
```

---

## Task 3: Wire `run_builtins` into schema-udev.c

**Files:**
- Modify: `schema-udev.c` (add `#include "udev_builtins.h"` and one call site in `dispatch()`)

**Interfaces:**
- Consumes: `run_builtins(sysroot, devpath, devnode, ev)` from Task 2; `uevent_get`.
- Produces: no new API — the daemon now populates builtin properties before rule matching.

- [ ] **Step 1: Add the include**

In `schema-udev.c`, after `#include "schema-udev.h"`, add:

```c
#include "udev_builtins.h"
```

- [ ] **Step 2: Add the single call site in `dispatch()`**

`dispatch(const struct uevent *ev)` currently takes a `const` event. Change it to populate builtin properties first. Replace the function's opening — from its signature through the `if (!action) return;` guard — with:

```c
static void dispatch(struct uevent *ev) {
    const char *action = uevent_get(ev, "ACTION");
    if (!action) return;
    const char *devpath = uevent_get(ev, "DEVPATH");
    if (devpath) {
        const char *devname = uevent_get(ev, "DEVNAME");
        char devnode[UE_VAL_MAX];
        const char *dn = NULL;
        if (devname) { snprintf(devnode, sizeof devnode, "/dev/%s", devname); dn = devnode; }
        run_builtins("/sys", devpath, dn, ev);
    }
    /* ... existing rule-match loop unchanged ... */
```

The `const` on the `dispatch` parameter must be dropped (it now mutates `ev`). Both callers pass a mutable `struct uevent` already: the hotplug path (`struct uevent ev; ... dispatch(&ev);`) and the coldplug callback. Confirm the callback typedef used by `coldplug_walk_root` accepts `void (*)(struct uevent *)`; if it is declared `const`, drop the `const` there too (this is within `schema-udev.c` / its callback typedef in `schema-udev.h` — if the typedef is in `schema-udev.h`, that is an allowed boundary change; note it in the commit).

- [ ] **Step 3: Build the daemon and the full suite**

Run: `make schema-udev && make test`
Expected: `schema-udev` links; suite green; `-Wall -Wextra` clean. If the coldplug callback signature mismatches, fix the `const` as described in Step 2.

- [ ] **Step 4: Verify the boundary diff is minimal**

Run: `git diff master -- schema-udev.h` — expect only the `UE_MAX_KEYS` line.
Run: `git diff master -- schema-udev.c` — expect only the `#include` and the `dispatch()` head change (plus a `const` drop if the callback typedef required it).
Expected: no unrelated changes.

- [ ] **Step 5: Run the VM boot test**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS` (timer fired, hang excised, dependent ran, SDBOOTED-DIR present). schema-udev is not PID 1, so the rail is unaffected; this confirms nothing regressed.

- [ ] **Step 6: Commit**

```bash
git add schema-udev.c schema-udev.h
git commit -m "feat(builtin-wiring): wire run_builtins into dispatch() before rule match (compute-only)"
```

---

## Task 4: Aggregate live parity gate

**Files:**
- Create: `tests/verify_builtins_live.sh`

**Interfaces:**
- Consumes: `run_builtins` from `udev_builtins.h`.
- Produces: the acceptance evidence — 0 mismatches across all `/sys` devices, both directions.

- [ ] **Step 1: Write the live gate script**

Create `tests/verify_builtins_live.sh` (mirror the structure of `tests/verify_hwdb_live.sh`):

```sh
#!/bin/sh
# Aggregate live parity gate for builtin wiring: run run_builtins over EVERY /sys
# device, diff the union of the six builtins' owned key-subsets vs `udevadm info`,
# BOTH directions. sudo (blkid reads raw block devices). Deferred keys excluded.
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/ub_driver.c <<'EOF'
#include "udev_builtins.h"
#include <stdio.h>
#include <string.h>
/* argv[1]=devpath (under /sys), argv[2]=devnode or "-" */
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *devnode = strcmp(argv[2], "-") ? argv[2] : NULL;
    struct uevent ev; ev.n = 0;
    /* rebuild the kernel payload props from sysfs uevent file */
    char up[PATH_MAX]; snprintf(up, sizeof up, "/sys%s/uevent", argv[1]);
    FILE *f = fopen(up, "r");
    if (f) {
        char line[UE_VAL_MAX + UE_KEY_MAX];
        while (fgets(line, sizeof line, f) && ev.n < UE_MAX_KEYS) {
            char *eq = strchr(line, '='); if (!eq) continue;
            *eq = 0; char *v = eq + 1; v[strcspn(v, "\n")] = 0;
            snprintf(ev.key[ev.n], UE_KEY_MAX, "%s", line);
            snprintf(ev.val[ev.n], UE_VAL_MAX, "%s", v); ev.n++;
        }
        fclose(f);
    }
    /* ensure DEVPATH present (uevent file omits it) */
    if (!uevent_get(&ev, "DEVPATH") && ev.n < UE_MAX_KEYS) {
        snprintf(ev.key[ev.n], UE_KEY_MAX, "DEVPATH");
        snprintf(ev.val[ev.n], UE_VAL_MAX, "%s", argv[1]); ev.n++;
    }
    int base = ev.n;
    run_builtins("/sys", argv[1], devnode, &ev);
    for (int i = base; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/ub_driver.c -o /tmp/ub_driver

# owned-subset key prefixes across the six builtins
KEYS='^ID_PATH=|^ID_PATH_TAG=|^ID_VENDOR|^ID_MODEL|^ID_SERIAL|^ID_REVISION=|^ID_TYPE=|^ID_USB|^ID_BUS=|^ID_INSTANCE=|^ID_PCI_|^ID_INPUT|^ID_NET_|^ID_FS_|^ID_PART_|_FROM_DATABASE='
# deferred keys excluded from BOTH sides
DEFER='^ID_OUI_FROM_DATABASE=|^ID_NET_DRIVER=|^ID_FS_SIZE=|^ID_FS_BLOCKSIZE=|^ID_FS_LASTBLOCK='

ours=$(mktemp); theirs=$(mktemp); total=0; devs=0
for uev in $(find /sys/devices -name uevent -printf '%h\n'); do
    devpath=${uev#/sys}
    devname=$(sed -n 's/^DEVNAME=//p' "$uev/uevent" 2>/dev/null | head -1)
    node="-"; [ -n "$devname" ] && node="/dev/$devname"
    /tmp/ub_driver "$devpath" "$node" 2>/dev/null \
        | grep -E "$KEYS" | grep -Ev "$DEFER" | sort > "$ours" || true
    udevadm info -q property -p "$uev" 2>/dev/null \
        | grep -E "$KEYS" | grep -Ev "$DEFER" | sort > "$theirs" || true
    [ -s "$ours" ] || [ -s "$theirs" ] || continue
    devs=$((devs+1))
    if ! diff -u "$theirs" "$ours" >/dev/null; then
        echo "### MISMATCH $devpath"; diff -u "$theirs" "$ours" || true
        total=$((total+1))
    fi
done
echo "checked $devs devices with builtin properties; $total mismatch(es)"
rm -f "$ours" "$theirs"
[ "$total" -eq 0 ]
```

- [ ] **Step 2: Make it executable and run it**

Run: `chmod +x tests/verify_builtins_live.sh && sudo tests/verify_builtins_live.sh`
Expected: `... 0 mismatch(es)` and exit 0.

- [ ] **Step 3: Resolve any guard delta (expected iteration)**

If mismatches appear, they are almost certainly a guard fidelity issue (over- or under-emission), not a builtin bug — the six builtins are individually gate-proven. For each mismatch:
- **over-emission** (schema has a key udev omits): tighten the corresponding guard in `ub_select` to exclude that device class, add a synthetic-tree case to `test_udev_builtins.c` proving the suppression, re-run `make test` then the live gate.
- **under-emission** (udev has a key schema omits): loosen/correct the guard, add the covering case, re-run both.
The most likely deltas: partition `ID_PATH` (udev derives partition `ID_PATH` from the parent disk plus a `-partN` suffix — if `path_id_build` on a partition devpath does not already match, restrict `UB_PATH` to whole disks and synthesize the partition suffix, OR confirm `path_id.h` handles partitions); and any device with a modalias where udev's composite hwdb lookups (excluded) differ from the single-key ones (already excluded via `DEFER`).

- [ ] **Step 4: Commit the gate**

```bash
git add tests/verify_builtins_live.sh
git commit -m "test(builtin-wiring): aggregate live udev parity gate (all /sys devices, both directions)"
```

---

## Self-Review

**1. Spec coverage:**
- `run_builtins` entry point + single call site → Task 2 (fn), Task 3 (wiring). ✓
- Auto-by-subsystem guarded dispatch mirroring installed rules → Task 2 `ub_select`. ✓
- Dispatch order hwdb→path_id→usb_id→input_id→net_id→blkid → Task 2 `run_builtins`. ✓
- Merge = append, no dedup, no cross-builtin collisions → Global Constraints + Task 2 (builtins append; return delta). ✓
- `UE_MAX_KEYS` 32→64 → Task 1 Step 1. ✓
- Compute-only/inert (no symlink/hook logic change) → Task 3 changes only `dispatch()` head, leaves the match/symlink/hook loop untouched. ✓
- Boundary minimal on `.c`/`.h` → Task 3 Step 4. ✓
- Aggregate live gate, both directions, union subset, deferred-key exclusions, sudo → Task 4. ✓
- vmtest PASS → Task 3 Step 5. ✓
- test file + Makefile line → Task 1. ✓

**2. Placeholder scan:** No TBD/TODO/"handle edge cases"/"similar to". Task 4 Step 3 is a real iteration procedure with concrete guard-tightening actions, not a placeholder. ✓

**3. Type consistency:** `ub_select`/`run_builtins` signatures identical in Interfaces, implementation, and driver. `UB_*` enum used consistently. Builtin signatures match the Global Constraints block (`path_id_build` returns `ssize_t`, blkid builtins take `devnode`, others take 3 args). `struct uevent` fields (`key`/`val`/`n`) used consistently. Helper names (`ub_kernel_name`, `ub_in`, `ub_has_modalias`, `ub_ancestor_in`) identical across Task 1 def and Task 2 use. ✓

**4. Known iteration point:** partition `ID_PATH` parity (Task 4 Step 3) is the one place the guard may need tightening against the live diff — flagged, with the concrete fix. This is the expected "live gate is the authority" loop that caught four spec bugs across the six builtins.
