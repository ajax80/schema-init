# schema-udev shadow db writer (sub-project B slice 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** schema-udev writes udev-format device db records (owned-subset: derived `E:` props + trailing `V:1`) to a shadow dir `/run/schema-udev/data`, live during coldplug and hot events, verified by file-vs-file parity against udevd's real `/run/udev/data`.

**Architecture:** Extract the existing db functions out of `schema-udev.h` (and the reader out of `udev-parity.h`) into a new `udev_db.h` — the single db home matching the `udev_builtins.h`/`udev_rules.h` module pattern — fixing two real defects while moving: `udev_db_record_build` must serialize only the `[kernel_n, ev->n)` derived delta with a *trailing* `V:1` (was: all props, leading `V:1`), and `udev_db_write` must be atomic (mkstemp+rename, was: bare `fopen("w")`). Add `udev_db_remove`. Wire one call site into `dispatch()`. Verify with a file-vs-file parity mode in `tools/udev-parity.c` plus a live gate.

**Tech Stack:** C (C11, header-only inline modules), POSIX syscalls, existing schema-init Makefile + test harness, `~/schema-livetest/vmtest.sh` boot rail.

## Global Constraints

- schema-udev listens on kernel uevent netlink **group 1 ONLY**. This slice adds a disk writer, NO socket changes. `sa.nl_groups = 1` and its "NEVER group 2" comment stay byte-identical. Group-2 libudev rebroadcast is out of scope (near-cutover).
- Shadow dir is `/run/schema-udev/data`. NEVER write to udevd's real `/run/udev/data` — it is read-only ground truth.
- Persisted record is the **owned subset only**: `E:` for the `[kernel_n, ev->n)` derived delta + trailing `V:1`. Do NOT emit `S:`/`G:`/`Q:`/`I:` lines (later slices).
- `kernel_n` = `ev->n` captured immediately BEFORE `run_builtins`. `run_builtins`/`run_rules` only append, so `[kernel_n, ev->n)` is exactly the synthesized props.
- Parity is semantic/set-based (order-independent). Byte-parity is a non-goal.
- Verification honesty (slice-1 lesson): gates assert the tool's own computed counters. NEVER `grep -v` a category to zero to force a PASS.
- Terse code: no docstrings/comments beyond what exists in the surrounding files. `printf` over heredoc in shell.

---

### Task 1: Extract db functions into `udev_db.h` (no behavior change)

Pure move + rewire. Contract stays identical this task so existing tests stay green; the fixes land in Task 2.

**Files:**
- Create: `udev_db.h`
- Modify: `schema-udev.h` (remove the three db functions), `udev-parity.h` (remove `udev_db_read_eprops` + `UDEV_DB_DIR`, add include), `tests/test_udev_db.c` (add include), `Makefile` (`parity` target deps)

**Interfaces:**
- Produces (moved verbatim, unchanged signatures this task): `udev_db_filename(const struct uevent*, char*, size_t) -> int`, `udev_db_record_build(const struct uevent*, char*, size_t) -> ssize_t`, `udev_db_write(const char*, const struct uevent*) -> int`, `udev_db_read_eprops(const char*, struct uevent*) -> int`. Defines `SCHEMA_UDEV_DB_DIR "/run/schema-udev/data"` and `UDEV_DB_DIR "/run/udev/data"`.

- [ ] **Step 1: Create `udev_db.h` with the moved functions**

Create `udev_db.h`. Header guard `UDEV_DB_H`. Include `schema-udev.h` and the syscall headers the functions use. Move `udev_db_filename`, `udev_db_record_build`, `udev_db_write` verbatim from `schema-udev.h` (lines ~360-406), and `udev_db_read_eprops` verbatim from `udev-parity.h`. Add both dir defines.

```c
#ifndef UDEV_DB_H
#define UDEV_DB_H

#include "schema-udev.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SCHEMA_UDEV_DB_DIR "/run/schema-udev/data"   /* OUR shadow dir */
#define UDEV_DB_DIR        "/run/udev/data"          /* udevd's real dir (read-only) */

/* (paste udev_db_filename verbatim from schema-udev.h) */
/* (paste udev_db_record_build verbatim from schema-udev.h) */
/* (paste udev_db_write verbatim from schema-udev.h) */
/* (paste udev_db_read_eprops verbatim from udev-parity.h) */

#endif /* UDEV_DB_H */
```

- [ ] **Step 2: Remove the moved functions from their old homes**

Delete the three `udev_db_*` functions (filename/record_build/write) from `schema-udev.h`. Delete `udev_db_read_eprops` and the `#define UDEV_DB_DIR "/run/udev/data"` from `udev-parity.h`, and add `#include "udev_db.h"` near the top of `udev-parity.h` (after its `#include "schema-udev.h"`). Add `#include "udev_db.h"` to `tests/test_udev_db.c` (after its `#include "../schema-udev.h"`).

- [ ] **Step 3: Update Makefile `parity` target dependency line**

`Makefile:53` — add the new headers so the target rebuilds when they change:

```make
parity: tools/udev-parity.c udev-parity.h udev_db.h udev_rules.h udev_builtins.h schema-udev.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o udev-parity tools/udev-parity.c
```

- [ ] **Step 4: Build everything and run the existing db test (still old contract)**

Run: `make clean && make && make test` (or the specific line `$(CC) $(CFLAGS) tests/test_udev_db.c -o /tmp/schema-test-udevdb && /tmp/schema-test-udevdb`)
Expected: compiles clean, `test_udev_db: OK`, and `udev-parity` still builds. No behavior changed.

- [ ] **Step 5: Commit**

```bash
git add udev_db.h schema-udev.h udev-parity.h tests/test_udev_db.c Makefile
git commit -m "refactor(db): extract db funcs into udev_db.h (no behavior change)"
```

---

### Task 2: Fix `udev_db_record_build` (delta + trailing V:1) and `udev_db_write` (atomic)

**Files:**
- Modify: `udev_db.h`
- Test: `tests/test_udev_db.c` (rewrite to new contract)

**Interfaces:**
- Consumes: Task 1's `udev_db.h`.
- Produces: `udev_db_record_build(const struct uevent *ev, int kernel_n, char *buf, size_t bufsz) -> ssize_t` — emits `E:k=v\n` for each `i` in `[kernel_n, ev->n)` with non-empty key AND value, then a trailing `V:1\n`; returns bytes used (`< bufsz`) or `-1` on overflow. `udev_db_write(const char *base_dir, const struct uevent *ev, int kernel_n) -> int` — atomic write via mkstemp+rename, creates `base_dir` (recursively) on demand.

- [ ] **Step 1: Rewrite `tests/test_udev_db.c` to the new contract (failing test)**

Replace the whole file. Seeds a uevent with kernel props at `[0, kernel_n)` and derived props after, asserts only the derived delta is serialized, `V:1` is the LAST line, and round-trips through `udev_db_write`/`udev_db_read_eprops` in a `mkdtemp` dir.

```c
#include "../udev_db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

int main(void) {
    char name[128];

    struct uevent c; memset(&c, 0, sizeof c);
    put(&c, "SUBSYSTEM", "mem"); put(&c, "MAJOR", "1"); put(&c, "MINOR", "3");
    put(&c, "DEVPATH", "/devices/virtual/mem/null");
    assert(udev_db_filename(&c, name, sizeof name) == 0 && strcmp(name, "c1:3") == 0);

    struct uevent b; memset(&b, 0, sizeof b);
    put(&b, "SUBSYSTEM", "block"); put(&b, "MAJOR", "8"); put(&b, "MINOR", "0");
    assert(udev_db_filename(&b, name, sizeof name) == 0 && strcmp(name, "b8:0") == 0);

    struct uevent ndev; memset(&ndev, 0, sizeof ndev);
    put(&ndev, "SUBSYSTEM", "net"); put(&ndev, "IFINDEX", "2");
    assert(udev_db_filename(&ndev, name, sizeof name) == 0 && strcmp(name, "n2") == 0);

    struct uevent o; memset(&o, 0, sizeof o);
    put(&o, "SUBSYSTEM", "acpi"); put(&o, "DEVPATH", "/devices/LNXSYSTM:00/AMDI0030:00");
    assert(udev_db_filename(&o, name, sizeof name) == 0 && strcmp(name, "+acpi:AMDI0030:00") == 0);

    /* delta serialization: kernel props [0,kernel_n), derived props after */
    struct uevent d; memset(&d, 0, sizeof d);
    put(&d, "SUBSYSTEM", "block"); put(&d, "MAJOR", "8"); put(&d, "MINOR", "0");
    put(&d, "DEVPATH", "/devices/x");
    int kernel_n = d.n;
    put(&d, "ID_FS_TYPE", "ext4"); put(&d, "ID_FS_UUID", "abc");
    char rec[4096];
    ssize_t rn = udev_db_record_build(&d, kernel_n, rec, sizeof rec);
    assert(rn > 0);
    assert(strcmp(rec, "E:ID_FS_TYPE=ext4\nE:ID_FS_UUID=abc\nV:1\n") == 0);
    assert(strstr(rec, "E:SUBSYSTEM=") == NULL);
    assert(strstr(rec, "E:MAJOR=") == NULL);
    assert(strstr(rec, "E:DEVPATH=") == NULL);
    /* V:1 is the trailing line */
    assert(strcmp(rec + rn - 4, "V:1\n") == 0);

    /* empty value skipped */
    struct uevent e; memset(&e, 0, sizeof e);
    int en = e.n;
    put(&e, "ID_A", "x"); put(&e, "ID_EMPTY", ""); put(&e, "ID_B", "y");
    ssize_t ern = udev_db_record_build(&e, en, rec, sizeof rec);
    assert(ern > 0 && strstr(rec, "ID_EMPTY") == NULL);
    assert(strstr(rec, "E:ID_A=x\n") && strstr(rec, "E:ID_B=y\n"));

    /* write -> read-back round-trips the derived E: set */
    char tmpl[] = "/tmp/schema-udev-db-XXXXXX";
    char *base = mkdtemp(tmpl); assert(base);
    assert(udev_db_write(base, &d, kernel_n) == 0);
    char path[256]; snprintf(path, sizeof path, "%s/b8:0", base);
    struct uevent got; assert(udev_db_read_eprops(path, &got) == 0);
    assert(got.n == 2);
    assert(strcmp(uevent_get(&got, "ID_FS_TYPE"), "ext4") == 0);
    assert(strcmp(uevent_get(&got, "ID_FS_UUID"), "abc") == 0);

    /* overflow -> -1, nothing usable */
    assert(udev_db_record_build(&d, kernel_n, rec, 3) == -1);

    unlink(path); rmdir(base);
    printf("test_udev_db: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `$(CC) $(CFLAGS) tests/test_udev_db.c -o /tmp/schema-test-udevdb && /tmp/schema-test-udevdb` (use the actual `CC`/`CFLAGS`, e.g. `cc -O2 -Wall -Wextra -std=gnu11`)
Expected: FAIL — compile error (arity mismatch: `udev_db_record_build`/`udev_db_write` still take the old signatures).

- [ ] **Step 3: Implement the fixed `udev_db_record_build`**

Replace the function body in `udev_db.h`:

```c
static inline ssize_t udev_db_record_build(const struct uevent *ev, int kernel_n,
                                           char *buf, size_t bufsz) {
    size_t used = 0;
    for (int i = kernel_n; i < ev->n; i++) {
        if (!ev->key[i][0] || !ev->val[i][0]) continue;
        int w = snprintf(buf + used, bufsz - used, "E:%s=%s\n", ev->key[i], ev->val[i]);
        if (w < 0 || (size_t)w >= bufsz - used) return -1;
        used += (size_t)w;
    }
    int w = snprintf(buf + used, bufsz - used, "V:1\n");
    if (w < 0 || (size_t)w >= bufsz - used) return -1;
    used += (size_t)w;
    return (ssize_t)used;
}
```

- [ ] **Step 4: Implement the atomic `udev_db_write` (+ recursive dir helper)**

Add a small recursive mkdir helper above `udev_db_write`, then replace `udev_db_write`:

```c
static inline int udev_db_ensure_dir(const char *d) {
    if (mkdir(d, 0755) == 0 || errno == EEXIST) return 0;
    if (errno != ENOENT) return -1;
    char parent[512];
    safe_copy(parent, d, sizeof parent);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return -1;
    *slash = '\0';
    if (udev_db_ensure_dir(parent) != 0) return -1;
    return (mkdir(d, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static inline int udev_db_write(const char *base_dir, const struct uevent *ev, int kernel_n) {
    char name[128];
    if (udev_db_filename(ev, name, sizeof name) != 0) return -1;
    if (udev_db_ensure_dir(base_dir) != 0) return -1;
    char buf[8192];
    ssize_t len = udev_db_record_build(ev, kernel_n, buf, sizeof buf);
    if (len < 0) return -1;
    char final[512], tmpl[512];
    if ((size_t)snprintf(final, sizeof final, "%s/%s", base_dir, name) >= sizeof final) return -1;
    if ((size_t)snprintf(tmpl, sizeof tmpl, "%s/.dbXXXXXX", base_dir) >= sizeof tmpl) return -1;
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    ssize_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, (size_t)(len - off));
        if (w < 0) { close(fd); unlink(tmpl); return -1; }
        off += w;
    }
    if (close(fd) != 0) { unlink(tmpl); return -1; }
    if (rename(tmpl, final) != 0) { unlink(tmpl); return -1; }
    return 0;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `$(CC) $(CFLAGS) tests/test_udev_db.c -o /tmp/schema-test-udevdb && /tmp/schema-test-udevdb`
Expected: PASS — `test_udev_db: OK`.

- [ ] **Step 6: Commit**

```bash
git add udev_db.h tests/test_udev_db.c
git commit -m "fix(db): record_build serializes derived delta + trailing V:1; atomic write"
```

---

### Task 3: Add `udev_db_remove`

**Files:**
- Modify: `udev_db.h`
- Test: `tests/test_udev_db.c` (append remove cases)

**Interfaces:**
- Produces: `udev_db_remove(const char *base_dir, const struct uevent *ev) -> int` — `udev_db_filename` → `unlink(base_dir/<key>)`; ENOENT is success; returns 0 on success (incl. already-absent), -1 on other errors or no derivable key.

- [ ] **Step 1: Add failing remove assertions to `tests/test_udev_db.c`**

Insert before the final `unlink(path); rmdir(base);` line:

```c
    /* remove unlinks the record; a second remove is still success (ENOENT) */
    assert(udev_db_remove(base, &d) == 0);
    struct uevent gone;
    assert(udev_db_read_eprops(path, &gone) != 0);   /* file is gone */
    assert(udev_db_remove(base, &d) == 0);            /* idempotent */
```

- [ ] **Step 2: Run to verify it fails**

Run: `$(CC) $(CFLAGS) tests/test_udev_db.c -o /tmp/schema-test-udevdb && /tmp/schema-test-udevdb`
Expected: FAIL — `udev_db_remove` undefined (link/compile error).

- [ ] **Step 3: Implement `udev_db_remove`**

Add to `udev_db.h`:

```c
static inline int udev_db_remove(const char *base_dir, const struct uevent *ev) {
    char name[128];
    if (udev_db_filename(ev, name, sizeof name) != 0) return -1;
    char path[512];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", base_dir, name) >= sizeof path) return -1;
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `$(CC) $(CFLAGS) tests/test_udev_db.c -o /tmp/schema-test-udevdb && /tmp/schema-test-udevdb`
Expected: PASS — `test_udev_db: OK`.

- [ ] **Step 5: Commit**

```bash
git add udev_db.h tests/test_udev_db.c
git commit -m "feat(db): udev_db_remove (unlink shadow record on remove)"
```

---

### Task 4: Wire the writer into `schema-udev.c` `dispatch()`

**Files:**
- Modify: `schema-udev.c`

**Interfaces:**
- Consumes: `udev_db_write`/`udev_db_remove`/`SCHEMA_UDEV_DB_DIR` from `udev_db.h`.

- [ ] **Step 1: Include `udev_db.h`**

Add after the existing `#include "udev_rules.h"` in `schema-udev.c`:

```c
#include "udev_db.h"
```

- [ ] **Step 2: Capture `kernel_n` and write/remove in `dispatch()`**

Modify `dispatch()` — capture `kernel_n` before `run_builtins`, and after `run_rules` write on add/change or remove on remove:

```c
static void dispatch(struct uevent *ev) {
    const char *action = uevent_get(ev, "ACTION");
    if (!action) return;
    const char *devpath = uevent_get(ev, "DEVPATH");
    int kernel_n = ev->n;
    if (devpath) {
        const char *devname = uevent_get(ev, "DEVNAME");
        char devnode[UE_VAL_MAX];
        const char *dn = NULL;
        if (devname) { snprintf(devnode, sizeof devnode, "/dev/%s", devname); dn = devnode; }
        run_builtins("/sys", devpath, dn, ev);
        run_rules("/sys", devpath, dn, ev);
        if (strcmp(action, "remove") == 0) udev_db_remove(SCHEMA_UDEV_DB_DIR, ev);
        else                               udev_db_write(SCHEMA_UDEV_DB_DIR, ev, kernel_n);
    }
    /* ... existing rule-match / symlink / hook loop unchanged ... */
}
```

- [ ] **Step 3: Build the daemon**

Run: `make schema-udev`
Expected: compiles clean.

- [ ] **Step 4: Live smoke — coldplug populates the shadow dir**

Run the daemon briefly so its coldplug walk runs, then confirm the shadow dir filled and udevd's real dir was untouched:

```bash
sudo rm -rf /run/schema-udev
sudo ./schema-udev &   # let coldplug complete (~1-2s)
sleep 2; sudo kill %1
ls /run/schema-udev/data | head
# spot-check a record: derived E: lines + trailing V:1, no E:DEVPATH
sudo cat "$(ls /run/schema-udev/data/b* 2>/dev/null | head -1)"
```
Expected: `/run/schema-udev/data` populated; a block record ends in `V:1` and has no `E:DEVPATH`/`E:SUBSYSTEM`. `/run/udev/data` is unchanged.

- [ ] **Step 5: Commit**

```bash
git add schema-udev.c
git commit -m "feat(schema-udev): write shadow db records live in dispatch"
```

---

### Task 5: File-vs-file parity mode in `tools/udev-parity.c`

Adds a db-parity path that compares OUR persisted record (built via `udev_db_record_build` — the exact bytes the daemon writes) against udevd's real db `E:` set, using the slice-1 device-class-aware classifier. Gate: 0 in-scope missing, 0 mismatch.

**Files:**
- Modify: `tools/udev-parity.c`, `udev_db.h` (add `udev_db_parse_eprops` buffer parser)

**Interfaces:**
- Consumes: `udev_db_record_build`, `udev_db_filename`, `udev_db_read_eprops`, `parity_in_scope_missing`.
- Produces: `udev_db_parse_eprops(const char *text, struct uevent *out) -> int` — parse `E:k=v` lines from an in-memory record buffer into `out`. New tool stdout counters: `IN-SCOPE MISSING (db): N`, `VALUE MISMATCHES (db): N`.

- [ ] **Step 1: Add `udev_db_parse_eprops` to `udev_db.h`**

```c
static inline int udev_db_parse_eprops(const char *text, struct uevent *out) {
    out->n = 0;
    for (const char *p = text; *p; ) {
        const char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
        if (p[0] == 'E' && p[1] == ':' && out->n < UE_MAX_KEYS) {
            const char *kv = p + 2;
            const char *eq = memchr(kv, '=', linelen - 2);
            if (eq) {
                char k[UE_KEY_MAX], v[UE_VAL_MAX];
                size_t kl = (size_t)(eq - kv); if (kl >= UE_KEY_MAX) kl = UE_KEY_MAX - 1;
                size_t vl = (size_t)(p + linelen - (eq + 1)); if (vl >= UE_VAL_MAX) vl = UE_VAL_MAX - 1;
                memcpy(k, kv, kl); k[kl] = '\0';
                memcpy(v, eq + 1, vl); v[vl] = '\0';
                safe_copy(out->key[out->n], k, UE_KEY_MAX);
                safe_copy(out->val[out->n], v, UE_VAL_MAX);
                out->n++;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}
```

- [ ] **Step 2: Add db counters and the db-parity block in `collect()`**

In `tools/udev-parity.c`: add file-scope counters `static int g_db_inscope_missing = 0, g_db_mismatch = 0;`. In `collect()`, capture `int kernel_n = ev.n;` immediately after `struct uevent ev = *ev_in;` and BEFORE `run_builtins`. After the existing `dbev` read block (where `path`/`dbev` are in scope), append the db-parity comparison built from OUR serialized record:

```c
    /* --- db parity: our PERSISTED record (bytes we write) vs udevd's E: set --- */
    char rbuf[8192];
    if (udev_db_record_build(&ev, kernel_n, rbuf, sizeof rbuf) > 0) {
        struct uevent ours;
        udev_db_parse_eprops(rbuf, &ours);
        for (int k = 0; k < dbev.n; k++) {
            const char *mine = uevent_get(&ours, dbev.key[k]);
            if (mine) {
                if (strcmp(mine, dbev.val[k]) != 0) {
                    printf("VALMIS-DB %s %s: ours='%s' theirs='%s'\n",
                           key, dbev.key[k], mine, dbev.val[k]);
                    g_db_mismatch++;
                }
            } else if (parity_in_scope_missing(dbev.key[k], sub, uevent_get(&ev, "DEVPATH"))) {
                printf("INSCOPE-MISS-DB %-9s %-24s @ %s\n", sub ? sub : "-", dbev.key[k],
                       uevent_get(&ev, "DEVPATH"));
                g_db_inscope_missing++;
            }
        }
    }
```

(Because the read at `UDEV_DB_DIR/<key>` succeeded, our `udev_db_filename` derivation already names a real udev record — key-derivation parity is verified implicitly for every compared device.)

- [ ] **Step 3: Print the db counters in `main()`**

After the existing summary prints in `main()`, add:

```c
    printf("IN-SCOPE MISSING (db): %d\n", g_db_inscope_missing);
    printf("VALUE MISMATCHES (db): %d\n", g_db_mismatch);
```

- [ ] **Step 4: Build and run the parity tool**

Run: `make parity && ./udev-parity`
Expected: builds clean; prints `IN-SCOPE MISSING (db): 0` and `VALUE MISMATCHES (db): 0`. If either is non-zero, the printed `INSCOPE-MISS-DB`/`VALMIS-DB` lines name the exact device+key — fix the writer or (if genuinely out-of-scope) document per-key in the classifier; NEVER filter the category to zero.

- [ ] **Step 5: Commit**

```bash
git add tools/udev-parity.c udev_db.h
git commit -m "test(db): file-vs-file db parity mode (persisted record vs udevd)"
```

---

### Task 6: Live gate script + vmtest rail

**Files:**
- Create: `tests/verify_db_live.sh`
- Modify: `Makefile` (optional convenience target)

**Interfaces:**
- Consumes: the daemon (`./schema-udev`) and `./udev-parity`.

- [ ] **Step 1: Write `tests/verify_db_live.sh`**

Runs the daemon's coldplug to populate the shadow dir, asserts the parity tool reports 0/0 for the db counters, and asserts no phantom shadow files (every shadow record corresponds to a real udevd record — catches a wrong-key write). Asserts the tool's own counters; no `grep -v` category filtering.

```sh
#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s schema-udev parity

sudo rm -rf /run/schema-udev
sudo ./schema-udev &
UDPID=$!
sleep 2
sudo kill "$UDPID" 2>/dev/null || true
wait "$UDPID" 2>/dev/null || true

# 1) parity tool: db counters must be exactly 0
OUT=$(./udev-parity)
echo "$OUT" | grep -E 'IN-SCOPE MISSING \(db\)|VALUE MISMATCHES \(db\)'
MISS=$(echo "$OUT" | sed -n 's/^IN-SCOPE MISSING (db): //p')
MMIS=$(echo "$OUT" | sed -n 's/^VALUE MISMATCHES (db): //p')
[ "$MISS" = "0" ] || { echo "FAIL: db in-scope missing=$MISS"; exit 1; }
[ "$MMIS" = "0" ] || { echo "FAIL: db value mismatches=$MMIS"; exit 1; }

# 2) no phantom shadow files: every shadow record names a real udevd record
PHANTOM=0
for f in /run/schema-udev/data/*; do
    [ -e "$f" ] || continue
    key=$(basename "$f")
    [ -e "/run/udev/data/$key" ] || { echo "PHANTOM: $key has no /run/udev/data counterpart"; PHANTOM=$((PHANTOM+1)); }
done
[ "$PHANTOM" = "0" ] || { echo "FAIL: $PHANTOM phantom shadow records"; exit 1; }

echo ">> RESULT: PASS (db live gate)"
```

Make it executable: `chmod +x tests/verify_db_live.sh`.

- [ ] **Step 2: Run the live gate**

Run: `./tests/verify_db_live.sh`
Expected: `>> RESULT: PASS (db live gate)`. On failure, the printed `INSCOPE-MISS-DB`/`VALMIS-DB`/`PHANTOM` lines localize it.

- [ ] **Step 3: Run the vmtest boot rail (schema-udev is not PID 1 — must be unchanged)**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS` — timer fired + hang excised + dependent ran + SDBOOTED-DIR present. This slice touches no PID-1 path; a regression here means something leaked into the boot rail.

- [ ] **Step 4: Commit**

```bash
git add tests/verify_db_live.sh Makefile
git commit -m "test(db): live shadow-db gate (0/0 counters + no phantom records)"
```

---

## Self-Review

**Spec coverage:**
- Extraction to `udev_db.h` (+ reader move) → Task 1. ✓
- `record_build` delta + trailing `V:1` → Task 2. ✓
- Atomic mkstemp+rename write → Task 2. ✓
- `udev_db_remove` → Task 3. ✓
- Dispatch wiring + `kernel_n` boundary + live write → Task 4. ✓
- File-vs-file parity gate reusing slice-1 classifier → Task 5. ✓
- Live gate asserting tool counters + no phantom records → Task 6. ✓
- vmtest rail unchanged → Task 6. ✓
- Rewrite `test_udev_db.c` (not append to old contract) → Task 2 Step 1. ✓
- Group-1-only, no group-2, shadow-dir-only → Global Constraints, honored throughout. ✓

**Type consistency:** `udev_db_record_build(ev, kernel_n, buf, bufsz)` and `udev_db_write(base_dir, ev, kernel_n)` signatures are consistent across the test (Task 2/3), the dispatch call site (Task 4), and the parity tool (Task 5). `kernel_n` is captured pre-`run_builtins` in both the daemon (Task 4) and the tool (Task 5). `udev_db_parse_eprops`/`udev_db_read_eprops` both fill a `struct uevent`.

**Placeholder scan:** Task 1 Step 1 uses `/* paste ... verbatim */` markers — intentional (a pure move of code that already exists at cited locations), not a logic placeholder. All new logic is written out in full.
