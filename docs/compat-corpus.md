# Compatibility corpus

How much of a real systemd unit survives `schema-import`, measured directive by
directive, not argued. `tools/compat-corpus.py` runs every `*.service` in the
directories it's given through the importer's translator, `unit_to_svc`, which
the RPM trigger and `--enabled` also use, after the same `_denied` check. The
unit's drop-ins in that tree (`<unit>.d/*.conf`) are merged as systemd merges
them. Dependencies resolve against the other units there. A sibling `.socket` is
passed as the activator when the service names it or the socket is enabled by
default. Every imported `.svc` then goes through PID 1's own loader
(`schema-init --check`), and a nonzero exit counts as a refusal.

**Not modelled**, because it decides *which* units a real import takes, not how
one translates: enabled state, units owned by a timer or path unit, dependency
cycle dropping, and an existing `.svc` that already claims the same `exec=`.

    make schema-init
    tools/compat-corpus.py --skip-systemd --label fedora44 --json f44.json /usr/lib/systemd/system

## What each directive is

| class | meaning |
|---|---|
| **mapped** | the importer consulted it and no warning names it |
| **warned** | consulted, and the generated `.svc` carries a `# schema-import WARN:` line, starting with that directive, saying what is lost |
| **silent** | the importer never consulted it for this unit: lost without a word |
| **inert** | no runtime effect in the `.svc`: `Description=`, `Documentation=`, the `[Install]` keys only the enable step reads (`WantedBy=`, `RequiredBy=`, `UpheldBy=`, `Also=`, `DefaultInstance=`), and `Wants=` (`--enabled` imports what it names and every `.svc` starts on its own; without `After=`, systemd orders nothing either) |
| **skipped-unit** | the unit was refused as a whole; its directives aren't classified |

*Consulted* is measured, not guessed: each directive name is a `str` subclass
that records any comparison that matched it (`==`, `!=`, `in (...)`, dict and set
membership, `startswith`, `endswith`). A read the tracker can't see (a key
lowered or sliced before comparing) would show as silent: a false alarm, never a
false all-clear. A key read only on a branch this unit doesn't take is silent
**for this unit**, which is the right answer: its value had no effect.

The JSON record keeps every unit's status, skip reason, warnings, loader
verdict, and each directive's section, key, value and class, for diffing runs
and distros.

## First run: Fedora 44 (blakbox, 2026-11-25, master `72d5cd5`)

302 non-systemd `.service` units: **211 imported, 91 skipped**. PID 1's loader
accepts every imported `.svc`. In the imported units, by directive occurrence:

| mapped | warned | silent | inert |
|---|---|---|---|
| 1566 | 273 | **443** | 452 |

Top skip reasons: `Type=forking` without an absolute `PIDFile=` (21), template
units (`foo@.service`, instances unsupported), `StandardInput=` (11), and units
schema-init owns itself (`_denied`, e.g. 6 for core dumps).

### What is lost silently (imported units)

| directive | units | what it means |
|---|---|---|
| `Service.RemainAfterExit` | 44 | probably harmless: a finished oneshot stays terminal here |
| `Unit.Conflicts` | 31 | starting one doesn't stop the other |
| `Service.ExecStop` | 17 | system imports stop by SIGTERM alone, by design (user imports map it), but without a warning |
| `Service.NotifyAccess` | 12 | |
| `Unit.StartLimit*` / `Service.StartLimit*` | 20+ | restart limits not mapped to `max_restarts=` |
| `Service.OOMScoreAdjust` | 7 | |
| `Service.StandardOutput` / `StandardError` | 11 | |
| `Unit.OnFailure` | 6 | failure handlers never run |

**The finding that mattered:** ordering-only and soft dependencies (`After=`,
`Before=`, `Wants=`) were the most common silent loss (102, 76 and 51 units).
They are now mapped: `After=`/`Before=` on a unit with a `.svc` become
`after=`/`before=`, ordering only (see how-it-works.md), and `Wants=` is inert as
above. Re-run on the same tree: **mapped 1744, warned 273, silent 214, inert 503**,
and PID 1's loader still takes every imported `.svc`.

## Second run: Debian, Ubuntu, Arch (2026-11-26)

    make compat-corpus          # network: fetches, then runs all four

`tools/compat-fetch.py` reads `tools/compat-corpus.lock.json`: for each distro a
dated snapshot archive (snapshot.debian.org, snapshot.ubuntu.com,
archive.archlinux.org, all 2026-11-20, URLs that never change) and the packages
pinned in it by version and sha256. It downloads each one, refuses a hash that
doesn't match, and extracts only the package's systemd unit dir and its drop-ins.
Each tree is rebuilt from the hash-keyed cache on every run. Nothing is vendored.
Arch packages and Ubuntu's are zstd, so it needs Python 3.14. `--resolve` re-pins the lock from the snapshots; change the
dates in the lock first to move forward.

The package set is the same ground on each: ssh, web and database servers,
printing, Bluetooth and networking daemons, virtualization and containers, the
desktop session services, storage (LVM, mdadm, NFS, Samba, iSCSI, multipath),
firewalls, and cloud and VM guest agents. Debian 76 packages, Ubuntu 84 (plus snapd,
apport and other Ubuntu-only units), Arch 74.

A foreign tree runs with `--foreign`, so the results don't depend on the machine
running the tool. Machine-fixed conditions are judged against one reference host:
bare-metal x86-64, cgroup v2, outside any user namespace, with AppArmor for
Debian and Ubuntu (their default) and without it for Arch. A bare `ExecStart=`
name counts as found on `PATH`.

| tree | units | imported | mapped | warned | silent | inert |
|---|---|---|---|---|---|---|
| Fedora 44 (host) | 302 | 211 | 1752 | 265 | 214 | 503 |
| Debian trixie | 146 | 95 | 943 | 150 | 127 | 276 |
| Ubuntu noble | 175 | 123 | 1083 | 172 | 133 | 354 |
| Arch | 172 | 130 | 1402 | 217 | 146 | 441 |

PID 1's loader accepts every imported `.svc` on all four.

**Silent losses are the same everywhere.** Here is each directive's count of
imported units with that loss (Fedora / Debian / Ubuntu / Arch):

| directive | F | D | U | A |
|---|---|---|---|---|
| `Service.RemainAfterExit` | 42 | 24 | 32 | 23 |
| `Unit.Conflicts` | 31 | 19 | 18 | 27 |
| `Service.ExecStop` | 17 | 12 | 12 | 11 |
| `Service.NotifyAccess` | 12 | 8 | 7 | 12 |
| `Install.Alias` | 5 | 8 | 7 | 8 |
| `Service.OOMScoreAdjust` | 7 | 3 | 5 | 7 |
| `Service.StandardOutput` / `StandardError` | 6/5 | 3/4 | 5/4 | 5/5 |
| `Service.TimeoutStartSec` | 5 | 4 | 3 | 7 |
| `Service.KeyringMode` | 8 | 5 | 2 | 4 |
| `Unit.OnFailure` | 6 | 3 | 3 | 1 |

So the work on silent losses doesn't depend on the distro, and the order in item 3
below still holds.

**Skips are the same too.** `Type=forking` without an absolute `PIDFile=` comes first
on every distro (Debian 14, Ubuntu 13, Arch 12), and template units come second.
Two more reasons are correct as they stand. All 16 cloud-init units gate on
`ConditionEnvironment=!KERNEL_CMDLINE=cloud-init=disabled`, an environment
variable that only systemd's generators set. The VMware and AppArmor units don't
hold on the reference host.

**Fixed from this run:** `ConditionVirtualization=!private-users` (Debian and
Ubuntu `iscsid.service`) was "not understood", which skipped the whole unit. The importer
now judges it: a `/proc/self/uid_map` other than the initial namespace's identity
map means a user namespace.

(These counts are the importer after stage 3's fixes, below. A warning is now credited to the directive it is about, and to the `protect_*` level a `ReadWritePaths=` lowered.)

## Third run: in force, not only accepted (2026-11-26)

    make compat-enforce         # network + KVM; ~40 s from the package cache

"Mapped" used to mean the importer read the directive and PID 1's loader took
the `.svc`. `tools/compat-enforce.py` checks that the setting is actually in force
for the running process. It replaces each imported unit's daemon with
`tests/qemu-ci/enforce-probe` and keeps the rest of the `.svc`, then boots the real
`schema-init` as PID 1 in a QEMU guest with all of them at once. The probe reports
what it got: uid, groups, capability sets, NoNewPrivs, seccomp mode, umask, cwd,
rlimits, nice, OOM score, environment, namespaces, which mounts are read-only,
and whether the host's `/tmp`, `/home`, `/root` and `/dev/mem` are visible. Where
a flag could lie, the probe tries the operation instead: a W+X mmap
(`MemoryDenyWriteExecute=`), a personality switch (`LockPersonality=`),
`SCHED_FIFO` (`RestrictRealtime=`), a setuid chmod (`RestrictSUIDSGID=`),
`unshare` (`RestrictNamespaces=`), and `swapoff` on a path that doesn't exist
(`SystemCallFilter=`). `swapoff` is in `@swap`, outside `@system-service`, and the
seccomp mode alone can't decide this one: the engine builds `LockPersonality=` and
its kin as seccomp filters too. Each systemd directive is then judged against
what systemd itself would have given the process:

| verdict | meaning |
|---|---|
| enforced | the process has what the unit asked for |
| NOT-ENFORCED | it doesn't; for a warned directive the warning already said so |
| unobservable | the probe can't tell. A non-root process is refused `SCHED_FIFO` whatever the unit says, and a filter that allows `swapoff` can't be seen by trying it. Each is judged against a hardening-free root control probe in the same boot |
| unchecked | no oracle for it: mostly the lifecycle keys, which are the pid1-boot job's |

The harness strips lifecycle keys from the `.svc` (exec, deps, ordering,
conditions, readiness, restart), because a missing daemon or dependency would keep
the probe from running. It keeps the importer's own `install -d` exec_pre, since
that is how `ConfigurationDirectory=` gets created. A required `EnvironmentFile=`
is created empty, standing in for the one the package ships. PID 1 loads at most
`MAX_SERVICES` (512) `.svc` files, so the units go in batches of 480, one boot per
batch. All 211 Fedora units load under one PID 1. Units that claim the same `*Directory=` go to different boots.

| tree | probed under PID 1 | enforced | NOT (all warned) | unobservable |
|---|---|---|---|---|
| Fedora 44 | 211/211 | 572 | 32 | 19 |
| Debian trixie | 95/95 | 302 | 17 | 14 |
| Ubuntu noble | 123/123 | 273 | 19 | 13 |
| Arch | 130/130 | 470 | 30 | 12 |

**Nothing mapped without a warning is out of force** on any of the four trees.
Getting there took three importer fixes, each found by this run:

- **`ConfigurationDirectory=` was created owned by `User=`** (chrony, colord, kea).
  systemd.exec(5) says every `*Directory=` except that one goes to `User=`, and
  a config dir stays root's. A fresh one is now root's.
- **`ReadWritePaths=` under `/sys` was dropped without a word.** fprintd's
  `/sys/devices` stayed read-only under `ProtectKernelTunables=`. The importer
  only read write paths under `ProtectSystem=`/`ProtectHome=` dirs. Paths under
  `/sys` and `/proc/sys`, and under `/sys/fs/cgroup` with
  `ProtectControlGroups=`, now become `rw_path=`, which ns.c binds after the
  read-only mounts.
- **`ExecStart=!` with `User=` gave root-owned State/Runtime/Logs dirs** (chrony
  on Debian and Ubuntu). systemd still hands those dirs to `User=` when it runs
  the command as root. The importer now writes `dir_owner=USER[:GROUP]`, an engine
  key that sets the directories' owner apart from the run user. The `bang`,
  `plus` and `grp` fixtures check it under PID 1.

The NOT-ENFORCED directives that are all warned, by count: `CapabilityBoundingSet=` 69
(no bounding-set key, closed below), `ProtectHome=read-only`/`tmpfs` 9 and
`ReadOnlyPaths=` 9 (no equivalents), `ProtectSystem=strict` 4 (mapped to `full`).

CI runs the same harness on every PR (`tests/qemu-ci/compat-enforce.sh`, in the
`qemu-pid1` job) over `tests/fixtures/compat-enforce`, which has one synthetic unit
per family of directives. It also runs one control: `fs-tampered` has `private_tmp=1`
cut from its `.svc` and must come back NOT-ENFORCED. Each oracle was shown to fail
when its line is cut.

## The top four silent losses, closed (2026-11-26)

Each is now read for every imported unit. It is either kept, because schema-init
already behaves the same way, or warned with `not enforced:`:

| directive | kept | warned |
|---|---|---|
| `RemainAfterExit=` | on a oneshot: a finished oneshot stays done and its dependents go on | on any other type (one Arch unit): the exit is supervised |
| `Conflicts=` | a `.target`: schema-init starts none, and its shutdown stops every service | a service or socket (libvirtd, tlp, getty), on the unit or its activating `.socket`: starting one doesn't stop the other |
| `ExecStop=` / `ExecStopPost=` | `kill [-SIG] $MAINPID` with the unit's own kill signal (compared by number) | anything else: a system service is still stopped by its `kill_signal` alone (user imports map them) |
| `NotifyAccess=` | `main`/`exec`: PID 1 takes notify messages only from the service's own cgroup | `all` on a notify unit (no `MAINPID=` hand-off on system imports), `none`, and any value on a non-notify type (no notify socket) |

Silent directives after the change: Fedora 105, Debian 57, Ubuntu 56, Arch 66.
What tops the list now is `Install.Alias`, `StartLimit*`, `KeyringMode`,
`OOMScoreAdjust`, `TimeoutStartSec` and `StandardOutput`/`StandardError`.

## `CapabilityBoundingSet=` in force (2026-11-27)

The engine has a `bounding_caps=` key now. It only takes capabilities out of the
bounding set, as systemd does. `keep_caps=` also cuts the permitted and effective
sets, so a `user=` service would lose the setuid it needs. The importer folds the
unit's lines the way systemd does: an empty value is no capabilities at all, a `~`
line drops what it names, and later lines OR in or take out. A `~` list stays a
drop list, so capabilities newer than schema-init's table stay, as under systemd.
Two cases are warned. A list too long for the engine's 511-byte line is written in
the other form (colord on all four distros and virtlogd on Ubuntu and Arch: a
36-name `~` list). A bounding set that lacks an `AmbientCapabilities=` cap is
widened by it, because the engine refuses to load that.

Run 4 (`make compat-enforce`): 559/559 units probed under PID 1. All 92
`CapabilityBoundingSet=` units are in force (Fedora 34, Debian 14, Ubuntu 14,
Arch 30) and 6 of them are warned. NOT-ENFORCED is down to the warned
`ProtectHome=` 9, `ReadOnlyPaths=` 9, `ProtectSystem=strict` 4 and one `UMask=`.
The CI fixtures `sys` (keep list) and `capinv` (`~` list) both fail when the
engine skips the drop.

## Start limits, start timeout, OOM score (2026-11-27)

| directive | maps to | warned |
|---|---|---|
| `OOMScoreAdjust=` | `oom_score_adj=` | out of -1000..1000; in user mode, below schema-user's own score (the kernel refuses an unprivileged lower one) |
| `TimeoutStartSec=` / `TimeoutSec=` | `start_timeout_sec=` on notify, oneshot and forking (until READY=1, the exit, or the pid file's adoption, as systemd), and on dbus past its worst healthy promotion (90 s); `infinity` is `0`. A start timeout now follows an explicit `restart=` (the engine used to excise), as systemd's `Restart=` does | dbus under 90 s, since the timer would kill a healthy service. Simple/exec say nothing: systemd's start ends at fork, so the timeout never runs there either. No `Type=` with `BusName=` is dbus, as in systemd |
| `StartLimitBurst=` / `StartLimitIntervalSec=` | `max_restarts=` burst−1 (systemd counts the first start), only with `Restart=` on; `infinity` is a window that never closes, the strictest | always: the window isn't kept (the count resets after a 5-minute run). Interval or burst 0 (or past 10000), systemd's "no limit", keeps the engine's 5 restarts then a 300 s pause. Nothing is written when `RestartSec=` spaces the starts past the window (systemd never hits the limit). `StartLimitAction=` other than `none` |

Run 5: 559/559 probed, 0 NOT-ENFORCED unwarned. `OOMScoreAdjust=` is in
force on all 22 units that set it. Silent directives: Fedora 105→73, Debian
57→44, Ubuntu 56→44, Arch 66→44. The top of the list now is `Install.Alias`,
`KeyringMode`, `StandardOutput`/`StandardError`, `RefuseManualStart`, `OnFailure`,
`IgnoreSIGPIPE` and `RequiresMountsFor`.

## The silent list, emptied (2026-11-27)

Every directive the importer used to skip without a word is now read. Each one is
mapped, warned or inert:

- **Mapped:** `PermissionsStartOnly=yes` becomes `+` on each `ExecStartPre=`.
  `ExecStart=` keeps `User=`, and an `ExecReload=` that systemd would run as root is
  warned. `RequiresMountsFor=` was already the `mount-fstab` dep. It was listed as
  silent only because an `or` chain stopped before reading it, and it is now warned
  when no `mount-fstab` is there to wait for: on every corpus tree, since a foreign
  tree has none. `OOMPolicy=stop`/`kill` is what the engine's `memory.oom.group`
  already does.
- **Warned:**
  - Interim, until the engine has keys for them: `OnFailure=`, `KeyringMode=`,
    `StandardOutput=`/`StandardError=` other than the journal, and `IgnoreSIGPIPE=no`.
  - With no engine key: `Alias=` other than `dbus-org.*`, `Delegate=`,
    `RuntimeDirectoryPreserve=` (when there is a `RuntimeDirectory=`),
    `SendSIGKILL=no`, `SendSIGHUP=yes`, `NonBlocking=yes`, `FailureAction=` and
    `SuccessAction=`, `Syslog*=` and `LogLevelMax=`, `Sockets=` naming another unit's socket, `OOMPolicy=continue`,
    `MemoryKSM=`, `FileDescriptorStoreMax=`, encrypted credentials, `OnSuccess=`,
    `RefuseManualStart=`/`RefuseManualStop=`, and `BindTo=` (the old spelling).
- **Inert:** `IgnoreOnIsolate=`, `AllowIsolate=`, `OnFailureJobMode=` and
  `RestartMode=`. The engine has no isolate, no job queue and no failed state to
  show dependents during a restart.

Run 6: silent directives are 0 on all four distros (Fedora 73→0, Debian 44→0,
Ubuntu 44→0, Arch 44→0). The imported counts are unchanged, and PID 1's loader
still takes every `.svc`.

## Stdio, keyring, SIGPIPE in the engine (2026-11-27)

The engine has `stdout=`, `stderr=`, `keyring_mode=` and `ignore_sigpipe=` now, and
the importer maps `StandardOutput=`/`StandardError=` (`null`, `file:`, `append:`,
`truncate:`), `KeyringMode=private`/`shared` and `IgnoreSIGPIPE=no` onto them.
`StandardError=journal` under a redirected stdout writes `stderr=log`, because the
engine's unset stderr follows stdout as systemd's `inherit` does. What stays warned
is the `+console`, `tty`, `socket` and `fd` forms. One default still differs:
systemd gives a system service a private session keyring when `KeyringMode=` is
unset, while the engine keeps the inherited one. tests/test_schema_user.sh checks
each key in a running service: the bytes in the files, the SigIgn mask, and the
session keyring serial.

## Next

1. `OnFailure=` stays warned. Its corpus targets are `emergency.target`,
   `plymouth-quit`, snapd's and dnf's offline-upgrade cleanup: none is imported or
   exists here. An engine key would also need a standby state for the target, so it
   doesn't start at boot like every other `.svc`.
2. Open questions on the defaults, where the engine and systemd differ with no
   directive in the unit. systemd times out a notify unit at 90 s
   (`DefaultTimeoutStartSec=`), while the engine's default for daemons is no limit.
   systemd gives a oneshot no start timeout, while the engine kills and excises it
   at 90 s. And with `KeyringMode=` unset, systemd gives a system service a private
   session keyring, while an imported one keeps the inherited keyring.
