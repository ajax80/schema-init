# schema-ctl verify — design

2026-11-19. Status: decisions settled with Jonathan 2026-11-19 (Approach 1; baseline option A; builder-check calls 1–5 approved).

## Why

Two real boot failures on blakbox, neither of which a syntax check would have caught:

1. **11-18, reboot #3.** Every `.svc` parsed. What changed was the new binary's *defaults*: `syscall_deny=` followed the host hardening-default switch, 63 untouched host `.svc` gained the preset, `mount-home` lost `mount(2)`, and the box came up with no `/home`, no keyboard. A live re-exec had already put the new binary in place, so the running PID 1 looked fine. Then all three fallbacks failed: the snapshot BLS entry had a blank `initrd` line (grub blscfg stops at it → no `root=` → panic), `schema-good-20260915`'s subvolume had been deleted while its entry and a stale `grub-btrfs.cfg` still pointed at it, and Eli couldn't ssh in. Recovery was booting systemd by hand.
2. **09-28, Seagate.** A drive on the mount path died: 150 s of mount retries, then a hung POST. The POST hang is out of reach; "a device the boot needs is missing or throwing I/O errors" is not.

Plus the 11-18 Optiplex find: the boot guard reported armed while grub.cfg lacked `09_schema_fallback` and no `schema-bootok.svc` was loaded — an inert safety net.

The question `verify` answers: **"if I reboot now, will this box come back, and if it doesn't, will the way back work?"**

## Goals

- One read-only command, `schema-ctl verify`, with a plain PASS / CHANGE / WARN / FAIL report and an exit code.
- It **blocks** the risky steps (Jonathan's call: both manual and blocking):
  - the live re-exec in the rpm `%posttrans` and the deb trigger,
  - `schema-snapshot arm`,
  - `schema-ctl reboot` / `poweroff`.
- It judges the configuration **the next boot will actually use**: the `init=` binary named by the default boot entry, that entry's kernel command line, the `.svc` files and drop-ins on disk.
- Intentional changes don't become a wall: a reviewed change set is accepted once (`--accept`) and stops blocking.

## Non-goals

- Proving a service works under its new sandbox. `verify` shows what changed; it can't know which syscalls a daemon needs.
- Blocking dnf/apt before install (Approach 2, rejected — see Approaches).
- Blocking every reboot path. Desktop menu, bare `reboot`, `systemctl reboot` through the shim are not gated (see Gates).
- Parsing arbitrary mount scripts (`mount-ocean-drives.sh` etc.). Disk checks cover fstab and the kernel log.
- JSON output in v1.
- Pi boot paths (`config.txt`/`cmdline.txt`, no GRUB) in v1: boot-entry checks report "not applicable"; `.svc` and disk checks still run (see §7).

## Anti-goals

- A second copy of PID 1's parsing or default rules (in Python, in schema-ctl, anywhere). That copy drifts and passes what PID 1 refuses — the same shape as the stale `/usr/local/bin/schema-snapshot` that shadowed the packaged one on Optiplex.
- A gate that cries wolf. If it blocks on noise, it gets `--force`d out of habit and protects nothing. Every FAIL must be something that really breaks a boot or a fallback.
- A gate that can strand a box: `--force` always exists, and `verify` failing to run is a WARN-and-proceed for poweroff, never a hang.

## Approaches

1. **Block at the reboot (chosen).** Upgrade installs normally; `%posttrans` / the deb trigger runs `verify` with the new binary; on FAIL or unaccepted CHANGE it skips the live re-exec and prints a loud banner. `schema-snapshot arm` and `schema-ctl reboot`/`poweroff` run `verify` and refuse until it passes or the change set is accepted. Harm happens at the reboot, so that's where the stop is; same mechanism for rpm and deb; nothing dnf-specific.
2. **dnf pre-transaction plugin.** Extract `schema-init` from the downloaded rpm, run its check mode, abort the transaction on failure. Cleanest stop, but dnf-specific code, a separate apt hook, and a broken plugin breaks upgrades. Rejected.
3. **Reimplement the checks in `schema-doctor` (Python).** Rejected outright: the anti-goal.

Jonathan picked 1 (2026-11-19).

## Design

### 1. `schema-init --check` — the new binary reports on itself

A non-PID-1 mode of the `schema-init` binary, following the `--reexec-check` precedent (`init.c:1747`): runs only when not PID 1, never touches the control socket, starts nothing.

```
schema-init --check [--cmdline "<kernel options>"] [<svc_dir>]
```

- Loads the service directory with the **same** `services_load()` / `service_parse` path PID 1 uses, drop-ins included, and the same cycle check (`services_check_cycles`).
- Resolves the hardening default the way `pid1_hardening_default()` does (`init.c:236`), but from `--cmdline` (the next boot's options) instead of `/proc/cmdline`, plus `/etc/schema-init/hardening-default`.
- Prints:
  - every load error, refusal ("hides exec — not loaded"), dropped default, and cycle, as `error:` / `note:` lines;
  - for every loaded service, its **effective settings** as canonical, sorted `name.key=value source` lines (one per setting: user/group, caps, no_new_privs, private_tmp, protect_system, protect_home, syscall_deny, syscall filter mode + count, seccomp flags, ns_kernel knobs, umask, supplementary groups, dynamic_user, deps, critical, oneshot, exec path).
- Exit 0 if everything loaded, 1 otherwise.

The effective-settings serializer is **one function** in `service.c`, also used by `schema-ctl status <svc>`'s hardening block where the fields overlap, so the two can't disagree.

### 2. The baseline — what last booted fine

A boot counts as good (Jonathan, option A):
- **Where `schema-bootok` is installed** (desktop boxes): when it sets `boot_success=1` — a login session holds the GPU.
- **Everywhere else**: when the rail has settled — every loaded service healthy (no EXCISED/RECOVERY/FRICTION, no restarts) for 60 s after boot. A small packaged oneshot (`schema-verify-baseline.svc`, shipped by both rpm and deb) does this; it does nothing on a box where `schema-bootok.svc` is loaded, so the stronger proof wins there.

At that moment the writer runs `schema-init --check` with the running binary and the running cmdline and stores the output as `/var/lib/schema-init/verify/baseline` (+ the binary's version and the date).

The first upgrade onto the version that ships `verify` has no baseline (the old binary has no `--check`): `WARN no baseline`, never a block, once.

Why not compare against the live PID 1: after a live re-exec the running PID 1 already *is* the new binary with the new rules, while the processes it supervises still carry the old sandbox. That is exactly the 11-18 state, and a live comparison would show nothing. "What last proved a good boot" is the honest reference.

No baseline yet (first install, bootok not wired): `verify` reports `WARN no baseline` and diffs nothing; the first good boot writes one.

### 3. `schema-ctl verify` — the report

Read-only. Needs root for the boot-entry and kernel-log reads; as a normal user it runs what it can and says what it skipped.

Sections, in order:

**Next boot**
- Find the default entry (grubenv `saved_entry`, else grub's default) under `/boot/loader/entries`.
- FAIL if: the entry is missing; it has a blank or whitespace-only line before its last key; no `root=` in its options; its `linux`/`initrd` files don't exist; its `init=` binary doesn't exist or isn't executable; `init=` points at schema-init but `<init> --version` fails to run.
- `init=` not schema-init (a systemd entry) → PASS with a note; the `.svc` sections are skipped.

**Services (next boot vs baseline)**
- Run `<init= binary> --check --cmdline "<entry options>"`.
- FAIL: any `error:` line, a cycle, a service whose `.svc` is present but refused (parse error, "hides exec — not loaded").
- `--check` loads `.grp` files the way `engine_load` does; a `dep=` naming a group is real.
- CHANGE: every effective-setting line that differs from the baseline, grouped by setting and summarized, e.g.

  ```
  [CHANGE] syscall_deny  unset → default (default)   63 services: mount-home getty-tty1 udevd sddm …
  [CHANGE] mount-home    no longer loads: protect_home hides exec
  ```
  (the second is a FAIL, shown here for the format)
- CHANGE (acceptable): a baseline service whose `.svc` file is gone — a deliberate removal.
- WARN: new services not in the baseline; dropped defaults; a `dep=` naming no service or group (PID 1 treats it as satisfied, `service.c:2468`, so it doesn't break a boot).

**Reading entries that aren't mounted.** BLS paths are relative to GRUB's root; on blakbox that's the btrfs top level (`linux /root/boot/vmlinuz-…`, snapshot entries `/schema-snap-…/boot/…`, `blsdir=/root/boot/loader/entries`), and fallback subvolumes aren't mounted. `verify` mounts the btrfs top level **read-only** on a private temporary mountpoint, checks, and unmounts — the same thing `schema-snapshot` already does. It writes nothing. When the default entry's `rootflags=subvol=` is not the running `/`, `--check` reads that subvolume's `init=` binary and `/etc/schema-init/services`, not the live ones.

**Ways back**
- The boot guard's fallback entry (`schema_fallback_id` in grubenv, else the newest `schema-snap-*`): the same entry checks as Next boot, plus its `rootflags=subvol=` subvolume must exist (`btrfs subvolume show`).
- Every other `schema-snap-*` / `schema-good-*` entry: same checks, WARN only (they're not what the guard boots).
- `/boot/grub2/grub.cfg` contains `schema_fallback_id` (the `09_schema_fallback` hook), the ESP grub.cfg is a stub, `schema-bootok` is installed and its `.svc` loads in `--check` — today's `check_armable()` rules, moved here.
- Any of the first three failing → FAIL. A guard that can't fire is a FAIL, not a warning.

**Disks**
- Every fstab line without `noauto`/`nofail`: its `UUID=`/`PARTUUID=`/`LABEL=`/path resolves to a present block device → else FAIL.
- Kernel log since boot: I/O errors, `EXT4-fs error`, btrfs `corrupt`/`errs:` on any block device → WARN naming the device.
- `nofail` entries missing → WARN.

**Result**
```
Result: 0 fail, 1 change (not accepted), 2 warn
```
Exit: 0 = pass (WARNs allowed); 1 = any FAIL; 2 = no FAIL but unaccepted CHANGE.

`--quiet` prints only the result line and non-PASS lines (for the gates).

### 4. Accepting a change set

`schema-ctl verify --accept` (root): runs verify; if there's no FAIL, writes the SHA-256 of the canonical CHANGE lines to `/var/lib/schema-init/verify/accepted`. While the current CHANGE set hashes the same, CHANGE no longer blocks (exit 0, lines still shown as `[CHANGE accepted]`). Any further change produces a new hash and blocks again. FAILs can never be accepted. The next good boot rewrites the baseline, which empties the CHANGE set naturally.

### 5. Gates

| Step | Behavior on exit 1 or 2 | Override |
|---|---|---|
| rpm `%posttrans` / deb trigger live re-exec | skip the re-exec; banner with the `--quiet` report and "run `schema-ctl verify`; `schema-ctl reexec` after accepting" | run `schema-ctl reexec` by hand after accepting |
| `schema-snapshot arm` | refuse, print report | `--force` |
| `schema-ctl reboot` / `poweroff`, **interactive only** | refuse, print report | `--force` |

**Only a person at a terminal is gated** (stdin and stderr are a tty). That covers `schema-ctl reboot`/`poweroff`, `sudo reboot` and `systemctl reboot` typed in a shell, which all arrive at `schema-ctl` through the shim or `schema-pi-halt`. Scripted callers are never gated, so a broken box can always go down: `schema-pi-deadline`'s recovery loop (`until schema-ctl reboot`), the dbus/udev flip healthchecks, `schema-flip-apply.sh`, scriptlets, cron.

If `verify` itself can't run (binary missing, crashed): re-exec and arm refuse; reboot/poweroff WARN and proceed (never strand a box that needs to go down).

The package upgrade itself never fails because of `verify`.

Not gated (by design): desktop-menu shutdown via `schema-logind`, and every non-interactive caller. These still get the existing `schema-doctor` desktop notification path: when the last `verify` at upgrade time failed, doctor raises a notification so it's seen before a GUI reboot. (Builder may defer this to a follow-up.)

### 7. Pi (deb) specifics

`init=` is the wrapper `/usr/sbin/schema-init-pi`; `--check` runs `/usr/sbin/schema-init --check` with `cmdline.txt`'s options, and the service directory the wrapper would choose (it may set `SCHEMA_SVC_DIR=/var/lib/schema-init/known-good/services`, `schema-init-pi.c:154`) — the same rule, not a guess. `SCHEMA_SVC_DIR=` on a Fedora entry's cmdline is honoured the same way. If the next boot's inputs can't be determined, the services section reports "not applicable" and does **not** block the deb re-exec.

## Constraints

- C, inside existing binaries (`schema-init`, `schema-ctl`, the shell `schema-snapshot`); no new runtime dependencies. Static PID 1 build must still pass its size and hardening-flag checks.
- `--check` must never act as PID 1: guard on `getpid() != 1` like `--reexec-check`, and never bind or connect the control socket.
- Works unprivileged as far as it can (tests run as a normal user in CI).
- Ships on Fedora (rpm) and Debian/Pi (deb); boot-entry checks are GRUB+BLS only in v1.

## Testing

- Unit: the serializer's output is stable and sorted; `--check` on a fixture dir with (a) a clean set, (b) a parse error, (c) a cycle, (d) a missing dep, (e) a default that hides exec; `--cmdline` flips the hardening default.
- The 11-18 regression as a test: a fixture baseline from a binary/defaults state *without* a knob, a "next" state that adds it under the hardening default → verify reports the CHANGE with the right service count and exits 2; `--accept` → exit 0; one more change → exit 2.
- Boot-entry fixtures: blank `initrd` line, missing `root=`, missing kernel file, missing subvolume → each FAIL.
- Gate tests: `schema-snapshot arm` and `schema-ctl reboot` refuse on exit 1/2 and pass with `--force` (with reboot stubbed).
- Before release: one schema-vmtest boot of a VM where the guard is armed via the gated path, and one deliberately broken fallback entry that verify must FAIL.

## Left to the builder

- Exact serializer field list and naming beyond the ones named above.
- How CHANGE lines are summarized/grouped in the human report.
- Where grubenv/BLS parsing lives in C and how much is shared with `schema-snapshot`.
- Whether `check_armable()` in `schema-snapshot` calls `schema-ctl verify` or keeps a thin copy for `--force` paths.
- Kernel-log reading mechanism (`/dev/kmsg` vs `klogctl`) and the exact error patterns.
