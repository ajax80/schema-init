# schema-doctor — standing supervisor (periodic promotion)

**Date:** 2026-08-27
**Branch:** `feat/schema-doctor-standing`
**Status:** design approved, pre-implementation
**Builds on:** [`2026-08-22-schema-doctor-design.md`](2026-08-22-schema-doctor-design.md) (the base engine, checks, and safety model — read it first)

## Motivation

The base schema-doctor runs **once at boot**: after schema-logind and a
session gate, it heals the SAFE checks, names the DEFERRED ones, and exits.
That catches the seam bugs that surface *at login*. But resilience is about
the machine while it's *running*, and a whole class of the same-family
quirks appears **after** boot and goes unseen until the operator notices:

- a helper daemon dies mid-session (PowerDevil crashes at hour 3);
- an ACL a running session depends on gets clobbered by a misbehaving app;
- a ksycoca rebuild-loop starts hours into a session;
- a session goes orphaned after a `Ctrl+Alt+F<n>` VT switch.

The boot-once doctor never looks again. **Promotion** turns it into a
standing supervisor that re-checks on a cadence and keeps a visible
health signal — *without* becoming a daemon (a long-lived process is itself
a new fragility to supervise). schema-init already has native repeating
interval timers (`on_boot_sec` + `on_active_sec`, re-armed by PID 1 on every
fire — see `init.c:392`), so "standing" is achieved by re-running the
existing one-shot engine on a timer.

## What is UNCHANGED

This is a **delta**, not a rewrite. Untouched from the base spec:

- the engine and its per-check run loop;
- all existing checks (6 at time of writing) and the `Check` interface;
- the **SAFE / DEFERRED** grading;
- the back-out discipline (per-check snapshot/restore, collateral-abort,
  whole-run exception guard → exit 0);
- the **boot run** — `schema-doctor.svc` (`--heal --wait 30`) stays exactly
  as-is: aggressive first-heal on a fresh box.

**Key safety fact carried over:** `SAFE` is *defined* as "heal touches
nothing a running session holds open," and the boot run already heals with a
live session present. So **SAFE already means safe-to-heal-on-a-live-desktop**
— periodic healing of SAFE checks introduces no *new* per-action risk. The
only genuinely new risk is *flapping*, addressed below.

## Non-goals (v1)

- **Not a daemon.** Timer-driven re-runs of the one-shot engine; no
  long-lived process.
- **No board LED integration.** The schema-board glow is deferred (see
  follow-ups); v1's visibility is the status file + desktop notifications.
- **No event-reactivity.** Cadence is a fixed interval, not
  break-triggered.
- **No wizard integration.** `--json` status is emitted for a future
  consumer, not wired to one here.
- **No config knobs for flap thresholds.** Hardcoded 3 heals / 30 min in
  v1; promote to `doctor.conf` only if a real box needs it.

## Architecture delta

### Execution model — two runs, one engine

| Run | Trigger | Mode flag | Heal policy | Notify | Status file |
|-----|---------|-----------|-------------|--------|-------------|
| **boot** | `schema-doctor.svc` (existing) | `--heal` | seed state, heal all SAFE, **no backoff** | no | written |
| **periodic** | `schema-doctor-periodic.svc` (new) | `--heal --periodic` | **flap-aware** (backoff + escalate) | on transition | written |

Boot-time breakage is expected and actively being resolved during login, so
the boot run stays aggressive and silent (no notifications popping during
startup). The `--periodic` flag is what switches on flap-awareness and
notify-on-transition.

New CLI verb: **`--status`** — print the last status file (human or, with
`--json`, machine-readable). Existing verbs (`--check`, `--explain`,
`--dry-run`, `--force`, `--json`) unchanged.

### The periodic timer service

New `services/schema-doctor-periodic.svc`:

```
name=schema-doctor-periodic
exec=/usr/local/bin/schema-doctor
args=--heal
args=--periodic
on_boot_sec=600      # first periodic run 10 min after boot
on_active_sec=600    # then every 10 min, re-armed natively by PID 1
needs_root=1
critical=0
```

(One `args=` per token — schema-init splits nothing on whitespace.) The boot
run covers t=0; the periodic timer covers everything after.

### Flap backoff + escalate

Because the doctor is a one-shot re-run (not a daemon), heal history lives in
a **state file**: `/var/lib/schema-init/doctor-state` (JSON, root-owned
`0644`). Per check it records the epoch seconds of recent *successful* heals.

On a `--periodic` run, for a check whose `detect()` returns BROKEN and whose
grade is SAFE:

```
prune this check's heal timestamps older than FLAP_WINDOW (1800s)
if len(heals) >= FLAP_THRESHOLD (3):
    state = CHRONIC            # do NOT heal — stop band-aiding
else:
    heal via snapshot/verify/back-out (base discipline)
    on verify() success: append now() to heals
when detect() == None (CLEAN) for a check that was BROKEN/CHRONIC:
    clear its heals + chronic mark   # recovered
```

- **DEFERRED** checks: unchanged — never heal, always report.
- A CHRONIC mark means "this keeps re-breaking; the real fix is elsewhere" —
  it stops the churn *and* makes the underlying problem visible instead of
  hiding it behind endless silent patches. It clears automatically the first
  time the check reads clean.
- `FLAP_THRESHOLD = 3`, `FLAP_WINDOW = 1800s` — module constants in v1.

**Reboot handling:** the state file stores the current `boot_id`
(`/proc/sys/kernel/random/boot_id`). On any run, if the stored `boot_id`
differs from the live one, heal history is reset (a fresh boot is a fresh
start; the boot run already re-seeds).

### Health signal — the status file

`/run/schema-init/doctor-status` (text, `0644`) written on **every** run,
with a `--json` twin. Per-check colour and an overall rollup (worst wins):

| Per-check condition | Colour |
|---|---|
| `detect()` clean | GREEN |
| broke and **healed** this run | AMBER |
| DEFERRED (named, not healed) | AMBER *(RED only if the check sets `critical`; none do in v1)* |
| SAFE heal **failed** (backed out) | RED |
| CHRONIC | RED |

Overall = worst colour present (`RED > AMBER > GREEN`). `schema-doctor
--status` prints it; headless-safe and always current.

### notify-send on transition

Only on `--periodic` runs, and **edge-triggered** by comparing to the prior
status stored in the state file (no repeat spam):

1. a check **newly** becomes CHRONIC or **newly** heal-fails (a RED-class
   event) → notify naming that check and its detail;
2. overall status **returns to GREEN** after having been AMBER/RED →
   notify "all clear" (recovery).

Successful auto-heals (AMBER) are **silent** — quiet self-healing is the
point; only escalations and recoveries interrupt the user.

**Targeting (root → user session):** the doctor already locates the active
session's uid for its checks. To notify, it **harvests
`DBUS_SESSION_BUS_ADDRESS` and the display vars from the session leader's
`/proc/<pid>/environ`**, then runs `notify-send` as that user with that env.
This is mandatory here, not incidental: on this platform the session bus is
`unix:path=/tmp/dbus-XXXX`, **not** `/run/user/1000/bus` (there is no
`systemd --user`) — so the address must be *read*, never assumed. Best-effort
throughout: no active session, no `notify-send` binary, or any notify error →
logged and skipped; the status file is still written and the run still exits
0. `notify=no` in `doctor.conf` disables notifications entirely.

### Config additions

`/etc/schema-init/doctor.conf` gains one key:

```
notify=yes           # yes|no — desktop notifications on transition (default yes)
```

Existing `heal=` and `disable=` unchanged. Flap thresholds are not
configurable in v1.

## Safety model

The base prime directive stands: **never leave the box worse than it found
it** (per-check back-out, collateral-abort, whole-run exception guard, exit 0,
`critical=0`). Promotion adds three guards, all fail-safe:

- **Flap guard** prevents a chronic breaker from being patched on every tick
  (churn) and from being hidden (it escalates to a visible CHRONIC/RED).
- **Notify is best-effort** — it can never fail a run; any error is logged
  and swallowed.
- **State-file corruption is tolerated** — an unparseable or missing state
  file is treated as empty (no history, nothing chronic), so a bad file
  degrades to "heal as if first time," never to a crash or a wrong skip.

## Data formats

**State file** `/var/lib/schema-init/doctor-state` (JSON):

```json
{
  "version": 1,
  "boot_id": "…",
  "last_overall": "GREEN|AMBER|RED",
  "last_run": 1724800000,
  "checks": {
    "<check-name>": {
      "heals": [1724799400, 1724800000],
      "chronic": false,
      "last_state": "GREEN|AMBER|RED"
    }
  }
}
```

**Status file** `/run/schema-init/doctor-status` (text; `--json` mirrors it):

```
schema-doctor: RED   2026-08-27 23:20:11   mode=periodic
  GREEN  card-input-acl      clean
  RED    powerdevil-running  CHRONIC (3 heals/30m, last 23:14)
  AMBER  session-single      DEFERRED (orphan #31 — --force or next boot)
```

## Testing

New unit tests (stdlib + fabricated fixtures, no hardware), alongside the
untouched per-check tests:

- **`test_doctor_flap.py`** — window pruning drops old heals; the 3rd heal in
  30 min escalates to CHRONIC and *skips* healing; a subsequent CLEAN detect
  clears the chronic mark and history; a differing `boot_id` resets history.
  Clock is injected.
- **`test_doctor_status.py`** — the GREEN/AMBER/RED rollup is computed
  correctly from fabricated per-check results (worst-wins), and the status
  file + `--json` are written and re-read by `--status`.
- **`test_doctor_notify.py`** — transitions fire notify (new-chronic,
  heal-fail, recovery-to-GREEN) and non-transitions do not; targeting locates
  the active session and constructs the correct `notify-send` argv **with the
  bus address read from a fabricated `/proc/<pid>/environ`**; the actual send
  is stubbed; a missing session / missing binary is a silent skip.
- **boot-vs-periodic mode** — `--heal` (boot) heals ignoring flap state and
  never notifies; `--heal --periodic` respects flap state and notifies.

The interval timer itself relies on schema-init's already-tested timer engine
(`SVC_TIMER` + `on_active_sec` re-arm); one smoke assertion that the new
`.svc` registers and arms is sufficient.

## Dependencies & follow-ups

- **schema-board LED integration** — surface doctor health on the recovery
  console glow. Deferred; fits the "visible when the desktop is wedged"
  ethos.
- **Flap thresholds in `doctor.conf`** — only if a real deployment needs
  tuning.
- **Wizard integration** — the `--json` status is emitted now for a future
  first-boot/wizard consumer; wiring it is out of v1 scope.
- The deep bugs the DEFERRED checks name (session-registration race,
  PowerDevil `login1` property) still have their real fixes tracked
  separately in the schema-logind work — promotion makes them *visible on a
  cadence*, it does not fix them.
