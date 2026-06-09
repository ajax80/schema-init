# PR Review Brief — sd_booted() signal (`/run/systemd/system`)

**For:** Greg (review + sanity-check)
**Branch:** `feat/sd-booted-dir` (off `master`, post-#9 merge)
**Change:** one line in `init.c` `mount_pseudo()` + a doc note + harness assertion.

## What it is (Track B, capability-surface)

libsystemd's `sd_booted()` is literally `access("/run/systemd/system/", F_OK) >= 0`.
Core `mount_pseudo()` already builds the `/run/systemd` skeleton
(`/run/systemd`, `/run/systemd/shutdown`); this adds the one dir that flips
`sd_booted()` 0→1 for unmodified userland:

```c
mkdir("/run/systemd/system",    0755);   /* sd_booted() signal */
```

Created in core, very early (before any service spawns) — same place and reason
systemd creates it in `mount-setup.c`. No service, no process, no ordering races.

## Why core (not an opt-in service like journal-sink)

`sd_booted()` is a *boot signal* — by definition it must be true before any
userland that checks it runs. A oneshot service would race against early services
(udev/polkit/dbus) that may call `sd_booted()` at startup. The dir sits alongside
the existing unconditional `/run/systemd` + `/run/systemd/shutdown` mkdirs, so it
is consistent with what core already does, not a new pattern.

## Empirically verified (host, libsystemd 259.5)

Tiny `sd_booted()` probe, with the global `mock_sd.so` LD_PRELOAD disabled:
- dir **absent**  → `sd_booted() = 0`
- dir **present** → `sd_booted() = 1`

→ This retires the per-process `/usr/local/lib/mock_sd.so` LD_PRELOAD shim
(originally added for the Plasma/KService ksycoca gate). A single empty dir now
gives the same signal system-wide, natively, with no preload into every process.
**Separate follow-up (after reboot onto this init): remove the global
`LD_PRELOAD=/usr/local/lib/mock_sd.so`** — do NOT remove it before the new init is
running, or KService loses the signal in the gap.

## VM harness gate — PASS

`~/schema-livetest/vmtest.sh` now asserts `SDBOOTED-DIR: present` from inside the
QEMU boot, alongside the #7/#8 rails. Latest run: PASS (timer fired, hang excised
at 90s, dependent ran, **and `/run/systemd/system` present at boot**).

## What to sanity-check

1. **Permissions** — `0755` matches systemd's dir; confirm nothing expects stricter.
2. **Side-effects of `sd_booted()=true`** — this flips a lot of software into
   "systemd is here" paths. Confirm the supporting surface justifies it: schema-logind
   systemctl/varlink bridge, ConsoleKit stub, `Inhibit()`/`ListInhibitors()` all
   already present (per doctrine). If any common consumer takes a systemd path we
   *don't* yet answer, flag it — that is the only real risk here.
3. **Early-boot ordering** — confirm `mount_pseudo()` runs before the first service
   spawn (it does, `init.c` main path), so no service can observe the dir absent.

## If you check in changes

Push to `feat/sd-booted-dir`, drop findings in `docs/reviews/pr_10_review_sd_booted.md`.
Already through the VM gate locally.
