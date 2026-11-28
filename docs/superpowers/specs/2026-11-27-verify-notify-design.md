# Desktop restart stays instant; verify problems become notifications

## Why

Charles's review (P1 item 3) found the desktop menu's Restart/Shut Down goes
through schema-logind and skips the verify gate that `sudo reboot` in a terminal
gets. Jonathan decided (2026-11-27) **not** to gate the desktop button:

- By the time Plasma calls login1 `Reboot`/`PowerOff`, ksmserver has already
  closed every app and ended the session. A refusal then protects nothing and
  strands the user at a login screen with no explanation.
- verify can take up to 105 s; `CanReboot` is polled constantly, so it can't run
  there either.
- "It's kind of nice to hit restart and actually get a restart."

What he does want: *if restarting would break something important, tell me.* The
time to tell him is when verify finds the problem (usually right after an
upgrade), not when he clicks Restart.

## Goals

- A desktop notification when a verify run finds a FAIL, within seconds.
- One notification per problem, and an "all clear" when it's fixed. No nagging.
- The decision written down where operators will read it.

## Non-goals / anti-goals

- Never block, delay or second-guess the desktop Restart/Shut Down.
- No new periodic verify runs (verify mounts boot roots and test-runs init;
  every 10 minutes on a desktop is waste).
- No popup for things that don't threaten the next boot.

## Approaches

- **A. The gate notifies directly** (schema-ctl, in C). Instant, but only the
  re-exec gate, and a second copy of doctor's find-the-session code.
- **B. Doctor runs verify every 10 min.** Catches hand edits, but heavy.
- **C. Every verify run records its result; doctor reports it.** Picked by
  Jonathan: instant through a path watch, one notification code path, shows in
  `schema-doctor` status too. Trade-off: a hand-edited `.svc` isn't seen until the
  next verify run (an upgrade, `schema-ctl verify`, `schema-snapshot arm`, `reexec`).

## Design (C)

1. **verify records its result.** A new `status_path` field in `verify_opts_t`:
   `verify_opts_env()` sets it to `/run/schema-init/verify-status` for the real
   `schema-ctl verify`, and it stays NULL in tests and in `--inspect`/custom-root
   runs, so they never overwrite the host's file. When set and run as root,
   `verify_run` writes it atomically (create `/run/schema-init` if missing, temp
   file + `rename`, 0644) after the `--accept` block, so an accept records `exit=0`:
   `exit=N`, `ts=<epoch>`, the `Result:` line, and up to 5 `[FAIL]` lines. `/run`
   empties at reboot, so a reboot starts clean. A non-root run writes nothing (it
   can't see enough to judge).
2. **doctor gets a `verify` check.** Reads the file:
   - exit 1 (FAIL) → **RED**: notification "schema-doctor: verify" with the first
     FAIL line and "run `schema-ctl verify`".
   - exit 2 (unaccepted CHANGE, lost baseline) and 3 (UNRESOLVED) → **AMBER**:
     shown in doctor status, no popup. *(My call: a package changing a service is
     routine and `--accept` exists; only a FAIL means the next boot may break.)*
   - `result_color()` today takes the colour from the check's static grade, so one
     check can't be RED for one exit and AMBER for another. The check's result gets
     an optional colour override that `result_color()` honours. (Greg)
   - exit 0 or no file → GREEN.
   The existing `notify_transitions` gives one popup on turning RED and "all
   clear" on recovery, and respects the existing `notify=no` config.
3. **Instant, via a watch.** A new `schema-doctor-verify.svc` with
   `watch_path=changed:/run/schema-init/verify-status` (edge-triggered; `exists:` is
   level-triggered and would re-fire every time the oneshot exits; the watch
   follows a file that doesn't exist yet and sees the atomic rename) runs
   `schema-doctor --verify-only`. *(My call; the 10-minute periodic run is the
   backstop if the watch misses.)* `--verify-only` is narrow (Greg):
   - evaluates only the verify check and updates only `checks["verify"].last_state`;
   - "all clear" is computed against every check's stored `last_state`, never
     against the one result, so a verify recovery while another check is RED
     says "verify: all clear" for verify alone and leaves `last_overall` alone;
   - patches only the verify entry in `doctor-status.json` / `doctor-status`, and
     doesn't rewrite `doctor-report.txt`.
   Both runs take an exclusive `flock` on the flap state file around
   load→notify→save, so the watch and the periodic run never interleave and never
   notify twice. *(My addition.)*
   The service copies `schema-doctor-periodic.svc`'s flags (`needs_root=1`,
   `critical=0`, `no_new_privs=0`, `private_tmp=0`, `protect_system=0`,
   `protect_home=0`, `syscall_deny=0`, so it can `setpriv` into the session for
   `notify-send`), lives beside it in `distros/fedora-installer/rail/services/`, and
   joins it in `DOCTOR_TIMERS` (schema-migrate.py) and `prevent-set.list`, so
   existing boxes get it and `no-doctor-timers` skips it too. (Greg)
   On a fresh boot (new `boot_id`) the verify check's `last_state` resets to GREEN
   with the heal history, so a FAIL from before the reboot doesn't come back as a
   spurious "all clear" ten minutes later. *(My call, from Greg's note.)*
4. **Document the decision.** operations.md: replace "The desktop menu's shutdown
   goes through schema-logind and is not gated" with the rationale above and how
   the notification works; note it in the threat model's logind section.

## Left to the builder

File format details, the exact notification wording, test fixtures (a fake
status file per exit code; a transition test that RED notifies once and GREEN
says all clear; verify writes the file as root and not as a user).
