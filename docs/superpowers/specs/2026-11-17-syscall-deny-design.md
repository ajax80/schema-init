# syscall_deny= — the schema-native denylist preset (follow-up to 4.6)

> **Superseded in part (2026-11-18):** unset no longer follows the hardening default and the doctor lint does not count it — see the amendment at the end. The Intent/Goals below are the original design that hung blakbox.

## Intent
The 10-13 plan (re-confirmed 11-17): a denylist knob for schema's own
services, on the 4.6 seccomp engine, tied to the hardening-default switch
like the other four knobs, so a host that opts in gets a sane syscall
denylist on every service that doesn't say otherwise.

## Goals
- `syscall_deny=` — tri-state like no_new_privs/private_tmp/…:
  - unset: follows the host's hardening-default (on → the preset, off →
    nothing);
  - `default`: the preset, explicitly;
  - a list of names/@groups (optionally `:ERRNO`): exactly those;
  - `0`: explicitly nothing (a service that needs, say, @clock opts out).
- The preset: `@clock @cpu-emulation @debug @module @mount @obsolete
  @raw-io @reboot @swap kexec_load kexec_file_load bpf` — the 10-13 list.
  Blocked calls get EPERM (the 4.6 default).
- Merged into the one filter: with no syscall_filter= it is a denylist;
  with a denylist the sets add; with an allowlist those names are taken out
  of it. syscall_error_number= applies as usual.
- `schema-ctl status <svc>` shows `syscall_deny` with its source
  (explicit / default), like the other knobs.
- `schema-doctor --hardening-lint` counts syscall_deny among the knobs a
  .svc leaves to the host default.
- The importer writes `syscall_deny=0` on every unit, as it writes the
  other four knobs explicitly: an imported unit means the same thing on any
  host (it carries systemd's own SystemCallFilter= if it had one).

## Non-goals
- Turning the switch on anywhere: that's the rollout (easy daemons first:
  avahi, wsdd, bluetoothd, upower; chronyd needs `syscall_deny=0` or a list
  without @clock).

## Tasks (TDD each)
1. Parse + finalize: HARD_SD bit; `default`/list/`0`/unset; merge rules;
   test_svc_seccomp cases (unset+switch off → none, unset+switch on →
   preset bits, explicit 0 under switch on → none, list, allowlist removal,
   denylist union).
2. Status line + schema-doctor lint key + importer `syscall_deny=0`
   (importer byte-identical fixtures updated).
3. Live: schema-user service with `syscall_deny=default` gets EPERM from
   `mount`/`swapoff` and runs otherwise; PID 1 check with one test unit.

## Amendment 2026-11-18 — opt-in only
Shipped on dev 1509, blakbox (hardening-default on since 09-27) cold-booted
with the preset on all 63 hand-written host `.svc` that predate the knob:
mount-home lost mount(2) → no /home, gettys/udevd/sddm filtered, boot hung.
The pinning above covered every generator but not files already on a host.
Unset `syscall_deny=` is now nothing regardless of the switch; the rollout is
explicit `syscall_deny=default` per service. Rule for any future knob: never
add it to the hardening-default set — that set is frozen at the four.
