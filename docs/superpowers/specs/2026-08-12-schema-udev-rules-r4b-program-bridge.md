# schema-udev R4b — PROGRAM/RESULT, IMPORT{program}, native fido_id

**Branch:** `feat/schema-udev-cutover-e3` · **Posture:** shadow-only, live box untouched, no deploy.
**Goal:** close the last `deferred_applies` residual so the R5 fidelity gate can pass — the E3-flip precondition. Reclaim what is udev's (`fido_id`, plus redirect to existing native ports); bridge what is genuinely foreign; leave nothing silently deferred that the installed ruleset actually exercises.

## Scope (from the real installed ruleset audit)

`IMPORT{program}` (~34 calls) splits three ways:
- **Native-port redirect** — `ata_id`, `v4l_id`, `cdrom_id` (5 rules): builtins already exist (`UB_ATA`/`UB_V4L`/`UB_CDROM`), just unwired from the program path.
- **Newly ported this round** — `fido_id` (1 rule): the pico-fido ACL blocker.
- **Bridged (fork/execvp)** — every genuinely-external helper: `scsi_id`×4, `dmsetup`×4, `mdadm`×2, `lvm`, `kpartx_id`, `alsactl`×2, `libinput-*`×3, `ipod/iphone-set-info`×4, `mtd_probe`, `dmi_memory_id`, `sh`, `cat`. Each bridged **udev-helper** (`scsi_id`) is logged as a tracked reclaim TODO for a later phase; the foreign tools (device-mapper, RAID, LVM, sound) are permanent bridges, not debt.

`PROGRAM=` (17) + `RESULT==` (3). `IMPORT{builtin}`: **zero** in the installed rules — keyboard/factory_reset/dissect_image/btrfs/net_setup_link are not invoked on this box and are **out of scope** (YAGNI).

## Architecture

### 1. Bridge primitive — `udev_run_capture`
New: `int udev_run_capture(const char *sysroot, const char *cmdstr, char *out, size_t outlen)` → child exit status, or `-1` on spawn/timeout failure.
- **`fork`/`execvp` only — never `system()`, never an implicit shell.** Rules that want a shell already spell it `PROGRAM="/bin/sh -c '...'"`.
- **Quote-aware argv tokenizer** for *argument splitting only* (must handle `'single-quoted args'`), not shell dispatch. Splits `cmdstr` into `argv[]` on unquoted whitespace, honoring single quotes.
- **Path resolution:** `argv[0]` absolute → exec directly; bare basename → search `/usr/lib/udev` first, then `PATH` (udev's libdir-first rule).
- **Timeout:** 180s (udev `EVENT_TIMEOUT_SEC`). On expiry: kill child, return `-1`. A hung `mtp-probe` must not stall the pipeline even in the shadow binary.
- **Stdout capture bound:** 16384 bytes (udev `UTIL_LINE_SIZE`). Truncate beyond; stderr discarded.

### 2. `IMPORT{program}` dispatch (rewrites `apply_import` line 593 defer)
The command value is `ruleset_subst`'d, then `argv[0]` basename decides:
1. **basename has a native port** (`ata_id`/`v4l_id`/`cdrom_id`/`fido_id` + existing `path_id`/`usb_id`/`hwdb`/`blkid`/`net_id`/`input_id`) → **always** call the native builtin via `run_builtin_bit`; never bridge. No split behavior — same helper is never native for one rule and bridged for another.
   - **Argv translation:** the audited argv universe (`ata_id --export $devnode`, `cdrom_id --lock-media $devnode`, `v4l_id $devnode`, `fido_id` no-arg) always references the current device via `$devnode`. Translation therefore ignores the argv device token and uses `ctx`'s own `devpath`/`devnode` (already computed in `apply_import`). Assumption, audited true across the installed rules: no rule passes a foreign device node to a ported helper. If a future helper's argv can't map cleanly, fall through to the bridge (step 2) rather than over-engineer.
   - `cdrom_id --lock-media`: the door-lock side effect is intentionally skipped by the port; irrelevant to property output.
2. **else → bridge:** `udev_run_capture`, parse stdout as `KEY=VALUE` lines → `uevent_set` (reuse the `import_db` property parser). Nonzero exit gates the rule (`return 0`), mirroring R4a's builtin-failure gate. Verify this abandon-on-failure semantics against udev during TDD.

`fido_id` is not a separate dispatch path — it is simply a newly-ported entry in the same native-port table as tier 1. Tiering here is "ported vs not," nothing more.

### 3. `fido_id` native builtin (new `UB_FIDO`)
`fido_id_build(const char *sysroot, const char *devpath, struct uevent *out)`: read the device's sysfs `report_descriptor`, scan for the FIDO usage page `0xF1D0`; on match set `ID_FIDO_TOKEN=1` and `ID_SECURITY_TOKEN=1`. No device-node arg needed (matches the no-arg `fido_id` invocation). Wire into `builtin_name_bit`, the native-port dispatch table, and `run_builtin_bit`.

### 4. `PROGRAM` + `RESULT`
`PROGRAM` is a **match clause** (udev always gates it on exit 0, regardless of `=` vs `==`), executed **in `rule_match`'s existing in-order loop at its token position** — all preceding match clauses have already passed by then.
- On reaching a `PROGRAM` clause: `ruleset_subst` its command string (env is consistent — earlier matches passed, ENV mutations happen only in the apply phase), run it, and: exit nonzero → `return 0` (rule fails); exit 0 → store trimmed stdout in new `ctx->result` (single cache).
- **Multiple `PROGRAM` clauses in one rule** are allowed and natural here: each overwrites `ctx->result`; a later `RESULT==` matches the latest. No defensive guarding — the in-order loop + single cache handles it.
- `RESULT==` → add to `match_dev_clause` as `rk_cmp(op, val, ctx->result)`.
- The apply phase reads the same `ctx->result` for `$result`/`%c`. `ctx->result` is reset per rule.

### 5. Substitutions
Add to `ruleset_subst` (currently deferred-verbatim): `$result` / `%c`, with optional `{N}` = the Nth **whitespace-delimited** token of `ctx->result` (1-based, udev semantics); bare `%c`/`$result` = the whole trimmed result. `$name`/`$links`/`$parent` — confirm against the real ruleset during planning; add only if actually used.

## Data flow (PROGRAM→RESULT, e.g. `mtp-probe`)
`rule_match` iterates clauses in order → ENV/ATTR matches pass → `PROGRAM="/usr/lib/udev/mtp-probe …"` subst'd + run, exit 0, stdout `"1"` → `ctx->result="1"` → `RESULT=="1"` matches → rule matches → `apply_rule` runs `SYMLINK+="libmtp-%k"`, `ENV{ID_MTP_DEVICE}="1"`.

## Error handling / gating
- `PROGRAM` nonzero exit → rule fails (match phase).
- `IMPORT{program}` bridge nonzero exit → rule gated (`return 0`), mirroring R4a builtins; verify faithfulness.
- Native-port failure inside redirect → same `rc<0` hard gate already in `run_builtin_bit`.
- `udev_run_capture` spawn/timeout failure → `-1`, treated as nonzero exit.

## Testing (TDD, per unit)
1. Quote-aware tokenizer: bare args, `'single quoted'`, embedded spaces, `sh -c '...'`.
2. `udev_run_capture`: fixture script — stdout capture, exit-status propagation, timeout kill, 16384 truncation, libdir-vs-PATH resolution.
3. `IMPORT{program}` dispatch: native redirect (`ata_id`/`v4l_id`/`cdrom_id`) uses the port not a fork; `fido_id` native; a foreign helper bridges + parses `KEY=VALUE`; bridge nonzero exit gates.
4. `fido_id_build`: FIDO-descriptor fixture sets `ID_FIDO_TOKEN`; non-FIDO HID sets nothing.
5. **PROGRAM→RESULT ordering** against real `mtp-probe` and `prefixdevname` rule shapes — the high-value test. Include a multi-`PROGRAM` single-rule case (latest wins).
6. `%c`/`$result` subst incl. indexed `%c{2}` (2nd whitespace token).
7. Live-smoke against real `/dev`: `deferred_applies` drops toward zero; no crash; `schema-udev.c` behavior unchanged where untouched.
8. Zero warnings c99/c11 × O0/O2; final opus review before close (as R2/R3/R4a).

## Non-goals
- `scsi_id` native port (bridged this round, tracked TODO).
- Un-ported `IMPORT{builtin}` names (not invoked on this box).
- Any live-box deploy or E3 flip. R5 integration + `make verify-rules-live` fidelity gate remain the flip precondition.
