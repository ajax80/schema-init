# Pi service kit — design

Date: 2026-10-28. Follows PR #442 (Pi apt packaging, released in v0.4.21).

## Why

Today `schema-pi enable` refuses unless the user has already written
`/etc/schema-init/services/sshd.svc`, and the README tells Pi users to write
their whole service set by hand. Nobody but Jonathan can do that. BlakPi runs
on schema-init only because of eleven hand-written services and scripts that
live on BlakPi alone.

Jonathan's picture (10-28): someone with a fresh Raspberry Pi OS (trixie)
install runs `apt install schema-init` and `sudo schema-pi enable`, reboots,
and has **ssh, network, a correct clock, and Pi-hole if they have it
installed**, without writing a single `.svc` file. Whatever else they had
running under systemd keeps running.

## Goals

1. **A shipped core kit** covering what systemd does early on Pi OS and
   schema-init does not, plus the clock:
   - loopback up, hostname from `/etc/hostname`, fstab mounts
     (`/boot/firmware`), swap on (`/dev/shm` is already mounted by PID 1,
     `init.c` early mounts — the kit does not repeat it);
   - udev and the coldplug trigger;
   - the system D-Bus;
   - the clock, set at boot without an RTC and without needing DNS
     (BlakPi's HTTP `Date` header from 1.1.1.1 / 8.8.8.8 works);
   - `pi-boot-firmware` (already shipped), `pi-bootok`, `pi-deadline`.
2. **Everything else comes from the importer.** `schema-pi enable` runs
   `schema-import --enabled`, which translates every systemd unit the box
   has enabled (ssh, NetworkManager or wpa_supplicant, pihole-FTL, unbound,
   tailscaled, cron, avahi, the Debian housekeeping timers, and whatever
   the user added). A dry run on BlakPi (10-28) imported 20 units with no
   errors.
3. **Importer fixes the Pi needs:**
   - `AmbientCapabilities=` must survive translation. Today pihole-FTL
     imports as `user=pihole` with no capabilities and cannot bind port 53.
     BlakPi works around it with `setpriv` in a hand script.
   - `apt-daily.timer` (`OnCalendar=*-*-* 6,18:00`) is skipped today, so
     the daily apt update never runs. It must translate.
   - Ordering on dbus/udev and runtime directories (`/run/sshd` for
     `sshd -D`) must be right in the imported units. The dry run showed no
     `dep=` lines at all; verify on hardware and fix what is missing.
4. **`schema-import` ships in the deb.** Today the deb does not carry it.
   It is Python; add `python3` to `Depends:` (Pi OS already has it).
5. **Boot OK counts only what you need to reach and run the box**
   (Jonathan, 10-28 — this replaces the 10-28 rule "every service
   FUNDAMENTAL/PERFECT"):
   - counted: the core kit, ssh, the network service, and **pihole-FTL when
     Pi-hole is installed** (Jonathan: Pi-hole failing counts);
   - not counted: every other imported service (avahi, udisks2,
     rpi-eeprom-update, timers, user services). They still run;
     `schema-pi status` lists any that are not up. They can never make a
     healthy Pi reboot or fall back.
6. **Detection, not flags.** Enable notices what is installed and includes
   it (Jonathan: "notice it's installed and include it"). No
   `--with-pihole` style options.
7. **BlakPi moves from its hand set onto the kit + import**, as the
   hardware proof, with its Pi-hole, DNS, tailscale and VNC still working.
8. **CI: GitHub Actions bumped to their Node 24 majors** in `deb.yml` and in
   schema-init-apt's `publish.yml` (checkout, upload/download-artifact,
   cache, upload-pages-artifact, deploy-pages are all on Node 20 majors).
   Verify each action's current major before pinning. The apt-repo push
   needs Jonathan's go at the time.

## Non-goals

- A desktop session, display manager, or console login beyond what the
  importer produces. (getty@tty1 is a template unit and is not imported; a
  console login is not required for this PR.)
- cloud-init. It is skipped by the importer (`ConditionEnvironment=`) and
  is first-boot provisioning on an already-provisioned box.
- OrangePi / Zero W. Its kernel oopses at random (114 in 8.5 h on
  6.12.75+rpt-rpi-v6), so it is held. The kit must work on armhf in
  principle; the Zero W hand kit in `distros/raspberry-pi-zero-w/` is
  folded in or retired once OrangePi is testable.
- Refreshing `pi-*.svc` on package upgrade (parked follow-up from #442).

## Anti-goals (a failure even if it works)

- Enable overwrites a `.svc` the user wrote. It never does: existing files
  win, as today.
- A flaky extra (avahi, eeprom updater) reboots a healthy Pi or sends it to
  fallback.
- A Pi that worked under systemd loses a service silently. Anything the
  importer skips or cannot translate is reported by enable, by name, with
  the reason the importer gives.
- Pi-hole comes up but cannot answer DNS (the ambient-capability gap).

## Constraints

- Debian/Pi OS trixie, arm64 and armhf (ARMv6). POSIX sh scripts
  (dash-compatible); `schema-init-pi` stays static C with fixed buffers.
- Everything goes through the existing boot guard: a kit or import mistake
  must end in the known-good/systemd fallback, never a brick.
- No `/usr/local` in the package. Hand-written BlakPi files under
  `/usr/local/bin` stay where they are until BlakPi is migrated, then are
  removed by hand (with a backup).
- Fedora is untouched: the kit and the Pi parts of enable only ship in the
  deb. Importer changes must not change Fedora output for units that
  already import correctly (the importer's existing tests must still pass).
- Never `restart` session daemons live; a changed `.svc` takes effect at
  reboot (schema-init rule).

## Decided how

- Kit files ship as `.svc.example` under `/usr/share/schema-init/` with
  scripts under `/usr/libexec/schema-init/`, the same way `pi-bootok`,
  `pi-deadline` and `pi-boot-firmware` ship today. Enable copies each into
  `/etc/schema-init/services/` only if absent.
- Names are generic (`pi-early`, `pi-timesync`, …), never host names.
- Enable order: core kit first, then `schema-import --enabled` (which never
  overwrites an existing `.svc`), then write the boot-OK list, then the
  existing known-good seed and cmdline edit.
- `sshd.svc` is no longer a precondition of enable. The precondition
  becomes "after the import, an ssh service exists"; enable refuses
  otherwise and says why.

## Calls made after the builder check (Claude's, for Jonathan to check)

- **Clock never blocks boot OK.** The README rule stands: a router outage
  is not a schema failure. The clock service tries at boot, never fails the
  boot (exits 0 like BlakPi's script), and keeps retrying in the background
  until it succeeds, then re-syncs periodically (`systemd-timesyncd` is
  reclaimed by the importer, so nothing else keeps the clock). It is in the
  core kit but not in the boot-OK list.
- **Logging is in the kit.** `journal-sink` (already shipped as
  `.svc.example`) owns `/dev/log` under schema-init, so the kit installs it.
  No `rsyslog.svc`, ever (schema-init rule). journal-sink is not in the
  boot-OK list: a logging failure must not reboot the Pi.
- **Stock udev and D-Bus**, as BlakPi runs today (`systemd-udevd`,
  `dbus-daemon`). The deb does not grow `schema-udev`/`schema-dbus` in this
  PR. Bus-activated imports (NetworkManager, wpa_supplicant) fall back to
  `stable_secs` readiness where `ready_bus_name=` needs the schema broker;
  that is acceptable.
- **Ambient capabilities are fixed for every distro**, not Debian only.
  Fedora units with `User=` + `AmbientCapabilities=` import without their
  capabilities today, which is wrong there too. Existing importer tests
  must still pass; new tests cover the caps case.
- **BlakPi is the development box (Jonathan, 10-28):** the builder may
  reboot it as often as testing needs, including deliberate failed boots
  that fall back. Each failed cycle costs the LAN's secondary DNS a few
  minutes; the primary Pi-hole covers it.
- The builder may read BlakPi's hand set (a copy is in the session
  scratchpad; the live files are on BlakPi under `/etc/schema-init/services`
  and `/usr/local/bin`).

## Folded in from Greg's review (10-28), each checked against the code

- **`RuntimeDirectory=` is not translated** (`schema-import.py` has no
  handling; `service.c` has no `runtime_dir` key). Debian's `ssh.service`
  relies on it for `/run/sshd`; without it `sshd -D` aborts and, since ssh
  counts toward boot OK, every boot fails. The importer must emit a
  privileged pre-step (`exec_pre=+/usr/bin/install -d -m <mode> -o <user>
  /run/<dir>`) or the engine gains a `runtime_dir=` key. Builder's choice;
  tested on ssh.
- **Ambient capabilities need the post-setuid sequence.** `service.c` has no
  `PR_CAP_AMBIENT` today. Ambient caps are cleared on the UID change, so the
  engine must: `PR_SET_KEEPCAPS` while root, set inheritable, drop
  groups/gid/uid, then raise each ambient cap, then exec. The svc key is a
  dedicated `ambient_caps=` (1:1 with `AmbientCapabilities=`); `keep_caps=`
  stays the bounding set.
- **Network services wait for coldplug.** The importer drops `After=` by
  design (ordering-only in systemd, while `dep=` waits for readiness), so
  imported network units start beside udev. On Wi-Fi, wpa_supplicant /
  NetworkManager can start before `wlan0` exists. Enable adds `dep=` on the
  kit's udev-trigger (and dbus for bus daemons, where the importer's
  implicit rule has not) to the detected network service, without editing
  the imported file's other lines.
- **Network detection order:** NetworkManager, systemd-networkd, networking
  (ifupdown), dhcpcd, wpa_supplicant — the first enabled one is the network
  service written to the boot-OK list.
- **ssh naming:** Debian's unit is `ssh.service`, imported as `ssh.svc`.
  `schema-pi` and `schema-pi-bootok` accept `ssh` or `sshd` (bootok
  hardcodes `sshd` today).
- **`promote()` copies `pi-boot-required` into known-good**, and bootok reads
  the known-good copy on a known-good boot.
- **unbound counts only when Pi-hole is installed and unbound is enabled**
  (coupled pair) — same as the call above.
- **Existing bugs to fix in passing:** `journal-sink.svc.example` points at
  `/usr/local/bin/schema-journal-sink`, but the deb installs
  `/usr/bin/schema-journal-sink`. `schema-pi` passes its test root to
  `schema-import` as `MIGRATE_ROOT=$SCHEMA_PI_ROOT`.
- **apt-daily's `6,18:00`:** multi-hour `on_calendar` (an hour mask in the
  calendar parser) rather than duplicate services.
- **Declined:** Greg suggests a missing `pi-boot-required` should mean
  "ssh + detected network" rather than today's rule. Kept: missing file =
  today's behaviour. The file is only missing on a Pi enabled before this
  PR (BlakPi), whose hand-picked set already passes today's rule; changing
  its gate silently on upgrade is the riskier path. Enable writes the file
  on every new or re-run enable.

## Left to the builder

- The exact kit unit names, their split, and their `dep=` graph.
- The format and location of the boot-OK list (Claude's call: a plain file,
  one service name per line, e.g. `/etc/schema-init/pi-boot-required`,
  written by enable, editable by the user, read by `schema-pi-bootok`;
  missing file = today's behaviour so already-enabled Pis keep working).
- How the importer expresses ambient capabilities (a new svc key such as
  `ambient_caps=`, or `keep_caps=` meaning ambient when `user=` is set) and
  how the engine applies them.
- How `apt-daily`'s `6,18:00` schedule maps (multi-hour `on_calendar`
  support, or two entries).
- How enable reports skipped/failed imports (stdout summary is enough).
- Whether `unbound` counts toward boot OK when it is Pi-hole's upstream
  (Claude's call: yes — Pi-hole without its upstream answers nothing).
