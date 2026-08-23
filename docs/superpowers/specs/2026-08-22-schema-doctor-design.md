# schema-doctor — self-healing seam checker

**Date:** 2026-08-22
**Branch:** `fix/fedora-installer-rail`
**Status:** design approved, pre-implementation

## Motivation

Every machine schema-init is deployed to comes up with *different but very
similar* bugs, clustered where schema's replacements meet desktop
expectations: sessions, seats, VT switching, uaccess ACLs, power. The udev
seam is already hardened by `verify-rules-live` and the flip seatbelt. The
**logind seam** is not — tonight (eli, first metal flip) it surfaced a
frozen mouse after `Ctrl+Alt+F<n>` (orphaned session #31 from a boot-time
registration race) and "power management settings could not be loaded"
(PowerDevil's login1 probe failing).

The goal is a subsystem that **sees these quirks and fixes the safe ones
itself** — the way a human operator would — and *names* the ones whose real
fix is too invasive to apply under a live desktop, so no deployed box needs
an operator to work out live kinks after install.

## Non-goals

- Not a daemon. It runs, reports, exits.
- Not in the boot-critical path. Its failure is a logged report, never a hang.
- Not a systemd replacement. It *uses* systemd (still installed) as an oracle.
- Not the fix for the deep bugs (session race, PowerDevil). v1 detects and
  names those; their real fixes land in schema-logind / registration later.

## Architecture

One file, Python 3, stdlib only (matches `schema-logind.py`):
`scripts/schema-doctor.py` → installed `/usr/local/bin/schema-doctor`.

A registry + runner; each invariant is a `Check` subclass. Two entry points,
one engine:

- **`schema-doctor.svc`** — schema-init oneshot, `critical=0`,
  `dep=schema-logind` plus a session-up gate (waits for an active session in
  `/run/systemd/sessions/` or a timeout). Runs *late* — the opposite of the
  seatbelt — because logind invariants only exist once a session is up.
  Heal enabled. Writes the report, always exits 0.
- **`schema-doctor` CLI** — same engine on demand:
  - `--check` — detect + report only, no heal
  - `--heal` — detect + heal + re-check (default for the svc)
  - `--explain <name>` — plain-language why for one check
  - `--dry-run` — show what heal *would* do, change nothing
  - `--force <name>` — run a DEFERRED check's heal anyway (operator override)
  - `--json` — machine-readable report

**Output:** `/var/log/schema-init/doctor-report.txt` (0644), one block per
check: state, finding, oracle-expected, action taken. `--json` mirror for
future wizard integration.

## The check interface

```python
SAFE, DEFERRED = "SAFE", "DEFERRED"

@dataclass
class Finding:
    detail: str          # what's wrong, human-readable
    oracle_said: str     # what systemd would produce (for the report)
    healable: bool       # can this check heal it at all

class Check:
    name    = "vt-mediation"   # stable kebab id
    summary = "VT switching is mediated for the active session"
    grade   = SAFE             # SAFE | DEFERRED

    def detect(self)      -> Finding | None:  ...  # None = healthy
    def explain(self, f)  -> str:              ...  # plain-language why
    def snapshot(self)    -> Any:              ...  # state before heal
    def heal(self, f)     -> None:             ...  # idempotent, reversible
    def verify(self)      -> bool:             ...  # True = resolved
    def back_out(self, snap) -> None:          ...  # restore snapshot
```

**Grade is the safety valve:**

- **SAFE** — heal touches nothing a running session holds open (apply a
  missing ACL, re-arm mediation on an already-live VT). Auto-heals every
  boot and on-demand.
- **DEFERRED** — the fix is correct but too invasive live (re-registering a
  session restarts the session, yanking the compositor). Never auto-heals;
  `detect()` still runs and the report names it, pointing at
  `--force <name>` or next-boot self-correction.

**Idempotency is mandatory** (every-boot): a SAFE `heal()` run twice is a
no-op the second time. `verify()` defaults to `detect() is None` but stays
separate so a check can verify a *narrower* condition than it detects.

## The run loop (per check)

```
for check in registry (respecting doctor.conf disable=):
    f = check.detect()
    if f is None: record CLEAN; continue
    record BROKEN; log check.explain(f)
    if not heal_enabled or check.grade != SAFE or not f.healable:
        record REPORTED (deferred/detect-only); continue
    snap = check.snapshot()
    check.heal(f)
    if check.verify():
        record HEALED; continue
    # heal did not resolve — restore and report
    check.back_out(snap); record REPORTED (heal-failed)

# after every heal, re-detect all previously-CLEAN checks; if any now
# breaks, back_out the check that just healed and ABORT the run.
```

**Prime directive: never leave the box worse than it found it.** Two loss
paths — heal didn't resolve, or heal caused collateral on another check —
both end in back-out + *reported, not healed*. The whole run is wrapped:
any uncaught exception → log, exit 0. `critical=0` makes the svc's failure
invisible to PID 1.

## v1 checks (the logind seam)

### 1. `session-single` — one real session, no orphan #31 · DEFERRED
`detect()`: read `/run/systemd/sessions/*`, cross-check against who is on the
active VT. Broken = the synthesised `LEGACY_ID=31` (VTNR=0) is the active
session, or coexists with a real one. Oracle: the real session should carry
the live VTNR (1) and the autologin leader as `LEADER`. Heal DEFERRED
(re-register restarts the session). Names Bug #1 every boot without touching
the live desktop; its real fix is the registration-race fix, tracked
separately.

### 2. `card-input-acl` — active user can open GPU + input · SAFE
`detect()`: for the active session's uid, `getfacl` each `/dev/dri/card*`,
`/dev/dri/renderD*`, `/dev/input/event*`; broken if the uaccess
`user:uid:rw` ACL is missing. Oracle: exactly what systemd's uaccess rules
grant the active-seat user (read from the shipped rules, tier 2 below).
`heal()`: `setfacl -m u:uid:rw`. `snapshot`/`back_out`: prior ACL mask.
Fully reversible, idempotent, safe live — silently rescues a compositor that
came up before the ACLs were laid. The poster child.

### 3. `vt-mediation` — Ctrl+Alt+F<n> is mediated · SAFE (with dependency)
`detect()`: `VT_GETMODE` on `/dev/tty0` — is the active VT in `VT_PROCESS`
with schema-logind as handler, and does login1 report an active session on
it? Broken = `VT_AUTO` (the frozen-mouse path). `heal()`: signal
schema-logind to re-run `_setup_vt_mediation`. **Dependency:** schema-logind
must expose a private re-arm entry point (it currently arms only on session
sync); adding that hook is part of this work. Until the hook lands this check
degrades to detect-only.

### 4. `login1-power` — PowerDevil's probes are answered · DEFERRED (detect-only v1)
`detect()`: make the exact dbus calls PowerDevil issues on load —
`CanSuspend`/`CanHibernate`/`CanPowerOff`/`CanReboot`, `ListInhibitors`, and
the Session power properties — against schema-logind's `login1`. Report which
call errors or is missing (Bug #3's fingerprint). Oracle: systemd-logind's
introspection of the same interface. Heal DEFERRED — a broken property is a
schema-logind fix; a down daemon is reported with a pointer at `schema-ctl`.

Two safe self-heals (ACLs now, VT-mediation once the hook lands), two honest
detect-and-name for the deep bugs.

## Safety model & the systemd oracle

**Back-out is per-check and total** (see run loop). **Global guard:** whole
run wrapped, uncaught exception → log + exit 0; svc is `critical=0`.

**Oracle — systemd is still installed; use it as ground truth, three tiers:**

1. **Live read** (cheap, read-only): `getfacl` reflects the real ACL,
   `VT_GETMODE` the real VT state, a `login1` dbus call answers or doesn't.
   No systemd process needed — these read the kernel/fs state systemd would
   have configured.
2. **Systemd's shipped data as reference:** the uaccess expectation comes
   from systemd's rules (`/usr/lib/udev/rules.d/73-seat-late.rules` et al.),
   read directly, so "what should be granted" tracks systemd, not a drifting
   hardcoded list.
3. **Cross-check a live systemd tool** when one exists and is safe read-only
   (e.g. `loginctl show-session`) — diagnostic detail in the report only,
   never a heal path (on a flipped box schema-logind owns the interface).

Mostly tiers 1+2; tier 3 is a diagnostic luxury.

**Config:** `/etc/schema-init/doctor.conf`, one file read, no parser
ceremony:
```
heal=no              # global: detect-only
disable=check1,check2
```

## Testing

**Per-check unit tests** (`tests/doctor/`), no hardware — each check driven
against a fabricated fixture (temp dir for device nodes, fake
`/run/systemd/sessions/` tree, stubbed dbus probe). Assert the discipline
mechanically:
- `detect` flags a seeded-broken fixture, passes a clean one;
- `heal → verify` closes the finding on a SAFE check;
- `back_out` restores the exact snapshot when `verify()` is forced to fail;
- **collateral abort** — seed a second check to break during the first's
  heal, assert the run aborts and both are reported-not-healed;
- **idempotency** — heal twice, second run a no-op.

**Real integration test** for `card-input-acl` under the existing vmtest rig
(reuse `tests/livetest/` plumbing, mirroring `udev-boot-vmtest.sh`): boot
schema-init PID 1, strip the uaccess ACL off `/dev/dri/card0`, run
`schema-doctor --heal`, assert the ACL returns and the snapshot/back-out path
is exercised.

**On-metal acceptance (deferred to eli reinstall):** `schema-doctor --check`
on the real flipped box must name the orphan-#31 (check 1) and the PowerDevil
probe failure (check 4) — proving the detect-and-name path catches tonight's
actual bugs on real hardware. Acceptance gate for the whole subsystem: *does
the doctor see what I see?*

## Dependencies & follow-ups

- **schema-logind re-arm hook** (for `vt-mediation` heal) — small private
  entry point; part of this work.
- **Registration-race fix** (real fix for `session-single`) — separate,
  tracked in the schema-logind session work.
- **PowerDevil property gap** (real fix for `login1-power`) — separate,
  once check 4 fingerprints exactly which call fails.
- **Wizard integration** (`--json` report surfaced in the first-boot
  wizard) — future, out of v1 scope.
