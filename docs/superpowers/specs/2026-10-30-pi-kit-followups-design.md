# Pi kit follow-ups — design

**Status:** draft, 2026-10-30. Jonathan approved doing the parked kit follow-ups (writable-path key, nested RuntimeDirectory, refresh pi-*.svc on upgrade, quoted entries) plus a Pi clock item.

## Why

The Pi service kit (2026-10-28) parked five follow-ups. Four ship here. The fifth changed after a look at BlakPi:

- **Writable paths.** schema-init has no `ReadWritePaths=` exception inside a read-only mount. The importer works around it by *lowering* `protect_system` until the path is writable (pihole-FTL: `ProtectSystem=full` + `ReadWriteDirectories=/etc/pihole` → `protect_system=1`, so all of `/etc` stays writable). 13 Fedora units take that downgrade.
- **Nested RuntimeDirectory.** Entries with `/` are dropped, because a root `install -d -o user` through a parent the user owns follows a symlink the user planted there. That drop also hits units that run as root, where the hazard doesn't exist: cockpit loses `/run/cockpit/tls`, and systemd-networkd loses `/run/systemd/netif`.
- **Quoted entries.** `RuntimeDirectory=`, `ReadWritePaths=` and `ReadWriteDirectories=` are split on whitespace. systemd allows quoting in these lists (`ReadWritePaths="/srv/my data"`). Today that produces `"/srv/my` and `data"`.
- **Kit refresh.** `schema-pi enable` copies `pi-*.svc` (and the other kit `.svc`) only when absent. A package upgrade that changes a shipped kit file never reaches an enabled Pi.
- **Clock (re-scoped).** The parked item was "tailscaled should wait for pi-timesync". The BlakPi boot of 2026-10-30 shows otherwise:
  - tailscaled started at 1969, logged `time jump detected` when the clock was set, and carried on.
  - The front-door outage that day was the Tailscale incident, not the clock.
  - The clock was set by **pihole-FTL's own NTP client** before pi-timesync's first answer arrived.
  - The real defect: pi-timesync's second source, `http://8.8.8.8/`, no longer answers HTTP at all (nor 8.8.4.4 or 9.9.9.9). Every Pi has run single-source since then. A single source may only move the clock forward, so a fast clock is never corrected, and the two-source agreement check never runs.
  - `208.67.222.222` (OpenDNS, Cisco) still returns a `Date` header and is a different operator from Cloudflare. (1.0.0.1 also answers, but it is Cloudflare again.)

## Goals

1. **Engine `rw_path=` key** (repeatable, at most 8). Each value is one absolute path with no `..` component. A trailing `/` is stripped, and whitespace inside is allowed (the whole line is the value). In the service's mount namespace, after the `protect_system`/`protect_home` read-only mounts, each `rw_path` that exists is bind-mounted onto itself and has `MOUNT_ATTR_RDONLY` cleared (recursively), so it is writable again. This is systemd's own mechanism.
   - A missing path is skipped silently (systemd's `-` prefix behaviour; schema-init can't create it inside a read-only parent anyway).
   - `rw_path=` alone, with no read-only knob, does nothing and creates no namespace.
   - An empty `rw_path=` in a drop-in clears the list, like `landlock_ro=`.
   - `schema-ctl status <svc>` lists each `rw_path`.
2. **Importer emits `rw_path=`** for `ReadWritePaths=`/`ReadWriteDirectories=` entries under `protect_system`'s read-only dirs, instead of lowering `protect_system`. pihole-FTL keeps `protect_system=full` plus `rw_path=/etc/pihole`. `protect_home` keeps the lowering: its dirs are an empty tmpfs, and there is nothing underneath to bind back. User mode emits no `rw_path=`, since it already drops every mount-ns key. `%`-specifier and relative entries keep today's skip.
3. **Nested RuntimeDirectory** is allowed when the `install -d` runs with no `-o` (system unit with no `User=` or `User=root`) or in user mode (the runner is the user already). It is still dropped for a system unit with a non-root `User=`. Entries with a `..` component, `.`/empty components, or `:` stay dropped everywhere.
4. **Quoted list values.** `RuntimeDirectory=`, `ReadWritePaths=` and `ReadWriteDirectories=` are split with shell-style quoting (the importer's existing `shlex`). An unbalanced quote falls back to whitespace split with a WARN. A `-`/`+` prefix outside the quotes still works (`-"/a b"` → `-/a b`).
5. **Kit refresh on upgrade.** New `schema-pi refresh`:
   - For each kit service, an installed `$SVC/<s>.svc` is replaced with the shipped example when it is byte-identical to the copy `schema-pi` last installed, i.e. the user hasn't edited it. The record is a sha256 in `/var/lib/schema-init/kit.sums`, written by `install_kit` and by `refresh`.
   - A file with no record that is identical to the current example gets recorded. A file with no record that differs is left alone, with one notice line.
   - An edited file is left alone, with one notice line naming it and the shipped example path.
   - It does nothing unless the kit is enabled (`status_enabled`).
   - `debian/schema-init.postinst` runs `schema-pi refresh || :` on `configure` when upgrading (`$2` set). Refreshed files take effect at the next boot, and the line it prints says so.
6. **pi-timesync second source:** `8.8.8.8` → `208.67.222.222`, in the script, its comments, and `tests/test_pi_kit.sh`'s stub.

## Non-goals

- `ReadOnlyPaths=`, `InaccessiblePaths=`, `ProtectSystem=strict` as true strict. `strict` stays mapped to `full`.
- Creating a missing `rw_path` or `RuntimeDirectory` cleanup at stop.
- Making tailscaled (or anything) wait for the clock. The evidence says it recovers on its own; revisit only with a failure that the clock causes.
- Live-reloading refreshed kit `.svc` files (a changed `.svc` needs a reboot or evict/re-add; never `restart` session daemons).

## Anti-goals

- `rw_path` must never widen beyond the named path. A bind of `/etc/pihole` must not make `/etc` writable.
- `rw_path` must never follow a path that resolves outside the read-only tree in a way that writes somewhere else unexpectedly. It is a bind of the path onto itself, so it writes only where the path already points. That matches systemd.
- Refresh must never overwrite a kit `.svc` the user edited.
- The `install -d -o user` symlink hazard (final review I2 of the kit) must stay closed.

## Testing

- `tests/test_mountns_parse.c`: `rw_path=` parsing covers good values, relative paths, `..`, more than 8, trailing slash, the drop-in reset, and copy through `services_load`.
- New root-gated runtime test (`tests/test_mountns_rw.c`): fork, `apply_mount_ns(FULL)` with an rw path under `/etc`. Inside, writing there succeeds, writing next to it in `/etc` gets `EROFS`, and a missing rw path is skipped. Skips when not root.
- `tests/test_import_units.py`: pihole-FTL-shaped unit → `protect_system=full` + `rw_path=/etc/pihole`. A protect_home path still lowers. Quoted `ReadWritePaths`/`RuntimeDirectory` cases. Nested RuntimeDirectory: root unit kept, `User=` unit dropped, `..` dropped. User-mode nested kept and no `rw_path`.
- `tests/test_pi_kit.sh`: refresh replaces an unedited kit file, keeps an edited one, records an unrecorded identical one, and leaves an unrecorded different one. It does nothing when the kit is not enabled. timesync stub moves to `208.67.222.222`.
- vmtest: a `.svc` with `protect_system=full` + `rw_path=` boots under PID 1. Its exec writes to the rw path and fails to write elsewhere in `/etc`.
- Hardware: BlakPi on the dev build. pihole-FTL re-imported carries `protect_system=full` + `rw_path=/etc/pihole`, DNS still answers, and Pi-hole can still save settings. `schema-pi refresh` records the 10 unedited kit files. pi-timesync log shows both sources.

## Builder's calls (mine, flag if wrong)

- Max 8 `rw_path` per service.
- Missing `rw_path` silently skipped, both with and without `-`.
- Refresh tracks by sha256 of what it installed, not by version.
- The pihole-FTL `.svc` on BlakPi is not re-imported automatically by the upgrade. The hardware check re-imports it by hand.
