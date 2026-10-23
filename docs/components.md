# The reclaimed daemons

## schema-udev

`schema-udev` is a native uevent→schema→action daemon — a small, purpose-built device manager that watches the kernel uevent stream and runs your hook scripts when a match fires. It runs in one of **two modes**.

### Shadow mode (default)

The safe default: `schema-udev` runs **alongside** real `systemd-udevd`, not replacing it. udevd still owns `/dev` population, symlinks, and driver binding; `schema-udev` just watches the same kernel uevent stream and fires your rules for the specific devices you care about (an ESP32 over USB-serial, an RFID reader, a sensor board).

It binds **kernel netlink group 1** (`UDEV_MONITOR_KERNEL`), never group 2 (`UDEV_MONITOR_UDEV`) — group 2 is udevd's own processed-event multicast, consumed by libudev/PipeWire for device enumeration. In shadow mode `schema-udev` only listens to the raw kernel group, so it has **zero observed impact** on PipeWire or desktop hotplug. Datagrams are checked against the kernel's `SCM_CREDENTIALS` (uid 0, pid 0) before being parsed, so a spoofed unprivileged uevent is dropped. Rule syntax, coldplug, and hooks below apply to both modes.

### Authoritative mode (the udevd cutover)

The endgame of schema reclamation: `schema-udev` **retires `systemd-udevd` and owns `/dev` itself.** This is a deliberate, reversible opt-in — off unless you explicitly arm it — because it moves device management for the whole machine under schema-init.

- **Arm it** by placing the sentinel flag `/etc/schema-init/schema-udev.live` and restarting the daemon; on the next launch schema-udev takes authority and `systemd-udevd` is retired. Its startup log records `mode=LIVE (owns real /dev, /dev/disk, ACLs)` so the mode is never ambiguous.
- In LIVE mode schema-udev takes on everything libudev/PipeWire/logind expect from a udev: device-node creation (native `mknod`, `RUN{builtin}` kmod loads, `ATTR{}` sysfs writes), the `/dev/disk/by-*` + `/dev/char` + `/dev/block` symlink farm, `uaccess` seat ACLs, the **group-2 libudev monitor broadcast**, and the `/run/udev/data` device database — the two interop encoders that are dormant in shadow mode are wired live here.
- **Rules** are interpreted natively from the distro's `*.rules` (`TAG+=`, `SYMLINK+=`, `PROGRAM`/`RESULT`, `IMPORT`, native `*_id` builtins) in addition to schema-init's own `*.dev` files, so tag/symlink-dependent consumers (FIDO2/pico-fido ACLs, snaps, power-button handling) keep working after udevd is gone.
- **Rollback is blessed and checksum-asserting:** a verified pristine backup of the stock device path is restored on disarm (remove the sentinel), so a bad cutover reverts cleanly to `systemd-udevd` authority on the next boot. Keep the backup — the sentinel protects the machine; it does not un-take authority on its own.

> ⚠️ Authoritative mode is the advanced path. Validate it in `schema-vmtest` LIVE mode (prove it boots owning `/dev`) before trusting it on hardware you can't easily recover, and confirm the `by-*` symlink set your mounts depend on is complete for your disks.

**Rule files** live in `/etc/schema-init/dev/*.dev`, one `key=value` per line:

```ini
name=esp32-serial
match_subsystem=tty
match_product=10c4/*
symlink=esp32
on_add=/usr/local/bin/esp32-up.sh
on_remove=/usr/local/bin/esp32-down.sh
```

- `match_*` keys map to raw kernel uevent keys (`match_subsystem` → `SUBSYSTEM`, `match_product` → `PRODUCT`, etc.), **ANDed together** — a rule only fires when every `match_*` key it declares matches. Values support `fnmatch(3)` globs (`10c4/*`).
- `symlink=<name>` creates a stable symlink `/dev/schema/<name>` → `/dev/<DEVNAME>` on `add`, unlinking it on `remove`. Created atomically prior to `on_add` hook execution. Name must be single-level (no `/` or `..`, max 63 chars) under the parallel `/dev/schema/` namespace to avoid writer contention with systemd-udevd.
- `on_add` / `on_remove` are hook commands run via `/bin/sh -c` with the full uevent exported as environment variables — `ACTION`, `DEVNAME`, `DEVPATH`, `PRODUCT`, `MODALIAS`, and whatever else the kernel sent.
- **Network interface names**: in LIVE mode schema-udev applies systemd's `.link` files (`/etc`, `/run`, `/usr/lib/systemd/network`; `NamePolicy=`, `Name=`, matched on `OriginalName=`, `MACAddress=`, `Driver=`) and renames NICs to the same predictable names systemd would (`eno1`, `enp3s0`, `wlp6s0`). To keep kernel names (`eth0`, `wlan0`), put `kernel` in `/etc/schema-init/net-naming` or boot with `net.ifnames=0`. A box that already ran schema-udev on kernel names keeps them across the upgrade that introduced this (the upgrade writes that file), because firewall rules and scripts may name `eth0`; delete it once those are updated.
- **Coldplug at startup**: On launch, `schema-udev` performs an in-process physical sysfs walk (`/sys/devices`) to synthesize events for devices already present at boot and fire `on_add` rules and symlinks without touching netlink or `/sys/*/uevent` files (ensuring zero `systemd-udevd` or desktop disturbance).
- Comments must be on their own line (`#` as the first non-blank character). There is no inline-comment stripping — a trailing `# note` after a value becomes part of the value. See `assets/example.dev` (fully inert — every line commented, safe to drop in as a template) and copy it to `/etc/schema-init/dev/<name>.dev` to activate.
- `SIGHUP` reloads all rule files from disk without restarting the daemon (`schema-ctl reload` or `kill -HUP` on its pid).
- Raw kernel (group 1) uevents deliver `DEVNAME` **without** the `/dev/` prefix (e.g. `DEVNAME=ttyUSB0`, not `/dev/ttyUSB0`) — don't anchor `match_devname` globs to `/dev/`, and hooks see `$DEVNAME` the same unprefixed way. Prefer keying on `match_subsystem` + `match_product` (vid/pid), as in the example above.

### libudev / `/run/udev` interop

schema-udev carries pure encoders for the two formats a udevd retirement
needs: the **libudev monitor** netlink frame (group 2) and the
**`/run/udev/data`** device-database record, unit-tested against real
captured frames. In **shadow mode** they are dormant — schema-udev neither
broadcasts on group 2 nor writes `/run/udev` while systemd-udevd runs, since
doing so would double libudev events and corrupt udev's database. In
**authoritative mode** (see above) they are wired live: schema-udev is the
sole writer of `/run/udev/data` and the sole group-2 broadcaster, which is
what lets libudev/PipeWire/logind keep enumerating devices with udevd gone.

---

## schema-doctor

`schema-doctor` is a self-healing seam checker. schema-init's replacements (its own logind, its own udev) meet desktop expectations at a handful of seams, and every machine comes up with *different but very similar* quirks clustered there — a compositor that raced ahead of its device ACLs, a power daemon that never autostarted, a menu-cache rebuild storm. The doctor diagnoses those seams, **heals the safe ones itself**, and **names the deep ones** (too invasive to fix under a live desktop — the real fix belongs at the source). It is a stdlib-only script, installed at `/usr/local/bin/schema-doctor`, that uses the still-installed systemd as its oracle for "what should this look like."

### The checks

Each check is graded **SAFE** (auto-healed on every run) or **DEFERRED** (detected and named, never auto-healed — run with `--force <name>` to override):

| Check | Grade | Catches |
|---|---|---|
| `card-input-acl` | SAFE | active user missing rw on a node the udev DB tags `uaccess` (drm `card*`, joysticks, SDR/FIDO) → re-applies the ACL |
| `vt-mediation` | SAFE | `Ctrl+Alt+F<n>` VT switching unmediated (the frozen-screen path) → re-arms it via schema-logind |
| `session-single` | DEFERRED | an orphaned placeholder session — registration lost the boot race |
| `login1-power` | DEFERRED | `login1` not answering the PowerOff/Reboot/Suspend/Hibernate/inhibitor queries PowerDevil makes on load |
| `powerdevil-running` | DEFERRED | PowerDevil not running, so power and screen-lock settings won't load |
| `ksycoca-loop` | DEFERRED | plasmashell and kded6 disagree on `XDG_MENU_PREFIX`, each rebuilding the menu cache in a loop |

The run loop is the safety heart: **snapshot → heal → verify**, per check. If a heal doesn't resolve the fault, or breaks a previously-clean check, it is **backed out** and reported not-healed; a collateral break aborts the whole run. The entire run is wrapped to always `exit 0` (`critical=0`) — the doctor can never block boot or leave the box worse than it found it. Config lives in `/etc/schema-init/doctor.conf` (`heal=no` for global detect-only, `disable=a,b`, `notify=no`).

### Two runs, one engine

The same one-shot engine runs in two roles — it is **not** a daemon:

- **Boot run** (`schema-doctor.svc`, `--heal --wait 30`): a late oneshot after a session exists, aggressive first-heal on a fresh box, silent.
- **Standing run** (`schema-doctor-periodic.svc`, `--heal --periodic`): a native schema-init interval timer (`on_boot_sec`/`on_active_sec`, re-armed by PID 1) re-runs the engine every 10 minutes to catch faults that appear *after* login — a helper that dies at hour 3, an ACL a bad app clobbers mid-session.

The standing run adds three fail-safe guards:

- **Flap guard** — heal history lives in `/var/lib/schema-init/doctor-state` (JSON, keyed by `boot_id`). A check healed **≥3 times in 30 minutes** is marked **CHRONIC**: the doctor stops band-aiding it and escalates it to a visible RED, because the real fix is elsewhere. The mark clears the first time the check reads clean.
- **Health signal** — `/run/schema-init/doctor-status` (text, plus a `--json` twin) is rewritten every run: per-check GREEN/AMBER/RED with an overall worst-wins rollup. `schema-doctor --status` prints it from anywhere.
- **Desktop notifications** — edge-triggered on `--periodic` runs only: a check newly going CHRONIC or heal-failing, or the box returning to all-GREEN. Successful auto-heals are silent — quiet self-healing is the point. Notifications reach the active user by reading `DBUS_SESSION_BUS_ADDRESS` from the session leader's environ (this platform's bus is `/tmp/dbus-XXXX`, not `/run/user/1000/bus`), and are best-effort — a notify failure never fails a run.

schema-doctor is the logind-seam counterpart to what `verify-rules-live` is for the udev seam: a standing assertion that the seam still holds.

---

## D-Bus compatibility

On a no-systemd desktop, several interfaces are missing that desktop environments expect. schema-logind (`distros/*/services/schema-logind.svc`) handles the session/power/host interfaces in a single Python process on the system bus. The `org.freedesktop.systemd1` management surface is served by its own process — see below.

| Interface | Why it matters | What schema-logind returns |
|-----------|---------------|---------------------------|
| `org.freedesktop.login1` | Power/reboot buttons, session tracking, polkit seat queries | PowerOff, Reboot, CanPowerOff, CanReboot, Inhibit, GetSessionByPID, mock Session/User/Seat objects |
| `org.freedesktop.ConsoleKit` | Cinnamon session manager uses ConsoleKit, not logind, for CanRestart/CanStop — controls restart button visibility | GetSessionForUnixProcess, CanRestart → True, CanStop → True, Restart/Stop → SIGINT/SIGTERM to PID 1 |
| `org.freedesktop.hostname1` | About This System panel, network-manager display | hostname, static hostname, OS pretty name, hardware vendor/model from `/sys/class/dmi/` |
| `org.freedesktop.systemd1` | `systemctl`, Cockpit's Services page, and KDE/GNOME unit-state queries — the full systemd-compat management surface | **Live** per-unit ActiveState/SubState/MainPID/NRestarts mapped from `schema-ctl`; ListUnits/ListUnitsFiltered, GetUnit, GetUnitFileState, StartUnit/StopUnit/RestartUnit driving `schema-ctl` for real; PropertiesChanged on state transitions. Served by a **separate** `schema-systemd1` process (see below) |
| `org.freedesktop.timedate1` | Date & Time settings panel: timezone, NTP status, clock | Timezone (from `/etc/localtime`), CanNTP/NTP/NTPSynchronized → true, TimeUSec; `SetTimezone` re-links `/etc/localtime` and writes `/etc/timezone` for real |

Without these stubs, KDE and GNOME panels hit the D-Bus default timeout (25–30s) before giving up. With them, the same queries return in <100ms.

**D-Bus policy required.** The systemd-shipped `org.freedesktop.login1.conf` policy denies all non-root calls to login1 by default — KDE and Cinnamon will never see the power buttons without a drop-in. Install the one from this repo:

```sh
sudo cp distros/shared/dbus/schema-logind.conf /etc/dbus-1/system.d/schema-logind.conf
sudo dbus-send --system --type=method_call --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.ReloadConfig
```

Then log out and back in (or reboot). The policy whitelists CanPowerOff, CanReboot, PowerOff, Reboot, and all session/seat methods schema-logind exports.

**schema-logind is not a dependency of schema-init itself** — it is a userspace service like any other. Drop its `.svc` file in your services directory and list it as a dep of your display manager:

```ini
name=sddm
exec=/usr/sbin/sddm
dep=dbus
dep=schema-logind
dep=polkitd
needs_root=1
```

**The real systemd1 surface (`schema-systemd1`).** Unlike the read-only stubs above, `org.freedesktop.systemd1` is served by its own process (`services/schema-systemd1.svc` → `scripts/schema-systemd1.py`), not schema-logind. It registers every schema-init unit as a `…/unit/<name>_2eservice` object and mirrors live state from `schema-ctl`, so `systemctl status <svc>` and Cockpit's Services page show real ActiveState/SubState/MainPID/restart counts — and `StartUnit`/`StopUnit`/`RestartUnit` drive `schema-ctl` for real. Unit names are validated before being handed to `schema-ctl` (argv/newline injection guard). State transitions emit `PropertiesChanged`, so Cockpit updates without a refresh. Design notes: `docs/superpowers/specs/2026-06-20-schema-systemd1-dbus-design.md`.

**The `sd_booted()` signal.** `mount_pseudo()` creates `/run/systemd/system` at early boot (`init.c`). `libsystemd`'s `sd_booted()` is a bare `access()` on that path, so any software gated on "is systemd the init?" — KService/ksycoca, elogind clients — gets a positive answer with no shim. This is what made the old `LD_PRELOAD` `mock_sd.so` workaround (which faked the check to stop KDE's ksycoca from spinning at idle) unnecessary: the signal is now native and costs one `mkdir`.

**The `sd_login_monitor` directories.** `libsystemd`'s `sd_login_monitor_new(NULL, …)` — used by WirePlumber's logind module and other session/seat-aware clients — sets an inotify watch on `/run/systemd/{sessions,seats,users,machines}`. If any of those directories is missing the call fails with `-ENOENT` and the client silently drops logind integration (for WirePlumber that means no device reservation, no session-based pause). schema-logind creates all four at startup, empty — matching what real logind does even with no active sessions — so those clients initialize cleanly. Costs four `mkdir`s.

### The system bus itself (`schema-dbus`)

Everything above runs *on* the D-Bus system bus. `schema-dbus` (`schema-dbus.c`) **is** that bus — a native C implementation of the message broker that replaces `dbus-daemon`/`dbus-broker` as the process owning `/run/dbus/system_bus_socket`. It does the real work of a bus daemon: `EXTERNAL` authentication, well-known and unique name ownership, client→client routing with reply tracking, match-rule signal delivery, the `org.freedesktop.DBus` driver, and **unix-fd passing** — libdbus can't demarshal fd-carrying messages, so the broker parses the wire format directly and forwards the fds opaquely (`sdbus_wire.h`).

It enforces the **same policy** as the stock daemon. The system's shipped `busconfig` XML (`/usr/share/dbus-1/system.conf` and its `system.d/` drop-ins) is *dissolved* into a flat allow/deny table at startup by `scripts/schema-dbus-run.sh`, which then execs the broker against it — so `<deny>`/`<allow>` rules for login1, polkit, NetworkManager and the rest are honored exactly. A conformance test checks the dissolved verdicts against a frozen corpus (`tests/test_sdbus_conformance.c`).

Activate it the way you'd flip udev — advanced, opt-in, reversible:

```sh
sudo make install-dbus-sp1                                    # installs broker + launcher, changes nothing yet
cp services/dbus.svc.sp1 /etc/schema-init/services/dbus.svc   # the flip
sudo reboot
```

The installer ISO and the migration wizard both offer this flip as an optional step after the udev cutover, in its own reboot, with a seatbelt health check that rolls back to stock `dbus-daemon` automatically if the bus doesn't come up (shipped since v0.4.1, hardware-tested on a real install). The manual steps above are for source builds.

**Config changes apply live.** Like `dbus-daemon`, the broker notices new or changed D-Bus config on its own: it watches the service dirs, the mask file and, on the system bus, `system.conf`, `system.d/` and `/etc/dbus-1`, and reloads once the files have been quiet for a second, so a `dnf` transaction costs one reload. `SIGHUP` and `org.freedesktop.DBus.ReloadConfig` run the same reload, and `systemctl reload dbus.service` goes through `ReloadConfig` (SIGHUP would kill a broker from before this change, still running on a box that upgraded and has not rebooted). A reload never restarts anything: connections, names, matches and pending replies stay put. On the system bus the busconfig is re-dissolved in a child (`python3` with the launcher's `dissect_policy.py`), so the bus keeps answering meanwhile; the new policy goes live only if it dissolved, parsed and passes a sanity check (no more than half the rules gone, and every current owner keeps `own` of its name). Any failure (bad XML, no `python3`, a dissolver crash or 30 s timeout) logs one line and keeps the old policy and activation table; `ReloadConfig` then answers `org.freedesktop.DBus.Error.Failed`. A new system activation file that names a `SystemdService=` goes live only through its `/usr/local/share/dbus-1/system-services` override from `schema-import --bus-overrides`, or at the next boot, so it is never started outside schema-init mid-update. Design: `docs/superpowers/specs/2026-10-23-schema-dbus-live-reload-design.md`.

Build needs `dbus-devel` (`make schema-dbus`). If the policy dissolve ever fails at boot, the launcher **self-heals to stock `dbus-daemon`** on the spot, so even a broken flip still comes up on a working bus; to roll back permanently, restore the stock `dbus.svc` (`exec=/usr/bin/dbus-daemon`, `args=--system`, `args=--nofork`) and reboot.

**Status.** Proven serving a full KDE Plasma desktop as the live system bus across reboots — kwin, plasmashell, polkit, PowerDevil, portals, WirePlumber, tailscale and schema-logind all routing through it. The driver interface is complete: on-demand **service activation** (implicit and `StartServiceByName`), `ReloadConfig`, `UpdateActivationEnvironment` (session bus), `ListQueuedOwners`, introspection and the driver properties, and **`BecomeMonitor`**, so `busctl monitor`, `dbus-monitor`, `busctl status` and `gdbus introspect` all work against it. Name ownership honors the policy's `own` rules exactly like `dbus-daemon`. Validate in `schema-vmtest` before flipping hardware.

**The session bus, too.** The same broker also runs the per-user session bus (`$XDG_RUNTIME_DIR/bus`). `scripts/schema-dbus-session-run.sh` is a drop-in for `plasma-dbus-run-session-if-needed`: it starts `schema-dbus` without `--system` (which is what selects session mode, with session.conf's allow-all policy), exports `DBUS_SESSION_BUS_ADDRESS`, and runs the session command as its child. If the broker won't come up, it falls back to stock `dbus-daemon --session`. A watchdog tears the broker down when the session exits, and ends the session if the broker dies mid-session, so the autologin loop respawns it cleanly instead of leaving a desktop with no bus. The `fedora-kde` autologin uses it by default. Proven live on a KDE Plasma desktop — kwin, plasmashell, Dolphin, Konsole, portals, the tray.

---

## Running packaged software

The D-Bus surface above lets tools *manage* schema-init units as though systemd were running. The other half of the compatibility story is **install-time**: an RPM or deb scriptlet that runs `systemctl enable foo` or `systemctl daemon-reload` must not error out on a schema-init box, and the `foo.service` it just dropped into `/usr/lib/systemd/system` has to become something schema-init can actually run. Two pieces cover this — a translator, not an emulator.

**The shim (`schema-systemctl`).** A `systemctl(1)` drop-in that packaging swaps in for `/usr/bin/systemctl` (via `alternatives` + a symlink). It honours systemd's verb and exit-code contract so scriptlets succeed: lifecycle verbs (`start`/`stop`/`restart`/`status`/`is-active`/`is-enabled`) drive `schema-ctl` for real, `enable`/`preset` record enable-intent to `/var/lib/schema-init/pending.list` (deduplicated) — the hand-off to the importer — and `daemon-reload` and the other no-op-here verbs exit `0` so nothing a caller pipes into hard-crashes. All logic lives in `systemctl_shim.h` (header-carried, static-inline; `schema-systemctl.c` is a thin entry point). Packaging is idempotent and reversible: `%post` saves the stock binary as `systemctl.real`, `%postun` restores exactly that — and on a box that never had systemd, removes the now-dangling shim symlink instead. Verified end-to-end through a real `dnf install`/`dnf remove` round-trip in a `fedora:44` container.

**The importer (`schema-import`).** `distros/fedora-installer/migrate/schema-import.py` drains `pending.list` and translates each queued `.service` into a native `.svc` on 80/20 field coverage: `ExecStart` → `exec=`/`args=` (with `$VAR`/`${VAR}` resolved against the unit's own `Environment=`, since schema-init `exec()`s with no shell), `Type=oneshot` → `oneshot=1`, `Type=notify`/`notify-reload` → `notify=1`, `Type=dbus` + `BusName=` → `ready_bus_name=`, systemd's `Restart=no` default → `no_restart=1`, `User=` → `user=` (else `needs_root=1`), and `Environment=` → `env=` — a `.svc` key applied via `setenv` in the child before `execv`. `Type=forking` with an absolute `PIDFile=` → `pid_file=`. Known ratholes are **logged and skipped, never half-translated**: `Type=forking` without one, `Type=dbus` without `BusName=`, template units (`foo@`), and units with no usable `ExecStart`. A few are skipped by name: `rsyslog` (journal-sink owns `/dev/log`), `abrtd`/`abrt-*` (schema-coredump owns cores), first-boot wizards (`plasma-setup`, `initial-setup`, `gnome-initial-setup`), and `intel_lpmd` unless the CPU is an Intel hybrid part, the only kind it runs on. Each skipped unit leaves `<name>.svc.skipped` beside the generated `.svc` files — the reason and the original unit, commented out — so it can be written by hand; PID 1 loads only `*.svc`, so the note is inert. A unit whose file isn't found is left queued; the drain is idempotent and rewrites the queue atomically (`--dry-run`/`--force` available). Design notes: `docs/superpowers/specs/2026-09-14-schema-systemctl-shim-design.md`.

Together this is the deployability turn: reclamation stops being "rewrite each daemon by hand" and becomes "install whatever ships, and import its units."
