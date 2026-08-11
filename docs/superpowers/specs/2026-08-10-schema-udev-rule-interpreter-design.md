# schema-udev rule interpreter (sub-project B-phase-2) — design

## Context

The udevd-retirement endgame decomposes into five sub-projects, each built
alongside a still-running udevd and verified by parity before udevd is retired
last: **A** builtin wiring (PR #88), **B** the rules engine, **C** persistent
`/dev/disk/by-*` symlinks, **D** uaccess ACL manager, **E** the cutover.

Sub-project **B** (spec `2026-08-06-schema-udev-rules-engine-design.md`) was
scoped to **property completeness** only — `IMPORT{parent}` propagation and
composite hwdb keys, reproducing udevd's `E:` output. It delivered exactly that
and no more. `udev_rules.h` is therefore not a rule-file interpreter: it is a
hardcoded C reimplementation of udev's property-derivation builtins.

The **E cutover** (spec `2026-08-10-schema-udev-cutover-design.md`) deferred two
capabilities to E3: real `/run/udev/data` record writes and group-2 rebroadcast.
Building toward E3 surfaced the gap this spec closes:

**Real udev records carry `G:`/`Q:` tags and `S:` symlinks that are produced by
executing the installed `.rules` files (`TAG+=`, `SYMLINK+=`). schema-udev has
no rule interpreter, so it cannot reproduce them. Retiring udevd without them
would silently break, on this box:**

- **`security-device`** tag → udev ACLs for the pico-fido FIDO2 hardware keys.
- **`snap_*`** tags (≈16 of them) → device-cgroup access for confined snaps
  (amberol, the winrar/wine stack, open-webui).
- **`power-switch`** → logind seeing the power button / lid.
- **`master-of-seat` / `switcheroo-discrete-gpu`** → seat mastering, GPU switch.

Empirical scale (blakbox, 2026-08-10): **168 installed rule files**, using the
full rule language — 3554 `ENV{}`, 3490 `ATTRS{}`, 1103 `SUBSYSTEM`, 691
`KERNEL`, 579 `TAG`, 560 `GOTO`, 480 `RUN`, 221 `LABEL`, 170 `IMPORT{}`, plus
`SYMLINK`, `TEST`, `PROGRAM`, `OPTIONS`, `MODE/GROUP/OWNER`, substitutions.
Across 471 live records the tag universe is: `systemd` (76), `seat` (53),
`power-switch` (18), `uaccess` (12), plus `master-of-seat`, `security-device`,
`switcheroo-discrete-gpu`, and the `snap_*` family.

Reproducing all tags/symlinks faithfully therefore requires **reimplementing
udev's rule-interpreter core**, not a shortcut subset. This is B-phase-2, the
missing precondition for E.

## Goals

- A native rule interpreter that loads the installed `.rules` set and, per
  device, computes the **same tags, symlinks, and node permissions** udevd would.
- Full-record fidelity: `S:`/`E:`/`G:`/`Q:`/`V:` (and `L:` where present) match
  real `/run/udev/data` across all in-scope devices.
- Built entirely in **shadow**: udevd stays authoritative for R1–R5; schema-udev
  computes and is compared only. No live write until the fidelity gate is green.

## Non-goals

- The E3 flip itself (retiring udevd) — separate, gated on this being green.
- Reimplementing every external helper natively up front (see Helper strategy).
- `hwdb` changes, `by-path` beyond what the persistent-storage rules emit.

## Decisions settled during brainstorming

1. **Helper strategy — native-only with shell fallback.** `IMPORT{builtin}=`
   routes to the existing C builtins (`ata_id`/`usb_id`/`cdrom_id`/`path_id`/
   `net_id`/`v4l_id`/`input_id`/`blkid_*`). `IMPORT{program}=` and `RUN+=` with
   no native equivalent shell out to the real `/usr/lib/udev/<helper>` as a
   **temporary bridge**; each bridge is a tracked "reclaim me" TODO.
2. **Sequencing — shadow-until-R5.** udevd in charge throughout; schema-udev
   compared via the fidelity gate; flip + retirement only after R5 is green
   across all 471 devices.
3. **File layout.** New `udev_ruleset.h` (parser + matcher + executor) —
   distinct from `udev_rules.h`, whose property-inheritance logic becomes
   reachable as `IMPORT{builtin}`/`IMPORT{parent}` handlers from the interpreter.

## Architecture — five slices

Each slice is its own spec → plan → TDD build, verified in shadow.

### R1 — Parser / loader
Read `/usr/lib/udev/rules.d`, `/run/udev/rules.d`, `/etc/udev/rules.d` in udev
precedence (merge by filename; later dir wins per name; files applied in
lexical name order). Tokenize each rule line into an ordered list of clauses:
`(key, subkey?, op, value)` where `op ∈ {== != = += -= :=}`. Preserve `LABEL:`
targets and `GOTO` destinations. Output an in-memory ruleset. Pure and
unit-testable byte-for-byte against real rule files.

### R2 — Matcher
Evaluate match clauses against a device: `ACTION`, `SUBSYSTEM`, `KERNEL`,
`ENV{}`, `ATTR{}`, `DRIVER`, `TAG` on the device itself; and the parent-walking
`SUBSYSTEMS`, `KERNELS`, `ATTRS{}`, `DRIVERS` which climb the sysfs ancestry.
Glob matching (`*`, `?`, `[...]`, alternation `a|b`) and string substitutions
(`$attr{}`, `$env{}`, `%k`, `%n`, `%b`, `$devpath`, …). Verified against
`udevadm test` per device.

### R3 — Executor
Apply assignment clauses in order with `GOTO`/`LABEL` control flow:
`ENV{}=/+=`, `TAG+=/-=`, `SYMLINK+=/=`, `OPTIONS`, `MODE`/`GROUP`/`OWNER`,
`NAME`. Accumulate the device's tag set and symlink set. `struct uevent` (or a
sibling struct) gains a tag list and symlink list. Verified on rule fixtures.

### R4 — IMPORT / RUN / TEST / PROGRAM
`IMPORT{builtin}` → native builtins; `IMPORT{parent}` → existing inheritance;
`IMPORT{db}` → read shadow record; `IMPORT{file}`/`IMPORT{cmdline}` native;
`IMPORT{program}` and `RUN+=` → native-or-shell-bridge per decision 1;
`TEST`/`PROGRAM` conditionals. Each shell bridge logged as a reclaim TODO.

### R5 — Integrate + fidelity gate
Feed computed tags → `udev_db_record_build_full` (already built, TDD'd),
symlinks → `S:` and the `/dev/disk/by-*` writer (sub-project C). Add
`make verify-rules-live`: for every device, compare schema-udev's full computed
record against real `/run/udev/data` / `udevadm info`. Green across all 471
in-scope devices is the precondition for the E3 flip.

## Fidelity definition

Per record, compared in shadow against real udevd output:

| Field | Comparison |
|-------|-----------|
| `E:` properties | exact set + values (already parity-green) |
| `S:` symlinks | exact set (by-id/by-path become interpreter output, not deferred) |
| `G:`/`Q:` tags | **exact set** — the point of this sub-project |
| `L:` link priority | exact when present |
| `V:` version | exact (`1`) |
| `I:` usec_initialized | **presence + `>0` only** — it is a first-seen clock value and cannot byte-match udevd; not value-compared |

"In-scope" excludes device classes explicitly deferred elsewhere and any device
whose only tags come from a not-yet-bridged helper (tracked, not silently
dropped — the gate reports them).

## Testing strategy

- R1–R4: pure unit tests (TDD, `make test`) against real rule-file fixtures and
  live sysfs, in the existing `-Wall -Wextra` harness.
- R5: live shadow parity (`make verify-rules-live`) + `schema-vmtest` boot.
- No live `/dev` or `/run/udev/data` write introduced by this sub-project.

## Risks

- **Scale/scope creep.** Full language across 168 files is large; the shadow
  gate bounds it — we know exactly which devices/tags still mismatch at any point.
- **Shell bridges lingering.** Each is a tracked reclaim TODO; acceptable as a
  bridge, not an endpoint.
- **`ATTRS{}`/`SUBSYSTEMS` tree-walk cost** at coldplug (471 devices × parent
  chains). Measure in R2; cache parent lookups if needed.

## Endgame position

R1→R5 green closes the last gap before **E3**: the flip may then retire udevd,
with GreyBox/Crystal on standby for the flip reboot. Until then, nothing about
the live box changes.
