# schema-migrate — in-place migration wizard (Fedora KDE v1)

**Date:** 2026-08-29
**Branch:** `feat/schema-doctor-migration-checks` (continues here; may split)
**Status:** design approved, pre-implementation
**Builds on:** [`2026-08-22-schema-doctor-design.md`](2026-08-22-schema-doctor-design.md) and
[`2026-08-27-schema-doctor-standing-supervisor-design.md`](2026-08-27-schema-doctor-standing-supervisor-design.md)
(the doctor is the post-reboot heal half — read those first).

## Motivation

schema-init installs cleanly on a fresh box (the ISO rail). But every existing
machine that wants schema has to be hand-converted, and each conversion
re-discovers the same seam wounds by hand — the exact toil that produced the
schema-doctor catalog. The goal: a USB tool that transitions an **existing,
already-installed OS** onto schema-init + schema-udev **in place, losing
nothing**, adds schema as a **non-default fallback boot entry** so the original
system stays bootable, and — on the reboot after — **closes the gaps the flip
opens** automatically. Not a fresh-install stick. A migrator.

The end state is a tool a stranger can run unattended and have their machine
land on schema with the majority of the wounds already healed. This spec is v1:
**Fedora KDE only** (what Optiplex runs). Multi-distro is an explicit later
phase (see Roadmap).

## Goals

- Plug a USB into a running Fedora KDE box, run `sudo schema-migrate`, reboot,
  and land on a working schema-init desktop with the known seams healed.
- **Lose nothing**: purely additive. The original init stays the default boot
  entry; the flip is reversible by picking the old entry or running
  `--uninstall`.
- Reuse what exists: `make install`, the `fedora-kde/` profile, `gen-mounts.sh`,
  the `rail/` services, and `schema-doctor` as the post-reboot heal.

## Non-goals (YAGNI for v1)

- **No systemd-unit → `.svc` translator.** Leftover third-party services are
  *named*, not translated. The translator is a deferred phase (Roadmap), offered
  *after* a proven reboot.
- **No multi-distro / non-KDE.** Fedora KDE only. The tool detects and refuses
  anything else in v1 rather than half-work it.
- **No making schema the default boot entry.** Fallback-only by design.
- **No network/hostname re-invention** beyond the generic `network-manager.svc`
  and a hostname unit generated from the live hostname.

## Form factor

A USB carrying the built schema artifacts (repo tree + compiled binaries) and a
driver script `schema-migrate` run **on the live, booted OS** — because the
runtime state that reveals the wounds (running services, real mounts, the
desktop env) is only visible from the booted system. The tool lives in the repo
at `distros/fedora-installer/migrate/`.

## Architecture — three phases, one round-trip

```
  Phase 1 DISCOVER (read-only)      Phase 2 DEPLOY (pre-reboot)         Phase 3 FINISH (first schema boot)
  ── system → migrate-profile.json  ── binaries + prevent-set +         ── schema-doctor.svc heals seams
     writes nothing                    generated mounts + packages         schema-migrate-finish reports
     runnable alone (--discover)        + boot entry + manifest             + offers the translate step
```

### Components (each testable in isolation, doctor-style injected roots)

| Component | Input → Output | Responsibility |
|---|---|---|
| `discover` | live system → `migrate-profile.json` | detect Fedora KDE; enumerate running services + live mounts/fstab + primary user; snapshot. **Read-only.** |
| `deploy` | profile + repo artifacts → installed files + `migrate-manifest.json` | `make install`; drop the curated prevent-set; generate host-specific mount/hostname units; `dnf install` prevent deps; record every change. |
| `bootentry` | active BLS entry → new alternate entry | clone the running boot entry, append `init=`, distinct id, **default untouched**. Add/remove. |
| `finish` | first schema boot → report + offer | show the migration report, list un-ported services by name, offer the deferred translate step. |
| `manifest` / uninstall | `migrate-manifest.json` → reversed system | remove exactly what deploy added (files, packages, boot entry). |

Each is a separate script under `distros/fedora-installer/migrate/`; `schema-migrate`
is the driver that sequences them and owns the CLI
(`--discover`, `--deploy`, default = discover+deploy, `--uninstall`, `--dry-run`).

## Phase 1 — Discover (read-only)

Detect the box is Fedora (`/etc/os-release` ID=fedora) and KDE (plasmashell /
sddm present); **refuse otherwise** in v1 with a clear message. Enumerate:

- **Running services** — the systemd unit set, split into: covered-by-prevent-set
  (dropped), schema-owned (PID1/journald/crond/resolved/udev — replaced), and
  **leftover third-party** (recorded for the Phase-3 name list).
- **Mounts** — live mounts + `/etc/fstab`, via `gen-mounts.sh`, minus the
  schema-owned pseudo-filesystems.
- **Primary user** — uid 1000 (autologin target).
- **Bootloader** — confirm BLS entries exist in `/boot/loader/entries/`.

Output: `migrate-profile.json`. `schema-migrate --discover` runs this alone and
prints a preview without touching the system.

## Phase 2 — Deploy (the flip, pre-reboot)

**1. Binaries.** `make install` with `PREFIX=/usr` → `schema-init`, `schema-ctl`,
`schema-udev`, `schema-subreaper`, `schema-journal-sink` in `/usr/bin`, the
services dirs (`/etc/schema-init/services`, `/usr/share/schema-init/services`),
and the logrotate drop-in.

**2. The generic prevent-set (curated — NOT the whole profile).** The
`fedora-kde/` profile is Jonathan's own box; it carries host-specific units that
must never reach a stranger. A new manifest,
`distros/fedora-installer/migrate/prevent-set.list`, names only the portable
desktop-seam core:

- **scripts:** `plasma-session-start.sh`, `schema-plasma-autologin.sh`,
  `schema-plasma-watchdog.sh`, `schema-autostart-runner.sh`, `schema-logind.py`,
  `seatd-run.sh`, `pipewire-run.sh`, `wireplumber-run.sh`,
  `pipewire-pulse-run.sh`, and the XDG dedup / menu-prefix env scripts.
- **config:** `plasma-env`, `plasma-workspace/env/*`, `polkit/`, `autostart/`.
- **services:** `dbus`, `seatd`, `schema-logind`, `plasma-autologin`, `polkitd`,
  `bluetoothd`, `network-manager`, `sshd`, `zram`, `schema-doctor`,
  `schema-doctor-periodic`.
- **EXCLUDED (host-specific, never deployed):** `frigate`, `greybox-audio`,
  `nordvpnd`, `ollama`, `mount-home-*`, `network-blakbox`, `loop-module`.

The list is the source of truth; deploy copies exactly what it names. Anything
not on the list is either generated (below) or excluded.

**3. Host-specific, generated from discovery.** Mount `.svc` units from the live
fstab/mounts (`gen-mounts.sh`); a hostname `.svc` from the live hostname; the
`plasma-autologin` unit pointed at the box's real primary user. Network stays the
generic `network-manager.svc`.

**4. Packages.** `dnf install` the prevent deps — `libavcodec-freeworld` (H.264),
`egl-wayland` (restores the Wayland EGL platform), and any missing schema runtime
deps (`seatd`, pipewire stack). Additive; each recorded to the manifest.

**5. Manifest.** `/var/lib/schema-init/migrate-manifest.json` records every file
written, every package `dnf install`ed (only those *not already present*, so
uninstall never removes a pre-existing package), and the boot-entry id. Powers
`--uninstall`.

**6. Boot entry (Fedora BLS).** Clone the **active** entry in
`/boot/loader/entries/`, set `title "<name> (schema-init)"`, append
`init=/usr/bin/schema-init` to `options`, assign a distinct entry id.
**`saved_entry` / GRUB default is left untouched** — original systemd boots by
default. Ensure the menu is visible (`GRUB_TIMEOUT`≥5 via `grub2-editenv` /
`/etc/default/grub` + `grub2-mkconfig` only if currently hidden). Rollback = pick
the original entry at the menu, or delete the one `.conf`.

## Phase 3 — Finish (first schema boot, post-reboot)

- **`schema-doctor.svc`** (already in the rail) runs late, heals the SAFE seams
  (`card-input-acl`, `vt-mediation`, `xdg-data-dirs-dup`, `nvidia-wayland-egl`),
  names the DEFERRED ones.
- **`schema-migrate-finish`** (new, oneshot, runs once then self-disables):
  renders the migration report — what deployed, doctor's result, and the list of
  **un-ported third-party services** by name — and **offers the translate step**
  (the deferred phase-2 tool). It writes a one-shot marker so it never re-runs.

## Safety & reversibility

- **Additive only.** Nothing on the existing system is deleted or overwritten in
  place; schema files live in `/usr/bin`, `/etc/schema-init`, `/var/lib/schema-init`,
  and one new BLS entry. The original init, kernel, and all data are untouched.
- **Fallback boot entry** = the master safety net. A broken schema boot is one
  menu selection away from the original working system.
- **Manifest-driven uninstall** removes exactly what was added; pre-existing
  packages are never removed.
- **`--dry-run`** prints the full plan (files, packages, boot entry) and changes
  nothing.
- The **doctor's own safety model** (snapshot → heal → verify → back-out,
  whole-run `exit 0`) governs Phase 3; it can never block the boot.

## Testing strategy

- **Unit tests** for `discover`, `deploy`, `bootentry`, and `manifest`/uninstall
  using injected roots (the doctor's `DOCTOR_ROOT` pattern): a temp tree standing
  in for `/boot/loader/entries`, `/etc`, `/var/lib`, a fake `os-release` and proc
  table. Assert the profile JSON, the exact file set deploy writes, the cloned
  BLS entry (title, `init=`, default untouched), and that uninstall reverses the
  manifest byte-for-byte.
- **Guinea pig: a fresh Fedora KDE VM** — the honest test of an *in-place*
  migration (GreyBox already runs schema-init, so it can't test the migration
  from stock). Install stock Fedora KDE, run `schema-migrate`, reboot into the
  schema entry, verify the desktop comes up, `schema-doctor --check` is clean,
  and `--uninstall` + reboot returns to stock. `schema-vmtest` covers the
  boot-as-PID1 half.
- **Idempotence:** re-running `schema-migrate` on an already-migrated box is a
  no-op (detect the manifest, refuse or update, never double-write).

## Roadmap (deferred, designed-for)

1. **Phase-2 service translator** — offered by `schema-migrate-finish` after a
   proven reboot. Auto-carries simple `Type=simple`/`oneshot` units; **names what
   it punts** (socket activation, `.timer`, template `@` units, `Type=notify`/
   `dbus`, complex ordering) rather than silently dropping. Safe because schema
   is already booting and a bad translation just means picking the old entry.
2. **Multi-distro / multi-DE** — generalize discovery + the prevent-set to
   Debian/other DEs once Fedora KDE is proven. The prevent-set-list mechanism is
   already the seam for this: one list per profile. **(Explicitly remembered as a
   follow-up per Jonathan.)**

## Open risks

- **BLS assumptions** — a box not using BLS (custom GRUB, rare on modern Fedora)
  needs a fallback path; v1 detects BLS in discover and refuses if absent rather
  than guess.
- **Primary-user detection** — uid 1000 is the assumption; a box with a
  non-standard primary user needs the profile to capture it explicitly.
- **Prevent-set completeness** — the curated list is the current best guess of
  the portable core; the VM round-trip is what proves it, and the doctor catches
  what the list misses.
