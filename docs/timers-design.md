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
| `on_calendar` | `HH:MM` local wall-clock time to fire at, every day. |

Setting any of these keys implies `oneshot=1`: the service runs, exits, and re-arms.

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

## Calendar timers (`on_calendar=HH:MM`)

Interval timers schedule off `CLOCK_MONOTONIC` — "N seconds from now". A calendar
timer schedules off the **wall clock**, so it must use `CLOCK_REALTIME`.

The same "PERFECT that re-arms on a deadline" machinery is reused; only the
deadline source changes. A calendar timer carries the `SVC_TIMER_CALENDAR` flag,
and its `timer_next` holds a `CLOCK_REALTIME` instant rather than a monotonic one.

- **Arming / re-arming** (`timer_arm_calendar()` in `init.c`): take the current
  local time, set `tm_hour`/`tm_min` to the target with `tm_sec=0` and
  `tm_isdst=-1`, and `mktime()` it. If that instant is already past (or today's
  occurrence equals now), advance `tm_mday` by one and recompute. The result is
  always strictly in the future.
- **Fire check**: in the `STATE_PERFECT` tick, calendar timers compare against
  `CLOCK_REALTIME` (interval timers still compare `CLOCK_MONOTONIC`).
- **Re-arm**: on completion, `reap()` recomputes the next day's target the same
  way. There is no "drop the flag" run-once path — a calendar timer always
  repeats.

**Why recompute every cycle instead of converting once to a monotonic deadline:**
recomputing against the live wall clock on each arm means DST transitions and NTP
clock steps (chrony stepping the clock after boot) self-correct at the next fire
— drift can never exceed one cycle. A one-shot monotonic conversion would lock in
whatever the clock said at arm time and fire at the wrong wall-clock minute if the
clock later moved.

**Catch-up (`persistent=1`):** by default a fire missed during downtime is *not*
replayed — arming picks the next future occurrence (plain non-`Persistent=`
behaviour). Setting `persistent=1` opts a calendar timer into catch-up:

- Last run is stamped to `/var/lib/schema-init/timers/<name>.stamp` (the realtime
  epoch) on every completion (`timer_stamp_write` in `reap()`).
- At **boot/reload arm only** (`timer_arm_persistent`), compute the most recent
  past `HH:MM` occurrence (`calendar_recent_occurrence`). If it is newer than the
  stamp, a fire was due while we were down → `timer_next = now` so it fires on the
  next tick (logged `timer-catchup`). Otherwise arm normally.
- A timer with **no stamp** (never run) is *seeded* with the current time rather
  than replayed, so enabling a persistent timer doesn't fire spuriously on first
  boot.
- Re-arm after a normal fire never catches up — only the initial arm does, so a
  single missed window produces a single catch-up, not a storm.

`persistent=1` is meaningful only with `on_calendar` (matching systemd, where
`Persistent=` pairs with `OnCalendar`); on an interval timer it is logged and
ignored.

## Precision

Fires on the 250 ms main-loop tick, ±1 tick. Fine for cron-class work. Do not
use timers for sub-second scheduling — that is what `watchdog_timeout_ms` and
the real-time control loop are for.

## Scope

**PR #7:** interval timers — `on_boot_sec` + `on_active_sec`.
Covers the bulk of cron use (`fstrim`, log rotation, backups, mem-sync).

**Calendar timers:** `on_calendar=HH:MM` daily wall-clock fire
(`CLOCK_REALTIME`, DST/NTP-safe via per-cycle recompute).

**This feature:** `persistent=1` catch-up for calendar timers — run a missed fire
once at boot, last-run stamped under `/var/lib/schema-init/timers/`.

**Known gaps (not yet implemented):**

- **Richer calendar forms** — day-of-week, multiple times per day, `*:0/15`
  style. Single daily `HH:MM` only for now.
- **Reload** — `schema-ctl reload` reloads a timer in `NEW_PROCESS`, so it fires
  once promptly after a reload rather than waiting out `on_boot_sec` again.
  Re-arm after first completion is unaffected.
