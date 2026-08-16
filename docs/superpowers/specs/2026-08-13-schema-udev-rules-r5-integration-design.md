# schema-udev R5 — rule-interpreter integration + live fidelity gate

**Date:** 2026-08-13
**Branch:** `feat/schema-udev-cutover-e3`
**Status:** design (shadow-only; live box untouched; systemd-udevd authoritative)

## Purpose

R1–R4b built and TDD'd a native udev `.rules` interpreter (`udev_ruleset.h`:
`ruleset_load_dirs` → `ruleset_apply` → `dev_ctx` accumulating `tags[]`,
`symlinks[]`, ported builtins, PROGRAM/IMPORT). It has never run in the daemon
and its output has never been materialized or measured against real udevd.

R5 is the **E3-flip precondition**. It does two things, both shadow-only:

1. **Wire** the interpreter into `schema-udev` dispatch so it runs under real
   coldplug + uevents and writes a full udev-format record
   (`S:`/`G:`/`Q:`/`E:`/`V:`) to a **new** shadow dir, leaving the existing
   dispatch pipeline byte-for-byte unchanged.
2. **Measure** — a standalone `verify-rules-live` gate that recomputes the
   interpreter's output across every device and diffs `S:` symlinks + `G:` tags
   against real `/run/udev/data`, exiting nonzero on any in-scope divergence.

R5 does **not** flip E3, does **not** write `/dev` or `/run/udev/data`, and does
**not** retire the existing `run_rules` + `disk_links` + `uaccess` paths. Those
retire only after the gate is green and E3 is cut.

## Non-goals (explicit)

- The `ata_id_build` under-emit on real sda (sysfs vendor-string padding). R5
  **surfaces** it as `KNOWN-DEBT` in the gate; it is fixed in its own later
  slice, and the gate goes green for that class when it lands.
- `scsi_id` native port (tracked reclaim debt) — same treatment: `KNOWN-DEBT`.
- Any change to `run_rules`, `disk_links`, `uaccess`, or the E:-only
  `SCHEMA_UDEV_DB_DIR` record path.

## Architecture

### A. Daemon wiring — `schema-udev.c` + `udev_db.h`

**The existing dispatch pipeline stays 100% untouched.** `run_builtins` →
`run_rules` → `udev_db_write` (E:-only, `SCHEMA_UDEV_DB_DIR`) → `disk_links` →
`uaccess` are unmodified. R5 adds a parallel, isolated shadow write.

New global ruleset, loaded from the real udev rule dirs:

```c
static struct ruleset g_ruleset;   /* rules[] is realloc'd heap */
static const char *const RULE_DIRS[] = {
    "/usr/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d" };

static void ruleset_reload(void) {
    free(g_ruleset.rules);                 /* heap, unlike fixed g_rules */
    memset(&g_ruleset, 0, sizeof g_ruleset);
    ruleset_load_dirs(RULE_DIRS, 3, &g_ruleset);
    fprintf(stderr, "[schema-udev] loaded %d native rule(s)\n", g_ruleset.n);
}
```

- Called once at startup next to `rules_reload()`, and again in the **existing
  signalfd SIGHUP branch** (`schema-udev.c:205`). There is **no async-signal
  race**: SIGHUP is delivered via `signalfd` and drained in the single-threaded
  poll loop, never concurrent with `dispatch`. The **only** hazard is the heap
  leak — `struct ruleset` owns a `realloc`'d `rules` array (unlike the fixed
  `g_rules` array), so the reload **must** `free(g_ruleset.rules)` first. That
  `free` is the whole difference from `rules_reload()`.

Startup also **wipes** the new shadow dir (mirrors `disk_links_wipe` /
`uaccess_wipe`) so stale shadow records never survive a restart.

**Dispatch insertion** — inside the existing `if (devpath) { … }` block, placed
**after** `run_builtins(...)` (so `ID_*` props exist and `kernel_n` is already
captured), operating on a **private deep copy** so nothing the interpreter does
is visible to the rest of dispatch:

```c
/* --- R5 shadow: native rule interpreter, isolated from the live pipeline --- */
struct uevent shadow_ev = *ev;         /* full deep copy: uevent is inline char[] arrays, no heap */
int pre_rules_n = shadow_ev.n;         /* post-builtins baseline */
struct dev_ctx ctx;
if (dev_ctx_init(&ctx, &shadow_ev, "/sys") == 0) {
    ruleset_apply(&g_ruleset, &ctx);
    if (strcmp(action, "remove") == 0) {
        udev_db_remove(SCHEMA_UDEV_RULES_DIR, &shadow_ev);
    } else if (ctx.nsym > 0 || ctx.ntags > 0 || shadow_ev.n > pre_rules_n) {
        const char *syms[DEVCTX_SYMLINKS_MAX];
        const char *tags[DEVCTX_TAGS_MAX];
        for (int i = 0; i < ctx.nsym;  i++) syms[i] = ctx.symlinks[i];
        for (int i = 0; i < ctx.ntags; i++) tags[i] = ctx.tags[i];
        udev_db_write_full(SCHEMA_UDEV_RULES_DIR, &shadow_ev, kernel_n,
                           syms, ctx.nsym, tags, ctx.ntags);
    }
}
```

**Why the deep copy is load-bearing.** `ruleset_apply` calls
`uevent_set(ctx->ev, …)` for ENV assignments, `IMPORT{db,cmdline,parent}`, and
every ported builtin. If it shared `ev`, those appended keys would land in
`[kernel_n, ev->n)` and leak into the *existing* E:-only shadow record and into
`disk_links`' `ID_*` reads downstream — silently breaking "dispatch stays
untouched." Because `struct uevent` is `char key[N][K]` / `char val[N][V]` +
`int n` with no pointers, `struct uevent shadow_ev = *ev;` is a complete
independent copy. `kernel_n` is unchanged and shared (it indexes the same
kernel-property prefix in both copies).

**Write-guard.** `ev->n > kernel_n` alone is ~always true (builtins already grew
`ev` before the interpreter runs), so it would degenerate to "builtins did
anything" and inflate the shadow dir with hundreds of empty-`S:`/`G:` records.
Guarding on `shadow_ev.n > pre_rules_n` (baseline captured *after* builtins)
plus `nsym`/`ntags` writes a record only when the **interpreter** contributed.

New dir constant in `udev_db.h`:

```c
#define SCHEMA_UDEV_RULES_DIR "/run/schema-udev/rules-data"
```

New atomic writer in `udev_db.h` — mirrors `udev_db_write` exactly (recursive
`udev_db_ensure_dir`, `mkstemp` + `rename`) but calls the already-built
`udev_db_record_build_full` and passes `usec_init = 0`:

```c
static inline int udev_db_write_full(const char *base_dir, const struct uevent *ev,
                                     int kernel_n,
                                     const char *const *symlinks, int nsym,
                                     const char *const *tags, int ntag);
```

`I:` is emitted as 0 (skipped by `record_build_full`): udevd's init-usec is
per-boot and never matchable from a coldplug pass. `Q:` = `G:` (what
`record_build_full` already does); the gate compares `G:` only, so `Q:` is
inert. Reuse the existing `udev_db_remove` for the new dir on `remove`.

### B. `verify-rules-live` gate — `tools/verify-rules-live.c` + Makefile target

Standalone, **read-only, writes nothing**. Independent of the daemon — it
recomputes from scratch rather than reading shadow records, so the daemon is not
a failure mode in the gate and the gate runs without the daemon.

Per device (`coldplug_walk_root("/sys", collect)`), mirroring the daemon
exactly: mutable `ev` copy, `kernel_n = ev.n`, `run_builtins("/sys", …)`, then
`dev_ctx_init` + `ruleset_apply`. Read real `/run/udev/data/<name>` (filename
via `udev_db_filename`), parse its `S:` link lines and `G:` tag lines (new tiny
reader `udev_db_read_links_tags` in `udev_db.h`, sibling to
`udev_db_read_eprops`). Then set-compare:

**Symlinks** (ours = `ctx.symlinks[]`, theirs = parsed `S:`):

- **`SYM-EXTRA`** — in ours, not theirs. A link udevd does not create =
  interpreter applied something wrong. **Fails the gate.**
- **`SYM-MISS`** — in theirs, not ours. Classified by prefix:
  `/dev/disk/by-id/`, `/dev/disk/by-path/`, `/dev/disk/by-uuid/`,
  `/dev/disk/by-partuuid/` links that trace to an unported / under-emitting
  builtin (`scsi_id`, `ata_id`) are **`KNOWN-DEBT`** — reported, **non-fatal**.
  Any other miss is **in-scope** and **fails the gate**.

**Tags** (ours = `ctx.tags[]`, theirs = parsed `G:`): set-compare. In-scope
mismatches (either direction) **fail the gate**.

**`E:` is not compared** — out of scope, and would be enormously noisy;
device-access behavior is governed by `S:`/`G:`, which is what the gate guards.

**Exit code:** `0` iff zero in-scope symlink + tag divergences (this is what
makes it a *precondition*, not merely a report). Output: a per-subsystem summary
(devices / with-udev-db / reproduced), the divergence list, and a `KNOWN-DEBT`
section. Reuse `udev-parity.h`'s classification helpers (`parity_builtin_hint`,
`keycount`) where they fit; the by-* prefix classifier is new and small.

**Makefile** — a `verify-rules-live` target mirroring `parity` (build the tool,
then run it), and add the new interpreter headers
(`udev_ruleset.h path_id.h udev_exec.h fido_id.h …`) to the `schema-udev`
prerequisite list.

## Data flow

```
add / change / coldplug:
  ev(kernel) --run_builtins--> ev(+ID_*)          [kernel_n captured pre-builtins]
     |                              |
     |  (existing pipeline, UNCHANGED)              (R5 shadow, on a private copy)
     |  run_rules -> udev_db_write(E:, DB_DIR)      shadow_ev = *ev; pre_rules_n = n
     |            -> disk_links -> uaccess          ruleset_apply(g_ruleset, ctx@shadow_ev)
     |                                              guard -> udev_db_write_full(
     v                                                 RULES_DIR, S:/G:/Q:/E:/V:)
remove:
     existing: udev_db_remove(DB_DIR)   +   R5: udev_db_remove(RULES_DIR)

verify-rules-live (offline, standalone):
  /sys walk -> per dev: run_builtins + ruleset_apply  (ours)
            -> read /run/udev/data/<name> S:,G:        (theirs)
            -> diff -> SYM-EXTRA/SYM-MISS/tag-diff -> in-scope vs KNOWN-DEBT
            -> exit 0 iff in-scope divergences == 0
```

## Error handling

- `dev_ctx_init` returns nonzero (no DEVPATH / overflow) → skip the shadow write
  for that event; existing pipeline unaffected.
- `udev_db_write_full` failure (mkstemp/rename/overflow) → same contract as
  `udev_db_write`: return `-1`, write nothing, leave no temp file. Shadow-only,
  so a failure is logged-and-ignored, never fatal to the daemon.
- Ruleset load failure at startup → `g_ruleset.n == 0`; `ruleset_apply` is a
  no-op; no shadow records written. Daemon still runs.
- Reload leak: `free(g_ruleset.rules)` before every reload (see A).

## Testing

- **`tests/test_udev_db.c`** — add a `udev_db_write_full` round-trip: write a
  record with 2 symlinks + 2 tags + an E: delta to a `mkdtemp` dir, read it
  back, assert `S:`/`G:`/`Q:`/`E:`/`V:` bytes and ordering; assert the
  write-guard-empty case; `udev_db_read_links_tags` extracts the `S:`/`G:` sets.
- **Live smoke (dry-run)** — coldplug walk with the sentinel absent; confirm
  `/run/schema-udev/rules-data` gains records carrying `S:`/`G:` lines for
  `/sys/block/sda`; confirm the existing `SCHEMA_UDEV_DB_DIR` records are
  **byte-identical** to a pre-R5 run (proves the deep-copy isolation).
- **`make test` green**, 0 warnings under c99 **and** c11 × `-O0`/`-O2`.
- **Gate run** — execute `verify-rules-live` on blakbox; eyeball the report;
  validate `KNOWN-DEBT` classification against sda/sdb by-id links; confirm any
  `SYM-EXTRA` is genuinely zero (or root-caused if not).
- **Live box untouched** — sentinel `/etc/schema-init/schema-udev.live` absent,
  `/usr/bin/schema-udev` md5 unchanged, systemd-udevd pid authoritative, nothing
  written to `/dev` or `/run/udev/data`. Verified at close.

## Files touched

- `udev_db.h` — `SCHEMA_UDEV_RULES_DIR`, `udev_db_write_full`,
  `udev_db_read_links_tags`.
- `schema-udev.c` — `g_ruleset` + `RULE_DIRS`, `ruleset_reload` (with the
  `free`), startup load + wipe, SIGHUP reload, the deep-copy shadow block in
  `dispatch`, remove-branch unlink.
- `tools/verify-rules-live.c` — new offline gate.
- `Makefile` — `verify-rules-live` target; `schema-udev` prereq header list.
- `tests/test_udev_db.c` — `udev_db_write_full` / `udev_db_read_links_tags`
  round-trip.

## Review deltas folded in (Greg, Opus 4.6)

1. **Deep-copy `ev` → `shadow_ev`** before `dev_ctx_init` — `ruleset_apply`
   mutates its `ev`; sharing it would leak interpreter props into the live
   pipeline. Real bug; accepted.
2. **`free(g_ruleset.rules)` on reload** — no SIGHUP async race (signalfd,
   single-threaded loop), but `struct ruleset` heap must be freed per reload.
3. **Write-guard on `pre_rules_n`** (post-builtins baseline), not `kernel_n` —
   otherwise the guard degenerates to "builtins ran" and inflates the shadow dir.
