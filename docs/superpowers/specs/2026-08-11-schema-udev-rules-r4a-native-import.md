# schema-udev R4a — native IMPORT / TEST + RUN-record

**Branch:** `feat/schema-udev-cutover-e3` · **Files:** `udev_ruleset.h`, `udev_builtins.h` (behavior-preserving refactor) + new `tests/test_udev_r4a.c` · **Date:** 2026-08-11

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

- **`udev_builtins.h`** (already exists, currently no caller — built for exactly this): `run_builtins(sysroot,devpath,devnode,ev)` runs every `ub_select`-applicable builtin in fixed udev precedence order, each into a scratch uevent absorbed via `ub_absorb`. **Every signature difference is already handled here** — `path_id_build`'s char-buffer is wrapped into `ID_PATH`/`ID_PATH_TAG` (it is *not* a `struct uevent *` port), `blkid` runs `blkid_pt_build`+`blkid_fs_build`, and the devnode-taking ports (`blkid`, `ata_id`, `v4l_id`, `cdrom_id`) get `devnode`. Bit enum: `UB_HWDB=1, UB_PATH=2, UB_USB=4, UB_INPUT=8, UB_NET=16, UB_BLKID=32, UB_ATA=64, UB_V4L=128, UB_CDROM=256`. Helpers `ub_add`/`ub_absorb`.
- **`udev_db.h`**: `udev_db_filename(ev,out,sz)` (builds `b<maj>:<min>` / `c<maj>:<min>` / `+<subsys>:<sysname>` / `n<ifindex>`), `udev_db_read_eprops(path,out)` (parses `E:KEY=val` lines into a `struct uevent`). These are the IMPORT{db}/{parent} readers.
- **Builtin ports** — do **not** call these directly; go through `udev_builtins.h` which already adapts their differing signatures. `path_id_build(sysroot,devpath,char*out,sz)` returns a buffer; `blkid_fs_build`/`blkid_pt_build`/`ata_id_build`/`v4l_id_build`/`cdrom_id_build` take a **4th `devnode` arg**; `hwdb_build`/`usb_id_build`/`input_id_build`/`net_id_build` are `(sysroot,devpath,struct uevent*)`.
- **`path_id.h`**: `pi_parent(char *cur)` (climb one sysfs level, in-place), `pi_base`, `pi_sysattr`, `pi_driver`, `pi_subsystem`.
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

`DEVCTX_RUNS_MAX 32`: measured max `RUN+=` per rule line = **1**; per-device accumulation across all matched rules is small. RUN is **record-only** — never gated, never executed, never in the fidelity record — so overflow silently drops the excess with zero correctness impact. 32 comfortably exceeds any observed per-device RUN count.

`dev_ctx_init` sets `dbroot = "/run/udev/data"` and `cmdline_path = "/proc/cmdline"`. Tests point them at fake files — the seam that keeps IMPORT{db}/{parent}/{cmdline} off live system state.

### 2. TEST — new match clause

`TEST{<octal>}=="<path>"` / `!=`. In `match_dev_clause`, before the final `return -1`:

- `ruleset_subst` the path (relative paths resolve against `ctx->sysroot`, mirroring udev's cwd semantics for TEST).
- `stat()` it. `==` → matches iff it exists; if `{octal}` present, additionally `(st.st_mode & 07777 & octal) == octal`. `!=` inverts the existence result.

TEST stops being a `-1`/deferred key. In `rule_match`, remove TEST from the deferred path — it is now a resolved gate. **This un-defers 70 clauses and shrinks the superset.**

### 3. IMPORT — native branches

Wired into `apply_rule` at the current `/* IMPORT / RUN / other: R4 */` no-op (line ~518). `IMPORT` uses an assign-op (`=`), but its subtypes split into **hard gates** and **soft imports** — the most error-prone part of this slice. The faithful udev semantics:

**IMPORT failure decision table**

| Clause | R4a runs it? | Success | Empty / failure |
|---|---|---|---|
| `TEST==` / `TEST!=` | yes, at **match** time (`rule_match`) | gate pass | **gate fail → `rule_match` returns 0**, rule never applies |
| `IMPORT{cmdline}` | yes | import keys, continue | import nothing, **continue** (soft) |
| `IMPORT{db}` | yes | import key, continue | import nothing, **continue** (soft) |
| `IMPORT{parent}` | yes | import matching keys, continue | import nothing, **continue** (soft) |
| `IMPORT{builtin}` (ported) | yes | status 0 → import keys, continue | **status < 0 → hard gate, stop applying this rule** |
| `IMPORT{builtin}` (un-ported) | no | — | flag `deferred_applies`, skip clause, **continue** |
| `IMPORT{program}` | no (R4b) | — | flag `deferred_applies`, skip clause, **continue** |
| `RUN` | recorded, never run | appended to `ctx->runs` | n/a |

**Return mechanism (pinned).** No new channel. `apply_rule` already returns `const char *` (a GOTO label, or `NULL` on normal completion). A hard-gate failure = **early `return NULL`**: stop applying the current rule; downstream in `ruleset_apply` this is identical to normal completion (advance to next rule, `i++`). Assignments already applied earlier in the same rule persist — faithful to udev, which applies tokens left-to-right and abandons the rule at the failing token without rollback. TEST is handled entirely in `rule_match` (it is a match op), so it never reaches `apply_rule`.

- **`IMPORT{cmdline}`** — read `ctx->cmdline_path` (`/proc/cmdline`; test-overridable). Tokenize on whitespace; for `key` or `key=val`, `uevent_set(ctx->ev, key, val?val:"1")`. Absent key → import nothing (no gate failure; cmdline import is best-effort in udev).
- **`IMPORT{db}`** — `udev_db_filename(ctx->ev, fn, sz)`; open `dbroot/fn`; `udev_db_read_eprops` into a scratch uevent; import the single named key (`c->val` names the property) via `uevent_set`. Missing file / missing key → no-op. Reads **real udevd's live db, read-only** — udevd is authoritative in shadow, so its record is ground truth, exactly what udevd itself reads during reprocessing.
- **`IMPORT{parent}`** — `pi_parent(ctx->sysdir)` to climb one level; `uevent_from_sysfs` to synth the parent's uevent; `udev_db_filename` on that; read parent's record; import keys matching the clause's glob (`c->val`) into `ctx->ev`. udev's IMPORT{parent} copies parent props whose names match the pattern.
- **`IMPORT{builtin}`** — **reuse `udev_builtins.h`, do not build a parallel dispatch table.** Refactor `run_builtins` to extract a behavior-preserving `run_builtin_bit(sysroot,devpath,devnode,ev,bit)` that runs a single `UB_*` bit's builtin (the existing `run_builtins` becomes: `for each selected bit: run_builtin_bit`). `import_builtin(ctx,name)` maps the rule's builtin name → `UB_*` bit (`hwdb→UB_HWDB, path_id→UB_PATH, usb_id→UB_USB, input_id→UB_INPUT, net_id→UB_NET, blkid→UB_BLKID`) and calls `run_builtin_bit` **directly, bypassing `ub_select`** — a rule that names a builtin runs it unconditionally, unlike the coldplug heuristic path. `devnode` = `/dev/` + `uevent_get(ev,"DEVNAME")`, or NULL when the device has no node (devnode-taking ports no-op gracefully on NULL). `run_builtin_bit` returns the builtin's own **status** (0 = ran, <0 = failed — e.g. `usb_id` on a non-USB device); status `< 0` → **hard gate**, stop applying the rest of this rule (see decision table). Gating on status, **not** on property count — a builtin may succeed and legitimately add zero properties, and rules gate on `IMPORT{builtin}` success (`IMPORT{builtin}="usb_id"` followed by `ENV{ID_BUS}=="usb"`). **Un-ported name** (`keyboard`, `factory_reset`, `dissect_image`, `btrfs`, `net_setup_link`) → set `last_rule_deferred`, bump `deferred_applies`, skip that clause, **continue** the rule (we cannot know its result, so we neither gate nor drop).

  This slice therefore touches `udev_builtins.h` as well as `udev_ruleset.h` — a pure extract-function refactor that leaves `run_builtins`' observable behavior identical (guarded by the existing `test_udev_builtins.c`). `schema-udev.c` stays byte-identical.

### 4. RUN — record, never execute

`RUN+=` / `RUN{builtin}` → append the (subst-expanded) value to `ctx->runs` if room. **Execute nothing.** RUN is a post-processing action (usb_modeswitch ×415, kmod ×10) with side effects that udevd owns in shadow, and it never lands in the fidelity-gated record (S/I/E/G/Q/V + tags + symlinks). Recording it keeps intent visible for R5/audit without dropping it silently.

### 5. Superset re-gate (GOTCHA #2 tightening)

After R4a, a rule is flagged deferred (`deferred_applies++`) only if it still carries an unresolved `PROGRAM`, `RESULT`, `IMPORT{program}`, or un-ported `IMPORT{builtin}`. TEST and native IMPORT no longer inflate the superset. The `deferred_applies` counter therefore drops toward the true residual that R4b will close.

**Accounting-order change (required).** Today `ruleset_apply` bumps `deferred_applies` from `last_rule_deferred` *before* calling `apply_rule` — fine when all deferrals were match-side. But `IMPORT{program}` and un-ported `IMPORT{builtin}` are **assign-op clauses seen only inside `apply_rule`**. So: `rule_match` sets `last_rule_deferred` at entry (as now) for match-side deferrals; `apply_rule` **OR-s in** more when it skips an apply-side deferred clause; `ruleset_apply` moves its `if (last_rule_deferred) deferred_applies++` to **after** `apply_rule`. One bump per applied rule, covering both sides.

## Tests (`tests/test_udev_r4a.c` — new; add to Makefile)

The suite is split by phase (`test_udev_rules.c` / `_matcher.c` / `_executor.c` / `_builtins.c` / `_db.c` / `_ruleset.c`). R4a adds match-clause + apply-branch behavior; give it its own `test_udev_r4a.c` rather than swelling `_ruleset.c`. The `run_builtins` refactor stays covered by the existing `test_udev_builtins.c` (behavior unchanged).

1. **TEST** — path exists/absent (`==`, `!=`); `{octal}` mode pass and fail; `$`-subst in the path. Fabricated tmp tree.
2. **IMPORT{cmdline}** — fake cmdline with `key` and `key=val`; present/absent.
3. **IMPORT{db}** — fake `dbroot/b8:0` record with `E:` lines → named key lands in ctx; missing key no-op; missing file no-op.
4. **IMPORT{parent}** — fake sysfs child+parent, parent db record → glob-matched keys inherited.
5. **IMPORT{builtin} success + soft/hard/deferred split** — `path_id`/`usb_id` against a fabricated sysfs tree, keys merged into ctx; ported builtin that imports **zero** keys → hard gate stops the rule (a later assignment in the same rule does **not** apply); un-ported name sets deferred flag, no crash, rule **not** failed (later assignment *does* apply). This test is the decision table's teeth.
6. **`run_builtin_bit` extraction** — assert `run_builtins` output is byte-identical before/after the refactor for a fabricated multi-builtin device (or lean on existing `test_udev_builtins.c`).
7. **RUN** — recorded into `ctx->runs`, count correct, **nothing executed** (assert a marker file the RUN command *would* have created does not exist).
8. **Re-gate** — a TEST-gated rule that R2/R3 over-matched now correctly rejects (`rule_match` returns 0); `deferred_applies` not bumped for a TEST-only rule.

## Gates (verification before "done")

- `make test` exit 0, all OK lines green.
- **Direct c11 compile** of the test TU with zero warnings — the Makefile hardcodes `-std=c99`, so `make test` alone does **not** CI-gate c11 (R3 deferred-minor (a)). Compile explicitly: `cc -std=c11 -Wall -Wextra ...`.
- Live-smoke: `ruleset_apply` on real `/sys/block/sda`, assert no crash and expected props/tags; **live box untouched** (dry-run, sentinel absent, working-tree binary, no deploy, no reboot).
- Boundary: `schema-udev.c` byte-identical to prior. R4a is headers + tests only — it edits `udev_ruleset.h` and refactors `udev_builtins.h` (behavior-preserving), never `schema-udev.c`.

## Out of scope (R4b / later)

- `IMPORT{program}` (32), `PROGRAM` (17), `RESULT` (3) — external helpers via shell bridge to `/usr/lib/udev/<helper>`, each a tracked reclaim TODO; `$result`/`$name`/`$links`/`$parent` deferred-substitution completion belongs here.
- Un-ported `IMPORT{builtin}` (`keyboard`, `factory_reset`, `dissect_image`, `btrfs`, `net_setup_link`).
- Actually executing RUN natively (kmod, usb_modeswitch) — a post-flip concern.
- R3 deferred-minors (b) GOTO-missing-label semantics, (c) vacuous live-smoke assert — carried forward.

## Parked / risks

- **IMPORT gate faithfulness** is the load-bearing subtlety: `{builtin}`/`{program}` are hard gates (fail → skip rest of rule), but `{db}`/`{parent}`/`{cmdline}` are soft (import-what's-there, always continue). Conflating the two would over- or under-apply. Pinned in the decision table; test 5 is its teeth.
- **hwdb builtin args**: `IMPORT{builtin}="hwdb <args>"` may carry `--subsystem=`/lookup-key arguments the port's `hwdb_build` ignores. If a live-gate mismatch surfaces at R5, revisit arg parsing then — do not speculatively build it now (YAGNI).
- **`udev_db_filename` for parent** requires the parent's MAJOR/MINOR or a `+subsystem:sysname` form; `uevent_from_sysfs` supplies what the parent's `uevent` file carries. A parent with no db record → inherit nothing (faithful).
