# schema-systemctl shim — design

**Date:** 2026-09-14
**Status:** approved for implementation
**Component:** `schema-systemctl` — a `systemctl(1)` drop-in for schema-init boxes
**Scope:** phase 1 of the compat-translator arc — the **shim only**. The
`.service`→`.svc` importer is a later night; here it is stubbed to a pending
queue. See [[project_schema_compat_translator]], [[project_schema_reclamation]].

## Purpose

Make RPM (and later deb) package scriptlets **succeed** on a box where
schema-init is PID 1 and systemd is not running. Fedora scriptlets call bare
`systemctl` from `%systemd_post`/`%systemd_preun`/`%systemd_postun`; on a
schema-init box the stock `systemctl` errors (no running systemd manager),
which aborts installs. The shim intercepts those calls, satisfies the exit-code
contract the scriptlets expect, and **captures enablement intent** so the
importer can later realize it as real `.svc` files.

This is the *translator* turn: reclamation stops being "rewrite each daemon"
and becomes "let whatever ships install cleanly, then import it."

## Non-goals (explicit, phase 1)

- **No `.service`→`.svc` importer.** `enable`/`preset` only record intent to a
  queue; no `.svc` is emitted tonight.
- **No deb path.** `deb-systemd-helper` / `deb-systemd-invoke` are a later
  concern; Fedora RPM first (blakbox is Fedora).
- **No real `mask`/`unmask` semantics.** Record-only, exit 0.
- **No `Environment=` handling.** That is an importer problem — flagged: the
  `.svc` parser has **no `env=` key**; add one on importer night.
- **Ratholes untouched** (per the agreed list): templated units (`foo@bar`),
  `.socket`/`.timer`/`.path`/`.target`, drop-ins, specifiers, dependency
  ordering, `Type=notify`. The shim log-and-skips these and exits 0.

## Language & delivery

- **C**, single libc-only binary, house style — ships next to `schema-ctl` in
  the **`schema-init-migrate`** subpackage (not the base package: installing
  schema-init must never change a box's init behavior on its own).
- Source: `schema-systemctl.c`. Build via the existing `BINS=` mechanism
  (add to `migrate_bins` in `schema-init.spec`, and to the `Makefile` bin
  list). Installed path of the binary itself: `%{_bindir}/schema-systemctl`.
- Shadowing of `/usr/bin/systemctl` is done by RPM scriptlets (below), not by
  the binary living at that path directly — so the file stays cleanly owned.

## Data flow & verb contract

Invocation: `systemctl [flags] <verb> [unit ...]`. Unit args may carry a
`.service` suffix or none.

| verb | action | exit |
|------|--------|------|
| `enable`, `preset` | append each resolved unit path to the queue (dedup) | 0 |
| `enable --now` | queue **and** `schema-ctl start <name>` if a `.svc` exists | 0 |
| `disable` | remove unit from queue; `rm` its `.svc` if present | 0 |
| `disable --now` | above **and** `schema-ctl stop <name>` | 0 |
| `daemon-reload`, `daemon-reexec` | `schema-ctl reload` (best-effort) | 0 |
| `start`, `stop`, `restart` | `schema-ctl <verb> <name>` if `.svc` exists; else no-op | 0 |
| `try-restart`, `reload-or-restart`, `reload` | restart only if active; else no-op | 0 |
| `is-enabled` | queued OR `.svc` exists → print `enabled`; else `disabled` | 0 / 1 |
| `is-active` | schema-ctl reports running → `active`; else `inactive` | 0 / 3 |
| `mask`, `unmask` | record-only marker (phase 1: log + exit 0) | 0 |
| `status` | best-effort `schema-ctl status` filtered to the unit | 0 |
| unknown verb | log `unhandled verb` to stderr | 0 |

**Exit-code contract is load-bearing.** Everything exits 0 **except**
`is-enabled` (1 when not enabled) and `is-active` (3 when inactive) — those two
communicate *state* through the exit code and scriptlets branch on them.
Everything else exiting 0 guarantees a scriptlet (`... || :`) never aborts an
install.

### Queue

- Path: `/var/lib/schema-init/pending.list`, one absolute unit path per line.
- `mkdir -p /var/lib/schema-init` (0755) before first write.
- **Dedup on append** (a unit enabled twice appears once).
- `disable` rewrites the file without the removed unit.
- This file is the **sole hand-off to importer night**: `schema-import` drains
  it. Nothing enabled during package installs is lost.

### Unit-name normalization

- Strip a trailing `.service` → schema-ctl service name (`foo.service` → `foo`).
- `.svc` path: `/etc/schema-init/services/<name>.svc`.
- **Skip + exit 0** (log `skipping <unit>: unsupported unit type`): any name
  containing `@` (templated), or ending `.socket`/`.timer`/`.path`/`.target`/
  `.mount`/`.slice`/`.scope`.

### Flags tolerated / ignored

Parse and ignore (must not be treated as unit args):
`--no-reload`, `--system`, `--user` (skip: exit 0), `--global` (skip),
`--quiet`/`-q`, `--no-ask-password`, `--no-block`, `--root=<path>` (ignored
phase 1), `--preset-mode=<x>`. `--now` is *acted on* (see table). Unknown
`--flags` are ignored with a stderr note, never consumed as units.

### schema-ctl coupling & degraded mode

- Lifecycle verbs shell to `schema-ctl <verb> <name>`.
- If the control socket is absent (box mid-install, not yet PID 1) or
  `schema-ctl` is missing: **exit 0** for lifecycle verbs (nothing to do yet),
  and `is-active` → inactive (3). Queue writes still succeed — they are plain
  file ops independent of PID 1.

## Shadowing stock `systemctl` (RPM scriptlets)

RPM scriptlets resolve bare `systemctl` via `PATH=/usr/bin:/bin:/usr/sbin:/sbin`
(no `/usr/local/bin`), and Fedora's `systemd` package owns `/usr/bin/systemctl`
and cannot be cleanly removed. Therefore the shim must **win at
`/usr/bin/systemctl`** via a divert-and-restore in the `-migrate` scriptlets.

**`%post migrate`** (idempotent):

```
# Prefer alternatives if the distro manages systemctl that way.
# NOTE: Fedora does NOT manage systemctl via alternatives today, so this
# branch is a defensive guard and is not expected to fire on blakbox.
if alternatives --display systemctl >/dev/null 2>&1; then
    alternatives --install /usr/bin/systemctl systemctl \
        %{_bindir}/schema-systemctl 100
else
    # Divert the stock binary once. Guard so a reinstall/upgrade of
    # schema-init-migrate never overwrites systemctl.real with the symlink.
    if [ ! -L /usr/bin/systemctl ] && [ -f /usr/bin/systemctl ]; then
        mv /usr/bin/systemctl /usr/bin/systemctl.real
    fi
    ln -sf %{_bindir}/schema-systemctl /usr/bin/systemctl
fi
```

**`%postun migrate`** (restore only on true removal, not upgrade):

```
if [ $1 -eq 0 ]; then
    if alternatives --display systemctl >/dev/null 2>&1; then
        alternatives --remove systemctl %{_bindir}/schema-systemctl
    elif [ -f /usr/bin/systemctl.real ]; then
        rm -f /usr/bin/systemctl
        mv /usr/bin/systemctl.real /usr/bin/systemctl
    fi
fi
```

**Re-assert on systemd upgrade** — a `systemd` package update reinstalls
`/usr/bin/systemctl` and clobbers the symlink. A file trigger in the `-migrate`
subpackage re-diverts automatically:

```
%transfiletriggerin migrate -- /usr/bin/systemctl
# systemd (re)wrote /usr/bin/systemctl; re-assert the diversion.
if [ ! -L /usr/bin/systemctl ] && [ -f /usr/bin/systemctl ]; then
    mv -f /usr/bin/systemctl /usr/bin/systemctl.real
    ln -sf %{_bindir}/schema-systemctl /usr/bin/systemctl
fi
```

This makes the diversion self-healing across systemd upgrades — the one edge
that a plain rename cannot survive on its own.

## Error handling

- Never abort a scriptlet: every path exits 0 except the two state-reporting
  verbs.
- `mkdir -p` the state dir before writing; a write failure logs to stderr and
  still exits 0 (install proceeds; intent loss is logged, not fatal).
- Malformed argv (no verb) → usage to stderr, exit 0 (scriptlet safety).

## Testing (TDD — write tests first)

Harness under `tests/` (shell, matching the existing live/migrate harnesses),
no live PID 1 required — point the shim at a temp `--root`-style env via
overridable `SCHEMA_STATE_DIR` / `SCHEMA_SVC_DIR` env vars (add these knobs so
tests need no root and no socket; default to the real paths).

Cases:
1. `enable foo.service` → queue has `.../foo.service`, exit 0.
2. `enable` twice → queue has one line (dedup).
3. `disable foo` → line removed, exit 0; removes stub `.svc` if present.
4. `is-enabled foo` → exit 1 when absent, exit 0 + `enabled` when queued.
5. `is-active foo` → exit 3 when not running (socket stubbed absent).
6. `preset foo.service bar.service` → both queued.
7. `daemon-reload` → exit 0 with socket absent.
8. Template skip: `enable foo@bar.service` → not queued, exit 0, stderr note.
9. Unit-type skip: `enable foo.socket` → not queued, exit 0.
10. Unknown verb `frobnicate` → exit 0, stderr note.
11. Flag handling: `enable --now --quiet foo.service` → queued; `--now`/`--quiet`
    not treated as units.
12. No-verb / empty argv → exit 0.

Wire into `make test` (C-level) where practical; the scriptlet-shadowing
`%post`/`%postun`/`%transfiletriggerin` logic is verified in the migrate
harness (`tests/migratetest`) against `stock-clean.qcow2`, not in unit tests.

## Files touched

- **new** `schema-systemctl.c`
- `Makefile` — add `schema-systemctl` to the build/install bin lists
- `schema-init.spec` — add to `migrate_bins`, `%files migrate`, and the
  `%post`/`%postun`/`%transfiletriggerin migrate` scriptlets above
- **new** `tests/` harness for the shim (+ `make test` hook)
- (later, importer night) `service.c`/`service.h` gain an `env=` key

## Open follow-ups (not this session)

- Importer (`schema-import`) that drains `pending.list` → `.svc`.
- `env=` key in the `.svc` parser for `Environment=` mapping.
- deb scriptlet path.
- `mask`/`unmask` real semantics.
