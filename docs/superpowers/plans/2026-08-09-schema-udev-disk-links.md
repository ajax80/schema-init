# schema-udev slice C — disk-links shadow tree — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the ID_* properties schema-udev already emits into a
`/dev/disk/by-*`-equivalent symlink farm built under a shadow tree
(`/dev/schema/disk/by-*`), so it never fights live udevd.

**Architecture:** A new self-contained header `disk_links.h` provides a
shared derive helper (uevent → list of `(tree, link-name)` pairs), an
`apply` that creates the links from a live uevent, a `gc` that merges the
shadow-db record with the live remove-uevent's kernel props to unlink them,
and a `wipe` for startup. `schema-udev.c` calls these from `dispatch()`
gated on `SUBSYSTEM=block`, plus a startup wipe before coldplug.

**Tech Stack:** C99 (`-std=c99 -Wall -Wextra -D_GNU_SOURCE -I.`), header-only
inline style matching `optical_fs.h`/`udev_db.h`, POSIX `nftw`.

## Global Constraints

- Shadow tree root is `/dev/schema/disk` (`#define SCHEMA_DISK_DIR`). NEVER
  write to `/dev/disk`, `/run/udev`, or any udevd-owned path.
- Six trees only: `by-uuid`, `by-label`, `by-partuuid`, `by-partlabel`,
  `by-path`, `by-diskseq`. `by-id` is DEFERRED — do not implement it.
- Link-name mapping (exact):
  - `by-uuid` ← `ID_FS_UUID_ENC` (fallback `ID_FS_UUID`)
  - `by-label` ← `ID_FS_LABEL_ENC`
  - `by-partuuid` ← `ID_PART_ENTRY_UUID`
  - `by-partlabel` ← `ID_PART_ENTRY_NAME` (already ENC — use verbatim)
  - `by-path` ← `ID_PATH`, plus `-part<PARTN>` if `DEVTYPE=partition`
  - `by-diskseq` ← `DISKSEQ`, plus `-part<PARTN>` if `DEVTYPE=partition`
- Only `by-path` and `by-diskseq` take the partition suffix; the other four
  never do.
- Symlink targets are RELATIVE: `../../../<devname>` (three `..`), matching
  udev's convention. `<devname>` is the bare basename of `DEVNAME`.
- Dispatch gate: `SUBSYSTEM=block`, full stop. Never gate on `MAJOR`.
- Boundaries that MUST NOT change: `sa.nl_groups = 1` in `schema-udev.c`;
  `run_builtins`/`ub_select`; `udev-parity.h`; Phase-2 `symlink=` behavior
  and `/dev/schema/<name>` links (the startup wipe removes ONLY
  `/dev/schema/disk`).
- All code header-only `static inline`, no docstrings/comments beyond what
  exists in sibling headers, no extra error handling beyond what's shown.

---

### Task 1: `disk_links.h` mechanism + unit tests

**Files:**
- Create: `disk_links.h`
- Create: `tests/test_disk_links.c`
- Modify: `Makefile` (add one test line after the `test_cdrom_media` line, ~line 115)

**Interfaces:**
- Consumes: `struct uevent`, `uevent_get`, `safe_copy` (from `schema-udev.h`);
  `udev_db_filename`, `udev_db_read_eprops`, `SCHEMA_UDEV_DB_DIR` (from
  `udev_db.h`).
- Produces (relied on by Task 2):
  - `#define SCHEMA_DISK_DIR "/dev/schema/disk"`
  - `struct disk_link { const char *tree; char name[UE_VAL_MAX]; };`
  - `int disk_links_derive(const struct uevent *ev, struct disk_link *out, int max);`
  - `int disk_links_apply(const char *base_dir, const struct uevent *ev);`
  - `int disk_links_gc(const char *base_dir, const char *db_dir, const struct uevent *ev);`
  - `void disk_links_wipe(const char *base_dir);`

- [ ] **Step 1: Write the failing test**

Create `tests/test_disk_links.c`:

```c
#include "../disk_links.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void put(struct uevent *e, const char *k, const char *v) {
    safe_copy(e->key[e->n], k, UE_KEY_MAX);
    safe_copy(e->val[e->n], v, UE_VAL_MAX);
    e->n++;
}

static void assert_link(const char *base, const char *tree, const char *name,
                        const char *want_target) {
    char lp[1024];
    snprintf(lp, sizeof lp, "%s/%s/%s", base, tree, name);
    char tgt[512];
    ssize_t l = readlink(lp, tgt, sizeof tgt - 1);
    assert(l > 0);
    tgt[l] = '\0';
    assert(strcmp(tgt, want_target) == 0);
}

int main(void) {
    /* ---- apply: whole disk ---- */
    char t1[] = "/tmp/schema-dl-XXXXXX";
    char *root = mkdtemp(t1); assert(root);
    char base[512]; snprintf(base, sizeof base, "%s/disk", root);

    struct uevent d; d.n = 0;
    put(&d, "SUBSYSTEM", "block");
    put(&d, "DEVNAME", "sda");
    put(&d, "DEVTYPE", "disk");
    put(&d, "MAJOR", "8"); put(&d, "MINOR", "0");
    put(&d, "DISKSEQ", "2");
    put(&d, "ID_FS_UUID_ENC", "e841ba0a-d7b9-42b6-b627-8ea27df85a54");
    put(&d, "ID_PATH", "pci-0000:02:00.1-ata-1.0");

    assert(disk_links_apply(base, &d) == 0);
    assert_link(base, "by-uuid", "e841ba0a-d7b9-42b6-b627-8ea27df85a54", "../../../sda");
    assert_link(base, "by-path", "pci-0000:02:00.1-ata-1.0", "../../../sda");
    assert_link(base, "by-diskseq", "2", "../../../sda");
    printf("test_disk_links apply-disk: OK\n");

    /* ---- apply: partition (suffix rule) ---- */
    char t2[] = "/tmp/schema-dl2-XXXXXX";
    char *root2 = mkdtemp(t2); assert(root2);
    char base2[512]; snprintf(base2, sizeof base2, "%s/disk", root2);

    struct uevent p; p.n = 0;
    put(&p, "SUBSYSTEM", "block");
    put(&p, "DEVNAME", "sda1");
    put(&p, "DEVTYPE", "partition");
    put(&p, "MAJOR", "8"); put(&p, "MINOR", "1");
    put(&p, "DISKSEQ", "2"); put(&p, "PARTN", "1");
    put(&p, "ID_PATH", "pci-0000:02:00.1-ata-1.0");
    put(&p, "ID_PART_ENTRY_UUID", "f746b242-7615-4bf6-9aca-b098677febc0");
    put(&p, "ID_PART_ENTRY_NAME", "Basic\\x20data\\x20partition");

    assert(disk_links_apply(base2, &p) == 0);
    /* suffixed */
    assert_link(base2, "by-path", "pci-0000:02:00.1-ata-1.0-part1", "../../../sda1");
    assert_link(base2, "by-diskseq", "2-part1", "../../../sda1");
    /* NOT suffixed, verbatim ENC name */
    assert_link(base2, "by-partuuid", "f746b242-7615-4bf6-9aca-b098677febc0", "../../../sda1");
    assert_link(base2, "by-partlabel", "Basic\\x20data\\x20partition", "../../../sda1");
    printf("test_disk_links apply-partition (suffix + ENC): OK\n");

    /* ---- gc: merge db record (derived props) + live ev (kernel props) ---- */
    char t3[] = "/tmp/schema-dldb-XXXXXX";
    char *dbdir = mkdtemp(t3); assert(dbdir);
    char rec[512]; snprintf(rec, sizeof rec, "%s/b8:1", dbdir);
    FILE *f = fopen(rec, "w"); assert(f);
    fprintf(f,
        "E:ID_PATH=pci-0000:02:00.1-ata-1.0\n"
        "E:ID_PART_ENTRY_UUID=f746b242-7615-4bf6-9aca-b098677febc0\n"
        "E:ID_PART_ENTRY_NAME=Basic\\x20data\\x20partition\n"
        "V:1\n");
    fclose(f);

    struct uevent rm; rm.n = 0;                 /* remove uevent: kernel props only */
    put(&rm, "SUBSYSTEM", "block");
    put(&rm, "MAJOR", "8"); put(&rm, "MINOR", "1");
    put(&rm, "DEVTYPE", "partition");
    put(&rm, "DISKSEQ", "2"); put(&rm, "PARTN", "1");

    assert(disk_links_gc(base2, dbdir, &rm) == 0);
    char chk[1024];
    snprintf(chk, sizeof chk, "%s/by-path/pci-0000:02:00.1-ata-1.0-part1", base2);
    assert(access(chk, F_OK) != 0);
    snprintf(chk, sizeof chk, "%s/by-diskseq/2-part1", base2);   /* from grafted DISKSEQ+PARTN */
    assert(access(chk, F_OK) != 0);
    snprintf(chk, sizeof chk, "%s/by-partuuid/f746b242-7615-4bf6-9aca-b098677febc0", base2);
    assert(access(chk, F_OK) != 0);
    snprintf(chk, sizeof chk, "%s/by-partlabel/Basic\\x20data\\x20partition", base2);
    assert(access(chk, F_OK) != 0);
    printf("test_disk_links gc (merge db+ev): OK\n");

    /* ---- wipe ---- */
    disk_links_wipe(base);
    assert(access(base, F_OK) != 0);
    printf("test_disk_links wipe: OK\n");

    printf("test_disk_links: ALL OK\n");
    return 0;
}
```

- [ ] **Step 2: Add the Makefile test line**

In `Makefile`, immediately after the `test_cdrom_media` line (~line 115),
add:

```makefile
	$(CC) $(CFLAGS) tests/test_disk_links.c -o /tmp/schema-test-disklinks && /tmp/schema-test-disklinks
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `make test 2>&1 | tail -20`
Expected: FAIL — compile error, `disk_links.h: No such file or directory`.

- [ ] **Step 4: Write `disk_links.h`**

Create `disk_links.h`:

```c
#ifndef DISK_LINKS_H
#define DISK_LINKS_H

#include "schema-udev.h"
#include "udev_db.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <ftw.h>

#define SCHEMA_DISK_DIR "/dev/schema/disk"

struct disk_link { const char *tree; char name[UE_VAL_MAX]; };

static inline int disk_links_derive(const struct uevent *ev,
                                    struct disk_link *out, int max) {
    int n = 0;
    const char *v;
    struct { const char *tree; const char *key; const char *fallback; } simple[] = {
        { "by-uuid",      "ID_FS_UUID_ENC",     "ID_FS_UUID" },
        { "by-label",     "ID_FS_LABEL_ENC",    NULL },
        { "by-partuuid",  "ID_PART_ENTRY_UUID", NULL },
        { "by-partlabel", "ID_PART_ENTRY_NAME", NULL },
    };
    for (size_t i = 0; i < sizeof simple / sizeof simple[0] && n < max; i++) {
        v = uevent_get(ev, simple[i].key);
        if ((!v || !v[0]) && simple[i].fallback) v = uevent_get(ev, simple[i].fallback);
        if (v && v[0]) {
            out[n].tree = simple[i].tree;
            safe_copy(out[n].name, v, sizeof out[n].name);
            n++;
        }
    }
    const char *devtype = uevent_get(ev, "DEVTYPE");
    const char *partn   = uevent_get(ev, "PARTN");
    int is_part = devtype && strcmp(devtype, "partition") == 0;
    struct { const char *tree; const char *key; } suff[] = {
        { "by-path",    "ID_PATH" },
        { "by-diskseq", "DISKSEQ" },
    };
    for (size_t i = 0; i < sizeof suff / sizeof suff[0] && n < max; i++) {
        v = uevent_get(ev, suff[i].key);
        if (!v || !v[0]) continue;
        out[n].tree = suff[i].tree;
        if (is_part && partn && partn[0])
            snprintf(out[n].name, sizeof out[n].name, "%s-part%s", v, partn);
        else
            safe_copy(out[n].name, v, sizeof out[n].name);
        n++;
    }
    return n;
}

static inline int dl_mkdir_p(const char *path) {
    char tmp[512];
    safe_copy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static inline int dl_link_one(const char *base_dir, const char *tree,
                              const char *name, const char *devname) {
    char treedir[768];
    if ((size_t)snprintf(treedir, sizeof treedir, "%s/%s", base_dir, tree) >= sizeof treedir)
        return -1;
    if (dl_mkdir_p(treedir) != 0) return -1;

    char target[600];
    if ((size_t)snprintf(target, sizeof target, "../../../%s", devname) >= sizeof target)
        return -1;

    char final[1024], tmp[1024];
    if ((size_t)snprintf(final, sizeof final, "%s/%s", treedir, name) >= sizeof final)
        return -1;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s/.%s.tmp.%d", treedir, name, (int)getpid()) >= sizeof tmp)
        return -1;

    unlink(tmp);
    if (symlink(target, tmp) != 0) return -1;
    if (rename(tmp, final) != 0) { unlink(tmp); return -1; }
    return 0;
}

static inline int disk_links_apply(const char *base_dir, const struct uevent *ev) {
    const char *devname = uevent_get(ev, "DEVNAME");
    if (!devname || !devname[0]) return -1;
    const char *slash = strrchr(devname, '/');
    if (slash) devname = slash + 1;

    struct disk_link links[8];
    int n = disk_links_derive(ev, links, 8);
    for (int i = 0; i < n; i++)
        dl_link_one(base_dir, links[i].tree, links[i].name, devname);
    return 0;
}

static inline int disk_links_gc(const char *base_dir, const char *db_dir,
                                const struct uevent *ev) {
    char name[128];
    if (udev_db_filename(ev, name, sizeof name) != 0) return -1;
    char path[512];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", db_dir, name) >= sizeof path) return -1;

    struct uevent merged; memset(&merged, 0, sizeof merged);
    udev_db_read_eprops(path, &merged);            /* derived props; ok if record absent */

    const char *k;
    const char *graft[] = { "DEVTYPE", "DISKSEQ", "PARTN" };
    for (size_t i = 0; i < sizeof graft / sizeof graft[0]; i++) {
        if ((k = uevent_get(ev, graft[i])) && merged.n < UE_MAX_KEYS) {
            safe_copy(merged.key[merged.n], graft[i], UE_KEY_MAX);
            safe_copy(merged.val[merged.n], k, UE_VAL_MAX);
            merged.n++;
        }
    }

    struct disk_link links[8];
    int n = disk_links_derive(&merged, links, 8);
    for (int i = 0; i < n; i++) {
        char lp[1024];
        if ((size_t)snprintf(lp, sizeof lp, "%s/%s/%s",
                             base_dir, links[i].tree, links[i].name) >= sizeof lp)
            continue;
        if (unlink(lp) != 0 && errno != ENOENT) { /* best-effort */ }
    }
    return 0;
}

static inline int dl_rm_cb(const char *p, const struct stat *sb,
                           int type, struct FTW *ftw) {
    (void)sb; (void)type; (void)ftw;
    remove(p);
    return 0;
}

static inline void disk_links_wipe(const char *base_dir) {
    nftw(base_dir, dl_rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

#endif /* DISK_LINKS_H */
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `make test 2>&1 | tail -25`
Expected: PASS — `test_disk_links: ALL OK`, and every other test still
green.

- [ ] **Step 6: Commit**

```bash
git add disk_links.h tests/test_disk_links.c Makefile
git commit -m "feat(disk_links): shadow-tree by-* symlink farm (derive/apply/gc/wipe)"
```

---

### Task 2: Wire into `schema-udev.c` + live parity gate

**Files:**
- Modify: `schema-udev.c` (include; `dispatch()` add/change/remove hooks; startup wipe in `main()`)
- Create: `tests/verify_disk_links_live.sh`

**Interfaces:**
- Consumes: `disk_links_apply`, `disk_links_gc`, `disk_links_wipe`,
  `SCHEMA_DISK_DIR` (Task 1); `SCHEMA_UDEV_DB_DIR` (`udev_db.h`);
  `uevent_get` (`schema-udev.h`).
- Produces: a running daemon that maintains `/dev/schema/disk/by-*` live.

- [ ] **Step 1: Add the include**

In `schema-udev.c`, next to the other builtin/db includes near the top, add:

```c
#include "disk_links.h"
```

- [ ] **Step 2: Wire the dispatch hooks**

In `schema-udev.c`, in `dispatch()`, replace this block (the db-write
branch, currently ~lines 95-96):

```c
        if (strcmp(action, "remove") == 0) udev_db_remove(SCHEMA_UDEV_DB_DIR, ev);
        else                               udev_db_write(SCHEMA_UDEV_DB_DIR, ev, kernel_n);
```

with:

```c
        const char *sub = uevent_get(ev, "SUBSYSTEM");
        int is_block = sub && strcmp(sub, "block") == 0;
        if (strcmp(action, "remove") == 0) {
            if (is_block) disk_links_gc(SCHEMA_DISK_DIR, SCHEMA_UDEV_DB_DIR, ev);
            udev_db_remove(SCHEMA_UDEV_DB_DIR, ev);
        } else {
            udev_db_write(SCHEMA_UDEV_DB_DIR, ev, kernel_n);
            if (is_block && (strcmp(action, "add") == 0 || strcmp(action, "change") == 0))
                disk_links_apply(SCHEMA_DISK_DIR, ev);
        }
```

- [ ] **Step 3: Add the startup wipe**

In `schema-udev.c` `main()`, find where coldplug is invoked (search for
`coldplug`). Immediately BEFORE the coldplug call, add:

```c
    disk_links_wipe(SCHEMA_DISK_DIR);
```

- [ ] **Step 4: Verify it builds and the full unit suite is still green**

Run: `make -s schema-udev && make test 2>&1 | tail -25`
Expected: clean build; all tests PASS including `test_disk_links`.

- [ ] **Step 5: Write the live parity gate**

Create `tests/verify_disk_links_live.sh`:

```sh
#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s schema-udev

sudo rm -rf /run/schema-udev
sudo rm -rf /dev/schema/disk
sudo ./schema-udev & UDPID=$!
sleep 3
sudo kill "$UDPID" 2>/dev/null || true
wait "$UDPID" 2>/dev/null || true

fail=0
for tree in by-uuid by-label by-partuuid by-partlabel by-path by-diskseq; do
    real="/dev/disk/$tree"
    ours="/dev/schema/disk/$tree"
    [ -d "$real" ] || { echo "skip $tree (udevd has none)"; continue; }
    rn=$(find "$real" -maxdepth 1 -mindepth 1 -type l -printf '%f\n' 2>/dev/null | sort)
    on=$(find "$ours" -maxdepth 1 -mindepth 1 -type l -printf '%f\n' 2>/dev/null | sort)
    if [ "$rn" != "$on" ]; then
        echo "FAIL $tree: name-set differs"
        echo "  only-udevd:"; comm -23 <(printf '%s\n' "$rn") <(printf '%s\n' "$on") | sed 's/^/    /'
        echo "  only-ours:";  comm -13 <(printf '%s\n' "$rn") <(printf '%s\n' "$on") | sed 's/^/    /'
        fail=1; continue
    fi
    for nm in $on; do
        rd=$(realpath "$real/$nm" 2>/dev/null || true)
        od=$(realpath "$ours/$nm" 2>/dev/null || true)
        [ "$rd" = "$od" ] || { echo "FAIL $tree/$nm: resolves '$od' != '$rd'"; fail=1; }
    done
    echo "OK $tree ($(printf '%s\n' "$on" | grep -c .) links)"
done

[ "$fail" = "0" ] || { echo ">> RESULT: FAIL"; exit 1; }
echo ">> RESULT: PASS (six in-scope by-* trees match udevd set-wise + resolved device; by-id excluded)"
```

Then make it executable:

```bash
chmod +x tests/verify_disk_links_live.sh
```

- [ ] **Step 6: Run the live parity gate**

Run: `sh tests/verify_disk_links_live.sh`
Expected: `>> RESULT: PASS`. Each of the six trees prints `OK <tree> (N
links)`. If any tree FAILs, the diff of only-udevd/only-ours names is
printed — fix the derive/suffix logic to match, do not adjust the gate to
pass.

- [ ] **Step 7: vmtest (no PID-1 regression)**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS` (timer fired, hang excised, dependent ran,
SDBOOTED-DIR present).

- [ ] **Step 8: Commit**

```bash
git add schema-udev.c tests/verify_disk_links_live.sh
git commit -m "feat(schema-udev): maintain /dev/schema/disk/by-* farm; live parity gate"
```

---

## Self-Review

**Spec coverage:**
- Six trees + exact mapping → Task 1 `disk_links_derive` + Global Constraints. ✓
- Partition `-part<PARTN>` suffix for by-path/by-diskseq → derive `suff[]` loop + partition apply test. ✓
- Relative `../../../<devname>` targets → `dl_link_one` + `assert_link`. ✓
- Atomic tmp+rename create → `dl_link_one`. ✓
- gc merges db record + kernel props (DEVTYPE/DISKSEQ/PARTN) → `disk_links_gc` graft loop + gc test. ✓
- Startup wipe of `/dev/schema/disk` only → `disk_links_wipe` + Task 2 Step 3. ✓
- `SUBSYSTEM=block` gate, apply on add/change, gc-before-db_remove → Task 2 Step 2. ✓
- Live parity gate, symlinks-only, by-id excluded → Task 2 Step 5. ✓
- vmtest → Task 2 Step 7. ✓
- Boundaries (nl_groups=1, no /dev/disk writes, parity classifier untouched) → Global Constraints; no task edits those. ✓

**Placeholder scan:** none — all code is literal.

**Type consistency:** `disk_links_derive/apply/gc/wipe` signatures and
`struct disk_link` identical across Task 1 definition, Task 1 tests, and
Task 2 call sites. `base_dir`/`db_dir` argument order consistent
(`gc(base_dir, db_dir, ev)`). ✓
