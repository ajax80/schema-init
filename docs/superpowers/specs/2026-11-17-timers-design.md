# Audit 4.9: timers — randomized delay, richer calendars

## Intent
Imported timers keep their schedule. Today every daily timer fires at 00:00
together (blakbox Mondays: restic, fstrim, plocate, btrbk, logrotate,
podman-auto-update in the same second) because `RandomizedDelaySec=` is
dropped, and `Sat`, `Sun *-*-1..7 1:00:00` and true `hourly` don't import.

## Goals
- `randomized_delay_sec=N` (seconds, timespan-free integer, 0..2678400):
  every arm of a timer — boot, interval, calendar, persistent catch-up —
  adds a uniform random 0..N whole seconds to `timer_next.tv_sec`, fresh
  each time (systemd's default, not `FixedRandomDelay=`). Random bytes from
  `getrandom(GRND_NONBLOCK)` (never blocks PID 1 on an uninitialized pool,
  as service.c already does), falling back to CLOCK_MONOTONIC ns mixed with
  the service name.
- `on_calendar=` grows to `[DOW] [DOM] H:M`:
  - DOW: names (`Mon`, `monday`), lists and ranges: `Sat,Sun`, `Mon..Fri`,
    `Mon,Wed..Fri`.
  - DOM: `15`, `1,15`, `1..7`.
  - H and M: `*`, a number, lists, ranges `a..b`, steps `a/s` (from a, every
    s; `*/s` = `0/s`; a step of 0 refuses the load).
  - DOW and DOM both given: both must hold (systemd's rule; `Sun 1..7 01:00`
    = first Sunday). Seconds are always :00. No month or year field.
  - Every value accepted today parses to the same schedule (`03:00`,
    `6,18:00`, `Mon 03:00`, `15 03:00`).
- Up to 4 `on_calendar=` lines per service; the next fire is the earliest
  (min) of their next occurrences, and catch-up compares the stamp with the
  latest (max) of their most recent past occurrences. In a drop-in, the first `on_calendar=` line replaces the lists from
  earlier files (a drop-in still overrides a schedule rather than adding
  one). An empty `on_calendar=` stays ignored (#514).
- The next occurrence is searched day by day for up to 8 years (so `Mon 31`
  is found); a spec with no occurrence refuses the load.
- Importer: `RandomizedDelaySec=` → `randomized_delay_sec=` (no more
  "dropped"); `OnCalendar=` forms: bare weekday → `DOW 00:00`;
  `*-*-DOM` with lists/ranges → DOM; `hourly`, `*:00`, `*-*-* *:00:00` →
  `*:00` (wall-aligned, was a 3600 s interval); `*:0/15` style; seconds
  only `:00`; several `OnCalendar=` lines up to 4; months/years skipped.
  `OnActiveSec=` alone → `on_boot_sec` (it counts from the timer's start,
  i.e. boot, or login for user timers). `AccuracySec=` stays ignored.
- `schema-ctl status <svc>` shows the next fire for a timer as local time:
  calendar timer_next is CLOCK_REALTIME already; an interval/boot one is
  CLOCK_MONOTONIC, shown as time(NULL) + (timer_next - now_mono).

## Non-goals
- `on_unit_inactive_sec=`: `on_active_sec` already measures from
  completion (systemd's `OnUnitInactiveSec`); documented, no new key.
- `FixedRandomDelay=`, month/year fields, seconds, `~` (last day of
  month), time zones in the spec, more than 4 calendar lines (a snap with
  1440 lines stays skipped).

## Mechanism
- New header-only `calendar.h` (pure, like the helpers it replaces in
  service.h): `cal_spec_t { uint8_t dow; uint32_t dom; uint32_t hours;
  uint64_t mins; }` with full masks meaning "any"; `calendar_parse()`,
  `calendar_next_after(specs, n, after)`, `calendar_recent_at_or_before(...)`.
  service_t carries `cal_spec_t timer_cal[4]` and `timer_cal_n`, replacing
  `timer_cal_hours/min/dow/dom`.
- Next: walk days forward with one mktime per day (at noon) to get its
  weekday and day of month; a day failing DOW/DOM is skipped there, so
  `Sun 1..7` costs ~1 call per day for 8 years at worst. Only a matching day
  tries its H:M slots (mktime with isdst=-1, DST self-correcting as now).
  Recent: the mirror, walking back.
- Random delay: one helper `timer_jitter(svc)` added at each arm site
  (`timer_arm_calendar`, the interval re-arm, boot arm, catch-up). The
  persistent stamp still records the actual run time.

## Approaches
1. Masks per field + day walk (picked: covers every blakbox form, keeps
   today's per-cycle wall-clock recompute and DST behaviour).
2. Port systemd's calendarspec: rejected, far larger than the forms in use.
3. Translate everything to intervals: rejected, loses wall alignment.

## Tests
- `tests/test_calendar.c` (fixed TZ): every old form unchanged; lists,
  ranges, steps; `Sun 1..7 01:00` lands on first Sundays; `Mon 31` found;
  impossible spec refused; multi-spec earliest wins; DST spring/fall day;
  recent-occurrence mirror.
- `tests/test_svc_*`: 4-line cap, drop-in replace, empty ignored,
  randomized_delay_sec range.
- Live (schema-user): `on_active_sec=1` + `randomized_delay_sec=2` gaps stay
  in [1, 3.5] s and are not all equal; status shows the next fire.
- Importer: each OnCalendar form above, RandomizedDelaySec mapped (the
  existing "dropped RandomizedDelaySec" assertions flip),
  OnActiveSec, the blakbox census (every timer but the template and the
  1440-line snap imports).
