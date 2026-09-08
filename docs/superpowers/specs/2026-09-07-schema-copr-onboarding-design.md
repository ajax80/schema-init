# schema COPR onboarding wizard — design

**Date:** 2026-09-07
**Status:** approved for planning
**Scope (v1, hard fence):** in-place conversion of an *already-running Fedora KDE* box onto schema-init, delivered by a COPR repo and driven by a polished GUI a Linux novice can complete.

## Goal

A person with little or no Linux experience — the motivating user is "dad" —
should be able to:

1. enable the schema COPR repo,
2. `dnf install schema-wizard` and say yes,
3. launch the wizard; a GUI appears and asks a few questions,
4. the machine reboots; the doctor heals the seam,
5. the wizard reappears, asks a few more questions,
6. the machine reboots again; the doctor takes over completely,
7. a **"Schema is successfully installed"** screen appears with a full report of
   whatever noncritical items remain.

"Runs as well as mine" is only promised where the desktop matches the one every
seam is proven on: **Fedora KDE**. GNOME and headless are explicitly out of v1.

## Why this shape

Two prior tools already carry most of the engine; this project *assembles and
elevates* them rather than building from scratch:

- **`distros/fedora-installer/migrate/schema-migrate.py`** — the in-place
  converter. Already: Fedora-KDE platform gate, profile discovery, deploy of the
  schema-init core + prevent-set + host mount/udev units + NM/resolv fix +
  desktop groups, **keeps stock systemd as the fallback boot entry**, GRUB menu
  made visible, a full `Manifest`-based `--uninstall`, and a post-reboot
  `--finish` report that reads the doctor's status. It is CLI-only and it
  **compiles schema-init on the target**.
- **`distros/fedora-installer/firstboot-flip-wizard.sh`** — a yad/GTK GUI that
  already walks the *optional udev flip* end to end, reboot-gated
  (`welcome → check → arm → reboot → confirm`), delegating every privileged
  action to a single sudo helper (`schema-flip-apply`), with a **headless
  seatbelt that auto-rolls-back** a flip that comes up unusable, before login.
- **`scripts/schema-doctor.py`** — the GREEN/AMBER/RED standing supervisor, with
  `render_report`, a `doctor-status` file, and desktop notifications.

The design reuses these engines and replaces the *delivery* (USB/ISO → COPR) and
the *front end* (CLI + yad → one Qt/QML wizard).

## Key decisions (settled during brainstorming)

1. **Path C — hybrid.** Ship the onboarding now; gate "stranger-ready" on
   hardening the **doctor** against hardware-*variable* bug classes (uaccess/acl,
   session seam, powerdevil), not on chasing this box's specific open bugs. Pure
   features (SP4 session-bus reclamation) and cosmetics (README absolutes) are
   deferred behind this push.
2. **v1 fence:** Fedora KDE, in-place, on a box that is already running. Not
   fresh install, not GNOME, not headless.
3. **Risk ladder = two reboots, by design.** Never flip everything at once on
   someone's only machine. Prove PID 1 on a stock-udev/stock-dbus foundation →
   heal → flip the desktop seam (udev + dbus) → heal → done. Each reboot is
   survivable because the layer below it is already proven, and a snapshot +
   fallback entry are always present.
4. **Choice philosophy = progressive disclosure.** A prominent *Recommended*
   default path; every advanced option present and visible in a **collapsed
   Advanced section** (nothing hidden), pre-set to the safe path, each dangerous
   toggle carrying the warning *"Don't enable or disable this unless you're
   certain what it does."* Ignoring Advanced == the safe path.
5. **Toolkit = Qt/QML in Python (PySide6).** The target *is* Plasma/Qt, so Qt is
   already installed (zero added desktop weight), the wizard looks native, and it
   shares a language with the (Python) doctor. The wizard always runs inside a
   logged-in Plasma session, never at a bare console, so no framebuffer GUI is
   needed.
6. **Prebuilt, not compiled on target.** The COPR RPM ships ready schema-init
   binaries; the wizard deploys installed files instead of pulling a toolchain
   and running `make`. Deterministic, fast, no gcc for dad.

## Package layout (COPR)

- **`schema-init`** — the core suite, **prebuilt**: `schema-init`, `schema-ctl`,
  `schema-subreaper`, the doctor, the migrate engine (refactored to skip the
  compile), the prevent-set, service `.svc` files, the flip helper
  (`schema-flip-apply`) and its seatbelt. Built from the existing
  `schema-init.spec`, extended to package the binaries and the fedora-kde /
  fedora-installer payloads.
- **`schema-wizard`** — the PySide6 Qt/QML GUI. Requires `schema-init` and
  `python3-pyside6`. Ships the wizard, its QML, and the XDG autostart entry that
  relaunches it at the right stage after each reboot.
- **Adoption:** enable COPR → `dnf install schema-wizard` (pulls `schema-init`) →
  launch the wizard.

## The unified stage machine

One stage file supersedes both migrate's manifest-presence check and the flip
wizard's `firstboot.state`. Stages:

```
INSTALLED → R1_PENDING → (reboot) → R1_HEAL → R2_PENDING → (reboot) → DONE
                                                      \→ ROLLED_BACK (seatbelt)
```

- **INSTALLED** — packages present, wizard launched, nothing changed yet.
- **R1 (Foundation), pre-reboot:** btrfs snapshot (if btrfs) → migrate engine
  deploy *without compile* (schema-init PID 1, stock udev/dbus, host units, NM
  fix, groups) → fallback systemd boot entry preserved → GRUB menu visible. Set
  `R1_PENDING`. Reboot into schema-init.
- **R1_HEAL:** first schema boot; `schema-doctor` heals the seam (acl, session,
  powerdevil, ksycoca…). Wizard autostarts, reads doctor-status.
- **R2 (Desktop seam), pre-reboot:** wizard shows what the doctor did, then arms
  the **udev flip + dbus broker** via the existing `schema-flip-apply` helper
  (seatbelt armed). Set `R2_PENDING`. Reboot.
- **DONE:** doctor is authoritative and standing; success screen + noncritical
  report. If the seatbelt rolled either flip back before login, the wizard lands
  in **ROLLED_BACK**: explain in plain language *why* (reuse `humanize_reason`),
  leave the machine clean on the proven foundation, offer the report — never a
  silent dead-end.

Privileged actions stay behind a single sudoers-permitted helper surface (the
migrate engine + `schema-flip-apply`); the GUI runs unprivileged in the user
session and shells to that helper, mirroring the yad wizard's model.

## GUI (Qt/QML, PySide6)

Carries the yad wizard's plain-language novice voice, elevated to native Plasma
polish. Screens:

- **Welcome / consent** (per round): what is about to happen, the safety net
  (snapshot + fallback entry + auto-rollback), password prompt.
- **Progress**: live step feedback during deploy / arm.
- **Advanced (collapsed expander)**: every layer listed — keep/remove fallback
  entry, udev flip on/off, dbus broker on/off, snapshot on/off, doctor timers —
  pre-set safe, each dangerous toggle carrying the warning line.
- **Between-round summary**: "your login worked, your GPU came up, here's what
  the doctor fixed," sourced from `doctor-status`.
- **Final**: **"Schema is successfully installed"** + scrollable noncritical
  report (doctor-status GREEN/AMBER items + migrate `leftover` services with the
  `--translate` offer).
- **Rolled-back**: humanized reason, report offered, clean exit.

## Safety (all reused)

- **btrfs snapshot** before R1 for one-command rollback (skipped gracefully on
  non-btrfs; surfaced in Advanced).
- **Fallback systemd boot entry** kept pristine (migrate already does this; the
  kernel-install hook keeps it across kernel updates).
- **Headless seatbelt** auto-rolls-back an unusable flip before login.
- **`schema-migrate --uninstall`** reverses the whole migration from the
  manifest.

## New work vs reuse

**New:**
- The Qt/QML GUI (`schema-wizard`) — the largest piece.
- COPR spec + **prebuilt** packaging (extend `schema-init.spec`; add
  `schema-wizard`).
- The unified stage machine + autostart-resume across two reboots.
- Doctor hardening for hardware-*variable* bug classes (acl/session/powerdevil)
  so an unfamiliar box self-heals — the item that earns "stranger-ready."

**Reused as-is (or lightly refactored):**
- migrate deploy/discover/uninstall engine (refactor: make the compile optional
  so it can consume prebuilt files).
- `schema-flip-apply` + seatbelt + `humanize_reason`.
- `schema-doctor` report/status/notify.

## Explicitly out of scope (v1)

- GNOME and headless targets.
- Fresh-install path (the ISO/ks route already exists separately).
- SP4 session-bus reclamation.
- README "absolutes" polish.

## Success criteria

1. On a clean Fedora KDE VM, `dnf install schema-wizard` + clicking the
   Recommended path through two reboots ends on the success screen with the
   desktop fully working on the flipped (udev + dbus) seam.
2. A deliberately broken seam is caught by the seatbelt and the wizard lands in
   ROLLED_BACK with a plain-language reason and a working desktop on the
   foundation layer.
3. `schema-migrate --uninstall` returns the VM to stock systemd.
4. Advanced lists every layer with warnings; ignoring it yields the safe path.
