# Timers — design notes

`schema-init` schedules periodic work with **timer keys on a service**, not a
separate unit type. A timer *is* a service that re-arms on a clock instead of
staying terminal after it exits.

## Why no separate `.timer` file

systemd splits `foo.timer` + `foo.service` and links them. That is two files
and a linking layer. `schema-init`'s model is "drop one `.svc`." So the timing
lives on the service itself — the service is periodic.

## Keys

| Key | Meaning |
|-----|---------|
| `on_boot_sec` | Seconds after boot before the **first** fire. `0` = fire at boot. |
| `on_active_sec` | Seconds after each **completion** before the next fire (the period). |

Setting either key implies `oneshot=1`: the service runs, exits, and re-arms.

```ini
name=trim
exec=/usr/sbin/fstrim
args=-a
needs_root=1
on_boot_sec=600        # first fire 10 min after boot
on_active_sec=86400    # then every 24 h after each completion
```

## How it works — it reuses what already exists

A timer recombines two proven mechanisms:

- `reap()` already moves a clean oneshot to `STATE_PERFECT`.
- `STATE_DORMANT` already wakes a service to `STATE_NEW_PROCESS` when a
  `CLOCK_MONOTONIC` deadline passes.

A timer = "PERFECT that re-arms to NEW_PROCESS on a deadline."

1. **Boot arming** — a timer service is born in `STATE_PERFECT` (as if it had
   already run) with `timer_next = boot + on_boot_sec`.
2. **Fire** — in the `STATE_PERFECT` tick, once `CLOCK_MONOTONIC ≥ timer_next`,
   state flips to `STATE_NEW_PROCESS`. The normal spawn path takes over, so
   **dependencies are still honored** (NEW_PROCESS waits on `service_deps_ready`).
3. **Re-arm** — when the child exits, `reap()` sets `STATE_PERFECT` and
   `timer_next = now + on_active_sec`, **regardless of exit code**. This is cron
   semantics: a failed run is not retried in a tight loop — it runs again next
   window. The exit code is logged (`timer-done` / `timer-failed`).

Period is measured from **completion** (like systemd `OnUnitInactiveSec`), which
is the simpler and safe default — a slow job never overlaps itself.

## Precision

Fires on the 250 ms main-loop tick, ±1 tick. Fine for cron-class work. Do not
use timers for sub-second scheduling — that is what `watchdog_timeout_ms` and
the real-time control loop are for.

## Scope

**This feature (PR #7):** interval timers — `on_boot_sec` + `on_active_sec`.
Covers the bulk of cron use (`fstrim`, log rotation, backups, mem-sync).

**Known gaps (not yet implemented):**

- **`on_calendar=HH:MM`** wall-clock fire — needs `CLOCK_REALTIME` plus a small
  `localtime` compare and DST handling. Follow-up.
- **Persistent / catch-up** (systemd `Persistent=true`, run jobs missed during
  downtime) — needs last-run stamped to disk. Follow-up.
- **Reload** — `schema-ctl reload` reloads a timer in `NEW_PROCESS`, so it fires
  once promptly after a reload rather than waiting out `on_boot_sec` again.
  Re-arm after first completion is unaffected.
