# schema-udev sub-project B slice 2 — shadow db writer

**Status:** design approved 2026-08-07. Endgame arc: udevd retirement. Slice 1 (property completeness, PR #89) landed the owned-subset property engine; this slice materializes those properties as on-disk udev db records so libudev clients can eventually read them at cutover.

## Goal

schema-udev writes udev-format device db records to a **shadow directory** (`/run/schema-udev/data`, never udevd's real `/run/udev/data`), live during coldplug and hot events, verified by file-vs-file parity against udevd's real db. The hazardous group-2 libudev rebroadcast is **explicitly out of scope** here — staged for near-cutover.

## Scope

### In scope
- New `udev_db.h`: db-key computation, owned-record serialization, atomic write, remove.
- One minimal call site in `schema-udev.c` `dispatch()`.
- Shadow-dir plumbing (`/run/schema-udev/data`, created on demand).
- Owned-subset record: `E:` lines for the derived property delta + `V:1`.
- File-vs-file parity gate (extends `tools/udev-parity.c`) + live gate (`tests/verify_db_live.sh`).
- Unit tests (`tests/test_udev_db.c`).

### Out of scope (owned by later slices — do NOT emit these lines)
- `S:` symlink lines — slice C (persistent `/dev/disk/by-*`).
- `G:`/`Q:` tag lines (systemd/seat/uaccess) — slice D + rules engine.
- `I:` init-seq / usec_initialized line — deferred (self-contained, no consumer yet).
- `L:` link-priority, `W:` watch — not reimplemented.
- **group-2 (`NETLINK_KOBJECT_UEVENT` group 2) libudev rebroadcast — near-cutover only.**

### HARD constraint
schema-udev listens on kernel uevent netlink **group 1 ONLY**. This slice adds a *disk writer*, no new socket behavior. The `sa.nl_groups = 1` bind and its "NEVER group 2" comment stay byte-identical.

## udev db format (observed on blakbox)

Records live at `/run/udev/data/<key>`. Line types:
- `E:key=value` — **derived** properties only. The raw kernel uevent keys (`DEVPATH`, `SUBSYSTEM`, `ACTION`, `SEQNUM`, `MAJOR`, `MINOR`, `DEVNAME`, `IFINDEX`) are **NOT** persisted — udev reconstructs them. Confirmed: no `E:DEVPATH`/`E:SUBSYSTEM` in any real record; only `ID_*`/rule-assigned keys appear.
- `S:`, `I:`, `G:`, `Q:` — out of scope (above).
- `V:1` — record version. Constant `1` across all files.
- `E:` lines are **not sorted** (insertion order). We do semantic/set parity, so emission order is irrelevant; byte-parity is a non-goal.

### db-key derivation (filename)
Precedence, first match wins:
1. `MAJOR` and `MINOR` both present → `b<MAJOR>:<MINOR>` if `SUBSYSTEM==block`, else `c<MAJOR>:<MINOR>`.
2. `SUBSYSTEM==net` and `IFINDEX` present → `n<IFINDEX>`.
3. Otherwise → `+<SUBSYSTEM>:<sysname>`, where `sysname` = last `/`-component of `DEVPATH`.
4. None derivable (e.g. no SUBSYSTEM) → return -1, skip the write (not an error).

Observed forms: `b253:0`, `c10:58`, `n1`, `+acpi:AMDI0030:00`.

## Module: `udev_db.h`

Header-only, matching the `udev_builtins.h` / `udev_rules.h` pattern. Depends on `schema-udev.h` (uevent, `uevent_get`, `safe_copy`).

```c
#define SCHEMA_UDEV_DB_DIR "/run/schema-udev/data"

/* Derive the db filename key from the uevent. Returns 0 and fills out[]
 * (NUL-terminated) on success; -1 if no key is derivable. */
int db_key(const struct uevent *ev, char *out, size_t outsz);

/* Serialize the OWNED record: one "E:k=v\n" per property in [kernel_n, ev->n),
 * then "V:1\n". kernel_n is ev->n captured before run_builtins ran, so
 * [kernel_n, ev->n) is exactly the builtins+rules-derived delta (no kernel
 * core props). Returns bytes written (< bufsz), or -1 on overflow. */
int db_serialize(const struct uevent *ev, int kernel_n, char *buf, size_t bufsz);

/* db_key -> serialize -> atomic write to SCHEMA_UDEV_DB_DIR/<key> via
 * mkstemp+rename. Creates the dir on demand. Returns 0 on success, -1 on
 * failure or when db_key returns -1 (nothing to persist). */
int db_write(const struct uevent *ev, int kernel_n);

/* db_key -> unlink SCHEMA_UDEV_DB_DIR/<key>. ENOENT is success. Returns 0
 * on success (incl. already-absent), -1 on other errors or no key. */
int db_remove(const struct uevent *ev);
```

**Property filtering:** only `[kernel_n, ev->n)` is persisted. `kernel_n` is captured in `dispatch()` immediately before `run_builtins`. Because `run_builtins`/`run_rules` only append (first-writer-wins), this range is exactly the synthesized properties — the udev db-flag equivalent. Any property with an empty key or empty value is skipped.

**Atomic write:** `mkstemp` a temp file in `SCHEMA_UDEV_DB_DIR`, write the full serialized buffer, `close`, `rename` over the final path. On any step failure, unlink the temp and return -1. `mkdir(SCHEMA_UDEV_DB_DIR, 0755)` (and its parent `/run/schema-udev`) is attempted before mkstemp; `EEXIST` is fine.

## Wiring: `schema-udev.c` `dispatch()`

```c
static void dispatch(struct uevent *ev) {
    const char *action = uevent_get(ev, "ACTION");
    if (!action) return;
    const char *devpath = uevent_get(ev, "DEVPATH");
    int kernel_n = ev->n;                       /* delta boundary, pre-builtins */
    if (devpath) {
        /* ... existing devname/dn setup ... */
        run_builtins("/sys", devpath, dn, ev);
        run_rules("/sys", devpath, dn, ev);
        if (strcmp(action, "remove") == 0) db_remove(ev);
        else                               db_write(ev, kernel_n);
    }
    /* ... existing rule-match / symlink / hook loop unchanged ... */
}
```

`kernel_n` is captured before the `if (devpath)` block so the boundary is correct even though writes only happen when `devpath` exists. Add `#include "udev_db.h"`. `schema-udev.h` is untouched. Net diff to `schema-udev.c`: one include + ~4 lines.

## Verification

### Unit tests — `tests/test_udev_db.c`
Synthetic-uevent tests (no `/sys` dependency where possible):
- `db_key` block → `b<maj>:<min>`; char → `c<maj>:<min>`; net → `n<ifindex>`; no-devnum → `+<subsys>:<sysname>`; underivable → -1.
- `db_serialize` emits only `[kernel_n, ev->n)` as `E:` lines + trailing `V:1`; asserts **no** `E:DEVPATH`/`E:SUBSYSTEM`/`E:MAJOR` even though those keys are in `[0, kernel_n)`.
- `db_write` then read-back round-trips the record; `db_remove` unlinks it (and second remove returns 0 via ENOENT). Uses a temp `SCHEMA_UDEV_DB_DIR` override if practical, else the real shadow path under the test's own key.
- overflow: a `bufsz` too small for the record returns -1, writes nothing.

### File-vs-file parity — `tools/udev-parity.c`
Extend the existing tool (reuse the slice-1 `parity_in_scope_missing()` / value-mismatch classifier verbatim). For every device in the `/sys` coldplug walk:
1. Read the device's sysfs uevent into a mutable copy; record `kernel_n = ev.n` at that point (the raw sysfs key count); run `run_builtins`+`run_rules`; `db_serialize(&ev, kernel_n, ...)` the owned record. This mirrors the daemon's own `kernel_n` boundary so the persisted set is identical to what `db_write` would produce live.
2. `db_key(ev)` → read udevd's real `/run/udev/data/<key>` via `udev_db_read_eprops`.
3. Compare **our persisted `E:` set** vs udevd's `E:` set with `parity_in_scope_missing` (in-scope missing) and value equality (mismatch). Assert `db_key` equals the real filename udevd used (key-derivation parity).
4. Print counters: `IN-SCOPE MISSING (db)`, `VALUE MISMATCHES (db)`, `KEY-DERIVATION MISMATCHES`.

**Gate:** all three counters `0` across every device that has a real udevd db file. Devices with no real udevd file are skipped (no ground truth), not counted as failures.

### Live gate — `tests/verify_db_live.sh`
After a real coldplug (schema-udev has populated `/run/schema-udev/data` on blakbox):
- For each real shadow file, read its `E:` set and the matching `/run/udev/data/<key>` `E:` set; assert owned-subset (in-scope missing 0, mismatch 0) using the parity tool's own counters — **not** a grep filter (slice-1 hollow-gate lesson: assert the tool's computed counter, never `grep -v` a category to zero).
- Assert every shadow filename corresponds to a real udevd filename (no phantom keys).
- PASS only if the tool reports 0/0/0.

### vmtest
schema-udev is not PID 1; the boot rail must pass unchanged. Run `cd ~/schema-livetest && ./vmtest.sh` — PASS = timer fired + hang excised + dependent ran + SDBOOTED-DIR present, exactly as before.

## Error handling
- `db_key` underivable → skip silently (return -1); not logged as error (normal for keyless devices).
- `db_write` I/O failure → return -1, temp file cleaned up; daemon continues (a db write failure must never drop the event or crash dispatch).
- `db_remove` ENOENT → success. Other unlink errors → -1, non-fatal.
- Shadow dir mkdir race (`EEXIST`) → fine.

## Corrections applied during review
*(populated post-Greg, as in slice 1)*

## Boundary summary
- New file: `udev_db.h`, `tests/test_udev_db.c`, `tests/verify_db_live.sh`.
- Modified: `schema-udev.c` (include + ~4 lines), `tools/udev-parity.c` (db-parity mode), `udev-parity.h` (only if a shared helper is needed — prefer reusing existing classifier untouched), `Makefile` (test target).
- Untouched: `schema-udev.h`, `udev_rules.h`, `udev_builtins.h`, the group-1 netlink bind.
