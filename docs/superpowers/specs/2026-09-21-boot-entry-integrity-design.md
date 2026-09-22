# boot-entry-integrity — schema-doctor check for BLS entry corruption

**Date:** 2026-09-21
**Branch:** TBD (created at implementation time)
**Status:** design approved, pre-implementation

## Motivation

Optiplex hung on boot tonight (grey screen, unresponsive to VT switching)
after a routine `dnf update`. Root cause: the update triggered a BLS resync
(exact tool not pinned down — flagged as a standing follow-up in the
2026-09-16 `root=UUID` incident notes) that rewrote **all four**
`schema-*.conf` boot entries' `options` line verbatim from
`/etc/kernel/cmdline`, stripping `init=/sbin/schema-init` from every one, and
reset `grubenv`'s `saved_entry` to the update's newly-installed stock kernel
entry. The box booted raw `systemd` as PID 1 on a filesystem it was never
meant to run under, which crashed catastrophically on SELinux relabeling.
Diagnosed and hand-repaired live over SSH; full recovery took about 25
minutes of an evening. The hand-repair initially restored only `init=`; a
follow-up review caught that the same verbatim-overwrite also stripped
Optiplex's `modprobe.blacklist=radeon` (from
`/etc/schema-init/kernel-cmdline.d/10-radeon.conf`) from every entry —
confirmed live (`/proc/cmdline` missing the arg, `radeon.ko` loaded
alongside `amdgpu.ko`, saved only by `amdgpu` winning the probe race) and
fixed on disk the same night.

This is the second time a BLS resync has clobbered a schema entry's custom
`options` (the first, 2026-09-16, hit only `root=` on one entry). Tonight's
was worse: every schema entry, plus the saved default. The goal is to close
this class of failure the way schema-doctor already closes logind-seam
failures: **detect it, heal it automatically within minutes, and give
Jonathan a one-click way to confirm a box is safe to reboot right after an
update** — instead of finding out from a hung screen.

## Non-goals

- Not a fix for *what* rewrites the entries — that tool is still unidentified
  and this check is deliberately agnostic to it: it repairs the symptom
  reliably regardless of cause.
- Not fleet-wide. Scoped to Optiplex's boot-persistence model (see below).
  greybox/eli (the `/etc/kernel/cmdline` in-place model) and any future
  installer-provisioned box are explicitly out of scope for this pass —
  flagged as follow-ups, not silently ignored.
- Not a change to schema-doctor's engine, grading, or run loop. This is one
  new `Check` subclass plus a small, backward-compatible extension to an
  existing marker file's semantics.

## Scope: which boxes this protects

The fleet has two boot-persistence models (established in the original
kernel-install schema-entry hook work, PR #107):

- **Separate-entry** (Optiplex only, today): stock BLS entries stay pristine;
  `init=` lives only in a parallel `schema-<version>.conf`. This is the model
  with the regression risk this check targets.
- **In-place `/etc/kernel/cmdline`** (greybox, eli): the schema `init=` lives
  directly in the cmdline `kernel-install` bakes into every new entry
  automatically — no separate schema-vs-stock split to lose sync, no bug here
  by design.

`detect()` globs `/boot/loader/entries/schema-*.conf`; on greybox/eli this
glob is empty and the check reports clean unconditionally. Safe to deploy
fleet-wide as-is, but it only does real work on Optiplex today.

**Follow-ups (explicitly out of scope here):**
1. The installer-ISO rail doesn't wire in the kernel-install hook at all yet
   (pre-existing open item, unrelated to tonight) — future dad-proof-installed
   boxes won't get this protection until that's done.
2. If the in-place `/etc/kernel/cmdline` model ever needs an equivalent guard
   (something stripping `init=` out of `/etc/kernel/cmdline` itself), that's a
   different failure shape and a different check.

## Architecture

One new `Check` subclass in `scripts/schema-doctor.py`, registered in
`REGISTRY` alongside the existing 13. No new files, no new services — it
rides the existing `schema-doctor.svc` (boot) and `schema-doctor-periodic.svc`
(every 10 min) entry points for free.

```python
class BootEntryIntegrity(Check):
    name    = "boot-entry-integrity"
    summary = "schema BLS entries keep init=schema-init; saved_entry stays on one"
    grade   = SAFE
```

### `detect()`

1. `entries = sorted(glob.glob(f"{ROOT}/boot/loader/entries/schema-*.conf"))`.
   Empty → clean (greybox/eli, or any box not yet migrated to schema-init).
2. Read `/etc/schema-init/kernel-cmdline.d/*.conf` the same way the
   kernel-install hook's `extra_args()` does (strip `#` comments, join,
   collapse whitespace) — this is the set of host-specific tokens (e.g.
   `modprobe.blacklist=radeon` on Optiplex) every schema entry is expected to
   carry.
3. For each entry, read its `options` line; broken if it's missing
   `init=\S*schema-init`, **or** missing any token from step 2. (Tonight's
   actual corruption stripped both in the same verbatim-overwrite-from-
   `/etc/kernel/cmdline` event — `/etc/kernel/cmdline` never carries the
   `kernel-cmdline.d` extras, only the hook's `add` path injects them, so a
   resync wipes both together. Checked independently so either one missing on
   its own is still caught.)
4. Read `saved_entry` via `grub2-editenv - list` (parsed the same way the
   kernel-install hook's tests already stub it — see Testing). Broken (third
   part of the same finding) if `<saved_entry>.conf`'s basename doesn't start
   with `schema-`, **or** that basename starts with `schema-` but the file
   doesn't actually exist under `entries` (a removed kernel left `saved_entry`
   dangling — GRUB falls back silently, `detect()` must not read the name
   alone as proof the entry is real).
5. If any part is broken, return one `Finding` carrying: the list of broken
   entry paths (each with its specific missing tokens), and whether
   `saved_entry` needs repointing. Otherwise `None`.

### `heal()`

- **Per broken entry:** strip any existing `init=\S*` token from the
  `options` line first (regex substitution) — an entry with a stale or
  foreign `init=` (e.g. `init=/usr/lib/systemd/systemd`) must not end up with
  two `init=` tokens; blind-append relies on "last one wins" kernel cmdline
  parsing instead of being correct. Resolve the real `schema-init` binary
  path via `SCHEMA_INIT_BIN` env override → `shutil.which("schema-init")`
  (same resolution order as the kernel-install hook — schema-doctor only
  ever runs on a box that's currently booted under a working schema-init, so
  `which` reliably finds the host's real path, e.g. `/sbin/schema-init` on
  Optiplex). Append `init=<path>`, then append any `kernel-cmdline.d` tokens
  missing from the line (same set `detect()` computed). Atomic write (temp
  file + `os.replace`, matching the hook's own style).
- **`saved_entry`, if broken:** target = `/etc/schema-init/boot-default`'s
  content (normalized — see below), if non-empty *and* `<content>.conf`
  exists; else the newest `schema-*.conf` by kernel version (`sort -V`
  shelled out, reusing the hook's own version-sort rather than reimplementing
  it in Python). Then `grub2-editenv - set saved_entry=<target>`.

### `snapshot()` / `back_out()`

Snapshot = `{path: original_options_line}` plus the original `saved_entry`
value. `back_out` rewrites each entry's options line back and resets
`saved_entry`, mirroring `card-input-acl`'s ACL snapshot/restore pattern.

### `verify()`

Default (`detect() is None`) — no narrower condition needed here.

## `/etc/schema-init/boot-default` — extended, backward compatible

**Today:** presence of the file (any content, including empty) arms the
kernel-install hook's "advance `saved_entry` to the newest schema entry on
every kernel add" behavior.

**New:** the file's *content* becomes an optional pin (e.g.
`schema-ssd-7.1.12-200.fc44.x86_64`), read by both the hook and this check.
Both readers normalize it the same way before use: strip surrounding
whitespace/newlines, and strip a trailing `.conf` if someone writes
`schema-foo.conf` instead of `schema-foo` (shell: `tr -d '\r\n'` then a
`${pin%.conf}`-style strip; Python: `.strip()` then
`removesuffix(".conf")`). An empty-after-normalization value is treated as
"no pin," same as an empty file today.

The two consumers' behavior is deliberately different, because they answer
different questions:

- **This check always protects against "reverted to stock"** — pin or no
  pin, marker present or absent — because that regression is never wanted.
  Heal target = pin if set and resolvable, else newest.
- **The hook's existing opt-in auto-advance is now pin-aware, and must
  actively enforce the pin, not just decline to move it.** `kernel-install`
  runs Fedora's own `90-loaderentry.install` before this hook (`99-`
  ordering) — and tonight's actual `saved_entry` ended up on the brand-new
  stock `7.2.6` entry, confirming something in that earlier stage sets
  `saved_entry` to the new kernel during the same transaction. A pin-branch
  that merely `return 0`s instead of writing the pin back would leave that
  stock value in place. `set_default_to()` becomes:

  ```sh
  set_default_to() {
      # $1 = kernel version being added/removed (the "natural" candidate)
      [ -f "$DEFAULT_MARKER" ] || return 0
      pin="$(cat "$DEFAULT_MARKER" 2>/dev/null | tr -d '\r\n ')"
      pin="${pin%.conf}"
      command -v grub2-editenv >/dev/null 2>&1 || return 0
      if [ -n "$pin" ] && [ -f "$ENTRIES/$pin.conf" ]; then
          grub2-editenv - set "saved_entry=$pin" 2>/dev/null || true
          return 0
      fi
      grub2-editenv - set "saved_entry=schema-$1" 2>/dev/null || true
  }
  ```

  No pin (today's behavior, unchanged) → advances on every add. Pin set and
  resolvable → **actively enforced** every time this runs, even though
  something upstream already moved `saved_entry` to the new stock entry in
  the same transaction (this is what would have kept Optiplex on
  `schema-ssd-7.1.12` tonight even after `7.2.6` landed). Pin set but
  now-broken (its kernel was removed) → falls back to advancing, so
  `saved_entry` never dangles.

**Deploy note:** set `/etc/schema-init/boot-default`'s content to
`schema-ssd-7.1.12-200.fc44.x86_64` on Optiplex as part of rollout, so the
pin takes effect immediately rather than only protecting future entries.

## Desktop shortcut

A permanent `~/Desktop/schema-boot-check.desktop` icon (unlike the one-shot
flip-wizard icon, this one is used after every future update indefinitely, so
it's installed once and never removes itself). Clicking it runs a small
script following `firstboot-flip-wizard.sh`'s existing pattern:

- **Privilege:** a single new passwordless-sudo entry, scoped to exactly
  `/usr/local/bin/schema-doctor --heal --json` (no wildcard args) — the whole
  privileged surface, same shape as the flip wizard's `HELPER`. Confirmed live
  on Optiplex tonight (`/usr/local/bin/schema-doctor`, root-owned, matches the
  original deploy) — this isn't a guess, but at implementation time re-verify
  the path on the actual target before writing the sudoers rule rather than
  assuming it's permanent; nothing packages schema-doctor as an RPM today
  (it's a plain file drop, not `%{_libexecdir}`), but if that ever changes
  this rule needs updating in lockstep.
- **No new CLI flag.** The script runs the existing full-suite `--heal
  --json` (already fast, already running unattended every 10 minutes) and
  filters the JSON array for `"name": "boot-entry-integrity"` — reuses a
  proven code path instead of adding an `--only NAME` selector to the shared
  engine for one caller.
- **One `yad` dialog**, three states:
  - clean → green, "Boot entries OK. Safe to reboot."
  - healed → amber, "Fixed N entries + saved_entry. Safe to reboot now." with
    a **Details** button showing the JSON detail/action fields.
  - couldn't heal (shouldn't happen for a SAFE-grade check, but the dialog
    must handle the report either way) → red, "Could not fix — do not
    reboot," full detail shown by default.

No new GUI framework — plain shell + `yad`, matching the existing wizard's
style exactly, not the QML wizard app (overkill for a single pass/fail
glance-and-close interaction).

## Testing

**Unit** (`tests/test_doctor_boot_entry_integrity.py`, mirroring
`tests/test_doctor_acl.py`'s shape): `DOCTOR_ROOT`-injected temp
`boot/loader/entries/` tree; a stub `grub2-editenv` script placed first on
`PATH` (same technique `tests/test_kernel_install_hook.py` already uses)
records/replays `saved_entry` without a real bootloader. Cases:
- clean fixture (all entries have `init=`, `saved_entry` points at a schema
  entry) → `detect()` returns `None`.
- one entry missing `init=` → detected, healed, `verify()` clean, idempotent
  on a second run.
- all entries missing `init=` (tonight's actual shape) → all healed in one
  pass.
- an entry has a stale/foreign `init=` (e.g. `init=/usr/lib/systemd/systemd`)
  → healed to exactly one `init=` token, not two.
- an entry is missing only a `kernel-cmdline.d` token (`init=` intact) →
  still detected and healed — the two checks are independent.
- entries missing both `init=` and the `kernel-cmdline.d` tokens (tonight's
  actual shape, confirmed live on Optiplex) → both restored in one pass.
- `saved_entry` pointed at a stock entry, no pin set → healed to newest
  `schema-*.conf` by version.
- `saved_entry` pointed at a stock entry, pin set and resolvable → healed to
  the pinned entry, not newest.
- pin set but its `.conf` no longer exists → falls back to newest (doesn't
  crash, doesn't dangle).
- `saved_entry` names a `schema-*.conf` that doesn't exist on disk (dangling,
  e.g. its kernel was removed) → detected as broken, not read as clean just
  because the name matches the prefix.
- marker content has trailing whitespace/newline or a `.conf` suffix →
  normalized before use, same heal target as the clean form.
- collateral / back-out path exercised the same way `card-input-acl`'s is.

**Hook** (`tests/test_kernel_install_hook.py`, new cases): pin resolvable →
`add`/`remove` *actively set* `saved_entry` to the pin, not leave it at
whatever `90-loaderentry.install` already set (this is the real bug tonight's
review caught — a prior no-op draft of this logic would pass a naive "pin
untouched" test while still failing to override the stock value); no pin →
today's advance behavior unchanged; pin broken → falls back to advancing;
marker content with whitespace/`.conf` suffix → normalized the same as the
Python side.

**Boot test:** `schema-vmtest` before this ever touches Optiplex live, given
tonight — seed a VM's schema entries with the exact corruption shape seen
tonight (all `init=` stripped, `saved_entry` on a stock entry) and confirm
the periodic svc heals it and the box comes up under `schema-init` on the
next simulated boot.

**On-Optiplex acceptance:** after deploy (`kill -HUP 1`, never `restart`),
manually strip `init=` from one entry and repoint `saved_entry` at a stock
entry by hand, then either wait for the 10-minute periodic tick or click the
new desktop shortcut, and confirm both heal and the box still boots correctly
on a real reboot afterward.

## Dependencies & follow-ups

- Installer-ISO rail wiring for the kernel-install hook (pre-existing open
  item, not created by this work).
- An equivalent guard for the in-place `/etc/kernel/cmdline` model
  (greybox/eli), if that model ever shows this failure shape.
- Pinning down which tool actually performs the BLS resync (still open from
  2026-09-16) — orthogonal to this work, which is deliberately cause-agnostic.
