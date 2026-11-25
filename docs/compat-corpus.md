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

## Next

1. **Debian, Ubuntu and Arch:** the same run over each distro's unit tree, from
   pinned package versions fetched and extracted at run time (not vendored), so
   the report is reproducible.
2. **Enforcement, not only acceptance:** "mapped" means the importer read it and
   PID 1's loader takes the result. The next step is checking that the setting is
   *in force* for the running service (the hardening vmtest does this for a few
   keys).
3. Turn the silent list into warnings or mappings, one directive at a time,
   next `RemainAfterExit=` and `Conflicts=`.
