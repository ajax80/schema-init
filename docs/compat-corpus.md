# Compatibility corpus

How much of a real systemd unit survives `schema-import`, measured directive by
directive, not argued. `tools/compat-corpus.py` runs every `*.service` in the
directories it's given through the importer, the same `unit_to_svc` the RPM
trigger and `--enabled` use. Dependencies resolve against the other units there,
and a sibling `.socket` is the activating socket. Every imported `.svc` then goes
through PID 1's own loader (`schema-init --check`).

    make schema-init
    tools/compat-corpus.py --skip-systemd --label fedora44 --json f44.json /usr/lib/systemd/system

## What each directive is

| class | meaning |
|---|---|
| **mapped** | the importer consulted it and no warning names it |
| **warned** | consulted, and the generated `.svc` carries a `# schema-import: WARN` saying what is lost |
| **silent** | the importer never consulted it for this unit: lost without a word |
| **inert** | no runtime effect in the `.svc`: `Description=`, `Documentation=`, `[Install]` (the enable step reads that) |
| **skipped-unit** | the unit was refused as a whole; its directives aren't classified |

*Consulted* is measured, not guessed: each directive name is a `str` subclass
that records any comparison that matched it (`==`, `in (...)`, dict membership,
`startswith`). A key read only on a branch this unit doesn't take is silent
**for this unit**, which is the right answer: its value had no effect.

The JSON record keeps every unit's status, skip reason, warnings, loader
verdict, and each directive's section, key, value and class, for diffing runs
and distros.

## First run: Fedora 44 (blakbox, 2026-11-25, master `72d5cd5`)

302 non-systemd `.service` units: **221 imported, 81 skipped**. PID 1's loader
accepts every imported `.svc`. In the imported units, by directive occurrence:

| mapped | warned | silent | inert |
|---|---|---|---|
| 1625 | 284 | **461** | 491 |

Top skip reasons: `Type=forking` without an absolute `PIDFile=` (21), template
units (`foo@.service`, instances unsupported), and `StandardInput=` (11).

### What is lost silently (imported units)

| directive | units | what it means |
|---|---|---|
| `Unit.After` | 111 | **ordering is not translated.** `dep=` comes only from `Requires=`/`Requisite=`/`BindsTo=` (plus the mount-fstab and dbus heuristics) |
| `Unit.Before` | 78 | the same, reversed |
| `Unit.Wants` | 52 | soft dependencies aren't translated. `Wants=X` + `After=X`, systemd's usual "start after X if it's around", becomes nothing |
| `Service.RemainAfterExit` | 48 | probably harmless: a finished oneshot stays terminal here |
| `Unit.Conflicts` | 32 | starting one doesn't stop the other |
| `Service.ExecStop` | 17 | system imports stop by SIGTERM alone, by design (user imports map it), but without a warning |
| `Service.NotifyAccess` | 12 | |
| `Unit.StartLimit*` / `Service.StartLimit*` | 20+ | restart limits not mapped to `max_restarts=` |
| `Service.OOMScoreAdjust` | 7 | |
| `Service.StandardOutput` / `StandardError` | 12 | |
| `Unit.OnFailure` | 6 | failure handlers never run |

**The finding that matters:** ordering-only and soft dependencies (`After=`,
`Before=`, `Wants=`) are the most common silent loss. A service imported from a
unit that says `Wants=foo.service After=foo.service` can start before `foo`.
Translating them, and how they should map onto schema-init's model, is a design
decision. This report only measures it.

## Next

1. **Debian, Ubuntu and Arch:** the same run over each distro's unit tree, from
   pinned package versions fetched and extracted at run time (not vendored), so
   the report is reproducible.
2. **Enforcement, not only acceptance:** "mapped" means the importer read it and
   PID 1's loader takes the result. The next step is checking that the setting is
   *in force* for the running service (the hardening vmtest does this for a few
   keys).
3. Turn the silent list into warnings or mappings, one directive at a time,
   starting with `After=`/`Wants=` once the mapping is decided.
