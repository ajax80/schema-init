# schema-import --user (PR D) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `schema-user` runs the units the user enabled in `~/.config/systemd/user`, generated at every start by `schema-import --user`, with a skip list so nothing starts twice.

**Architecture:** `schema-import.py` gains a user mode (module-level context switched on by `--user`) that resolves the user-enabled closure, translates with the existing `import_one`/`unit_to_svc` path into a staging dir, swaps it into `$XDG_RUNTIME_DIR/schema-user/services/`, and writes a skip manifest. `schema-user` spawns it (10 s cap) before `engine_load`. `schema-ctl --user status` appends the manifest. Base package ships the importer in libexec plus `/etc/schema-init/user-skip`.

**Tech Stack:** Python 3 stdlib (importer), C (schema-user, schema-ctl), RPM spec, POSIX sh tests.

**Spec:** `docs/superpowers/specs/2026-10-22-schema-import-user-design.md` (refines §5–§6 of `2026-10-20-schema-user-supervisor-design.md`).

## Global Constraints

- **Enabled set = user-enabled only:** `.wants/`/`.requires/` of `default.target`, `sockets.target`, `timers.target`, `basic.target` in `$XDG_CONFIG_HOME/systemd/user` only, then `Wants=`/`Requires=` recursively. `/etc/systemd/user` and `/usr/lib/systemd/user` `*.target.wants` are never read.
- Unit file search path, highest first: `$XDG_CONFIG_HOME/systemd/user` → `/etc/systemd/user` → `/run/systemd/user` → `/usr/local/lib/systemd/user` → `/usr/lib/systemd/user` (system dirs prefixed by `MIGRATE_ROOT`).
- Session targets (skipped, reason `phase 2 session target`): `graphical-session.target`, `graphical-session-pre.target`, `plasma-*.target`, `gnome-session*.target`.
- Specifiers: `%h`→`$HOME`, `%t`→`$XDG_RUNTIME_DIR`, `%S`→`$XDG_STATE_HOME` (default `~/.local/state`), `%C`→`$XDG_CACHE_HOME` (`~/.cache`), `%E`→`$XDG_CONFIG_HOME` (`~/.config`), `%u` user name, `%U` uid, `%T`→`/tmp`, `%V`→`/var/tmp`, plus existing `%n %N %p %%`.
- Skip files: `/etc/schema-init/user-skip` + `$XDG_CONFIG_HOME/schema-user/skip`; one name per line, `#` comments, bare name = `.service`.
- Default `/etc/schema-init/user-skip` content: `pipewire.service`, `pipewire.socket`, `pipewire-pulse.service`, `pipewire-pulse.socket`, `wireplumber.service`, `syncthing.service`.
- Manifest: `$XDG_RUNTIME_DIR/schema-user/skipped`, lines `<unit>\t<reason>`.
- Import timeout in `schema-user`: **10 s**. Importer path: `/usr/libexec/schema-init/schema-import`.
- User mode never writes outside `$XDG_RUNTIME_DIR/schema-user/` and never runs the pending queue, `bus_overrides`, or `schema-ctl add`.
- System-mode output must stay byte-identical: existing `tests/test_import_units.py` and `tests/test_migrate_import_enabled.py` stay green unchanged.
- No `needs_root=` and no `user=` lines in user-mode `.svc` (a non-root engine refuses `needs_root=1`: `service.c:147`).
- Login is never affected: importer failure/timeout → `schema-user` logs and continues.

## Review Focus

1. **Stale services after disabling a unit** — a unit removed from `~/.config/.../default.target.wants` must vanish from the services dir at next start (Task 2 test `stale_svc_removed`).
2. **Importer hangs or crashes** — `schema-user` must still start and answer `status` within the 10 s cap (Task 3 tests `import_hang`, `import_fail`).
3. **Distro-enabled `dbus.socket`** in `/etc/systemd/user/sockets.target.wants` — must never be generated (Task 1 test `distro_wants_ignored`).
4. **Hand-written or leftover files in the services dir** — swapped away atomically, no half-written dir if the importer dies mid-write (Task 2 test `crash_leaves_old_dir`).
5. **`ExecStart=` reset in a drop-in** (`ExecStart=` empty then new value) — the override wins, not the original (Task 1 test `dropin_execstart_reset`).

---

### Task 1: User-mode unit resolution (paths, masks, drop-ins, enabled closure, skip list)

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-import.py` (new section "User mode" before `main`; `_unit_dirs`, `find_unit` consult the user context)
- Test: `tests/test_import_user.py` (new, standalone like `test_import_units.py`: `check(name, ok)`, exit non-zero on any FAIL)

**Interfaces:**
- Produces:
  - `user_context(home: str, uid: int, user: str, env: Mapping[str,str]) -> dict` — keys `config` (`$XDG_CONFIG_HOME` or `home/.config`), `runtime`, `state`, `cache`, `home`, `uid`, `user`.
  - `set_user_mode(ctx: dict | None) -> None` — module global `_USER`; `_unit_dirs()` returns the user search path when set.
  - `user_unit_text(name: str) -> str | None` — main file + drop-ins (`<name>.d/*.conf` across all dirs, lexical by file name, higher-precedence dir wins on equal names) concatenated in order; `None` if not found; raises `Masked` if the winning entry is a `/dev/null` symlink or empty file.
  - `read_skip_list(paths: list[str]) -> set[str]` — normalised to full unit names.
  - `user_enabled_set(skip: set[str]) -> tuple[list[str], list[tuple[str,str]]]` — `(units_to_generate, skipped)`; `skipped` entries `(unit, reason)` with reasons exactly `handled elsewhere (skip list)`, `phase 2 session target`, `masked`, `template unit`, `path unit`, `not found`.

- [ ] **Step 1: Write the failing tests** in `tests/test_import_user.py`, building a temp tree (`MIGRATE_ROOT` for system dirs, temp `HOME`/`XDG_CONFIG_HOME`):
  - `precedence`: same unit in `~/.config/systemd/user` and `/usr/lib/systemd/user` → text from `~/.config`.
  - `mask`: `~/.config/systemd/user/foo.service -> /dev/null`, foo enabled → skipped `("foo.service","masked")`; a masked unit nothing enables → not in skipped.
  - `dropin_order`: `/usr/lib/.../foo.service.d/10-a.conf` and `~/.config/.../foo.service.d/20-b.conf` both applied, 20 after 10.
  - `dropin_execstart_reset`: drop-in with `ExecStart=` then `ExecStart=/bin/new` → translated exec is `/bin/new` (assert via Task 2's translation once it exists; here assert `user_unit_text` contains both lines in order).
  - `closure`: `default.target.wants/a.service`, `a` has `Wants=b.service`, `b` has `Requires=c.socket` → generate `[a.service, b.service, c.socket]` (order irrelevant).
  - `distro_wants_ignored`: `/etc/systemd/user/sockets.target.wants/dbus.socket` and `/usr/lib/systemd/user/default.target.wants/x.service` exist → neither generated nor listed.
  - `session_target`: `~/.config/.../graphical-session.target.wants/gammastep.service` → skipped `("gammastep.service","phase 2 session target")`.
  - `skip_list`: `syncthing.service` enabled, system skip file contains `syncthing` (bare) → skipped `("syncthing.service","handled elsewhere (skip list)")`; per-user file adds another; comments/blank lines ignored.
  - `template_and_path`: `foo@.service` and `bar.path` enabled → skipped with `template unit` / `path unit`.
  - `fleet_optiplex`: only `obsidian-tunnel.service` enabled (real unit text from the spec) → generate `[obsidian-tunnel.service]`, skipped `[]`.
  - `fleet_blakbox`: `syncthing.service` enabled (unit in `/usr/lib`), `akonadi_control.service -> /dev/null` not enabled, default skip file → generate `[]`, skipped `[("syncthing.service","handled elsewhere (skip list)")]`.

- [ ] **Step 2: Run** `python3 tests/test_import_user.py` — Expected: fails (`AttributeError: ... user_context`).

- [ ] **Step 3: Implement** the Interfaces above. `Masked` is a new exception class next to `Skip`. A bare `X.service.wants`/`.requires` link counts the same as `.wants`. Do not touch `enabled_units()` (system mode).

- [ ] **Step 4: Run** `python3 tests/test_import_user.py` and `python3 tests/test_import_units.py && python3 tests/test_migrate_import_enabled.py` — Expected: all PASS.

- [ ] **Step 5: Commit** `feat(import): user-mode unit resolution — search path, masks, drop-ins, user-enabled closure, skip list`

---

### Task 2: User-mode translation, output swap, manifest, `--user` CLI

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-import.py` (`_specifiers`, `socket_lines` `%t`, user-mode post-filter, `_svc_graph(dirpath=None)`, `main`)
- Test: `tests/test_import_user.py` (extend)

**Interfaces:**
- Consumes: Task 1's `user_context`, `set_user_mode`, `user_unit_text`, `read_skip_list`, `user_enabled_set`.
- Produces:
  - `run_user_import(ctx: dict, out_dir: str, skip_paths: list[str], dry_run: bool=False) -> dict` — counts `{"generated": n, "skipped": n, "error": n}`; writes `out_dir` (via sibling staging dir `out_dir + ".new"`, then rename swap; old dir removed after) and the manifest at `os.path.join(os.path.dirname(out_dir), "skipped")`.
  - CLI: `schema-import --user [--out DIR] [-n]` — default `--out` = `$XDG_RUNTIME_DIR/schema-user/services`, default skip paths = `[MIGRATE_ROOT/etc/schema-init/user-skip, $XDG_CONFIG_HOME/schema-user/skip]`; exit 0 unless an exception/IO error; prints `user-import generated=%d skipped=%d error=%d`. `XDG_RUNTIME_DIR` unset → exit 2 with a message.

- [ ] **Step 1: Write the failing tests:**
  - `specifiers`: `ExecStart=%h/bin/x --run %t/sock --s %S --c %C --e %E --u %u --U %U --tmp %T %V` → args show `HOME`, runtime, `HOME/.local/state`, `HOME/.cache`, `HOME/.config`, user, uid, `/tmp`, `/var/tmp`.
  - `no_root_lines`: generated `.svc` has neither `needs_root=` nor `user=`; a unit with `User=nobody` gets a `# schema-import WARN:` line mentioning `User=`.
  - `socket_runtime`: `ListenStream=%t/foo.sock` → `listen=stream:<runtime>/foo.sock`.
  - `deps_in_set_only`: `a` `After=`/`Requires=` `b.service` (in set) and `network-online.target` → `dep=b` kept, no dep on network-online.
  - `cycle_dropped`: `a Requires=b`, `b Requires=a` → output loads without a cycle (at most one of the two `dep=` lines kept).
  - `obsidian_svc`: Optiplex fixture → `obsidian-tunnel.svc` has `exec=/usr/bin/ssh`, `args=-N` … `args=blakbox`, and no `no_restart=1` (Restart=always).
  - `stale_svc_removed`: run once with `a` enabled, remove the wants link, run again → `a.svc` gone.
  - `crash_leaves_old_dir`: monkeypatch the writer to raise after the first file → `out_dir` still holds the previous run's files, exit counts `error=1`, no `.new` left behind on the next successful run.
  - `manifest`: blakbox fixture → manifest content is exactly `syncthing.service\thandled elsewhere (skip list)\n`.
  - `no_writes_outside`: after a run, the temp tree has no new files outside `out_dir`, `out_dir + ".new"`-free, and the manifest (walk and compare).
  - `cli_no_runtime`: `main(["--user"])` with `XDG_RUNTIME_DIR` unset → returns 2.
  - `dropin_execstart_reset` (from Task 1) now asserted on the translated `exec=`.

- [ ] **Step 2: Run** `python3 tests/test_import_user.py` — Expected: new tests FAIL.

- [ ] **Step 3: Implement.** Translate each unit by reusing `import_one` with `SCHEMA_SVC_DIR` pointed at the staging dir (so `_svc_graph`/`_drop_cycles`/`_exec_owner` operate on the user set only); in user mode `find_unit`/file reads go through `user_unit_text`. Post-filter user-mode bodies: drop `needs_root=1`, turn `user=` into a WARN line. Write `.svc` mode 0600, dirs 0700. Translation `Skip`s go to the manifest with their message as the reason.

- [ ] **Step 4: Run** `python3 tests/test_import_user.py tests/test_import_units.py tests/test_migrate_import_enabled.py` (each standalone) — Expected: all PASS.

- [ ] **Step 5: Commit** `feat(import): schema-import --user writes the user's services and skip manifest`

---

### Task 3: `schema-user` runs the importer at start

**Files:**
- Modify: `schema-user.c` (before `engine_load(svc_dir)` at `schema-user.c:326`)
- Test: `tests/test_schema_user.sh` (extend; existing cases set `SCHEMA_USER_IMPORT=` to keep their hand-written `.svc` files)

**Interfaces:**
- Produces: env override `SCHEMA_USER_IMPORT` — unset → `/usr/libexec/schema-init/schema-import`; set to empty → no import (tests, recovery); otherwise that path. Invoked as `<path> --user --out <svc_dir>`; stdout/stderr appended to `<log_dir>/import.log`.
- `static void run_import(void)` in `schema-user.c`: fork/exec, poll `waitpid(WNOHANG)` up to **10 s**, then `SIGKILL` + reap; logs one line `TAG "import: exit N"` / `"import: killed after 10 s"` / `"import: not run (%s)"` to stdout. Never calls `die()`.

- [ ] **Step 1: Write the failing tests** in `tests/test_schema_user.sh` (new section after the existing run, fresh `T2` tree):
  - `import_runs`: stub importer script writes `x.svc` (a sleeper) into the `--out` dir → `schema-ctl --user status` lists `x` running within 5 s.
  - `import_fail`: stub exits 1 → schema-user starts, `status` answers, out has `import: exit 1`.
  - `import_hang`: stub `sleep 60` → `status` answers within 13 s, out has `import: killed after 10 s`, no stub process left.
  - `import_disabled`: `SCHEMA_USER_IMPORT=` → hand-written `.svc` survives and runs.
  - Existing tests export `SCHEMA_USER_IMPORT=` at the top.

- [ ] **Step 2: Run** `make schema-user schema-ctl && sh tests/test_schema_user.sh` — Expected: new cases FAIL.

- [ ] **Step 3: Implement `run_import()`** and call it after the log dir exists and before `engine_load`. The child gets the same environment schema-user built for services (HOME, XDG_*, PATH) and stdin `/dev/null`.

- [ ] **Step 4: Run** `make test` — Expected: PASS (includes `tests/test_schema_user.sh`).

- [ ] **Step 5: Commit** `feat(schema-user): run schema-import --user at start (10 s cap, never fatal)`

---

### Task 4: `schema-ctl --user status` shows skipped units

**Files:**
- Modify: `schema-ctl.c` (user mode, `status` without a service argument, text and `--kv` output only)
- Test: `tests/test_schema_user.sh` (extend)

**Interfaces:**
- Consumes: manifest path `$XDG_RUNTIME_DIR/schema-user/skipped` (Task 2).
- Produces: after the server's table, one line per manifest entry: `  %-24s skipped (%s)\n` (unit, reason). `--kv`: `name=<unit> state=skipped reason="<reason>"`. `--json` unchanged in this PR *(plan call: JSON is built server-side in `engine.c`; merging client-side is fragile — goes with the PR E shim, which consumes JSON)*. Missing/empty manifest → nothing appended. System mode untouched.

- [ ] **Step 1: Write the failing test:** write a manifest with `syncthing.service\thandled elsewhere (skip list)` → `schema-ctl --user status` output contains `syncthing.service` and `skipped (handled elsewhere (skip list))`; `schema-ctl --user status --json` output does not contain `syncthing`.

- [ ] **Step 2: Run** `make schema-ctl && sh tests/test_schema_user.sh` — Expected: FAIL.

- [ ] **Step 3: Implement** in `schema-ctl.c` after printing the reply, guarded by `user_mode` and the bare `status`/`status --kv` command.

- [ ] **Step 4: Run** `make test` — Expected: PASS.

- [ ] **Step 5: Commit** `feat(schema-ctl): --user status lists skipped units with the reason`

---

### Task 5: Packaging — importer in base libexec, default skip list, ISO fallback

**Files:**
- Create: `config/user-skip` (the six default names, one per line, a `#` header comment saying what it is and how to retire a line)
- Modify: `Makefile` install target (add `install -D -m 0755 distros/fedora-installer/migrate/schema-import.py $(DESTDIR)$(PREFIX)/libexec/schema-init/schema-import` and `install -D -m 0644 config/user-skip $(DESTDIR)/etc/schema-init/user-skip`)
- Modify: `schema-init.spec` base `%files` (`%{_libexecdir}/schema-init/schema-import`, `%config(noreplace) %{_sysconfdir}/schema-init/user-skip`); `%changelog`/description line for the feature
- Modify: `distros/fedora-installer/schema.ks` no-RPM fallback (install both, as #403 did for `schema-user`)
- Test: `tests/test_spec_user_import.py` (new, pattern of `tests/test_spec_systemctl_shim.py`)

- [ ] **Step 1: Write the failing test:** asserts the base `%files` section (the one starting `%files` with no subpackage name) lists `%{_libexecdir}/schema-init/schema-import` and `%config(noreplace) %{_sysconfdir}/schema-init/user-skip`; `Makefile` installs both; `schema.ks` mentions `user-skip` and `libexec/schema-init/schema-import`; `config/user-skip` non-comment lines == the six defaults.
- [ ] **Step 2: Run** `python3 tests/test_spec_user_import.py` — Expected: FAIL.
- [ ] **Step 3: Implement** the file edits above. `schema-init-migrate` keeps `%{_bindir}/schema-import` unchanged.
- [ ] **Step 4: Run** `make test-all` and a local `rpmbuild -bb` (as for #403) then `rpm -qlp` on the base RPM — Expected: suite green; base RPM lists both paths.
- [ ] **Step 5: Commit** `packaging: ship schema-import in base libexec and /etc/schema-init/user-skip`

---

### Task 6: Verify — VM, then fleet (no code)

- [ ] **Step 1: schema-vmtest** (skill `schema-vmtest`): in the VM's user enable a trivial unit (`~/.config/systemd/user/vmt.service` `ExecStart=/usr/bin/sleep infinity`, link in `default.target.wants`) → after autologin `schema-ctl --user status` shows `vmt` running; then set the importer non-executable → desktop still up, `status` answers, `import.log`/out shows the failure.
- [ ] **Step 2: Open draft PR**, one `/code-review` pass, fix, CI green, merge; wait for dev COPR.
- [ ] **Step 3: Optiplex** (`dnf upgrade 'schema-init*'`, reboot): `XDG_RUNTIME_DIR=/run/user/1000 schema-ctl --user status` shows `obsidian-tunnel` running; `ss -ltn` on Optiplex shows `127.0.0.1:27124`; Kodi/cams/audio up. Remove `~/.local/bin/schema-autostart-runner.sh` there if the build contains #404.
- [ ] **Step 4: blakbox** (upgrade, reboot): `status` shows no services and `syncthing.service skipped (handled elsewhere (skip list))`; exactly one `syncthing` process.
- [ ] **Step 5: Eli**: before rebooting, run `schema-import --user --out /tmp/eli-preview -n` (or into a scratch `--out`) as the user and review with Jonathan; then upgrade + reboot, desktop unaffected.
- [ ] **Step 6:** Update memory (`project_schema_user_supervisor.md`, NOW) with verified results.
