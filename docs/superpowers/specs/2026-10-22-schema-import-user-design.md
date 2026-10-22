# PR D — `schema-import --user` + skip list

**Date:** 2026-10-22 · **Parent spec:** `2026-10-20-schema-user-supervisor-design.md` §5–§6 (this document refines them; where they differ, this one wins) · **Status:** design, approved in conversation 2026-10-22.

## Intent

`schema-user` (PR B/C, shipped in v0.4.17) runs on every fleet box and supervises nothing: it loads `.svc` files from `$XDG_RUNTIME_DIR/schema-user/services/`, and nothing writes any there. PR D fills that directory from the user's real systemd user unit files, so anything enabled with `systemctl --user` actually runs under schema-init — without anything starting twice.

## Goals

- `schema-import.py` gains a `--user` mode that turns the user's **enabled** user units into `.svc` files in `$XDG_RUNTIME_DIR/schema-user/services/`.
- `schema-user` runs it at every start (fresh login / linger boot always reflects current units).
- A skip list keeps units that something else already provides from being generated.
- `schema-ctl --user status` shows what was skipped and why.
- The importer is installed on every box that has `schema-user` (today it ships only in `schema-init-migrate`, which Optiplex does not have).

**Done when, on the real fleet:**
- Optiplex: `obsidian-tunnel.service` (enabled in `~/.config/systemd/user/default.target.wants`, `ssh -N -L 27124:… blakbox`) comes up by itself after a reboot and `schema-ctl --user status` shows it running.
- blakbox: `syncthing.service` is listed as skipped (handled by its XDG autostart) and is not started a second time; nothing else is generated.
- Eli: whatever its enabled user units are, the generated set is reviewed (`cat` the dir) before its reboot; desktop unaffected.
- Desktop login, Kodi, audio unchanged on all three.

## Non-goals (this PR)

- `systemctl --user` shim mapping, `enable`/`disable`, and `schema-ctl --user daemon-reload` → PR E. (`daemon-reload` doesn't exist yet, and the engine's `handle_reload` refuses a zero-service config — `engine.c:2176` — which is a normal user state; solving that touches code shared with PID 1, so it waits.) In this PR the import runs only at `schema-user` start; a changed unit takes effect at the next login/boot.
- Session D-Bus activation helper, Python shim `StartUnit` forwarding → PR E/F.
- Units reachable only through session targets (`graphical-session.target`, `graphical-session-pre.target`, `plasma-*.target`, `gnome-session*.target`) → Phase 2. Listed as skipped.
- Distro/admin-enabled user units (`/etc` and `/usr/lib` `*.target.wants`) → Phase 2 (see §1).
- Template user units (`foo@.service`) → skipped as today for system units.
- `.path` units → skipped with warning.
- Retiring the PipeWire / Syncthing workarounds — separate per-host rollout steps after this PR (parent spec §10).

## Anti-goals (failure even if it "works")

- **Login must never wait on or break because of the import.** If the import crashes, hangs or produces garbage, `schema-user` logs it and runs with whatever valid `.svc` files exist (possibly none). The desktop comes up regardless.
- **Nothing starts twice.** A unit already provided by the session script or a system `.svc` must never be generated (PipeWire stack especially — a second PipeWire = no audio).
- **Never write outside `$XDG_RUNTIME_DIR/schema-user/`** in user mode (no `/etc/schema-init/services`, no pending queue, no bus overrides).
- No silently dead units: every enabled unit that is not generated appears in status with a reason.

## Decisions made with Jonathan (2026-10-22)

- blakbox dead user units were cleaned up **before** this PR so it wakes nothing unwanted: openclaw-gateway, hermes-gateway, wifi-densepose, claude-brain, claude-sync removed with their data; gammastep and the old April user backup (`blakbox-backup.{service,timer}`, `~/backup-blakbox.sh`) moved to `~/.config/systemd/user/retired-20261022/`.
- **Parent spec rollout step 4 is dropped:** the nightly backup stays the root system `.svc` (it covers `/etc`, `/root`, …); there is no user backup timer any more.
- Split: this PR = import + packaging + skip list + status. Shim/activation = PR E.

## Design

### 1. Import, user mode

`schema-import --user [--out DIR] [-n]` (stdlib-only, same file `distros/fedora-installer/migrate/schema-import.py`).

- **Search path, highest precedence first:** `$XDG_CONFIG_HOME/systemd/user` (default `~/.config/systemd/user`) → `/etc/systemd/user` → `/run/systemd/user` → `/usr/local/lib/systemd/user` → `/usr/lib/systemd/user`. `MIGRATE_ROOT` prefixes the system dirs as today (tests).
- **Masks:** a unit file that is a symlink to `/dev/null` (or empty) in a higher-precedence dir masks the unit everywhere. A masked unit only appears in the skip manifest if something enables it (the manifest is "enabled but not generated"); masks nothing enables are ignored *(builder-check call)*.
- **Drop-ins:** `<unit>.d/*.conf` from every dir in the path, applied in systemd's order, as for system units.
- **Enabled set (Jonathan 2026-10-22: user-enabled only):** start from the `.wants/` and `.requires/` directories of `default.target`, `sockets.target`, `timers.target`, `basic.target` **in `$XDG_CONFIG_HOME/systemd/user` only** — i.e. what the user (or `systemctl --user enable`) turned on. Then follow `Wants=`/`Requires=` of units in the set, recursively. Unit *files* (and drop-ins) still resolve through the whole search path (blakbox's `syncthing.service` is enabled in `~/.config` but lives in `/usr/lib/systemd/user`). Distro/admin defaults in `/etc/systemd/user/*.target.wants` and `/usr/lib/systemd/user/*.target.wants` are **not** read in Phase 1: on Fedora they are `dbus.socket` (would start a second session bus), `systemd-tmpfiles-*`, `systemd-ask-password.socket`, `drkonqi-*` (built on systemd-coredump), `grub-boot-success.timer`, `unity-gtk-module`, `snapd.session-agent.socket`, PipeWire sockets — they wait for Phase 2 with the session work. Entries in `~/.config/systemd/user/<session target>.wants/` (gammastep-style) are recorded as skipped, reason `phase 2 session target`. (The existing system-mode `enabled_units()` only globs one dir's `*.target.wants`; user mode needs the recursive `Wants=`/`Requires=` follow.)
- **Specifiers** in user mode: `%h`→`$HOME`, `%t`→`$XDG_RUNTIME_DIR`, `%S`→`$XDG_STATE_HOME` (default `~/.local/state`), `%C`→`$XDG_CACHE_HOME` (`~/.cache`), `%E`→`$XDG_CONFIG_HOME` (`~/.config`), `%u`/`%U` user name/uid, `%T`→`/tmp`, `%V`→`/var/tmp`, plus the existing `%n %N %p %%`. Socket `%t` maps to `$XDG_RUNTIME_DIR` (system mode maps it to `/run`).
- **Unit translation:** reuse `unit_to_svc` / `timer_to_svc` / `socket_lines` unchanged where possible. User-mode differences:
  - `User=`/`Group=` in a user unit → dropped with a WARN line (services run as the user anyway).
  - No `needs_root`; hardening keys emitted as today (schema-user already runs with hardening default 0).
  - `dep=` lines only to other units in the same user set; deps on system units (`network-online.target` etc.) are dropped with a note, as system mode does for unknown deps.
  - *Follow-up (2026-10-22):* `After=`/`Wants=`/`Requires=network-online.target` on the service (or a timer's/socket's target) becomes a first `exec_pre=-<nm-online> -s -q -t 60`. The user supervisor has no network-online.target, so Optiplex's obsidian-tunnel started before DHCP, hit "Network is unreachable" ×5 in one second and went DORMANT. The `-` keeps systemd's semantics: the wait orders the start, never fails it. No `nm-online`, or 8 `ExecStartPre=` already → a WARN line, no wait.
  - **No cycles:** the importer runs the existing `_drop_cycles` logic against the graph of the `--out` set being written (parameterise `_svc_graph` by directory), because `schema-user` exits on a cyclic config (`schema-user.c:326`). Test it *(builder-check call)*.
  - `.socket` → `listen=` path (PipeWire's `pipewire.socket` is the proving case, though it is skip-listed by default).
  - `.timer` → existing timer keys.
- **Output:** one `.svc` per generated unit in `--out` (default `$XDG_RUNTIME_DIR/schema-user/services/`). Each run **replaces the directory's generated contents** (write to a temp dir in the same parent, then swap), so a removed unit disappears. Plain text, `cat`-able, keeps the existing `# schema-import WARN:` lines.
- **Skip records:** a manifest `$XDG_RUNTIME_DIR/schema-user/skipped` — one line per enabled-but-not-generated unit: `<unit>\t<reason>`. Reasons include `handled elsewhere (skip list)`, `phase 2 session target`, `masked`, `template unit`, `path unit`, and every existing `Skip(...)` message from translation.
- Exit 0 on success even if some units were skipped; non-zero only on a crash/IO error.
- In user mode none of the system-mode side effects run: no pending queue, no `bus_overrides`, no `schema-ctl add`, no `_exec_owner` lookup against `/etc/schema-init/services` (the skip list replaces it).

### 2. Skip list

- Files: `/etc/schema-init/user-skip` (system-wide) and `$XDG_CONFIG_HOME/schema-user/skip` (per user). Format: one unit name per line, `#` comments, blank lines ignored; a bare name without suffix means `.service`. Union of both files.
- Listed units are never generated; they go to the skip manifest with reason `handled elsewhere (skip list)`.
- **Default content** of `/etc/schema-init/user-skip`, shipped as a `%config(noreplace)` file in the package: `pipewire.service`, `pipewire.socket`, `pipewire-pulse.service`, `pipewire-pulse.socket`, `wireplumber.service` (the session script and/or system `.svc`s already run them on every fleet box; with user-enabled-only they only matter if a user enables them by hand — the guard stays), plus `syncthing.service` (blakbox runs it from XDG autostart). Retiring a workaround later = remove the workaround **and** its line, reboot, verify.

### 3. `schema-user` runs the import

- At startup, before `engine_load(svc_dir)`: run `/usr/libexec/schema-init/schema-import --user` as a child with a **hard timeout (10 s)**, stdout/stderr to the user's schema-user log dir. On timeout → kill it, log, continue. On non-zero exit → log, continue with whatever is in the directory.
- Kill switch / login rules from the parent spec are unchanged; the import adds no new path by which login can block (schema-user is already started asynchronously by logind).

### 4. Status shows skipped units

`schema-ctl --user status` appends the skip manifest after the service table, one line per unit: `  <unit>  skipped (<reason>)`. `status --kv` gets `state=skipped reason=…` lines. `status --json` is unchanged in this PR *(plan call: JSON is built server-side; it moves with the PR E shim, which consumes it)*. System-mode status is unchanged.

### 5. Packaging

- Install the importer as `/usr/libexec/schema-init/schema-import` in the **base `schema-init` package** (every box with `schema-user` has base). `schema-init-migrate` keeps `/usr/bin/schema-import` for system use — same source file; the builder chooses whether migrate's copy becomes a symlink to the libexec one or both are installed from the one source.
- Ship `/etc/schema-init/user-skip` (`%config(noreplace)`) in base.
- `schema.ks` no-RPM fallback (as #403 did for `schema-user`): install both.

## Testing

- Extend `tests/test_import_units.py` (or a sibling `test_import_user.py`) with a fake user tree via `MIGRATE_ROOT` + a temp `HOME`/`XDG_*`: precedence, mask, drop-in, closure through `~/.config` `.wants` and `Wants=`, `/etc` + `/usr/lib` `.wants` ignored (a `dbus.socket` fixture must NOT be generated), session-target-only unit skipped, specifiers (`%h %t %S %C %E %u %U`), skip list (both files, bare names), User= dropped, socket `%t` → runtime dir, timer, output replaces stale files, skip manifest content, no writes outside `--out`.
- A fixture modelled on the real fleet: Optiplex (`obsidian-tunnel` → one `.svc` with the ssh argv and `restart` policy), blakbox (only `syncthing` enabled + masked, un-enabled akonadi → zero generated, one skip line, no masked line).
- C side: a test or `make test` check that `schema-user` survives the importer failing (non-executable / exits 1 / sleeps past the timeout) and still serves `status`.
- schema-vmtest: autologin boot with a user unit enabled in the VM's user → running after login; importer made to fail → desktop still up.
- Fleet, one reboot each after dev COPR: Optiplex (obsidian-tunnel up, `ss -ltn` shows 27124 on Optiplex), blakbox (syncthing skipped, single syncthing process), Eli (review generated set first).

## Left to the builder

- Module layout inside `schema-import.py` (new functions vs. parameterising existing ones), as long as system mode output is byte-identical (existing tests stay green).
- Exact temp-dir swap mechanics and the manifest's file name, if a better one fits existing conventions.
- How `schema-user` spawns/timeboxes the child (reuse engine helpers if they fit).
