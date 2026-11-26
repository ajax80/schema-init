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
| Fedora 44 (host) | 302 | 211 | 1744 | 273 | 214 | 503 |
| Debian trixie | 146 | 95 | 945 | 148 | 127 | 276 |
| Ubuntu noble | 175 | 123 | 1085 | 170 | 133 | 354 |
| Arch | 172 | 130 | 1401 | 218 | 146 | 441 |

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

## Next

1. **Enforcement, not only acceptance:** "mapped" means the importer read it and
   PID 1's loader takes the result. The next step is checking that the setting is
   *in force* for the running service (the hardening vmtest does this for a few
   keys).
2. Turn the silent list into warnings or mappings, one directive at a time,
   next `RemainAfterExit=` and `Conflicts=`.
