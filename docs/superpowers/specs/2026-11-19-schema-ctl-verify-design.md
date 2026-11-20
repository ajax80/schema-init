# schema-ctl verify — design

2026-11-19. Status: decisions settled with Jonathan 2026-11-19 (Approach 1; baseline option A; builder-check calls 1–5 approved).

## Why

Two real boot failures on blakbox, neither of which a syntax check would have caught:

1. **11-18, reboot #3.** Every `.svc` parsed. What changed was the new binary's *defaults*: `syscall_deny=` followed the host hardening-default switch, 63 untouched host `.svc` gained the preset, `mount-home` lost `mount(2)`, and the box came up with no `/home`, no keyboard. A live re-exec had already put the new binary in place, so the running PID 1 looked fine. Then all three fallbacks failed: the snapshot BLS entry had a blank `initrd` line (grub blscfg stops at it → no `root=` → panic), `schema-good-20260915`'s subvolume had been deleted while its entry and a stale `grub-btrfs.cfg` still pointed at it, and Eli couldn't ssh in. Recovery was booting systemd by hand.
2. **09-28, Seagate.** A drive on the mount path died: 150 s of mount retries, then a hung POST. The POST hang is out of reach; "a device the boot needs is missing or throwing I/O errors" is not.

Plus the 11-18 Optiplex find: the boot guard reported armed while grub.cfg lacked `09_schema_fallback` and no `schema-bootok.svc` was loaded — an inert safety net.

The question `verify` answers: **"if I reboot now, will this box come back, and if it doesn't, will the way back work?"**

**Ground rule: `verify` never reports PASS for an input it did not actually resolve.** If it can't determine which entry GRUB will pick, which root filesystem, which `init=` binary or which service directory the next boot uses, it says so (`[UNRESOLVED] …`) and that counts as FAIL for re-exec/arm and as WARN-and-proceed for an interactive reboot. It never substitutes the running system's values and implies it checked the next boot.

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
- Bootloaders other than GRUB+BLS and the Pi in v1 (no `/boot/grub2/grubenv` or no BLS entries directory — Debian's `/boot/grub`, systemd-boot): Next boot, Services and Ways back each report `[note] not applicable: unsupported bootloader (v1)` and never contribute exit 1/2/3; disk checks still run. A dangling grubenv symlink, or a missing grubenv while fstab's `/boot` is unmounted, is `[UNRESOLVED]`, not unsupported.

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
schema-init --check --root <target-root> --cmdline "<kernel options>" [--svc-dir <dir>]
```

- The binary run is the **candidate** — `<target-root>/<init= path>` from the entry being checked — never the installed one by assumption.
- Every input is read under `--root`: `<root>/etc/schema-init/hardening-default`, the service directory, `.grp` files. `--svc-dir` overrides the directory explicitly; otherwise it is chosen by the same rule PID 1 uses, applied to `--cmdline` (`SCHEMA_SVC_DIR=` on the cmdline, validated as PID 1 validates it; the Pi wrapper's rule, §7). The PID-1 startup path's env handling is reproduced deliberately in this mode, not inherited.
- Loads with the **same** `services_load()` / `service_parse` path PID 1 uses, drop-ins included, and the same cycle check (`services_check_cycles`).
- Resolves the hardening default the way `pid1_hardening_default()` does (`init.c:236`), from `--cmdline` and the root's file, not `/proc/cmdline` and the live `/etc`.
- Prints:
  - every load error, refusal ("hides exec — not loaded"), dropped default, and cycle, as `error:` / `note:` lines;
  - for every loaded service, its **effective settings** as canonical, sorted `name.key=value [source]` lines.
- Exit 0 if everything loaded, 1 otherwise.

**Serializer contract.** One function in `service.c`, run on the already-parsed `service_t` (no second parser), also used by `schema-ctl status <svc>`'s hardening block where fields overlap. It must emit **every parsed property that can affect loading, execution, readiness, dependency order, privilege, isolation or lifecycle**, at minimum:

| Category | Fields |
|---|---|
| Execution | exec, full argv, exec_pre / exec_stop / exec_stop_post / exec_reload lines (with `+`/`-` prefixes), expand_args |
| Identity | user, group, supplementary groups, dynamic_user, ambient/keep caps |
| Environment | env= values, env files named |
| Filesystem | working_dir, runtime/state/cache/logs dirs, mount-ns knobs, ns_kernel knobs, landlock rules, umask |
| Security | no_new_privs, seccomp (mode, the resolved syscall set as a hash + count, error action, flags), syscall_deny |
| Dependencies | dep=, group membership, critical |
| Readiness | ready mode (notify / bus name / path / pid_file), ready paths, timeouts |
| Lifecycle | oneshot, restart policy and statuses, restart_sec, kill mode/signals, watchdog, timers, watch_path |
| Resources | priority, nice/io/sched, rlimits, cgroup keys |

A test enumerates the `.svc` keys the parser accepts and fails if one has no serializer line — so a new key can't silently escape the baseline.

**Value vs source.** Each line carries the effective value and, separately, its source (explicit / default / dropped). Only **value** changes are CHANGEs and go into the accept hash; a source-only change (explicit `no_new_privs=1` replacing a defaulted `1`) is shown as a note, never blocks.

### 2. The baseline — what last booted fine

A boot counts as good (Jonathan, option A):
- **Where `schema-bootok` is installed** (desktop boxes): `schema-bootok` itself, right after it sets `boot_success=1`, runs `schema-ctl verify --write-baseline`. Direct call, not a second service polling grubenv.
- **Everywhere else**: a small packaged oneshot (`schema-verify-baseline.svc`, rpm and deb) waits for the rail to settle, then runs the same command. It exits at once when `schema-bootok.svc` is loaded, so the stronger proof wins. It depends on the persistent `/var` mount (the same discipline as the timer stamps in `engine.c`), so it can't write into the tmpfs underneath.
  - *Settled*: on `CLOCK_MONOTONIC`, 60 s in which every loaded service is in a healthy state (FUNDAMENTAL, SETTLED, FULL_TRUST, PERFECT; oneshots that finished PERFECT; timers/watch_path services waiting for their first fire count as healthy) and no service restarts. A restart or a service going unhealthy restarts the 60 s. The exact state list is the builder's to confirm against `engine.c`.

`--write-baseline` runs `schema-init --check` with the **running** binary (`/proc/1/exe`) and the **running** cmdline, and writes `/var/lib/schema-init/verify/baseline` atomically (temp file + `rename`, `O_NOFOLLOW`, root-owned dir `0700`, file `0600`). A failed write is logged and shows up in the next `verify`.

**Baseline file format** is versioned: first line `schema-verify-baseline v1`, then `binary_version=`, `created=`, the boot-critical cmdline inputs (below), then the service lines.

**Boot-critical cmdline inputs** are part of the baseline: `root=`, `rootflags=`, `init=`, `rd.*`, `modprobe.*`, `SCHEMA_SVC_DIR=`, the hardening-default override. A change in any of them between baseline and next boot is a CHANGE (valid-looking entry, different root = still a different boot). The rest of the cmdline (kernel version in paths, `quiet`, etc.) is not compared.

**Missing baseline, three cases:**
1. Never established (no `/var/lib/schema-init/verify/established` marker): `WARN no baseline`, never blocks. Covers first install and the first upgrade onto this version (the old binary has no `--check`).
2. Writer hasn't succeeded since the marker appeared: same as 3.
3. Marker present but baseline missing, unreadable, or wrong format version: **blocking** (exit 2, like an unaccepted CHANGE, but not acceptable); cleared by the next good boot rewriting it, or `--force`. Losing the baseline must not quietly switch change detection off.

The first upgrade onto the version that ships `verify` has no baseline (the old binary has no `--check`): `WARN no baseline`, never a block, once.

Why not compare against the live PID 1: after a live re-exec the running PID 1 already *is* the new binary with the new rules, while the processes it supervises still carry the old sandbox. That is exactly the 11-18 state, and a live comparison would show nothing. "What last proved a good boot" is the honest reference.

No baseline yet (first install, bootok not wired): `verify` reports `WARN no baseline` and diffs nothing; the first good boot writes one.

### 3. `schema-ctl verify` — the report

Read-only. Needs root for the boot-entry and kernel-log reads; as a normal user it runs what it can and says what it skipped.

Sections, in order:

**Which entry boots next.** Three entries are resolved and reported separately, from grubenv + the generated grub.cfg:
1. **Actual next boot** — what GRUB picks on the next boot: `next_entry` if set (one-shot), else `saved_entry`/default, then the `09_schema_fallback` rule applied (`boot_counter` set, `boot_success=0`, counter `0`/`-1`, `schema_fallback_id` set → the fallback). This one drives the main FAIL decision.
2. **Normal default** — the entry picked when no one-shot and no fallback condition applies.
3. **Guard fallback** — `schema_fallback_id`, else the newest `schema-snap-*` (what `arm` would set).
`saved_entry` decides only when grub.cfg's header sets `default="${saved_entry}"`; a numeric default (or an unset `saved_entry`) is a position in blscfg's menu order — F44 blscfg (grub2-2.12-60) compares the entry file names, `.conf` stripped, split at the last two `-` into name, version, release, each by rpm's vercmp, newest first; it ignores the `version` key — which resolves only when no entry was left unread and no menu entry precedes `blscfg`. If the selection can't be resolved (unknown grub.cfg layout, an entry id that matches nothing), that's `[UNRESOLVED]`, never PASS. A `schema_fallback_id` naming no entry is FAIL.

**Entry checks** (applied to each of the three):
- the entry file exists; no blank or whitespace-only line before its last key;
- the options line is parsed as kernel arguments (quoting respected); duplicate or conflicting `root=`, `rootflags=` or `init=` → FAIL; no `root=` → FAIL;
- `root=` resolves to a present device — **the entry's** device, not the running root's;
- with no `initrd`, `root=` is a form the kernel resolves alone (`PARTUUID=`, `PARTLABEL=`, `/dev/<name>`, `major:minor`) — `UUID=`/`LABEL=`/`/dev/mapper/…` there → FAIL (blakbox 09-16 panic); an `initrd` of only `$variables` → UNRESOLVED;
- `rootflags=subvol=` names a subvolume that exists on that device;
- the `linux` path and **every** `initrd` path the entry references exist;
- `init=` is executable inside the target root; when it is schema-init, `<init> --version` runs;
- the target service directory can be determined and `--check` on it exits 0.

**Next boot**
- The entry checks above, on the actual next entry.
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
- A fallback or other snapshot entry whose `init=` is a schema-init older than `--check` → WARN "predates --check; services not checked" (it still boots); the actual next boot predating `--check` stays UNRESOLVED.
- `/boot/grub2/grub.cfg` contains the `09_schema_fallback` block **as live code** — the `set default="${schema_fallback_id}"` line inside the `boot_counter`/`boot_success` condition, outside comments, and after the point where the header sets `default` (or it would be overridden) — not just the variable name. The ESP grub.cfg is a stub. `schema-bootok` is installed, its `.svc` loads in `--check`, and its `exec=` exists and is executable. These replace today's grep-based `check_armable()` rules.
- Any of the first three failing → FAIL. A guard that can't fire is a FAIL, not a warning.

**Disks** — fstab entries are classified first, and the diagnostic names the class:
- **Local block-backed, required** (no `noauto`/`nofail`, a `UUID=`/`PARTUUID=`/`LABEL=`/`/dev/…` source, a disk filesystem type): source doesn't resolve to a present block device → FAIL.
- **Local block-backed, `nofail`**: missing → WARN.
- **Swap**: missing device → WARN.
- **Network** (nfs, cifs, sshfs…), **virtual** (tmpfs, proc, sysfs…), **bind** mounts, **automount** (`x-systemd.automount`), and layered sources (`/dev/mapper/…` for a not-yet-opened LUKS) → device presence not checked; noted, never FAIL.
- Kernel log since boot: I/O errors, `EXT4-fs error`, btrfs `corrupt`/`errs:` on any block device → WARN naming the device.

```
[FAIL] /home — required btrfs device UUID=… not present
[WARN] /mnt/nas — network filesystem; device presence not checked
[WARN] /backup — nofail mount is currently unavailable
```

**Result**
```
Result: 0 fail, 1 change (not accepted), 2 warn
```
Exit: 0 = pass (WARNs allowed); 1 = any FAIL; 2 = no FAIL but unaccepted CHANGE.

`--quiet` prints only the result line and non-PASS lines (for the gates).

### 4. Accepting a change set

`schema-ctl verify --accept` (root): runs verify; if there's no FAIL, writes the SHA-256 of the canonical CHANGE lines (values only, not sources) to `/var/lib/schema-init/verify/accepted` — atomic temp + `rename`, `O_NOFOLLOW`, same root-owned `0700` dir. Every gate re-runs verify on the current files; it never trusts an earlier report, so an edit made after `--accept` produces a new hash and blocks. While the current CHANGE set hashes the same, CHANGE no longer blocks (exit 0, lines still shown as `[CHANGE accepted]`). Any further change produces a new hash and blocks again. FAILs can never be accepted. The next good boot rewrites the baseline, which empties the CHANGE set naturally.

### 5. Gates

| Step | Behavior on exit 1 or 2 | Override |
|---|---|---|
| rpm `%posttrans` / deb trigger live re-exec | skip the re-exec; banner with the `--quiet` report and "run `schema-ctl verify`; `schema-ctl reexec` after accepting" | run `schema-ctl reexec` by hand after accepting |
| `schema-ctl reexec` (manual) | refuse | `--force` |
| `schema-snapshot arm` | refuse, print report | `--force` |
| `schema-ctl reboot` / `poweroff`, **interactive only** | refuse, print report | `--force` |

**Only terminal-attached callers are gated** (stdin and stderr are a tty). That is not proof a human typed it — a script started from a terminal inherits the tty and is gated too, which is acceptable: it is still someone at a keyboard about to reboot. The system's own callers are never terminal-attached (services, scriptlets, cron); the ones that must never block also pass `--force` explicitly, so the rule doesn't rest on tty detection alone. That covers `schema-ctl reboot`/`poweroff`, `sudo reboot` and `systemctl reboot` typed in a shell, which all arrive at `schema-ctl` through the shim or `schema-pi-halt`. Scripted callers are never gated, so a broken box can always go down: `schema-pi-deadline`'s recovery loop (`until schema-ctl reboot`), the dbus/udev flip healthchecks, `schema-flip-apply.sh`, scriptlets, cron.

Three distinct outcomes, worded distinctly:
- **Verifier unavailable** (binary missing, crashed, timed out): re-exec and arm refuse; interactive reboot/poweroff print `[WARN] verification unavailable; proceeding with reboot` and go — never strand a box that needs to go down.
- **Verifier ran, found FAIL** (exit 1) or **unaccepted CHANGE / lost baseline** (exit 2): the gate refuses, showing the findings.

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

Added from the 11-19 outside review (all expected results as specified above): `next_entry` differing from `saved_entry`; guard exhausted with `boot_success=0` → the fallback is the actual next entry; `schema_fallback_id` naming no entry → FAIL; candidate root device differing from the running one → the candidate is inspected; candidate subvolume with a different `hardening-default` → its file is used; `SCHEMA_SVC_DIR` picking the Pi known-good dir → that dir is checked; argv change with the same exec → CHANGE; `exec_pre` change → CHANGE; baseline unreadable after establishment → blocks; bootok success → baseline written; baseline oneshot ordered after `/var`; absent NFS mount → noted, not FAIL; absent required local fs → FAIL; fallback variable only in a grub.cfg comment → FAIL; files edited after `--accept` → blocks again; source-only change → note, exit 0.

The 11-18 regression is tested **twice**: with fixtures (proves the comparison), and with real binaries — dev build `434dcac` (the 11-18 build, `syscall_deny` following the hardening default) against v0.4.27 (no `syscall_deny`), same `.svc` set and hardening default ON — proving the resolver and serializer actually expose the change.

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
