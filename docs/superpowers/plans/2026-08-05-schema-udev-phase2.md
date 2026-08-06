# schema-udev Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `schema-udev` with coldplug (firing rules for already-present devices at startup via an in-process sysfs walk) and declarative symlinks (creating stable symlinks under `/dev/schema/<name>` -> `/dev/<DEVNAME>` on `add` and removing them on `remove`).

**Architecture:** Pure logic (`symlink` grammar validation, `symlink_apply`/`symlink_clear`, and `uevent_from_sysfs` synthesizer) lives in `schema-udev.h` so unit tests can test them against `/tmp` without root or touching real `/dev`. `coldplug_walk()` uses `nftw(..., FTW_PHYS)` over `/sys/devices`. Symlinks are created atomically (`symlink` to temp name + `rename()`) before `on_add` hooks run, and removed before `on_remove` hooks run.

**Tech Stack:** C99 (`-std=c99 -Wall -Wextra -D_GNU_SOURCE`), `nftw(3)` (`FTW_PHYS`), `symlink(2)`, `rename(2)`, `unlink(2)`, `readlink(2)`. Freestanding unit tests in `tests/`.

---

## Global Constraints

- Language: C99 (`-std=c99 -Wall -Wextra -D_GNU_SOURCE`). Must build clean with zero compiler warnings.
- Safety: Coldplug MUST be an in-process sysfs read walk ONLY. Never write to `/sys/*/uevent`, never touch netlink group 2 or broadcast events.
- Symlinks: Managed strictly under `#define SCHEMA_DEV_DIR "/dev/schema"`. Never touch `/dev/serial/by-id/` or other systemd-udevd directories.
- Symlink atomic operation: `symlink()` to `/dev/schema/.<name>.tmp.XXXXXX` + `rename()` over `/dev/schema/<name>`.
- Hook ordering: Symlink created *before* `on_add` execution; symlink unlinked *before* `on_remove` execution.
- Rules: Reloading rules via `SIGHUP` does NOT trigger coldplug. Coldplug runs once at startup after initial rule load.

---

### Task 1: Declarative Symlink Grammar + Pure Validation (Header + Unit Test)

**Files:**
- Modify: `schema-udev.h` (add `char symlink[64];` to `struct dev_rule`; handle `symlink` key + validation in `dev_rule_set`)
- Create: `tests/test_symlink.c`
- Modify: `Makefile` (add compile line to `test:` target)

**Interfaces:**
- Produces: `struct dev_rule` with field `char symlink[64];`
- Updates: `dev_rule_set(r, "symlink", val)` → `0` ok, `-1` invalid name (empty, `/`, `..`, length >= 64).

- [ ] **Step 1: Write the failing test** — `tests/test_symlink.c`

```c
#include "../schema-udev.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    struct dev_rule r; memset(&r, 0, sizeof r);

    /* Valid symlink name */
    assert(dev_rule_set(&r, "symlink", "esp32") == 0);
    assert(strcmp(r.symlink, "esp32") == 0);

    /* Valid with underscores and dashes */
    memset(&r, 0, sizeof r);
    assert(dev_rule_set(&r, "symlink", "my_device-1") == 0);
    assert(strcmp(r.symlink, "my_device-1") == 0);

    /* Invalid: empty */
    memset(&r, 0, sizeof r);
    assert(dev_rule_set(&r, "symlink", "") == -1);
    assert(r.symlink[0] == '\0');

    /* Invalid: contains slash */
    memset(&r, 0, sizeof r);
    assert(dev_rule_set(&r, "symlink", "sub/esp32") == -1);
    assert(r.symlink[0] == '\0');

    /* Invalid: dot-dot traversal */
    memset(&r, 0, sizeof r);
    assert(dev_rule_set(&r, "symlink", "..") == -1);
    assert(r.symlink[0] == '\0');

    /* Invalid: 64 chars (exceeds max 63) */
    char longname[128]; memset(longname, 'a', 64); longname[64] = '\0';
    memset(&r, 0, sizeof r);
    assert(dev_rule_set(&r, "symlink", longname) == -1);
    assert(r.symlink[0] == '\0');

    printf("test_symlink (grammar): OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_symlink.c -o /tmp/t-sym-gram && /tmp/t-sym-gram`
Expected: FAIL — `symlink` field / validation missing or undefined.

- [ ] **Step 3: Modify `schema-udev.h`**

Add `char symlink[64];` to `struct dev_rule`. Update `dev_rule_set`:
```c
    } else if (strcmp(key, "symlink") == 0) {
        size_t len = strlen(val);
        if (len == 0 || len >= 64 || strchr(val, '/') || strstr(val, "..")) return -1;
        snprintf(r->symlink, sizeof r->symlink, "%s", val);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_symlink.c -o /tmp/t-sym-gram && /tmp/t-sym-gram`
Expected: PASS — `test_symlink (grammar): OK`.

- [ ] **Step 5: Wire into Makefile `test:` target**

Add compile & run line for `tests/test_symlink.c` in `Makefile`.

- [ ] **Step 6: Run `make test` & commit**

```bash
git add schema-udev.h tests/test_symlink.c Makefile
git commit -m "feat(schema-udev): symlink grammar key + path validation & tests"
```

---

### Task 2: Symlink Apply / Clear Functions (Header + Unit Test)

**Files:**
- Modify: `schema-udev.h` (add `symlink_apply` and `symlink_clear` helpers taking explicit base directory)
- Modify: `tests/test_symlink.c` (add apply/clear tests)
- Modify: `Makefile`

**Interfaces:**
- Produces: `int symlink_apply(const char *base_dir, const char *name, const char *devname)` → `0` ok, `-1` fail. Atomically creates `/base_dir/name` -> `/dev/devname` (or `devname` directly if it starts with `/`).
- Produces: `int symlink_clear(const char *base_dir, const char *name)` → `0` ok (unlinked or ignored `ENOENT`), `-1` fail.

- [ ] **Step 1: Append test cases to `tests/test_symlink.c`**

```c
#include <unistd.h>
#include <sys/stat.h>

void test_symlink_fs(void) {
    char tmpl[] = "/tmp/schema-udev-test-symXXXXXX";
    char *base = mkdtemp(tmpl);
    assert(base);

    char linkpath[256];
    snprintf(linkpath, sizeof linkpath, "%s/esp32", base);

    /* Create symlink: esp32 -> /dev/ttyUSB0 */
    assert(symlink_apply(base, "esp32", "ttyUSB0") == 0);

    char target[256];
    ssize_t len = readlink(linkpath, target, sizeof(target) - 1);
    assert(len > 0);
    target[len] = '\0';
    assert(strcmp(target, "/dev/ttyUSB0") == 0);

    /* Atomic overwrite: change target to /dev/ttyUSB1 */
    assert(symlink_apply(base, "esp32", "ttyUSB1") == 0);
    len = readlink(linkpath, target, sizeof(target) - 1);
    assert(len > 0);
    target[len] = '\0';
    assert(strcmp(target, "/dev/ttyUSB1") == 0);

    /* Absolute devname specified */
    assert(symlink_apply(base, "esp32", "/dev/bus/usb/001/002") == 0);
    len = readlink(linkpath, target, sizeof(target) - 1);
    assert(len > 0);
    target[len] = '\0';
    assert(strcmp(target, "/dev/bus/usb/001/002") == 0);

    /* Clear symlink */
    assert(symlink_clear(base, "esp32") == 0);
    assert(access(linkpath, F_OK) != 0);

    /* Clear non-existent symlink -> no error */
    assert(symlink_clear(base, "esp32") == 0);

    /* Clean up temp dir */
    rmdir(base);
}
```
Call `test_symlink_fs()` inside `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_symlink.c -o /tmp/t-sym && /tmp/t-sym`
Expected: FAIL — `symlink_apply` / `symlink_clear` undefined.

- [ ] **Step 3: Add `symlink_apply` and `symlink_clear` to `schema-udev.h`**

```c
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#define SCHEMA_DEV_DIR "/dev/schema"

static inline int symlink_apply(const char *base_dir, const char *name, const char *devname) {
    if (!base_dir || !name || !devname || name[0] == '\0') return -1;
    
    struct stat st;
    if (stat(base_dir, &st) != 0) {
        if (mkdir(base_dir, 0755) != 0 && errno != EEXIST) return -1;
    }

    char target[512];
    if (devname[0] == '/') snprintf(target, sizeof target, "%s", devname);
    else snprintf(target, sizeof target, "/dev/%s", devname);

    char tmppath[512], finalpath[512];
    snprintf(tmppath, sizeof tmppath, "%s/.%s.tmp.%d", base_dir, name, (int)getpid());
    snprintf(finalpath, sizeof finalpath, "%s/%s", base_dir, name);

    unlink(tmppath);
    if (symlink(target, tmppath) != 0) return -1;
    if (rename(tmppath, finalpath) != 0) {
        unlink(tmppath);
        return -1;
    }
    return 0;
}

static inline int symlink_clear(const char *base_dir, const char *name) {
    if (!base_dir || !name || name[0] == '\0') return -1;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", base_dir, name);
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_symlink.c -o /tmp/t-sym && /tmp/t-sym`
Expected: PASS — `test_symlink: OK`.

- [ ] **Step 5: Run `make test` & commit**

```bash
git add schema-udev.h tests/test_symlink.c Makefile
git commit -m "feat(schema-udev): atomic symlink_apply and symlink_clear functions + tests"
```

---

### Task 3: sysfs uevent Synthesizer (Header + Unit Test)

**Files:**
- Modify: `schema-udev.h` (add `uevent_from_sysfs`)
- Create: `tests/test_coldplug.c`
- Modify: `Makefile`

**Interfaces:**
- Produces: `int uevent_from_sysfs(const char *dirpath, struct uevent *ev)` → `0` ok, `-1` fail (no `uevent` file).
  - Reads `dirpath/uevent`.
  - Sets `ACTION=add`.
  - Sets `DEVPATH=` (dirpath with leading `/sys` stripped, starting with `/devices/`).
  - Sets `SUBSYSTEM=` from `readlink(dirpath/subsystem)` basename if present.
  - Parses `KEY=VALUE` lines into `ev`.

- [ ] **Step 1: Write the failing test** — `tests/test_coldplug.c`

```c
#include "../schema-udev.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int main(void) {
    char tmpl[] = "/tmp/schema-udev-test-sysXXXXXX";
    char *sysroot = mkdtemp(tmpl);
    assert(sysroot);

    /* Build fake sysfs path: <sysroot>/devices/pci0000:00/0000:00:14.0/usb1/1-4/1-4:1.0/ttyUSB0 */
    char devdir[512];
    snprintf(devdir, sizeof devdir, "%s/devices/pci0000:00/0000:00:14.0/usb1/1-4/1-4:1.0/ttyUSB0", sysroot);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", devdir);
    assert(system(cmd) == 0);

    /* Create subsystem symlink -> .../class/tty */
    char subtarget[512], sublink[512];
    snprintf(subtarget, sizeof subtarget, "%s/class/tty", sysroot);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", subtarget);
    assert(system(cmd) == 0);
    snprintf(sublink, sizeof sublink, "%s/subsystem", devdir);
    assert(symlink(subtarget, sublink) == 0);

    /* Create uevent file */
    char uevent_file[512];
    snprintf(uevent_file, sizeof uevent_file, "%s/uevent", devdir);
    FILE *f = fopen(uevent_file, "w");
    assert(f);
    fputs("MAJOR=188\nMINOR=0\nDEVNAME=ttyUSB0\nPRODUCT=10c4/ea60/100\n", f);
    fclose(f);

    /* Synthesize struct uevent */
    struct uevent ev;
    assert(uevent_from_sysfs(sysroot, devdir, &ev) == 0);

    assert(strcmp(uevent_get(&ev, "ACTION"), "add") == 0);
    assert(strcmp(uevent_get(&ev, "DEVPATH"), "/devices/pci0000:00/0000:00:14.0/usb1/1-4/1-4:1.0/ttyUSB0") == 0);
    assert(strcmp(uevent_get(&ev, "SUBSYSTEM"), "tty") == 0);
    assert(strcmp(uevent_get(&ev, "DEVNAME"), "ttyUSB0") == 0);
    assert(strcmp(uevent_get(&ev, "MAJOR"), "188") == 0);
    assert(strcmp(uevent_get(&ev, "PRODUCT"), "10c4/ea60/100") == 0);

    /* Assert matching rule fires */
    struct dev_rule r; memset(&r, 0, sizeof r);
    dev_rule_set(&r, "name", "esp32");
    dev_rule_set(&r, "match_subsystem", "tty");
    dev_rule_set(&r, "match_product", "10c4/*");
    dev_rule_set(&r, "on_add", "/bin/true");
    assert(dev_rule_match(&r, &ev) == 1);

    printf("test_coldplug (uevent_from_sysfs): OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_coldplug.c -o /tmp/t-cold-syn && /tmp/t-cold-syn`
Expected: FAIL — `uevent_from_sysfs` undefined.

- [ ] **Step 3: Implement `uevent_from_sysfs` in `schema-udev.h`**

```c
/* Synthesize a struct uevent from a sysfs device directory D (which contains a uevent file).
 * sysroot is the sysfs root path (usually "/sys", or a /tmp test root).
 * Strips sysroot from dirpath to yield DEVPATH starting with "/devices/". */
static inline int uevent_from_sysfs(const char *sysroot, const char *dirpath, struct uevent *ev) {
    char upath[512];
    snprintf(upath, sizeof upath, "%s/uevent", dirpath);
    FILE *f = fopen(upath, "r");
    if (!f) return -1;

    memset(ev, 0, sizeof *ev);

    /* Set ACTION=add */
    snprintf(ev->key[ev->n], UE_KEY_MAX, "ACTION");
    snprintf(ev->val[ev->n], UE_VAL_MAX, "add");
    ev->n++;

    /* Set DEVPATH (strip sysroot prefix) */
    const char *devpath = dirpath;
    size_t sroot_len = strlen(sysroot);
    if (strncmp(dirpath, sysroot, sroot_len) == 0) devpath = dirpath + sroot_len;
    snprintf(ev->key[ev->n], UE_KEY_MAX, "DEVPATH");
    snprintf(ev->val[ev->n], UE_VAL_MAX, "%s", devpath);
    ev->n++;

    /* Resolve SUBSYSTEM from subsystem symlink */
    char sublink[512], subtarget[512];
    snprintf(sublink, sizeof sublink, "%s/subsystem", dirpath);
    ssize_t slen = readlink(sublink, subtarget, sizeof(subtarget) - 1);
    if (slen > 0) {
        subtarget[slen] = '\0';
        char *bname = strrchr(subtarget, '/');
        const char *subsys = bname ? bname + 1 : subtarget;
        snprintf(ev->key[ev->n], UE_KEY_MAX, "SUBSYSTEM");
        snprintf(ev->val[ev->n], UE_VAL_MAX, "%s", subsys);
        ev->n++;
    }

    /* Read KEY=VALUE lines from uevent file */
    char line[512];
    while (fgets(line, sizeof line, f) && ev->n < UE_MAX_KEYS) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';
        
        /* Skip duplicate ACTION, DEVPATH, SUBSYSTEM if in file */
        if (uevent_get(ev, line) != NULL) continue;

        snprintf(ev->key[ev->n], UE_KEY_MAX, "%s", line);
        snprintf(ev->val[ev->n], UE_VAL_MAX, "%s", val);
        ev->n++;
    }

    fclose(f);
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_coldplug.c -o /tmp/t-cold-syn && /tmp/t-cold-syn`
Expected: PASS — `test_coldplug (uevent_from_sysfs): OK`.

- [ ] **Step 5: Wire into Makefile & commit**

```bash
git add schema-udev.h tests/test_coldplug.c Makefile
git commit -m "feat(schema-udev): uevent_from_sysfs synthesizer + tests"
```

---

### Task 4: Coldplug sysfs Walker (`coldplug_walk()`) (Header/C + Unit Test)

**Files:**
- Modify: `schema-udev.h` (add `coldplug_walk_root` helper using `nftw`)
- Modify: `tests/test_coldplug.c` (test walking a multi-device directory tree)
- Modify: `Makefile`

**Interfaces:**
- Produces: `int coldplug_walk_root(const char *sysroot, void (*on_event)(const struct uevent *ev))` → walks `sysroot/devices`, finds every `uevent` file, calls `uevent_from_sysfs`, passes synthesized `struct uevent` to `on_event`.

- [ ] **Step 1: Append coldplug walker test to `tests/test_coldplug.c`**

```c
static int g_coldplug_events = 0;
static void test_handler(const struct uevent *ev) {
    if (strcmp(uevent_get(ev, "ACTION"), "add") == 0) g_coldplug_events++;
}

void test_coldplug_walk(void) {
    char tmpl[] = "/tmp/schema-udev-test-walkXXXXXX";
    char *sysroot = mkdtemp(tmpl);
    assert(sysroot);

    /* Create sysroot/devices/dev1/uevent and sysroot/devices/dev2/uevent */
    char d1[512], d2[512], cmd[1024];
    snprintf(d1, sizeof d1, "%s/devices/dev1", sysroot);
    snprintf(d2, sizeof d2, "%s/devices/dev2", sysroot);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s' '%s'", d1, d2);
    assert(system(cmd) == 0);

    char u1[512], u2[512];
    snprintf(u1, sizeof u1, "%s/uevent", d1); FILE *f1 = fopen(u1, "w"); fputs("DEVNAME=dev1\n", f1); fclose(f1);
    snprintf(u2, sizeof u2, "%s/uevent", d2); FILE *f2 = fopen(u2, "w"); fputs("DEVNAME=dev2\n", f2); fclose(f2);

    g_coldplug_events = 0;
    assert(coldplug_walk_root(sysroot, test_handler) == 0);
    assert(g_coldplug_events == 2);
}
```
Call `test_coldplug_walk()` in `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_coldplug.c -o /tmp/t-cold && /tmp/t-cold`
Expected: FAIL — `coldplug_walk_root` undefined.

- [ ] **Step 3: Implement `coldplug_walk_root` in `schema-udev.h`**

```c
#include <ftw.h>

static const char *g_coldplug_sysroot = NULL;
static void (*g_coldplug_cb)(const struct uevent *ev) = NULL;

static int coldplug_ftw_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag == FTW_F) {
        const char *bname = strrchr(fpath, '/');
        if (bname && strcmp(bname + 1, "uevent") == 0) {
            char dirpath[512];
            snprintf(dirpath, sizeof dirpath, "%s", fpath);
            char *last_slash = strrchr(dirpath, '/');
            if (last_slash) *last_slash = '\0';
            
            struct uevent ev;
            if (uevent_from_sysfs(g_coldplug_sysroot, dirpath, &ev) == 0 && g_coldplug_cb) {
                g_coldplug_cb(&ev);
            }
        }
    }
    return 0;
}

static inline int coldplug_walk_root(const char *sysroot, void (*on_event)(const struct uevent *ev)) {
    char devroot[512];
    snprintf(devroot, sizeof devroot, "%s/devices", sysroot);
    struct stat st;
    if (stat(devroot, &st) != 0) return 0;  /* missing sysfs dir -> no-op */

    g_coldplug_sysroot = sysroot;
    g_coldplug_cb = on_event;
    return nftw(devroot, coldplug_ftw_cb, 32, FTW_PHYS);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_coldplug.c -o /tmp/t-cold && /tmp/t-cold`
Expected: PASS — `test_coldplug: OK`.

- [ ] **Step 5: Run `make test` & commit**

```bash
git add schema-udev.h tests/test_coldplug.c Makefile
git commit -m "feat(schema-udev): sysfs coldplug_walk_root using nftw + tests"
```

---

### Task 5: Wire Symlinks & Coldplug into Daemon (`schema-udev.c`) + Live Integration Test

**Files:**
- Modify: `schema-udev.c`

**Interfaces:**
- Updates `dispatch()` in `schema-udev.c`:
  - On `ACTION=add`: If matching rule has `symlink` set and event has `DEVNAME`, call `symlink_apply(SCHEMA_DEV_DIR, r->symlink, devname)` *before* `run_hook()`.
  - On `ACTION=remove`: If matching rule has `symlink` set, call `symlink_clear(SCHEMA_DEV_DIR, r->symlink)` *before* `run_hook()`.
- Updates `main()` in `schema-udev.c`:
  - Calls `coldplug_walk_root("/sys", dispatch)` right after initial `rules_reload()`, before `poll()` loop.

- [ ] **Step 1: Modify `schema-udev.c`**

In `dispatch()`:
```c
        if (hook) {
            if (strcmp(action, "add") == 0 && g_rules[i].symlink[0]) {
                const char *dn = uevent_get(ev, "DEVNAME");
                if (dn) {
                    if (symlink_apply(SCHEMA_DEV_DIR, g_rules[i].symlink, dn) == 0) {
                        fprintf(stderr, "[schema-udev] created symlink %s/%s -> %s\n",
                                SCHEMA_DEV_DIR, g_rules[i].symlink, dn);
                    }
                }
            } else if (strcmp(action, "remove") == 0 && g_rules[i].symlink[0]) {
                symlink_clear(SCHEMA_DEV_DIR, g_rules[i].symlink);
                fprintf(stderr, "[schema-udev] removed symlink %s/%s\n",
                        SCHEMA_DEV_DIR, g_rules[i].symlink);
            }

            fprintf(stderr, "[schema-udev] matched %s %s %s -> %s\n",
                    g_rules[i].name, action,
                    uevent_get(ev, "DEVNAME") ? uevent_get(ev, "DEVNAME") : uevent_get(ev, "DEVPATH"),
                    hook);
            run_hook(hook, ev);
        }
```

In `main()`:
```c
    rules_reload();
    fprintf(stderr, "[schema-udev] running coldplug sysfs walk...\n");
    coldplug_walk_root("/sys", dispatch);
    fprintf(stderr, "[schema-udev] listening on kernel uevent netlink (group 1)\n");
```

- [ ] **Step 2: Build `schema-udev` & run `make test`**

Run: `make schema-udev && make test`
Expected: builds clean without warnings, all tests pass.

- [ ] **Step 3: Live integration test — symlink creation & coldplug firing**

```bash
sudo mkdir -p /etc/schema-init/dev
d=$(ls -d /sys/bus/usb/devices/*/ | grep -E '/[0-9]+-[0-9]+/$' | head -1)
prod=$(sed -n 's/^PRODUCT=//p' "$d/uevent")

# Write rule with symlink and on_add hook
printf 'name=phase2-test\nmatch_subsystem=usb\nmatch_product=%s\nsymlink=phase2-usb-link\non_add=/usr/bin/touch /tmp/phase2-hook-marker\n' "$prod" \
  | sudo tee /etc/schema-init/dev/phase2.dev

rm -f /tmp/phase2-hook-marker
sudo rm -f /dev/schema/phase2-usb-link

# Start daemon -> coldplug walk should fire on_add and create symlink for already present USB device!
sudo ./schema-udev &
sleep 0.5
sudo kill %1

# Assert coldplug created symlink and fired hook!
ls -l /dev/schema/phase2-usb-link
ls -l /tmp/phase2-hook-marker

# Cleanup
sudo rm -f /etc/schema-init/dev/phase2.dev /dev/schema/phase2-usb-link /tmp/phase2-hook-marker
```
Expected: `/dev/schema/phase2-usb-link` exists and points to the USB device, `/tmp/phase2-hook-marker` exists.

- [ ] **Step 4: Commit**

```bash
git add schema-udev.c
git commit -m "feat(schema-udev): wire coldplug sysfs walk and /dev/schema/ symlink lifecycle into daemon"
```

---

### Task 6: Packaging, Documentation, Inert Example, and vmtest

**Files:**
- Modify: `assets/example.dev` (add commented `symlink=` example line)
- Modify: `README.md` (document Phase 2 `symlink=` and `/dev/schema/`)
- Test: `schema-vmtest`

- [ ] **Step 1: Update `assets/example.dev`**

Add commented `symlink=` example line:
```ini
# symlink=esp32                                # creates /dev/schema/esp32 -> /dev/ttyUSB0
```

- [ ] **Step 2: Update `README.md`**

Add section detailing `symlink=<name>`, `/dev/schema/` namespace, and startup coldplug behavior.

- [ ] **Step 3: Run `make test` & `schema-vmtest`**

Run: `make test && ~/schema-livetest/vmtest.sh`
Expected: `make test` green, `vmtest` PASSES.

- [ ] **Step 4: Commit**

```bash
git add assets/example.dev README.md
git commit -m "docs(schema-udev): document symlink key, /dev/schema/ namespace, and coldplug"
```
