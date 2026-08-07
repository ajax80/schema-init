# schema-udev sub-project B slice 2 — shadow db writer

**Status:** design approved 2026-08-07. Endgame arc: udevd retirement. Slice 1 (property completeness, PR #89) landed the owned-subset property engine; this slice materializes those properties as on-disk udev db records so libudev clients can eventually read them at cutover.

## Goal

schema-udev writes udev-format device db records to a **shadow directory** (`/run/schema-udev/data`, never udevd's real `/run/udev/data`), live during coldplug and hot events, verified by file-vs-file parity against udevd's real db. The hazardous group-2 libudev rebroadcast is **explicitly out of scope** here — staged for near-cutover.

## Scope

### In scope
- Extract the existing db functions from `schema-udev.h` (+ the reader from `udev-parity.h`) into a new `udev_db.h`, fixing the record-build delta boundary and atomic write while moving; add `udev_db_remove`.
- One minimal call site in `schema-udev.c` `dispatch()`.
- Shadow-dir plumbing (`/run/schema-udev/data`, created on demand).
- Owned-subset record: `E:` lines for the derived property delta + trailing `V:1`.
- File-vs-file parity gate (extends `tools/udev-parity.c`) + live gate (`tests/verify_db_live.sh`).
- Rewritten unit tests (`tests/test_udev_db.c`).

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

## Module: `udev_db.h` (extraction + fix)

The db functions already exist in `schema-udev.h` (`udev_db_filename`, `udev_db_record_build`, `udev_db_write`) and the reader `udev_db_read_eprops` lives in `udev-parity.h`. This slice **moves them all into a new `udev_db.h`** (the single db home, matching the `udev_builtins.h` / `udev_rules.h` module-per-concern pattern) and fixes the two real defects while moving. `udev_db.h` includes `schema-udev.h` (for `struct uevent`, `uevent_get`, `safe_copy`) plus `sys/stat.h`, `sys/types.h`, `unistd.h`, `errno.h`, `stdio.h`, `string.h`, `stdlib.h`.

`schema-udev.h` loses the three db functions (shrinks). `udev-parity.h` loses `udev_db_read_eprops` and gains `#include "udev_db.h"`. `#define UDEV_DB_DIR "/run/udev/data"` (udevd's real dir, read-only ground truth) stays reachable via `udev_db.h`.

```c
#define SCHEMA_UDEV_DB_DIR "/run/schema-udev/data"  /* OUR shadow dir */
#define UDEV_DB_DIR        "/run/udev/data"         /* udevd's real dir (read-only) */

/* UNCHANGED (moved verbatim): derive the db filename key. net+IFINDEX first,
 * then devnum (b/c<maj>:<min>), then +<subsystem>:<sysname>. Returns 0 + fills
 * out[]; -1 if none derivable. Net devices never carry MAJOR/MINOR, so the
 * net-first order is equivalent to udev's devnum-first for real devices. */
int udev_db_filename(const struct uevent *ev, char *out, size_t outsz);

/* FIXED: serialize the OWNED record — one "E:k=v\n" per property in
 * [kernel_n, ev->n), then a TRAILING "V:1\n". kernel_n is ev->n captured
 * before run_builtins ran, so [kernel_n, ev->n) is exactly the builtins+rules
 * delta (no kernel core props like DEVPATH/SUBSYSTEM/MAJOR). Skips any entry
 * with an empty key or empty value. Returns bytes written (< bufsz), -1 on
 * overflow. (Was: all props from 0, leading V:1.) */
ssize_t udev_db_record_build(const struct uevent *ev, int kernel_n,
                             char *buf, size_t bufsz);

/* FIXED: udev_db_filename -> record_build -> ATOMIC write to base_dir/<key>
 * via mkstemp+rename. Creates base_dir on demand. Returns 0; -1 on failure or
 * when udev_db_filename returns -1 (nothing to persist). base_dir is a param
 * so tests pass a temp dir and the daemon passes SCHEMA_UDEV_DB_DIR.
 * (Was: bare fopen("w").) */
int udev_db_write(const char *base_dir, const struct uevent *ev, int kernel_n);

/* NEW: udev_db_filename -> unlink base_dir/<key>. ENOENT is success. Returns 0
 * (incl. already-absent), -1 on other errors or no derivable key. */
int udev_db_remove(const char *base_dir, const struct uevent *ev);

/* UNCHANGED (moved from udev-parity.h): read E: lines of a db file into out. */
int udev_db_read_eprops(const char *path, struct uevent *out);
```

**Property filtering:** only `[kernel_n, ev->n)` is persisted. `kernel_n` is captured in `dispatch()` immediately before `run_builtins`. Because `run_builtins`/`run_rules` only append (first-writer-wins), this range is exactly the synthesized properties — the udev db-flag equivalent. Entries with an empty key or empty value are skipped.

**Atomic write:** `mkstemp` a temp file in `base_dir`, write the full serialized buffer, `close`, `rename` over the final path. On any step failure, unlink the temp and return -1. `mkdir(base_dir, 0755)` (and its parent for the daemon's `/run/schema-udev`) is attempted first; `EEXIST` is fine.

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
        if (strcmp(action, "remove") == 0) udev_db_remove(SCHEMA_UDEV_DB_DIR, ev);
        else                               udev_db_write(SCHEMA_UDEV_DB_DIR, ev, kernel_n);
    }
    /* ... existing rule-match / symlink / hook loop unchanged ... */
}
```

`kernel_n` is captured before the `if (devpath)` block so the boundary is correct even though writes only happen when `devpath` exists. Add `#include "udev_db.h"`. Net diff to `schema-udev.c`: one include + ~4 lines. `schema-udev.h` **shrinks** (the three db functions move out) — it is not left byte-identical this slice.

## Verification

### Unit tests — `tests/test_udev_db.c` (REWRITE)
This file **already exists** and asserts the OLD behavior (all props from index 0, leading `V:1`). It must be rewritten to the new contract, not appended to. Synthetic-uevent tests (no `/sys` dependency where possible), writing to a temp `base_dir` from `mkdtemp`:
- `udev_db_filename` block → `b<maj>:<min>`; char → `c<maj>:<min>`; net → `n<ifindex>`; no-devnum → `+<subsys>:<sysname>`; underivable (no subsystem, no devnum) → -1.
- `udev_db_record_build(ev, kernel_n, ...)` emits only `[kernel_n, ev->n)` as `E:` lines followed by a **trailing** `V:1`; asserts **no** `E:DEVPATH`/`E:SUBSYSTEM`/`E:MAJOR` even though those keys sit in `[0, kernel_n)`, and asserts the last line is `V:1`.
- `udev_db_write(tmpdir, ev, kernel_n)` then `udev_db_read_eprops` round-trips the derived `E:` set; `udev_db_remove(tmpdir, ev)` unlinks it, and a second remove returns 0 (ENOENT).
- overflow: a `bufsz` too small for the record returns -1, writes nothing.

### File-vs-file parity — `tools/udev-parity.c`
Extend the existing tool (reuse the slice-1 `parity_in_scope_missing()` / value-mismatch classifier verbatim). For every device in the `/sys` coldplug walk:
1. Read the device's sysfs uevent into a mutable copy; record `kernel_n = ev.n` at that point (the raw sysfs key count); run `run_builtins`+`run_rules`; `udev_db_record_build(&ev, kernel_n, ...)` the owned record, then parse its `E:` lines back out (or serialize+re-read) to get our persisted set. This mirrors the daemon's own `kernel_n` boundary so the persisted set is identical to what `udev_db_write` would produce live.
2. `udev_db_filename(ev)` → read udevd's real `/run/udev/data/<key>` via `udev_db_read_eprops`.
3. Compare **our persisted `E:` set** vs udevd's `E:` set with `parity_in_scope_missing` (in-scope missing) and value equality (mismatch). Assert `udev_db_filename` equals the real filename udevd used (key-derivation parity).
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
- New file: `udev_db.h`, `tests/verify_db_live.sh`.
- Rewritten: `tests/test_udev_db.c` (old contract → new delta/trailing-`V:1` contract).
- Modified:
  - `schema-udev.h` — **remove** `udev_db_filename`/`udev_db_record_build`/`udev_db_write` (moved to `udev_db.h`).
  - `udev_db.h` — receives the three moved functions (with the record_build + write fixes), `udev_db_remove` (new), and `udev_db_read_eprops` (moved from `udev-parity.h`); defines `SCHEMA_UDEV_DB_DIR` + `UDEV_DB_DIR`.
  - `udev-parity.h` — drop `udev_db_read_eprops` + its `UDEV_DB_DIR` define, add `#include "udev_db.h"`. Classifier untouched.
  - `schema-udev.c` — `#include "udev_db.h"` + ~4 lines in `dispatch()`.
  - `tools/udev-parity.c` — add the db file-vs-file parity mode.
  - `Makefile` — `test_udev_db` target + any include-dep update.
- Untouched: `udev_rules.h`, `udev_builtins.h`, the **group-1** netlink bind and its "NEVER group 2" comment.
