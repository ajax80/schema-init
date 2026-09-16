# Boot-success guard & btrfs rollback

Turns a boot-breaking schema-init deploy into a one-reboot recovery instead of
an all-night debugging session. For btrfs roots that boot schema-init as PID 1
(with or without an initramfs).

## The problem

A deploy can leave PID 1 healthy but the desktop dead (a logind/seat/display
regression). The boot "succeeds" while the screen stays black, and there is no
known-good state to fall back to. Recovery then means live debugging on the
hardware.

## The pieces

- **`schema-snapshot`** — before a risky deploy, make a writable btrfs snapshot
  of `/` and `/home` as sibling subvols, each with a self-contained BLS boot
  entry (kernel and args derived from the current default entry, `rootflags`
  repointed at the snapshot, fstab rewritten to the frozen subvols). Booting the
  entry gives a real read-write desktop — no initramfs overlay needed.
  - `schema-snapshot create [label]` — snapshot + BLS entry, auto-prune to
    `SCHEMA_SNAP_KEEP` (default 3).
  - `schema-snapshot list | delete <name> | prune`
  - `schema-snapshot arm [N] | disarm` — see below.

- **`schema-bootok`** — a root oneshot run late in the rail. Waits up to
  `SCHEMA_BOOTOK_TIMEOUT` (default 90s) for a live compositor (`kwin_wayland`
  holding a `/dev/dri/card*` fd), then sets `boot_success=1` and clears
  `boot_counter`. On timeout it leaves `boot_success=0`, so the fallback stays
  armed. Success means *desktop reached*, not *init finished*.

- **`09_schema_fallback`** — a grub.d hook that runs after stock
  `08_fallback_counting`. When the boot counter is exhausted and the last boot
  did not confirm success, it boots the snapshot BLS id in `schema_fallback_id`
  instead of stock numeric `default=1` (BLS-safe).

## How it fits together

GRUB already ships `08_fallback_counting` and `10_reset_boot_success`:
`boot_success` is reset to 0 every boot, `boot_counter` is decremented each boot
it stays 0, and when the counter is exhausted GRUB picks the fallback. This
guard just supplies the missing halves — a *real* success signal (desktop up)
and a *real* fallback target (the snapshot).

## Deploy flow

    make safe-install        # snapshot pre-deploy + install new binary/rail
    schema-snapshot arm      # point schema_fallback_id at that snapshot, set boot_counter
    reboot

If the new boot reaches the desktop, `schema-bootok` marks it good and disarms.
If it does not, the counter runs down and GRUB boots the pre-deploy snapshot.

## Activation (one-time, reboot-gated)

    make install-bootguard                 # confirmer + grub.d hook + svc
    ln -s schema-bootok.svc <rail>         # add to the boot rail (dep=display-manager)
    grub2-mkconfig -o /boot/grub2/grub.cfg # pick up 09_schema_fallback

Validate the counting logic under vmtest (boot, withhold `boot_success`, reboot,
assert the fallback id is chosen) before trusting it on hardware.

## Tunables / defaults

- `boot_counter` (arm's `N`): **2** — tolerate one flaky boot before falling back.
- auto-reboot-on-hang: **off** — on timeout the guard only logs; pick the
  fallback entry manually (the box stays reachable over SSH).
- `SCHEMA_BOOTOK_TIMEOUT`: **90s** — clears a healthy cold boot with margin,
  well under a multi-minute stall.
- `SCHEMA_SNAP_KEEP`: **3** snapshots retained.

## Manual rollback

At power-on, reveal the GRUB menu (Esc / hold Shift) and pick a
`schema-init snapshot …` or `schema-init fallback …` entry. It is one-shot; a
normal reboot returns to the default. Work done inside a snapshot session writes
to that snapshot subvol and its frozen `/home`, not the live system — it is a
recovery boot, not a work session.
