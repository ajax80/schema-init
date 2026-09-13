# Why schema-init — one-page fact sheet

*Source of truth for outreach. Every claim here is measured or true as written — don't add to it, pull from it. If someone asks something not covered here, say "I'll check with the author" rather than guessing.*

---

## The one-line version
schema-init is a minimal PID 1 for Linux that supervises services with a weight-state machine instead of unit files — and, piece by piece, a native, **readable** replacement for the systemd daemons around it. AGPL-3.0. `github.com/ajax80/schema-init`

## The pitch in three sentences
- systemd isn't just PID 1 — it's a constellation of always-on daemons (journald, logind, dbus-broker, resolved, udevd, timers) holding RAM and waking the CPU whether you use them or not.
- schema-init replaces PID 1 with a **single static binary** and lets you retire those daemons **one at a time**, each backing out with a single reboot.
- The point isn't only *less* — it's an init layer you can **read top to bottom and own**.

## The numbers (all single-node, measured — never extrapolate to fleets)
- **PID 1 footprint:** 1.2 MB RSS on a minimal boot; 3.3–4.0 MB running a 47-service KDE desktop. **One thread, in every case.**
- **RAM returned:** ~½ GB freed on identical hardware/desktop vs systemd (~1.1 GB used vs ~1.6–2.0 GB). Idle swap drops from hundreds of MB to **zero**.
- **Power:** 92–99% C10 residency, ~1.25 W full-SoC package draw at a working desktop, idle load average 0.03 (vs 0.10–0.20 under systemd). Read from Intel RAPL hardware counters, not estimated.
- Test box: a salvaged Dell Inspiron (i3, 4 GB) that swapped constantly under systemd, now runs a full desktop with room to spare.

> ⚠️ **Honesty rule — say this if scale comes up:** these are single-node measurements on one i3 laptop. Don't multiply a per-node idle delta by a fleet size — an init's own power draw is a tiny slice of a server's total. At scale the real levers are density, footprint, boot time, attack surface, and determinism — not init power draw.

## What it reclaims (opt in one at a time, reboot to undo)
- `schema-logind` — sessions, power, seats + hostname1/timedate1/systemd1 D-Bus surfaces
- `schema-udev` — device management, authoritative over `/dev`
- `schema-journal-sink` — journald-shaped endpoint that drains to a plain logfile, no journal database
- built-in `.svc` timers — retire cron and systemd `.timer` units
- `schema-dbus` — the D-Bus system bus broker itself

## The privacy angle (lead with this for the Liberated-fork author)
- schema-init reclaims systemd's daemons **wholesale** — none of systemd's birthdate/age baggage comes along.
- The repo just went through a **full privacy scrub**.
- Shared value, not a cold pitch: this person already forked over exactly this concern.

## The ask — always low-barrier
- **Not** "join the team." The ask is: *"boot it in a VM, tell me what breaks."*
- Zero-risk paths exist and are the whole point of the on-ramp:
  - **Lane 0:** `git clone` + `make` + `make test` — needs only a compiler, no root, no VM. Just proves it builds and passes ~30 unit tests.
  - **Lane 1:** build a bootable ISO and watch it boot in QEMU — never touches their real bootloader or `/dev`.
  - **Fedora:** prebuilt via COPR (`dnf copr enable ajax80/schema-init`) or a prebuilt installer ISO from the latest release.
- Every step is reversible with a single reboot. Say that early — it's what makes people willing to try.

## What NOT to claim
- Don't promise fleet/server savings (see honesty rule above).
- Don't call the GUI wizard battle-tested — it hasn't been shipped-tested on a live desktop yet; point people to the `schema-migrate` CLI for the proven route.
- Don't guarantee it fits every distro — the tested on-ramps are Debian (ISO) and Fedora KDE (COPR/migrator).
