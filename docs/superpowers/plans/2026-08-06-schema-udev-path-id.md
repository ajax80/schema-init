# schema-udev `path_id` builtin Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a pure C builtin that reproduces systemd-udev's `ID_PATH` / `ID_PATH_TAG` for a device, byte-for-byte, by walking its sysfs ancestry.

**Architecture:** A new header `path_id.h` holds `path_id_build(sysroot, devpath, out, outsz)`, which starts at the leaf device dir and walks parent directories root-ward. At each node it reads the node's `subsystem` and dispatches to a bus handler that *prepends* a path component and returns the ancestor to continue from. Components prepended leaf→root produce a root→leaf string. The function reads sysfs and writes nothing. It is NOT wired into the live daemon — mechanism only, off by default (same boundary as Phase 3a).

**Tech Stack:** C (C11, `-Wall -Wextra`, static build), header-only inline functions in the style of `schema-udev.h`, `assert`-based unit tests that build fake sysfs trees under `mkdtemp` (the exact pattern of `tests/test_coldplug.c`).

## Global Constraints

- **Byte-parity is the acceptance bar.** Every component format is fixed by blakbox ground truth in `docs/superpowers/specs/2026-08-06-schema-udev-path-id-design.md`. Do not invent or "clean up" formats.
- **`schema-udev.c` MUST stay byte-identical to master.** path_id is never `#include`d or called from it. Boundary gate: `git diff master..HEAD -- schema-udev.c` empty AND `grep path_id schema-udev.c` empty.
- **Read-only.** Never write sysfs, never open netlink, never touch `/run/udev`. Same safety class as the coldplug walker.
- **Scope is the six blakbox handlers only:** pci, usb, scsi(ata + default), nvme, platform. No other udev bus types.
- **Exact component formats (verified against live sysfs + `/run/udev/data`):**
  - pci: `pci-<sysname>` (kernel dir name); emit only the leaf-most pci function, skip all pci ancestors.
  - usb: `usb-0:<rest>` where `<rest>` = interface sysname after the first `-` (`1-4:1.0` → `4:1.0`); literal `0:` constant; only for `usb_interface` nodes (basename contains `:`).
  - ata: `ata-<port_no>.<M>`; `port_no` from `<ataN>/ata_port/<ataN>/port_no` sysattr (NOT the `ataN` dir number — they diverge); `<M>` = suffix after `.` in the `<ataN>/link*/devN.M` dir.
  - scsi default: `scsi-<H>:<C>:<T>:<L>` from scsi_device sysname, **H rebased** by subtracting the lowest `hostN` index among the host's sibling dirs.
  - nvme: `nvme-<nsid>`; `nsid` from the leaf block device's `nsid` sysattr.
  - platform: `platform-<sysname>` (kernel dir name).
  - `ID_PATH_TAG` = ID_PATH with every char not in `[A-Za-z0-9]` replaced by `_`.
- **Anchor guard:** emit `ID_PATH` only if a pci or platform component was produced; otherwise `path_id_build` returns −1.
- **Commit style:** end commit messages with the two trailer lines used in this repo (`Co-Authored-By:` and `Claude-Session:`), or match the repo's existing trailer convention.

## File Structure

- **Create `path_id.h`** — the builtin: `path_id_tag`, `path_id_build`, and static sysfs helpers. Includes `schema-udev.h` for `safe_copy`.
- **Create `tests/test_path_id.c`** — `assert`-based unit tests that build fake sysfs trees under `mkdtemp` and assert exact ID_PATH + TAG.
- **Modify `Makefile`** — add one `test:` line compiling and running `tests/test_path_id.c`.
- **Create `tests/verify_path_id_live.sh`** — live acceptance harness diffing `path_id_build` output against real udev across every device with an `ID_PATH` (Claire's 0-mismatch gate; Greg may run it on blakbox too).
- **`schema-udev.c`** — untouched.

---

### Task 1: Scaffold `path_id.h` — `path_id_tag` + sysfs helpers + build wiring

**Files:**
- Create: `path_id.h`
- Create: `tests/test_path_id.c`
- Modify: `Makefile` (the `test:` target, after the `test_parity.c` line)

**Interfaces:**
- Consumes: `safe_copy(char*, const char*, size_t)` from `schema-udev.h`.
- Produces:
  - `int path_id_tag(const char *id_path, char *out, size_t outsz)` → 0, or −1 on overflow.
  - `int pi_subsystem(const char *devdir, char *out, size_t outsz)` → 0/−1; basename of `<devdir>/subsystem` symlink.
  - `int pi_sysattr(const char *devdir, const char *attr, char *out, size_t outsz)` → 0/−1; trimmed single line of `<devdir>/<attr>`.
  - `const char *pi_base(const char *dir)` → pointer to basename within `dir`.
  - `int pi_parent(char *cur)` → truncate `cur` to parent dir in place; −1 if no `/`.
  - `void pi_prepend(char *path, size_t pathsz, const char *comp)` → `path` becomes `comp` or `comp-<path>`.
  - `#define PATH_ID_MAX 512`

- [ ] **Step 1: Write the failing test**

Create `tests/test_path_id.c`:

```c
#include "../path_id.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* --- fake-sysfs builders (same idiom as test_coldplug.c) --- */
static void mkdirp(const char *path) {
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", path);
    assert(system(cmd) == 0);
}
static void mkfile(const char *path, const char *content) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdirp(dir); }
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}
static void mklink(const char *linkpath, const char *target) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", linkpath);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdirp(dir); }
    unlink(linkpath);
    assert(symlink(target, linkpath) == 0);
}

static void test_tag(void) {
    char out[PATH_ID_MAX];
    assert(path_id_tag("pci-0000:00:00.0", out, sizeof out) == 0);
    assert(strcmp(out, "pci-0000_00_00_0") == 0);

    assert(path_id_tag("pci-0000:08:00.3-usb-0:1:1.0-scsi-0:0:0:0", out, sizeof out) == 0);
    assert(strcmp(out, "pci-0000_08_00_3_usb_0_1_1_0_scsi_0_0_0_0") == 0);

    char tiny[4];
    assert(path_id_tag("abcd", tiny, sizeof tiny) == -1);   /* overflow */
    printf("test_tag OK\n");
}

static void test_helpers(void) {
    char tmpl[] = "/tmp/schema-pathid-h-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);

    char devdir[1024];
    snprintf(devdir, sizeof devdir, "%s/devices/pci0000:00/0000:02:00.1/ata1", root);
    mkdirp(devdir);

    char sublink[1200], subtgt[1200];
    snprintf(subtgt, sizeof subtgt, "%s/class/ata_port", root);
    mkdirp(subtgt);
    snprintf(sublink, sizeof sublink, "%s/subsystem", devdir);
    assert(symlink(subtgt, sublink) == 0);

    char attr[1200];
    snprintf(attr, sizeof attr, "%s/port_no", devdir);
    mkfile(attr, "6\n");

    char out[256];
    assert(pi_subsystem(devdir, out, sizeof out) == 0);
    assert(strcmp(out, "ata_port") == 0);

    assert(pi_sysattr(devdir, "port_no", out, sizeof out) == 0);
    assert(strcmp(out, "6") == 0);

    assert(strcmp(pi_base(devdir), "ata1") == 0);

    char cur[1024];
    snprintf(cur, sizeof cur, "%s", devdir);
    assert(pi_parent(cur) == 0);
    assert(strcmp(pi_base(cur), "0000:02:00.1") == 0);

    char path[PATH_ID_MAX] = "";
    pi_prepend(path, sizeof path, "usb-0:1:1.0");
    assert(strcmp(path, "usb-0:1:1.0") == 0);
    pi_prepend(path, sizeof path, "pci-0000:08:00.3");
    assert(strcmp(path, "pci-0000:08:00.3-usb-0:1:1.0") == 0);

    printf("test_helpers OK\n");
}

int main(void) {
    test_tag();
    test_helpers();
    printf("ALL path_id tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -Wall -Wextra tests/test_path_id.c -o /tmp/t_pathid`
Expected: FAIL — `path_id.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `path_id.h`:

```c
#ifndef PATH_ID_H
#define PATH_ID_H

#include "schema-udev.h"
#include <limits.h>

#define PATH_ID_MAX 512

static inline int path_id_tag(const char *id_path, char *out, size_t outsz) {
    size_t i = 0;
    for (; id_path[i]; i++) {
        if (i + 1 >= outsz) return -1;
        char c = id_path[i];
        out[i] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9')) ? c : '_';
    }
    if (i >= outsz) return -1;
    out[i] = '\0';
    return 0;
}

static inline int pi_subsystem(const char *devdir, char *out, size_t outsz) {
    char link[PATH_MAX], target[PATH_MAX];
    if ((size_t)snprintf(link, sizeof link, "%s/subsystem", devdir) >= sizeof link) return -1;
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n <= 0) return -1;
    target[n] = '\0';
    char *b = strrchr(target, '/');
    safe_copy(out, b ? b + 1 : target, outsz);
    return 0;
}

static inline int pi_sysattr(const char *devdir, const char *attr, char *out, size_t outsz) {
    char p[PATH_MAX];
    if ((size_t)snprintf(p, sizeof p, "%s/%s", devdir, attr) >= sizeof p) return -1;
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    if (!fgets(out, (int)outsz, f)) { fclose(f); return -1; }
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

static inline const char *pi_base(const char *dir) {
    const char *b = strrchr(dir, '/');
    return b ? b + 1 : dir;
}

static inline int pi_parent(char *cur) {
    char *b = strrchr(cur, '/');
    if (!b || b == cur) return -1;
    *b = '\0';
    return 0;
}

static inline void pi_prepend(char *path, size_t pathsz, const char *comp) {
    char tmp[PATH_ID_MAX];
    if (path[0]) snprintf(tmp, sizeof tmp, "%s-%s", comp, path);
    else         snprintf(tmp, sizeof tmp, "%s", comp);
    safe_copy(path, tmp, pathsz);
}

#endif /* PATH_ID_H */
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -Wall -Wextra tests/test_path_id.c -o /tmp/t_pathid && /tmp/t_pathid`
Expected: PASS — prints `test_tag OK`, `test_helpers OK`, `ALL path_id tests passed`.

- [ ] **Step 5: Wire the Makefile**

Add this line to the `test:` target immediately after the `tests/test_parity.c` line:

```make
	$(CC) $(CFLAGS) tests/test_path_id.c -o /tmp/schema-test-pathid && /tmp/schema-test-pathid
```

Run: `make test` — expected: all existing tests plus `ALL path_id tests passed`.

- [ ] **Step 6: Commit**

```bash
git add path_id.h tests/test_path_id.c Makefile
git commit -m "feat(path_id): scaffold builtin header, tag transform, sysfs helpers"
```

---

### Task 2: Core walk + pci & platform handlers + anchor guard

**Files:**
- Modify: `path_id.h` (add `path_id_build` + three handler stubs)
- Modify: `tests/test_path_id.c` (add pci/platform tests)

**Interfaces:**
- Consumes: all helpers from Task 1.
- Produces:
  - `ssize_t path_id_build(const char *sysroot, const char *devpath, char *out, size_t outsz)` → ID_PATH length, or −1 (no anchor / read error / buffer too small).
  - Static handler stubs (bodies filled in Tasks 3–5), signature:
    `int pi_handle_usb/scsi/nvme(const char *leafdir, char *cur, size_t cursz, char *path, size_t pathsz)` → 1 if it emitted and advanced `cur`, else 0.

- [ ] **Step 1: Write the failing test**

Add these functions to `tests/test_path_id.c` and call them from `main` before the final print:

```c
/* Build the two shared PCI ancestor dirs used by several tests. Returns the
 * DEVPATH (relative) via out_devpath for the deepest dir created. */
static void mk_pci_node(const char *root, const char *relpath) {
    char dir[2048], sub[2048], tgt[2048];
    snprintf(dir, sizeof dir, "%s%s", root, relpath);
    mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir);
    snprintf(tgt, sizeof tgt, "%s/bus/pci", root);
    mklink(sub, tgt);
}

static void test_pci_bare(void) {
    char tmpl[] = "/tmp/schema-pathid-pci-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* GPU: /devices/pci0000:00/0000:00:03.1/0000:07:00.0  (bridge skipped) */
    char rootdir[2048];
    snprintf(rootdir, sizeof rootdir, "%s/devices/pci0000:00", root);
    mkdirp(rootdir);                                          /* host bridge root: no subsystem */
    mk_pci_node(root, "/devices/pci0000:00/0000:00:03.1");    /* bridge (pci) */
    mk_pci_node(root, "/devices/pci0000:00/0000:00:03.1/0000:07:00.0");

    char out[PATH_ID_MAX], tag[PATH_ID_MAX];
    assert(path_id_build(root, "/devices/pci0000:00/0000:00:03.1/0000:07:00.0",
                         out, sizeof out) > 0);
    assert(strcmp(out, "pci-0000:07:00.0") == 0);   /* leaf-most only, bridge skipped */
    assert(path_id_tag(out, tag, sizeof tag) == 0);
    assert(strcmp(tag, "pci-0000_07_00_0") == 0);
    printf("test_pci_bare OK\n");
}

static void test_platform(void) {
    char tmpl[] = "/tmp/schema-pathid-plat-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* bare platform: /devices/platform/AMDI0030:00 */
    char dir[2048], sub[2048], tgt[2048];
    snprintf(dir, sizeof dir, "%s/devices/platform/AMDI0030:00", root);
    mkdirp(dir);
    snprintf(tgt, sizeof tgt, "%s/bus/platform", root);
    snprintf(sub, sizeof sub, "%s/subsystem", dir);
    mklink(sub, tgt);

    char out[PATH_ID_MAX];
    assert(path_id_build(root, "/devices/platform/AMDI0030:00", out, sizeof out) > 0);
    assert(strcmp(out, "platform-AMDI0030:00") == 0);
    printf("test_platform OK\n");
}

static void test_pci_platform(void) {
    char tmpl[] = "/tmp/schema-pathid-pp-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* pcspkr: /devices/pci0000:00/0000:00:14.3/PNP0800:00 */
    char base[2048];
    snprintf(base, sizeof base, "%s/devices/pci0000:00", root); mkdirp(base);
    mk_pci_node(root, "/devices/pci0000:00/0000:00:14.3");
    char dir[2048], sub[2048], tgt[2048];
    snprintf(dir, sizeof dir, "%s/devices/pci0000:00/0000:00:14.3/PNP0800:00", root);
    mkdirp(dir);
    snprintf(tgt, sizeof tgt, "%s/bus/platform", root);
    snprintf(sub, sizeof sub, "%s/subsystem", dir);
    mklink(sub, tgt);

    char out[PATH_ID_MAX];
    assert(path_id_build(root, "/devices/pci0000:00/0000:00:14.3/PNP0800:00",
                         out, sizeof out) > 0);
    assert(strcmp(out, "pci-0000:00:14.3-platform-PNP0800:00") == 0);
    printf("test_pci_platform OK\n");
}

static void test_unanchored(void) {
    char tmpl[] = "/tmp/schema-pathid-un-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* a device with only a 'virtual' subsystem chain -> no pci/platform anchor */
    char dir[2048], sub[2048], tgt[2048];
    snprintf(dir, sizeof dir, "%s/devices/virtual/misc/foo", root);
    mkdirp(dir);
    snprintf(tgt, sizeof tgt, "%s/class/misc", root);
    snprintf(sub, sizeof sub, "%s/subsystem", dir);
    mklink(sub, tgt);
    char out[PATH_ID_MAX];
    assert(path_id_build(root, "/devices/virtual/misc/foo", out, sizeof out) == -1);
    printf("test_unanchored OK\n");
}
```

Also add to `main`: `test_pci_bare(); test_platform(); test_pci_platform(); test_unanchored();`

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -Wall -Wextra tests/test_path_id.c -o /tmp/t_pathid && /tmp/t_pathid`
Expected: FAIL — `path_id_build` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `path_id.h` before `#endif`. First the handler stubs (real bodies land in Tasks 3–5):

```c
static inline int pi_handle_usb(const char *leafdir, char *cur, size_t cursz,
                                char *path, size_t pathsz) {
    (void)leafdir; (void)cur; (void)cursz; (void)path; (void)pathsz;
    return 0;   /* stub: filled in Task 3 */
}
static inline int pi_handle_scsi(const char *leafdir, char *cur, size_t cursz,
                                 char *path, size_t pathsz) {
    (void)leafdir; (void)cur; (void)cursz; (void)path; (void)pathsz;
    return 0;   /* stub: filled in Task 5 */
}
static inline int pi_handle_nvme(const char *leafdir, char *cur, size_t cursz,
                                 char *path, size_t pathsz) {
    (void)leafdir; (void)cur; (void)cursz; (void)path; (void)pathsz;
    return 0;   /* stub: filled in Task 4 */
}
```

Then the core builder:

```c
static inline ssize_t path_id_build(const char *sysroot, const char *devpath,
                                    char *out, size_t outsz) {
    char devroot[PATH_MAX], cur[PATH_MAX], leafdir[PATH_MAX];
    if ((size_t)snprintf(devroot, sizeof devroot, "%s/devices", sysroot) >= sizeof devroot)
        return -1;
    if ((size_t)snprintf(cur, sizeof cur, "%s%s", sysroot, devpath) >= sizeof cur)
        return -1;
    safe_copy(leafdir, cur, sizeof leafdir);

    size_t rootlen = strlen(devroot);
    char path[PATH_ID_MAX] = "";
    int anchored = 0;
    char comp[PATH_ID_MAX], sub[128];

    while (strlen(cur) > rootlen) {
        if (pi_subsystem(cur, sub, sizeof sub) != 0) {
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (strcmp(sub, "pci") == 0) {
            snprintf(comp, sizeof comp, "pci-%s", pi_base(cur));
            pi_prepend(path, sizeof path, comp);
            anchored = 1;
            /* skip all pci ancestors (bridges) */
            for (;;) {
                if (pi_parent(cur) != 0 || strlen(cur) <= rootlen) break;
                if (pi_subsystem(cur, sub, sizeof sub) != 0 || strcmp(sub, "pci") != 0) break;
            }
            continue;
        }
        if (strcmp(sub, "platform") == 0) {
            snprintf(comp, sizeof comp, "platform-%s", pi_base(cur));
            pi_prepend(path, sizeof path, comp);
            anchored = 1;
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (strcmp(sub, "usb") == 0) {
            if (pi_handle_usb(leafdir, cur, sizeof cur, path, sizeof path)) continue;
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (strcmp(sub, "scsi") == 0) {
            if (pi_handle_scsi(leafdir, cur, sizeof cur, path, sizeof path)) continue;
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (strcmp(sub, "nvme") == 0) {
            if (pi_handle_nvme(leafdir, cur, sizeof cur, path, sizeof path)) continue;
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (pi_parent(cur) != 0) break;
    }

    if (!anchored || path[0] == '\0') return -1;
    size_t plen = strlen(path);
    if (plen + 1 > outsz) return -1;
    memcpy(out, path, plen + 1);
    return (ssize_t)plen;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -Wall -Wextra tests/test_path_id.c -o /tmp/t_pathid && /tmp/t_pathid`
Expected: PASS — `test_pci_bare OK`, `test_platform OK`, `test_pci_platform OK`, `test_unanchored OK`.

- [ ] **Step 5: Commit**

```bash
git add path_id.h tests/test_path_id.c
git commit -m "feat(path_id): core sysfs walk with pci + platform handlers"
```

---

### Task 3: usb handler

**Files:**
- Modify: `path_id.h` (`pi_handle_usb` body)
- Modify: `tests/test_path_id.c` (add usb test)

**Interfaces:**
- Consumes: helpers from Task 1; called by `path_id_build` from Task 2.
- Produces: real `pi_handle_usb` — emits `usb-0:<rest>` for a `usb_interface` node (basename contains `:`), then advances `cur` past all usb ancestors to the first non-usb parent; returns 1. For non-interface usb nodes returns 0.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_path_id.c` and call from `main`:

```c
static void test_usb(void) {
    char tmpl[] = "/tmp/schema-pathid-usb-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* /devices/pci0000:00/0000:00:07.0/0000:02:00.0/usb1/1-4/1-4:1.0/ttyUSB0
       ID_PATH for the tty leaf = pci-0000:02:00.0-usb-0:4:1.0 */
    mk_pci_node(root, "/devices/pci0000:00/0000:00:07.0");
    mk_pci_node(root, "/devices/pci0000:00/0000:00:07.0/0000:02:00.0");

    const char *pcirel = "/devices/pci0000:00/0000:00:07.0/0000:02:00.0";
    /* usb host + device + interface, each subsystem=usb */
    char rel[2048], dir[2600], sub[2600], utgt[2100];
    snprintf(utgt, sizeof utgt, "%s/bus/usb", root); mkdirp(utgt);

    snprintf(rel, sizeof rel, "%s/usb1", pcirel);
    snprintf(dir, sizeof dir, "%s%s", root, rel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, utgt);

    snprintf(rel, sizeof rel, "%s/usb1/1-4", pcirel);
    snprintf(dir, sizeof dir, "%s%s", root, rel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, utgt);

    snprintf(rel, sizeof rel, "%s/usb1/1-4/1-4:1.0", pcirel);
    snprintf(dir, sizeof dir, "%s%s", root, rel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, utgt);

    /* tty leaf */
    char ttytgt[2100];
    snprintf(ttytgt, sizeof ttytgt, "%s/class/tty", root); mkdirp(ttytgt);
    snprintf(rel, sizeof rel, "%s/usb1/1-4/1-4:1.0/ttyUSB0", pcirel);
    snprintf(dir, sizeof dir, "%s%s", root, rel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, ttytgt);

    char out[PATH_ID_MAX];
    assert(path_id_build(root, rel, out, sizeof out) > 0);
    assert(strcmp(out, "pci-0000:02:00.0-usb-0:4:1.0") == 0);
    printf("test_usb OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -Wall -Wextra tests/test_path_id.c -o /tmp/t_pathid && /tmp/t_pathid`
Expected: FAIL — assert on `test_usb` (stub emits nothing, so result is `pci-0000:02:00.0` only, not the usb component).

- [ ] **Step 3: Write the implementation**

Replace the `pi_handle_usb` stub body in `path_id.h` with:

```c
static inline int pi_handle_usb(const char *leafdir, char *cur, size_t cursz,
                                char *path, size_t pathsz) {
    (void)leafdir; (void)cursz;
    const char *name = pi_base(cur);
    const char *dash = strchr(name, '-');
    /* only usb_interface nodes (name has both '-' and ':', e.g. 1-4:1.0) emit */
    if (!dash || !strchr(name, ':')) return 0;
    char comp[PATH_ID_MAX];
    snprintf(comp, sizeof comp, "usb-0:%s", dash + 1);
    pi_prepend(path, pathsz, comp);
    /* consume: climb past all usb ancestors to the first non-usb parent */
    char sub[128];
    for (;;) {
        if (pi_parent(cur) != 0) break;
        if (pi_subsystem(cur, sub, sizeof sub) != 0 || strcmp(sub, "usb") != 0) break;
    }
    return 1;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -Wall -Wextra tests/test_path_id.c -o /tmp/t_pathid && /tmp/t_pathid`
Expected: PASS — `test_usb OK`, all prior tests still pass.

- [ ] **Step 5: Commit**

```bash
git add path_id.h tests/test_path_id.c
git commit -m "feat(path_id): usb interface handler"
```

---

### Task 4: nvme handler

**Files:**
- Modify: `path_id.h` (`pi_handle_nvme` body)
- Modify: `tests/test_path_id.c` (add nvme test)

**Interfaces:**
- Consumes: helpers from Task 1; called by `path_id_build`.
- Produces: real `pi_handle_nvme` — reads `nsid` from `leafdir`, emits `nvme-<nsid>`, advances `cur` to the parent, returns 1. If `nsid` unreadable, returns 0.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_path_id.c` and call from `main`:

```c
static void test_nvme(void) {
    char tmpl[] = "/tmp/schema-pathid-nvme-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* /devices/pci0000:00/0000:00:01.1/0000:01:00.0/nvme/nvme0/nvme0n1
       ID_PATH = pci-0000:01:00.0-nvme-1 */
    mk_pci_node(root, "/devices/pci0000:00/0000:00:01.1");
    mk_pci_node(root, "/devices/pci0000:00/0000:00:01.1/0000:01:00.0");

    const char *pcirel = "/devices/pci0000:00/0000:00:01.1/0000:01:00.0";
    char rel[2048], dir[2600], sub[2600], ntgt[2100], btgt[2100];
    snprintf(ntgt, sizeof ntgt, "%s/class/nvme", root); mkdirp(ntgt);
    snprintf(btgt, sizeof btgt, "%s/class/block", root); mkdirp(btgt);

    /* container 'nvme' dir: no subsystem */
    snprintf(dir, sizeof dir, "%s%s/nvme", root, pcirel); mkdirp(dir);
    /* nvme0 controller: subsystem=nvme */
    snprintf(rel, sizeof rel, "%s/nvme/nvme0", pcirel);
    snprintf(dir, sizeof dir, "%s%s", root, rel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, ntgt);
    /* nvme0n1 namespace/block leaf: subsystem=block, nsid=1 */
    snprintf(rel, sizeof rel, "%s/nvme/nvme0/nvme0n1", pcirel);
    snprintf(dir, sizeof dir, "%s%s", root, rel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, btgt);
    char nsid[2600]; snprintf(nsid, sizeof nsid, "%s/nsid", dir); mkfile(nsid, "1\n");

    char out[PATH_ID_MAX];
    assert(path_id_build(root, rel, out, sizeof out) > 0);
    assert(strcmp(out, "pci-0000:01:00.0-nvme-1") == 0);
    printf("test_nvme OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -Wall -Wextra tests/test_path_id.c -o /tmp/t_pathid && /tmp/t_pathid`
Expected: FAIL — result is `pci-0000:01:00.0` only (nvme stub emits nothing).

- [ ] **Step 3: Write the implementation**

Replace the `pi_handle_nvme` stub body in `path_id.h` with:

```c
static inline int pi_handle_nvme(const char *leafdir, char *cur, size_t cursz,
                                 char *path, size_t pathsz) {
    (void)cursz;
    char nsid[64];
    if (pi_sysattr(leafdir, "nsid", nsid, sizeof nsid) != 0) return 0;
    char comp[PATH_ID_MAX];
    snprintf(comp, sizeof comp, "nvme-%s", nsid);
    pi_prepend(path, pathsz, comp);
    if (pi_parent(cur) != 0) return 1;
    return 1;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -Wall -Wextra tests/test_path_id.c -o /tmp/t_pathid && /tmp/t_pathid`
Expected: PASS — `test_nvme OK`.

- [ ] **Step 5: Commit**

```bash
git add path_id.h tests/test_path_id.c
git commit -m "feat(path_id): nvme handler"
```

---

### Task 5: scsi handler (ata transport + default with host rebase)

**Files:**
- Modify: `path_id.h` (`pi_handle_scsi` body + two directory-scan helpers)
- Modify: `tests/test_path_id.c` (add ata test + nested usb-scsi rebase test)

**Interfaces:**
- Consumes: helpers from Task 1; called by `path_id_build`; relies on `pi_handle_usb` (Task 3) for the nested composite.
- Produces: real `pi_handle_scsi` — fires only for a scsi_device node (basename matches `%u:%u:%u:%u`). Detects ATA transport by an `ataN` ancestor above the `hostN` dir; emits `ata-<port_no>.<M>` (consuming up to the ata port's parent) or `scsi-<H-rebased>:<C>:<T>:<L>` (consuming up to the host's parent). Returns 1; returns 0 if the basename is not a scsi_device sysname.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_path_id.c` and call from `main`:

```c
static void test_ata(void) {
    char tmpl[] = "/tmp/schema-pathid-ata-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* /devices/pci0000:00/0000:00:01.3/0000:02:00.1/ata1/host0/target0:0:0/0:0:0:0/block/sda
       ID_PATH for the block leaf = pci-0000:02:00.1-ata-1.0 */
    mk_pci_node(root, "/devices/pci0000:00/0000:00:01.3");
    mk_pci_node(root, "/devices/pci0000:00/0000:00:01.3/0000:02:00.1");

    const char *pcirel = "/devices/pci0000:00/0000:00:01.3/0000:02:00.1";
    char rel[2048], dir[2600], sub[2600], stgt[2100], btgt[2100], f[2700];
    snprintf(stgt, sizeof stgt, "%s/bus/scsi", root); mkdirp(stgt);
    snprintf(btgt, sizeof btgt, "%s/class/block", root); mkdirp(btgt);

    /* ata1 (ata_port host); port_no lives at ata1/ata_port/ata1/port_no */
    snprintf(dir, sizeof dir, "%s%s/ata1", root, pcirel); mkdirp(dir);
    snprintf(f, sizeof f, "%s%s/ata1/ata_port/ata1/port_no", root, pcirel);
    mkfile(f, "1\n");
    /* ata_device dev1.0 under link1 */
    snprintf(f, sizeof f, "%s%s/ata1/link1/dev1.0/uevent", root, pcirel);
    mkfile(f, "DEVTYPE=ata_device\n");
    /* host0/target0:0:0/0:0:0:0 (scsi_device) */
    snprintf(dir, sizeof dir, "%s%s/ata1/host0", root, pcirel); mkdirp(dir);
    snprintf(rel, sizeof rel, "%s/ata1/host0/target0:0:0/0:0:0:0", pcirel);
    snprintf(dir, sizeof dir, "%s%s", root, rel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, stgt);
    /* block/sda leaf */
    snprintf(rel, sizeof rel, "%s/ata1/host0/target0:0:0/0:0:0:0/block/sda", pcirel);
    snprintf(dir, sizeof dir, "%s%s", root, rel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, btgt);

    char out[PATH_ID_MAX];
    assert(path_id_build(root, rel, out, sizeof out) > 0);
    assert(strcmp(out, "pci-0000:02:00.1-ata-1.0") == 0);
    printf("test_ata OK\n");
}

static void test_usb_scsi_rebase(void) {
    char tmpl[] = "/tmp/schema-pathid-us-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    /* .../0000:08:00.3/usb4/4-1/4-1:1.0/host9/target9:0:0/9:0:0:0/block/sdd
       ID_PATH = pci-0000:08:00.3-usb-0:1:1.0-scsi-0:0:0:0  (host9 rebased -> 0) */
    mk_pci_node(root, "/devices/pci0000:00/0000:00:07.1");
    mk_pci_node(root, "/devices/pci0000:00/0000:00:07.1/0000:08:00.3");

    const char *pcirel = "/devices/pci0000:00/0000:00:07.1/0000:08:00.3";
    char rel[2048], dir[2600], sub[2600], utgt[2100], stgt[2100], btgt[2100];
    snprintf(utgt, sizeof utgt, "%s/bus/usb", root); mkdirp(utgt);
    snprintf(stgt, sizeof stgt, "%s/bus/scsi", root); mkdirp(stgt);
    snprintf(btgt, sizeof btgt, "%s/class/block", root); mkdirp(btgt);

    const char *usbdirs[] = { "usb4", "usb4/4-1", "usb4/4-1/4-1:1.0" };
    for (int i = 0; i < 3; i++) {
        snprintf(rel, sizeof rel, "%s/%s", pcirel, usbdirs[i]);
        snprintf(dir, sizeof dir, "%s%s", root, rel); mkdirp(dir);
        snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, utgt);
    }
    /* host9 under the usb interface (only host -> rebases to 0) */
    snprintf(rel, sizeof rel, "%s/usb4/4-1/4-1:1.0/host9/target9:0:0/9:0:0:0", pcirel);
    snprintf(dir, sizeof dir, "%s%s", root, rel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, stgt);
    snprintf(rel, sizeof rel, "%s/usb4/4-1/4-1:1.0/host9/target9:0:0/9:0:0:0/block/sdd", pcirel);
    snprintf(dir, sizeof dir, "%s%s", root, rel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, btgt);

    char out[PATH_ID_MAX];
    assert(path_id_build(root, rel, out, sizeof out) > 0);
    assert(strcmp(out, "pci-0000:08:00.3-usb-0:1:1.0-scsi-0:0:0:0") == 0);
    printf("test_usb_scsi_rebase OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -Wall -Wextra tests/test_path_id.c -o /tmp/t_pathid && /tmp/t_pathid`
Expected: FAIL — scsi stub emits nothing; ata/scsi components missing.

- [ ] **Step 3: Write the implementation**

Add these two scan helpers to `path_id.h` (above `pi_handle_scsi`), then replace the `pi_handle_scsi` stub body.

Scan helpers:

```c
#include <sys/types.h>

/* Lowest integer N among sibling dirs named "<prefix>N" inside parent_dir.
 * Returns the min, or -1 if none found. */
static inline int pi_min_index(const char *parent_dir, const char *prefix) {
    DIR *d = opendir(parent_dir);
    if (!d) return -1;
    size_t plen = strlen(prefix);
    int min = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, plen) != 0) continue;
        const char *num = e->d_name + plen;
        if (num[0] < '0' || num[0] > '9') continue;
        int v = atoi(num);
        if (min < 0 || v < min) min = v;
    }
    closedir(d);
    return min;
}

/* Find the ata_device number M (suffix after '.') by scanning
 * <atadir>/link*<any>/dev<N>.<M>. Writes M to out. Returns 0/-1. */
static inline int pi_ata_devnum(const char *atadir, char *out, size_t outsz) {
    DIR *d = opendir(atadir);
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL && found != 0) {
        if (strncmp(e->d_name, "link", 4) != 0) continue;
        char linkdir[PATH_MAX];
        if ((size_t)snprintf(linkdir, sizeof linkdir, "%s/%s", atadir, e->d_name) >= sizeof linkdir)
            continue;
        DIR *ld = opendir(linkdir);
        if (!ld) continue;
        struct dirent *le;
        while ((le = readdir(ld)) != NULL) {
            if (strncmp(le->d_name, "dev", 3) != 0) continue;
            const char *dot = strrchr(le->d_name, '.');
            if (!dot) continue;
            safe_copy(out, dot + 1, outsz);
            found = 0;
            break;
        }
        closedir(ld);
    }
    closedir(d);
    return found;
}
```

`pi_handle_scsi` body (replace the stub):

```c
static inline int pi_handle_scsi(const char *leafdir, char *cur, size_t cursz,
                                 char *path, size_t pathsz) {
    (void)leafdir; (void)cursz;
    /* only a scsi_device sysname H:C:T:L */
    unsigned H, C, T, L;
    if (sscanf(pi_base(cur), "%u:%u:%u:%u", &H, &C, &T, &L) != 4) return 0;

    /* climb to the hostN dir */
    char hostdir[PATH_MAX];
    safe_copy(hostdir, cur, sizeof hostdir);
    while (strncmp(pi_base(hostdir), "host", 4) != 0) {
        if (pi_parent(hostdir) != 0) return 0;
    }
    /* the dir above hostN: ata port (ata transport) or the plain bus parent */
    char above[PATH_MAX];
    safe_copy(above, hostdir, sizeof above);
    if (pi_parent(above) != 0) return 0;
    const char *abase = pi_base(above);

    char comp[PATH_ID_MAX];
    if (strncmp(abase, "ata", 3) == 0 && abase[3] >= '0' && abase[3] <= '9') {
        /* ATA transport: ata-<port_no>.<M> */
        char port[64], atap[PATH_MAX], devnum[64];
        snprintf(atap, sizeof atap, "%s/ata_port/%s", above, abase);
        if (pi_sysattr(atap, "port_no", port, sizeof port) != 0) return 0;
        if (pi_ata_devnum(above, devnum, sizeof devnum) != 0) safe_copy(devnum, "0", sizeof devnum);
        snprintf(comp, sizeof comp, "ata-%s.%s", port, devnum);
        pi_prepend(path, pathsz, comp);
        /* consume up to the ata port's parent */
        safe_copy(cur, above, cursz);
        if (pi_parent(cur) != 0) return 1;
        return 1;
    }

    /* default transport: rebase H by the lowest sibling host index */
    char hostparent[PATH_MAX];
    safe_copy(hostparent, hostdir, sizeof hostparent);
    if (pi_parent(hostparent) != 0) return 0;
    int offset = pi_min_index(hostparent, "host");
    if (offset < 0) offset = 0;
    snprintf(comp, sizeof comp, "scsi-%u:%u:%u:%u", H - (unsigned)offset, C, T, L);
    pi_prepend(path, pathsz, comp);
    /* consume up to the host's parent */
    safe_copy(cur, hostparent, cursz);
    return 1;
}
```

Note: `pi_handle_scsi` uses `cursz` (unused-suppression removed since `safe_copy(cur, ..., cursz)` now uses it). Ensure the `(void)cursz;` line is deleted when `cursz` becomes used — keep only `(void)leafdir;`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -Wall -Wextra tests/test_path_id.c -o /tmp/t_pathid && /tmp/t_pathid`
Expected: PASS — `test_ata OK`, `test_usb_scsi_rebase OK`, all prior green.

- [ ] **Step 5: Run the full suite + boundary gate**

Run: `make test` — expected all tests pass including `ALL path_id tests passed`.
Run: `git diff master..HEAD -- schema-udev.c` — expected: empty.
Run: `grep -n path_id schema-udev.c` — expected: no output.

- [ ] **Step 6: Commit**

```bash
git add path_id.h tests/test_path_id.c
git commit -m "feat(path_id): scsi handler with ata transport and host rebase"
```

---

### Task 6: Live acceptance harness + vmtest

**Files:**
- Create: `tests/verify_path_id_live.sh`

**Interfaces:**
- Consumes: the full `path_id.h` from Tasks 1–5.
- Produces: a standalone shell script that builds a tiny driver, runs `path_id_build` over every live device with a real `ID_PATH`, and reports mismatches. This is the 0-mismatch acceptance gate.

- [ ] **Step 1: Create the harness**

Create `tests/verify_path_id_live.sh`:

```sh
#!/bin/sh
# Live parity gate: run path_id_build over every /sys device that real udev
# assigned an ID_PATH, diff against ground truth from `udevadm info`.
# Requires: a Linux box with systemd-udev populated (blakbox). Read-only.
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/pathid_driver.c <<'EOF'
#include "path_id.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    char out[PATH_ID_MAX];
    if (path_id_build("/sys", argv[1], out, sizeof out) < 0) { printf("\n"); return 0; }
    printf("%s\n", out);
    return 0;
}
EOF
cc -Wall -Wextra /tmp/pathid_driver.c -o /tmp/pathid_driver

total=0; miss=0
for dev in $(find /sys/devices -name uevent -printf '%h\n'); do
    devpath=${dev#/sys}
    want=$(udevadm info -q property -p "$dev" 2>/dev/null | sed -n 's/^ID_PATH=//p')
    [ -z "$want" ] && continue
    total=$((total+1))
    got=$(/tmp/pathid_driver "$devpath")
    if [ "$got" != "$want" ]; then
        miss=$((miss+1))
        printf 'MISMATCH %s\n  want=%s\n  got =%s\n' "$devpath" "$want" "$got"
    fi
done
printf 'path_id live parity: %d devices, %d mismatches\n' "$total" "$miss"
[ "$miss" -eq 0 ]
```

Make it executable: `chmod +x tests/verify_path_id_live.sh`

- [ ] **Step 2: Run the live gate (on blakbox)**

Run: `sh tests/verify_path_id_live.sh`
Expected: `path_id live parity: <~182> devices, 0 mismatches` and exit 0.
If any MISMATCH lines appear, they name the exact devpath + want/got — fix the responsible handler and re-run. Do not proceed with a nonzero mismatch count.

- [ ] **Step 3: vmtest (PID-1 rail regression)**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS`. path_id is not in the PID-1 path, so this only confirms nothing else regressed.

- [ ] **Step 4: Commit**

```bash
git add tests/verify_path_id_live.sh
git commit -m "test(path_id): live udev parity acceptance harness"
```

---

## Self-Review

**Spec coverage:**
- Core walk + anchor guard → Task 2. ✓
- pci handler → Task 2. ✓
- platform handler → Task 2. ✓
- usb handler (literal `0:`, interface-only, skip-to-host) → Task 3. ✓
- nvme handler (nsid from leaf) → Task 4. ✓
- scsi ata (`port_no` sysattr, `.M`) + default (host rebase) → Task 5. ✓
- `ID_PATH_TAG` transform → Task 1. ✓
- `path_id.h` new file, `test_path_id.c`, Makefile wiring → Tasks 1–5. ✓
- schema-udev.c byte-identical boundary gate → Task 5 Step 5. ✓
- Live 0-mismatch acceptance across all 182 → Task 6. ✓

**Type consistency:** `path_id_build`, `path_id_tag`, `pi_subsystem`, `pi_sysattr`, `pi_base`, `pi_parent`, `pi_prepend`, `pi_handle_usb/scsi/nvme`, `pi_min_index`, `pi_ata_devnum` — signatures identical everywhere referenced. Handlers all take `(leafdir, cur, cursz, path, pathsz)` and return int. `PATH_ID_MAX` = 512 used consistently.

**Placeholder scan:** No TBD/TODO. One deliberate cleanup note in Task 2 Step 1 flags scratch lines the implementer must delete; the authoritative asserted calls are complete. Task 4's `pi_handle_nvme` has a redundant `if (pi_parent(cur) != 0) return 1; return 1;` — intentional (advance-then-return, both paths return 1 so the walk continues from the parent regardless).

## Notes for the verifier (Claire)

- The unit tests fabricate sysfs trees, so they prove format logic but not real-topology quirks. The **live harness in Task 6 is the real gate** — re-run it yourself on blakbox and require 0 mismatches across all ~182 devices before landing.
- Confirm the boundary: `git diff master..HEAD -- schema-udev.c` empty and `grep path_id schema-udev.c` empty.
- Watch the ata case specifically: `port_no` must come from the sysattr, not the `ataN` dir name (blakbox `ata9` has `port_no=1`) — a real device where they diverge is the one that would slip past unit tests.
