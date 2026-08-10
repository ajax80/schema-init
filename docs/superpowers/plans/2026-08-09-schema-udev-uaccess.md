# schema-udev slice D — uaccess ACL manager (dry-run/verify) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Record, per uaccess-eligible device, the intended active-seat-user ACL decision to a shadow area — matching what systemd-logind actually applies — without ever mutating a real ACL (dry-run; real application deferred to cutover/slice E).

**Architecture:** A new self-contained header `uaccess.h` provides the active-uid reader (parses `ACTIVE_UID=` from schema-logind's `seat0` projection), an eligibility predicate (`SUBSYSTEM ∈ {sound,video4linux,media}`), and shadow-record write/clear/wipe. `schema-udev.c` calls record on add/change and clear on remove, plus a startup wipe, alongside the existing disk_links hooks.

**Tech Stack:** C99 (`-std=c99 -Wall -Wextra -D_GNU_SOURCE -I.`), header-only inline style matching `disk_links.h`. POSIX file I/O only — no ACL libraries.

## Global Constraints

- **DRY-RUN — HARD BOUNDARY:** `uaccess.h` MUST NOT include `<sys/acl.h>` /
  `<acl/libacl.h>` or call any `acl_*` function. It writes only shadow text
  records. This is verified by grep over the diff.
- Shadow area: `#define SCHEMA_UACCESS_DIR "/run/schema-udev/uaccess"`. Never
  write anywhere else; never mutate a real device node or `/run/udev`.
- Active uid source: `#define SEAT0_PATH "/run/systemd/seats/seat0"`, parse
  the `ACTIVE_UID=<uid>` line. `-1` if absent/unreadable/no active session.
- Eligibility: `SUBSYSTEM ∈ {sound, video4linux, media}`, nothing else.
- Record key: `c<maj>:<min>` from kernel `MAJOR`/`MINOR` (char prefix `c`).
- Record contents (exact):
  ```
  DEVNODE=/dev/<DEVNAME>
  GRANT_UID=<uid>
  ACL=user:<uid>:rw
  ```
- `DEVNAME` is a kernel uevent property (always present on char nodes, e.g.
  `snd/controlC0`) — no `run_builtins` needed to populate it.
- Deferred (documented, out of scope): `dri`, `usb` webcam, `hidraw`,
  `rfkill`, `udmabuf`, optical `sr0`.
- Boundaries unchanged: `sa.nl_groups = 1`; `scripts/schema-logind.py`
  (read-only input); `udev-parity.h`; `run_builtins`/`ub_select`; disk_links;
  Phase-2 `symlink=`.
- Header-only `static inline`; no docstrings/comments beyond sibling-header
  norms; no error handling beyond what's shown.

---

### Task 1: `uaccess.h` mechanism + unit tests

**Files:**
- Create: `uaccess.h`
- Create: `tests/test_uaccess.c`
- Modify: `Makefile` (add one test line after the `test_disk_links` line)

**Interfaces:**
- Consumes: `struct uevent`, `uevent_get`, `safe_copy` (from `schema-udev.h`).
- Produces (relied on by Task 2):
  - `#define SCHEMA_UACCESS_DIR "/run/schema-udev/uaccess"`
  - `#define SEAT0_PATH "/run/systemd/seats/seat0"`
  - `int uaccess_active_uid(const char *seat_path);`
  - `int uaccess_eligible(const struct uevent *ev);`
  - `int uaccess_record(const char *dir, const char *seat_path, const struct uevent *ev);`
  - `int uaccess_clear(const char *dir, const struct uevent *ev);`
  - `void uaccess_wipe(const char *dir);`

- [ ] **Step 1: Write the failing test**

Create `tests/test_uaccess.c`:

```c
#include "../uaccess.h"
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

int main(void) {
    /* ---- eligibility ---- */
    struct uevent e;
    e.n = 0; put(&e, "SUBSYSTEM", "sound");        assert(uaccess_eligible(&e) == 1);
    e.n = 0; put(&e, "SUBSYSTEM", "video4linux");  assert(uaccess_eligible(&e) == 1);
    e.n = 0; put(&e, "SUBSYSTEM", "media");        assert(uaccess_eligible(&e) == 1);
    e.n = 0; put(&e, "SUBSYSTEM", "dri");          assert(uaccess_eligible(&e) == 0);
    e.n = 0; put(&e, "SUBSYSTEM", "hidraw");       assert(uaccess_eligible(&e) == 0);
    e.n = 0; put(&e, "SUBSYSTEM", "block");        assert(uaccess_eligible(&e) == 0);
    e.n = 0;                                        assert(uaccess_eligible(&e) == 0);
    printf("test_uaccess eligible: OK\n");

    /* ---- active_uid ---- */
    char st[] = "/tmp/ua-seat-XXXXXX"; int sfd = mkstemp(st); assert(sfd >= 0);
    dprintf(sfd, "# This is private data. Do not parse.\nACTIVE=31\nACTIVE_UID=1000\nUIDS=1000\n");
    close(sfd);
    assert(uaccess_active_uid(st) == 1000);

    char st2[] = "/tmp/ua-seat2-XXXXXX"; int sfd2 = mkstemp(st2); assert(sfd2 >= 0);
    dprintf(sfd2, "ACTIVE=31\nUIDS=1000\n");   /* no ACTIVE_UID line */
    close(sfd2);
    assert(uaccess_active_uid(st2) == -1);
    assert(uaccess_active_uid("/tmp/ua-nonexistent-file-xyz") == -1);
    printf("test_uaccess active_uid: OK\n");

    /* ---- record ---- */
    char dirt[] = "/tmp/ua-dir-XXXXXX"; char *dir = mkdtemp(dirt); assert(dir);
    struct uevent s; s.n = 0;
    put(&s, "SUBSYSTEM", "sound"); put(&s, "DEVNAME", "snd/controlC0");
    put(&s, "MAJOR", "116"); put(&s, "MINOR", "7");
    assert(uaccess_record(dir, st, &s) == 0);

    char rec[512]; snprintf(rec, sizeof rec, "%s/c116:7", dir);
    FILE *f = fopen(rec, "r"); assert(f);
    char buf[512]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    assert(strstr(buf, "DEVNODE=/dev/snd/controlC0"));
    assert(strstr(buf, "GRANT_UID=1000"));
    assert(strstr(buf, "ACL=user:1000:rw"));
    printf("test_uaccess record: OK\n");

    /* ---- ineligible clears a stale record at the same key ---- */
    struct uevent ie; ie.n = 0;
    put(&ie, "SUBSYSTEM", "dri"); put(&ie, "DEVNAME", "dri/card1");
    put(&ie, "MAJOR", "116"); put(&ie, "MINOR", "7");
    assert(uaccess_record(dir, st, &ie) == 0);   /* not eligible -> clears c116:7 */
    assert(access(rec, F_OK) != 0);
    printf("test_uaccess ineligible-clears: OK\n");

    /* ---- no active uid -> no record ---- */
    assert(uaccess_record(dir, st2, &s) == 0);   /* st2 has no ACTIVE_UID */
    assert(access(rec, F_OK) != 0);
    printf("test_uaccess no-uid-no-record: OK\n");

    /* ---- explicit clear is idempotent ---- */
    assert(uaccess_record(dir, st, &s) == 0);    /* recreate */
    assert(access(rec, F_OK) == 0);
    assert(uaccess_clear(dir, &s) == 0);
    assert(access(rec, F_OK) != 0);
    assert(uaccess_clear(dir, &s) == 0);         /* second clear: no-op */
    printf("test_uaccess clear-idempotent: OK\n");

    /* ---- wipe removes all records ---- */
    assert(uaccess_record(dir, st, &s) == 0);
    uaccess_wipe(dir);
    assert(access(rec, F_OK) != 0);
    printf("test_uaccess wipe: OK\n");

    printf("test_uaccess: ALL OK\n");
    return 0;
}
```

- [ ] **Step 2: Add the Makefile test line**

In `Makefile`, immediately after the `test_disk_links` line, add:

```makefile
	$(CC) $(CFLAGS) tests/test_uaccess.c -o /tmp/schema-test-uaccess && /tmp/schema-test-uaccess
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `make test 2>&1 | tail -20`
Expected: FAIL — `uaccess.h: No such file or directory`.

- [ ] **Step 4: Write `uaccess.h`**

Create `uaccess.h`:

```c
#ifndef UACCESS_H
#define UACCESS_H

#include "schema-udev.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>

#define SCHEMA_UACCESS_DIR "/run/schema-udev/uaccess"
#define SEAT0_PATH         "/run/systemd/seats/seat0"

static inline int uaccess_active_uid(const char *seat_path) {
    FILE *f = fopen(seat_path, "r");
    if (!f) return -1;
    char line[256];
    int uid = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "ACTIVE_UID=", 11) == 0) { uid = atoi(line + 11); break; }
    }
    fclose(f);
    return uid;
}

static inline int uaccess_eligible(const struct uevent *ev) {
    const char *sub = uevent_get(ev, "SUBSYSTEM");
    if (!sub) return 0;
    return strcmp(sub, "sound") == 0
        || strcmp(sub, "video4linux") == 0
        || strcmp(sub, "media") == 0;
}

static inline int ua_keyname(const struct uevent *ev, char *out, size_t outsz) {
    const char *maj = uevent_get(ev, "MAJOR");
    const char *min = uevent_get(ev, "MINOR");
    if (!maj || !min) return -1;
    if ((size_t)snprintf(out, outsz, "c%s:%s", maj, min) >= outsz) return -1;
    return 0;
}

static inline int uaccess_clear(const char *dir, const struct uevent *ev) {
    char key[64];
    if (ua_keyname(ev, key, sizeof key) != 0) return -1;
    char path[512];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, key) >= sizeof path) return -1;
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}

static inline int ua_mkdir_p(const char *path) {
    char tmp[512];
    safe_copy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1; *p = '/'; }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static inline int uaccess_record(const char *dir, const char *seat_path,
                                 const struct uevent *ev) {
    if (!uaccess_eligible(ev)) return uaccess_clear(dir, ev);
    int uid = uaccess_active_uid(seat_path);
    if (uid < 0) return uaccess_clear(dir, ev);
    const char *devname = uevent_get(ev, "DEVNAME");
    if (!devname || !devname[0]) return -1;

    char key[64];
    if (ua_keyname(ev, key, sizeof key) != 0) return -1;
    if (ua_mkdir_p(dir) != 0) return -1;

    char final[512], tmp[512];
    if ((size_t)snprintf(final, sizeof final, "%s/%s", dir, key) >= sizeof final) return -1;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s/.%s.tmp.%d", dir, key, (int)getpid()) >= sizeof tmp) return -1;

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "DEVNODE=/dev/%s\nGRANT_UID=%d\nACL=user:%d:rw\n", devname, uid, uid);
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, final) != 0) { unlink(tmp); return -1; }
    return 0;
}

static inline void uaccess_wipe(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) < sizeof path)
            unlink(path);
    }
    closedir(d);
}

#endif /* UACCESS_H */
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `make test 2>&1 | tail -25`
Expected: PASS — `test_uaccess: ALL OK`, and every other test still green.

- [ ] **Step 6: Verify the dry-run boundary**

Run: `grep -nE 'acl_|sys/acl.h|libacl' uaccess.h; echo "exit=$?"`
Expected: no matches (grep exit 1). `uaccess.h` has no ACL-mutation surface.

- [ ] **Step 7: Commit**

```bash
git add uaccess.h tests/test_uaccess.c Makefile
git commit -m "feat(uaccess): dry-run shadow decision recorder (eligible/active-uid/record/clear/wipe)"
```

---

### Task 2: Wire into `schema-udev.c` + live parity gate

**Files:**
- Modify: `schema-udev.c` (include; `dispatch()` add/change + remove; startup wipe in `main()`)
- Create: `tests/verify_uaccess_live.sh`

**Interfaces:**
- Consumes: `uaccess_record`, `uaccess_clear`, `uaccess_wipe`,
  `SCHEMA_UACCESS_DIR`, `SEAT0_PATH` (Task 1); `uevent_get` (`schema-udev.h`).
- Produces: a daemon that maintains `/run/schema-udev/uaccess/*` shadow
  decisions live.

- [ ] **Step 1: Add the include**

In `schema-udev.c`, next to `#include "disk_links.h"`, add:

```c
#include "uaccess.h"
```

- [ ] **Step 2: Wire the dispatch hooks**

In `schema-udev.c` `dispatch()`, replace this block:

```c
        if (strcmp(action, "remove") == 0) {
            if (is_block) disk_links_gc(SCHEMA_DISK_DIR, SCHEMA_UDEV_DB_DIR, ev);
            udev_db_remove(SCHEMA_UDEV_DB_DIR, ev);
        } else {
            udev_db_write(SCHEMA_UDEV_DB_DIR, ev, kernel_n);
            if (is_block && (strcmp(action, "add") == 0 || strcmp(action, "change") == 0))
                disk_links_apply(SCHEMA_DISK_DIR, ev);
        }
```

with:

```c
        if (strcmp(action, "remove") == 0) {
            if (is_block) disk_links_gc(SCHEMA_DISK_DIR, SCHEMA_UDEV_DB_DIR, ev);
            uaccess_clear(SCHEMA_UACCESS_DIR, ev);
            udev_db_remove(SCHEMA_UDEV_DB_DIR, ev);
        } else {
            udev_db_write(SCHEMA_UDEV_DB_DIR, ev, kernel_n);
            if (is_block && (strcmp(action, "add") == 0 || strcmp(action, "change") == 0))
                disk_links_apply(SCHEMA_DISK_DIR, ev);
            if (strcmp(action, "add") == 0 || strcmp(action, "change") == 0)
                uaccess_record(SCHEMA_UACCESS_DIR, SEAT0_PATH, ev);
        }
```

- [ ] **Step 3: Add the startup wipe**

In `schema-udev.c` `main()`, immediately after the existing
`disk_links_wipe(SCHEMA_DISK_DIR);` line, add:

```c
    uaccess_wipe(SCHEMA_UACCESS_DIR);
```

- [ ] **Step 4: Verify it builds and the full unit suite is still green**

Run: `make -s schema-udev && make test 2>&1 | tail -25`
Expected: clean build; all tests PASS including `test_uaccess`.

- [ ] **Step 5: Write the live parity gate**

Create `tests/verify_uaccess_live.sh`:

```sh
#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s schema-udev

sudo rm -rf /run/schema-udev
sudo ./schema-udev & UDPID=$!
sleep 3
sudo kill "$UDPID" 2>/dev/null || true
wait "$UDPID" 2>/dev/null || true

UADIR=/run/schema-udev/uaccess
fail=0

# SECURITY-CRITICAL (forward / only-ours): every shadow decision must match a
# real ACL logind applied. A shadow grant with no matching logind ACL = FAIL.
n_fwd=0
for rec in "$UADIR"/*; do
    [ -e "$rec" ] || continue
    n_fwd=$((n_fwd + 1))
    node=$(sed -n 's/^DEVNODE=//p' "$rec")
    uid=$(sed -n 's/^GRANT_UID=//p' "$rec")
    if ! getfacl -p "$node" 2>/dev/null | grep -q "^user:$uid:rw-"; then
        echo "FAIL security: shadow-granted $node (uid $uid) has NO matching logind ACL"
        fail=1
    fi
done
echo "forward/only-ours: $n_fwd shadow decisions, all matched logind ACL"

# COMPLETENESS (reverse): every in-scope real uaccess node has a shadow record.
# In-scope subsystems only: sound, video4linux, media. Deferred (excluded):
# dri, usb, hidraw, rfkill, udmabuf, optical.
aid=$(sed -n 's/^ACTIVE_UID=//p' /run/systemd/seats/seat0)
n_rev=0
for node in /dev/snd/* /dev/video* /dev/media*; do
    [ -c "$node" ] || continue
    getfacl -p "$node" 2>/dev/null | grep -q "^user:$aid:rw-" || continue  # logind didn't grant
    maj=$((0x$(stat -c%t "$node"))); min=$((0x$(stat -c%T "$node")))
    n_rev=$((n_rev + 1))
    if [ ! -e "$UADIR/c$maj:$min" ]; then
        echo "FAIL completeness: $node (c$maj:$min) has logind ACL but NO shadow record"
        fail=1
    fi
done
echo "reverse: $n_rev in-scope logind-granted nodes, all have shadow records"

[ "$fail" = 0 ] || { echo ">> RESULT: FAIL"; exit 1; }
echo ">> RESULT: PASS (dry-run uaccess decisions == logind ACLs; sound/video4linux/media in-scope; dri/usb/hidraw/rfkill/udmabuf/optical deferred)"
```

Then make it executable:

```bash
chmod +x tests/verify_uaccess_live.sh
```

- [ ] **Step 6: Run the live parity gate**

Run: `sh tests/verify_uaccess_live.sh`
Expected: `>> RESULT: PASS`, with `forward/only-ours` and `reverse` counts
both > 0 (this host has ~18 in-scope uaccess nodes). If a FAIL prints, fix
`uaccess.h` eligibility/record logic — do NOT weaken the gate.

- [ ] **Step 7: vmtest (no PID-1 regression)**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS`.

- [ ] **Step 8: Commit**

```bash
git add schema-udev.c tests/verify_uaccess_live.sh
git commit -m "feat(schema-udev): record uaccess ACL decisions (dry-run); live parity gate"
```

---

## Self-Review

**Spec coverage:**
- Eligibility `SUBSYSTEM ∈ {sound,video4linux,media}` → `uaccess_eligible` + Task 1 tests. ✓
- Active uid from seat0 `ACTIVE_UID=` (−1 if none) → `uaccess_active_uid` + tests. ✓
- Shadow record key `c<maj>:<min>` + exact contents → `uaccess_record`/`ua_keyname` + test asserting all three fields. ✓
- Dry-run (no `acl_*`) → Task 1 Step 6 grep gate + Global Constraints. ✓
- Clear on remove / ineligible / no-uid → `uaccess_clear`, `uaccess_record` early-returns, tests. ✓
- Startup wipe of uaccess dir only → `uaccess_wipe` + Task 2 Step 3. ✓
- Wiring add/change record, remove clear → Task 2 Step 2. ✓
- Live gate: forward/only-ours (security) + reverse completeness, deferred excluded → Task 2 Step 5. ✓
- vmtest → Task 2 Step 7. ✓
- Boundaries (nl_groups=1, schema-logind.py read-only, udev-parity.h untouched, no writes outside uaccess dir) → Global Constraints; no task edits those. ✓

**Placeholder scan:** none — all code literal.

**Type consistency:** `uaccess_active_uid`/`eligible`/`record`/`clear`/`wipe`
signatures identical across Task 1 definition, Task 1 tests, and Task 2 call
sites. `uaccess_record(dir, seat_path, ev)` argument order consistent
everywhere. Record key format `c<maj>:<min>` consistent between
`ua_keyname` and the gate's `stat`-derived key. ✓
