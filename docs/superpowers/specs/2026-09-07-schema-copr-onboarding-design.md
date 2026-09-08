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
  fedora-installer payloads. Also ships the escalation drop-in
  `/etc/sudoers.d/schema-wizard` (see Privileged surface) and owns the root
  stage file `/var/lib/schema-init/wizard-stage.json`.
- **`schema-wizard`** — the PySide6 Qt/QML GUI. Requires `schema-init` and
  `python3-pyside6`. Ships the wizard, its QML, and the XDG autostart entry
  (`/etc/xdg/autostart/schema-wizard.desktop`) that relaunches it at the right
  stage after each reboot, plus a `~/Desktop` launcher for on-demand re-runs.
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
- **Teardown (DONE and ROLLED_BACK):** the wizard removes its root autostart
  entry (`/etc/xdg/autostart/schema-wizard.desktop`, via the helper) and the
  `~/Desktop` launcher, so it stops relaunching on every login. The stage file
  is left at its terminal value as an idempotency record; a re-run from the
  Desktop launcher reads it and no-ops if terminal. This mirrors the yad
  wizard's `finish_clean()`.

### State location

The authoritative stage lives in one **root-owned** file,
`/var/lib/schema-init/wizard-stage.json` (0644, world-readable), alongside the
existing `migrate-profile.json` / `migrate-manifest.json`. Only the privileged
helper writes stage transitions; the unprivileged GUI reads it to decide which
screen to show. This replaces the split between migrate's manifest-presence
check and the flip wizard's per-user `firstboot.state`. Per-user GUI ephemera
(which sub-panel is expanded, etc.) may still live under `$XDG_STATE_HOME`, but
never the authoritative stage.

### Privileged surface

The GUI runs **unprivileged** in the Plasma session and performs every
system-changing action through a **single fixed helper** — the migrate engine
and `schema-flip-apply` — never inline. v1 grants that helper via a
`/etc/sudoers.d/schema-wizard` drop-in: `NOPASSWD` for the exact absolute helper
path(s) only, nothing wildcarded. This mirrors the model the yad flip wizard
already ships and proves, and the root-owned fixed-path helper is the entire
attack surface. (Alternative considered: a polkit `.policy` + `pkexec`, which
swaps the standing `NOPASSWD` rule for a per-action native KDE auth dialog.
Noted as post-v1 hardening; not required for the first cut.) The password the
consent screen collects is the user's own login credential used to gate intent —
the escalation itself rides the locked helper.

## GUI (Qt/QML, PySide6)

Carries the yad wizard's plain-language novice voice, elevated to native Plasma
polish. Screens:

- **Welcome / consent** (per round): what is about to happen, the safety net
  (snapshot + fallback entry + auto-rollback), password prompt.
- **Recovery card (before R1's reboot — mandatory, cannot be skipped):** the one
  catastrophe the GUI cannot rescue is R1 booting schema-init and *never
  reaching SDDM* (black screen / early panic) — there is no desktop left to draw
  a wizard on. So *before* arming R1, a full-screen card shows, in plain
  language and large type, exactly how to recover by hand:
  *"If the screen stays black for more than 2 minutes after this restart, hold
  the power button to turn the computer off, turn it back on, and at the boot
  menu use the arrow keys to pick the entry that does **not** say
  '(schema-init)', then press Enter. Your computer will start exactly as it does
  today."* The card tells the user to **photograph it with their phone** and
  requires an explicit *"I've saved these instructions"* check before Continue
  becomes active. The same text is written to `~/schema-recovery.txt` and to the
  ESP/`/boot` as a fallback readable from another machine. (The migrate engine
  already makes the GRUB menu visible and keeps the stock entry, so the menu
  will be there to pick from.)
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
- **Manual fallback recovery** for the one case automation can't cover — R1
  never reaching a desktop: the mandatory Recovery card (above) + the pristine
  visible GRUB entry + `~/schema-recovery.txt`/`/boot` copy give the user a
  hand-recoverable path back to stock systemd with no working desktop required.
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
5. The mandatory Recovery card blocks Continue until acknowledged; on a VM forced
   to a black-screen R1, selecting the non-`(schema-init)` GRUB entry boots
   straight back to stock systemd with a working desktop.
