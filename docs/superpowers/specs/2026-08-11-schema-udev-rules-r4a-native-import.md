# schema-udev R4a — native IMPORT / TEST + RUN-record

**Branch:** `feat/schema-udev-cutover-e3` · **File:** `udev_ruleset.h` (+ `tests/test_udev_ruleset.c`) · **Date:** 2026-08-11

## Context

R1 (parser) → R2 (matcher) → R3 (executor) are done and reviewed. R3 applies rules that R2 **over-matched**, because `TEST`/`PROGRAM`/`RESULT`/`IMPORT{program}` clauses were deferred (GOTCHA #2 — R2's match set is a *superset*, flagged via `ctx->deferred_applies`). R4 closes that gap. It is split into **R4a (this spec — pure-native, no shell, no side effects)** and **R4b (shell bridges — a later slice)**.

Everything runs **shadow-only**: real systemd-udevd stays authoritative (pid 207 on blakbox), owns real `/dev` + `/run/udev/data` + group-2. schema-udev computes in a working-tree binary, wired to nothing live. The R5 fidelity gate (`make verify-rules-live` across 471 devices) is the E3-flip precondition; R4a exists to make that gate reachable for every rule that resolves natively.

## Live workload (168 installed `.rules`, blakbox)

| Mechanism | Count | R4a disposition |
|---|---|---|
| `TEST==` / `TEST!=` (`{octal}` optional) | 70 | **native** — `stat()` + mode mask; a real match gate |
| `IMPORT{cmdline}` | 6 | **native** — read `/proc/cmdline` |
| `IMPORT{db}` | 29 | **native** — read device's own `/run/udev/data` record |
| `IMPORT{parent}` | 13 | **native** — inherit from parent's db record |
| `IMPORT{builtin}` | 90 | **native** — dispatch to existing ports; un-ported → deferred |
| `RUN+=` / `RUN{builtin}` | 480 / 12 | **record intent, execute nothing** |
| `IMPORT{program}` | 32 | **deferred to R4b** (external helper, shell bridge) |
| `PROGRAM` / `RESULT` | 17 / 3 | **deferred to R4b** |

The `usb`/`pci`/`disk`/`systemd` entries seen in a naive `IMPORT{builtin}` grep are false positives (second quoted token on the same rule line), not builtin names. The real builtin name universe invoked here: `hwdb`, `path_id`, `usb_id`, `blkid`, `input_id`, `net_id` (all **ported**), plus `keyboard`, `factory_reset`, `dissect_image`, `btrfs`, `net_setup_link` (un-ported → deferred).

## Reused, do not reinvent

- **`udev_db.h`**: `udev_db_filename(ev,out,sz)` (builds `b<maj>:<min>` / `c<maj>:<min>` / `+<subsys>:<sysname>` / `n<ifindex>`), `udev_db_read_eprops(path,out)` (parses `E:KEY=val` lines into a `struct uevent`). These are the IMPORT{db}/{parent} readers.
- **Builtin ports** (`struct uevent *out`, byte-exact, live-verified): `hwdb_build`, `usb_id_build`, `input_id_build`, `net_id_build`, `blkid_fs_build`, `blkid_pt_build`; `path_id_build` (char-buffer, emits `ID_PATH`/`ID_PATH_TAG` — wrapped).
- **`schema-udev.h`**: `uevent_get`/`uevent_set` (R3), `uevent_from_sysfs(sysroot,dir,ev)` (synthesizes a parent's uevent so its db filename resolves).
- **`udev_ruleset.h`**: `ruleset_subst` (`$`/`%` token expansion), `rk_cmp`, `match_dev_clause`, `rule_match`, `apply_rule`, `dev_ctx`.

## Design

All R4a code lands in `udev_ruleset.h`. No new files.

### 1. `dev_ctx` growth (minimal)

```c
#define DEVCTX_RUNS_MAX 32
char runs[DEVCTX_RUNS_MAX][UE_VAL_MAX];  int nruns;   /* recorded RUN intent; never executed */
const char *dbroot;                                   /* "/run/udev/data"; test-overridable */
const char *cmdline_path;                             /* "/proc/cmdline"; test-overridable */
```

`dev_ctx_init` sets `dbroot = "/run/udev/data"` and `cmdline_path = "/proc/cmdline"`. Tests point them at fake files — the seam that keeps IMPORT{db}/{parent}/{cmdline} off live system state.

### 2. TEST — new match clause

`TEST{<octal>}=="<path>"` / `!=`. In `match_dev_clause`, before the final `return -1`:

- `ruleset_subst` the path (relative paths resolve against `ctx->sysroot`, mirroring udev's cwd semantics for TEST).
- `stat()` it. `==` → matches iff it exists; if `{octal}` present, additionally `(st.st_mode & 07777 & octal) == octal`. `!=` inverts the existence result.

TEST stops being a `-1`/deferred key. In `rule_match`, remove TEST from the deferred path — it is now a resolved gate. **This un-defers 70 clauses and shrinks the superset.**

### 3. IMPORT — native branches

Wired into `apply_rule` at the current `/* IMPORT / RUN / other: R4 */` no-op (line ~518). `IMPORT` uses an assign-op (`=`); in udev it acts as an **implicit gate** — a native IMPORT that fails imports nothing *and skips the rest of the rule's application*. R4a honors that: `apply_rule` returns a sentinel (or sets a flag) so the caller stops applying the current rule when a native IMPORT hard-fails, matching udev.

- **`IMPORT{cmdline}`** — read `ctx->cmdline_path` (`/proc/cmdline`; test-overridable). Tokenize on whitespace; for `key` or `key=val`, `uevent_set(ctx->ev, key, val?val:"1")`. Absent key → import nothing (no gate failure; cmdline import is best-effort in udev).
- **`IMPORT{db}`** — `udev_db_filename(ctx->ev, fn, sz)`; open `dbroot/fn`; `udev_db_read_eprops` into a scratch uevent; import the single named key (`c->val` names the property) via `uevent_set`. Missing file / missing key → no-op. Reads **real udevd's live db, read-only** — udevd is authoritative in shadow, so its record is ground truth, exactly what udevd itself reads during reprocessing.
- **`IMPORT{parent}`** — `pi_parent(ctx->sysdir)` to climb one level; `uevent_from_sysfs` to synth the parent's uevent; `udev_db_filename` on that; read parent's record; import keys matching the clause's glob (`c->val`) into `ctx->ev`. udev's IMPORT{parent} copies parent props whose names match the pattern.
- **`IMPORT{builtin}`** — dispatch table `name → port fn`:
  `path_id, usb_id, input_id, net_id, hwdb, blkid`. Call `<port>_build(ctx->sysroot, devpath, &scratch)` into a scratch uevent; merge every scratch key into `ctx->ev` via `uevent_set`. `path_id` wrapped (buffer → set `ID_PATH`/`ID_PATH_TAG`). `blkid` maps to `blkid_fs_build` then `blkid_pt_build` (both, as udev's blkid builtin emits FS + PT props). Builtin returns error/empty → gate fails (skip rest of rule), matching udev. **Un-ported name** (`keyboard`, `factory_reset`, `dissect_image`, `btrfs`, `net_setup_link`) → set `last_rule_deferred`, bump `deferred_applies`, skip that clause (do not fail the rule — we cannot know its result).

### 4. RUN — record, never execute

`RUN+=` / `RUN{builtin}` → append the (subst-expanded) value to `ctx->runs` if room. **Execute nothing.** RUN is a post-processing action (usb_modeswitch ×415, kmod ×10) with side effects that udevd owns in shadow, and it never lands in the fidelity-gated record (S/I/E/G/Q/V + tags + symlinks). Recording it keeps intent visible for R5/audit without dropping it silently.

### 5. Superset re-gate (GOTCHA #2 tightening)

After R4a, a rule is flagged deferred (`deferred_applies++`) only if it still carries an unresolved `PROGRAM`, `RESULT`, `IMPORT{program}`, or un-ported `IMPORT{builtin}`. TEST and native IMPORT no longer inflate the superset. The `deferred_applies` counter therefore drops toward the true residual that R4b will close.

## Tests (`tests/test_udev_ruleset.c`, extend existing)

1. **TEST** — path exists/absent (`==`, `!=`); `{octal}` mode pass and fail; `$`-subst in the path. Fabricated tmp tree.
2. **IMPORT{cmdline}** — fake cmdline with `key` and `key=val`; present/absent.
3. **IMPORT{db}** — fake `dbroot/b8:0` record with `E:` lines → named key lands in ctx; missing key no-op; missing file no-op.
4. **IMPORT{parent}** — fake sysfs child+parent, parent db record → glob-matched keys inherited.
5. **IMPORT{builtin}** — dispatch to `path_id`/`usb_id` against a fabricated sysfs tree, keys merged into ctx; un-ported name sets deferred flag, no crash, rule not failed.
6. **RUN** — recorded into `ctx->runs`, count correct, **nothing executed** (assert a marker file the RUN command *would* have created does not exist).
7. **Re-gate** — a TEST-gated rule that R2/R3 over-matched now correctly rejects (`rule_match` returns 0); `deferred_applies` not bumped for a TEST-only rule.

## Gates (verification before "done")

- `make test` exit 0, all OK lines green.
- **Direct c11 compile** of the test TU with zero warnings — the Makefile hardcodes `-std=c99`, so `make test` alone does **not** CI-gate c11 (R3 deferred-minor (a)). Compile explicitly: `cc -std=c11 -Wall -Wextra ...`.
- Live-smoke: `ruleset_apply` on real `/sys/block/sda`, assert no crash and expected props/tags; **live box untouched** (dry-run, sentinel absent, working-tree binary, no deploy, no reboot).
- Boundary: `schema-udev.c` byte-identical to prior (R4a is header-only + tests).

## Out of scope (R4b / later)

- `IMPORT{program}` (32), `PROGRAM` (17), `RESULT` (3) — external helpers via shell bridge to `/usr/lib/udev/<helper>`, each a tracked reclaim TODO; `$result`/`$name`/`$links`/`$parent` deferred-substitution completion belongs here.
- Un-ported `IMPORT{builtin}` (`keyboard`, `factory_reset`, `dissect_image`, `btrfs`, `net_setup_link`).
- Actually executing RUN natively (kmod, usb_modeswitch) — a post-flip concern.
- R3 deferred-minors (b) GOTO-missing-label semantics, (c) vacuous live-smoke assert — carried forward.

## Parked / risks

- **IMPORT gate faithfulness** is the load-bearing subtlety: udev treats a failed IMPORT as a rule-skip. Getting the native-IMPORT-fails-→-skip-rest-of-rule path wrong would either over- or under-apply. Covered by test 5 (un-ported must *not* fail the rule) and the builtin-empty case.
- **hwdb builtin args**: `IMPORT{builtin}="hwdb <args>"` may carry `--subsystem=`/lookup-key arguments the port's `hwdb_build` ignores. If a live-gate mismatch surfaces at R5, revisit arg parsing then — do not speculatively build it now (YAGNI).
- **`udev_db_filename` for parent** requires the parent's MAJOR/MINOR or a `+subsystem:sysname` form; `uevent_from_sysfs` supplies what the parent's `uevent` file carries. A parent with no db record → inherit nothing (faithful).
