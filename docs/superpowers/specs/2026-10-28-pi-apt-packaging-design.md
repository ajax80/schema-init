# schema-init on Raspberry Pi — apt packages and updates (project 1 of 5)

Status: draft for Jonathan's review. Calls marked **(my call)** are Claire's,
not agreed; everything else is his.

## Intent

schema-init should work as well on a Raspberry Pi as it does on Fedora. Pis are
the most common embedded Linux boards, and embedded is where schema-init's
small footprint matters most. Today a Pi gets schema-init only by hand-copying
binaries: OrangePi runs a 0.1.3 build copied in on Sep 25 while the fleet is on
0.4.20, so fixes that already exist on master (e.g. ctl restart no longer
parking a service in DORMANT for 5 minutes) never reached it.

A Pi user should add one apt repo, install schema-init, and from then on get
updates with `apt upgrade` like any other package — the same experience
Fedora users get from COPR.

This is project 1. The rest, each with its own design later:

2. Debian housekeeping: package maintainer scripts restart services through
   schema (today they call Debian's real `systemctl`, which has nothing to talk
   to), and Debian's systemd timers run (`/run/systemd/system` makes cron.daily
   logrotate/apt/dpkg/man-db skip themselves, so OrangePi has not rotated logs
   since ~09-14).
3. A complete Pi service kit (the repo kit covers 6 of OrangePi's 15 services).
4. Clock (no RTC: logs say 1969 until sync) and dated rail.log lines.
5. The Pi kernel oops (34 on OrangePi, 33 in the `tailscale` CLI): confirm it
   is not schema, report upstream.

## Goals

- Signed .deb packages for:
  - **armhf, ARMv6 + VFP** — runs on every Pi in 32-bit mode, including
    Zero / Zero W / Pi 1. Debian's own armhf baseline is ARMv7 and would crash
    there; the build must target ARMv6.
  - **arm64** — Pi 3, 4, 5, Zero 2 W on 64-bit Raspberry Pi OS.
  - **amd64** Debian — comes nearly free from the same workflow. **Packages
  only (my call):** no enable command and no boot fallback on amd64 yet
  (Debian boots through GRUB; a GRUB fallback is later work). The README says
  so plainly.
- Target OS: Raspberry Pi OS / Debian 13 (trixie) **(my call: trixie only;
  bookworm is out unless asked)**.
- Built by GitHub Actions, the hosted builder, on every merge (dev) and every
  release tag (stable). No packages built by hand.
- Published as an apt repo on GitHub Pages with two channels, `stable` and
  `dev`, mirroring COPR `ajax80/schema-init` and `ajax80/schema-init-dev`.
- Install is one documented line plus `apt install schema-init`.
- `apt upgrade` re-execs PID 1 in place, as `dnf upgrade` does on Fedora.
- **Boot fallback for everyone:** if a boot never reaches "boot OK", the next
  boot falls back to the last schema-init that did; if there is none, it boots
  systemd. Most Pis are headless, and a Pi that will not come back means
  pulling the SD card.
- A documented, one-command way back to systemd, plus the by-hand way (edit
  `cmdline.txt` on the SD card from another computer).
- Test boxes on the dev channel: **OrangePi** (Pi Zero W, armhf) and
  **BlakPi** (Pi 3B, arm64, currently systemd; it is the backup Pi-hole and
  Jonathan accepts Pi-hole moving elsewhere if needed).

## Non-goals (this project)

- Projects 2–5 above.
- Fedora-only subpackages: `-migrate` (btrfs, BLS, dracut, grub.d) and
  `-wizard` (PySide6). `-session` (desktop seat/logind) is also out: Pi targets
  here are headless **(my call)**.
- Replacing Debian's `systemd-udevd` / `dbus-daemon` with schema-udev /
  schema-dbus on Pi. Keep Debian's for now, as OrangePi does today.
- OBS. Checked 10-28: its Raspbian 13 target is armv7l only and Debian 13 is
  x86_64 only — no arm64, and ARMv6 output is not guaranteed.

## Anti-goals

- **A Pi that cannot boot after an upgrade or install.** This outranks every
  other goal.
- Installing the package silently switching PID 1. Installing puts files on
  disk; becoming PID 1 is a separate, explicit step **(my call)**.
- Breaking a running Pi's services during upgrade (e.g. dropping BlakPi's
  Pi-hole or OrangePi's tailscale front door).
- Signing keys or fleet data in the git repo. The private key lives only in a
  GitHub Actions secret (plus an offline backup in KeePassXC, never GitHub).

## How it works

### Build (GitHub Actions)

- New workflow, separate from `ci.yml`. Triggers: push to `master` → dev; tag
  `v*` → stable.
- arm64: GitHub's free native ARM runner, in a `debian:trixie` container.
- armhf: must produce ARMv6 code, and must build against a **Raspbian /
  Raspberry Pi OS userland** (a trixie armhf rootfs under QEMU user emulation,
  or that rootfs used as a cross sysroot). Debian's own armhf libc is ARMv7;
  linking against it, statically above all, pulls ARMv7 startup code into the
  binary and a Zero W dies with SIGILL, whatever `-march` says. **Gate:** CI
  fails the armhf job unless `readelf -A` on every shipped ELF reports
  `Tag_CPU_arch: v6` or `v6KZ` (what ARM1176JZF-S builds report); v7 or newer
  fails.
- `make test` runs on arm64 natively and on armhf under QEMU.
- `schema-analyze` needs cargo; it is built on all arches if it builds cleanly,
  else dropped from the Debian package for now **(my call)**. Rust's
  `arm-unknown-linux-gnueabihf` is the ARMv6 target (v7 is `armv7-…`), which
  is what a native build inside Raspbian uses.

### Packaging

- Debian source package (`debian/` dir) built with standard tooling
  (dpkg-buildpackage). Binary packages: `schema-init` (engine, ctl, bootok,
  journal-sink, subreaper, recorder, coredump, board, user, the fallback
  wrapper) and `schema-init-daemons` only if something in it is needed on Pi
  **(builder decides from what project 1 actually ships)**. `schema-init`
  must not depend on `-daemons` (the Fedora spec's `Requires` does not carry
  over).
- Layout follows Debian policy: `/usr/bin`, `/usr/sbin`,
  `/usr/libexec/schema-init`, `/etc/schema-init`. **Nothing in `/usr/local`**
  (Policy §9.1.1; lintian errors). The Fedora package's `/usr/local/bin`
  helpers move to `/usr/libexec/schema-init` in the .deb, and any shipped
  `.svc` or code that names their path follows.
- Versions mirror COPR so channels order correctly: stable `0.4.20-1`, dev
  `0.4.20-1.<count>.<date>git<sha>`. dpkg orders `1.1324 > 1` and the next
  stable `0.4.21-1` above any `0.4.20` dev build, same as rpm. The spec version
  bump before tag stays the single source of truth.

### Repo (GitHub Pages)

- **Permanent URL: `https://ajax80.github.io/schema-init-apt`**, a separate
  repo `ajax80/schema-init-apt` holding only the published repo (Jonathan,
  10-28). Every user's apt source points here, so it never moves.
- The dev channel keeps the last 10 builds per arch; stable keeps every
  release until size forces pruning. Stay well under the 1 GB Pages cap.
- Publish as a Pages **artifact** (`actions/deploy-pages`), not as git commits
  of .debs: git history would keep every deleted package and outgrow the
  quota within weeks. Where the retained pool lives between runs (GitHub
  Releases, an Actions cache, or fetched back from the live site) is the
  builder's.
- Signed with a dedicated repo key (`InRelease` + `Release.gpg`).
- One suite per channel (`stable`, `dev`), component `main`, arches
  `armhf arm64 amd64`.
- Keep at least the last few versions in the pool so a user can
  `apt install schema-init=<older>` to roll back.
- Install steps in the README, in the shape users know from Tailscale:
  keyring to `/usr/share/keyrings/`, and a deb822
  `/etc/apt/sources.list.d/schema-init.sources` with `Signed-By:` (trixie's
  default format).

### Install, enable, upgrade

- `apt install` puts files down and stops. It prints how to enable.
- `schema-ctl pi enable` (name is the builder's) backs up
  `/boot/firmware/cmdline.txt`, adds `init=/sbin/schema-init-log`, and seeds
  the known-good copy. `schema-ctl pi disable` restores systemd. Both are
  idempotent and keep `cmdline.txt` on one line.
- On upgrade, re-exec PID 1 in place (`schema-ctl reexec`) when PID 1 is
  schema-init; if re-exec refuses, say so and leave the old binary running
  until reboot. dpkg has no `%posttrans`, and `postinst` runs mid-run while
  other packages are still unpacking, so the re-exec runs from a **dpkg
  trigger** the postinst activates, which dpkg processes once at the end of
  the run.
- Migration of OrangePi from its hand install: its 15 `.svc` files and
  `/usr/local/bin` wrapper scripts stay as-is and unmanaged by the package;
  only the binaries and the wrapper become package-owned.

### Boot fallback

Replace OrangePi's shell wrapper `schema-init-log` with a **small static C
program** (the kernel's `init=` target). A shell script as PID 1 dies, and
panics the kernel, if a bad upgrade breaks `/bin/sh` or its libraries; a
static binary has no such dependency. The wrapper:

- remounts `/` read-write if needed, reads and updates the boot counter under
  `/var/lib/schema-init/`, and `sync`s before going on, so the count survives
  a reset;
- passes its arguments and environment through unchanged to whatever it
  execs;
- before exec'ing systemd, points stdin/stdout/stderr back at `/dev/console`
  (today's wrapper sends them to a log file, which would break systemd's
  emergency shell and serial console).

**Boot counter states:** `boot_attempts` (0, 1, 2) and a `last_fallback`
record (when, and to what). boot-ok sets `boot_attempts=0`. When the wrapper
falls back to systemd it also resets `boot_attempts=0` and writes
`last_fallback`; otherwise the next boot would see 2 and go straight to
systemd again, which is permanent systemd by accident. `schema-ctl status`
and `fallback.log` report `last_fallback`.

- A `boot-ok` oneshot clears the counter once the boot is healthy, and only
  then promotes the running set to known-good.
- **"Boot OK" = sshd up and every service running (Jonathan, 10-28):** all
  services FUNDAMENTAL/PERFECT, none failed, excised or given up. A network
  address is **not** required. A router outage or a Pi moved to a new network
  leaves every service running without a lease, so it is not a schema failure
  and must not cost reboots. A schema bug that breaks Wi-Fi still shows up as
  a failed `wpa-supplicant` or `dhcpcd` and still counts. The Fedora
  `schema-bootok` waits for a compositor, which is wrong for headless Pis, so
  the Pi default differs.
- **Boot-OK deadline (Jonathan, 10-28):** if boot-ok has not fired about
  5 minutes after PID 1 starts, the Pi reboots itself to try again. A Zero W
  boot is ~50 s plus up to 30 s for DHCP, so 5 min leaves margin **(exact
  value my call, configurable)**. The deadline is **a small supervised
  service, not code in PID 1 (my call, per Greg's review):** it reboots
  cleanly through `schema-ctl` when time runs out, needs no watchdog, and
  ships only in the Pi config, so Fedora's PID 1 stays untouched. PID 1 keeps
  holding `/dev/watchdog` (`init.c`) for true hangs; verify `bcm2835_wdt` on
  Zero W and Pi 3/4/5 kernels.
- **After two failed boots in a row:** boot the known-good set if there is one
  that differs from what just failed; otherwise `exec /lib/systemd/systemd`
  directly. **systemd is for that one boot only (Jonathan, 10-28)**: `init=`
  stays in `cmdline.txt` and the next boot tries schema-init again. The
  systemd boot has no deadline, so it stays up until something else reboots
  it; the worst case is two reboots, not a loop. Every fallback is logged to
  `/var/log/schema-init/fallback.log` and shown by `schema-ctl status`.
- **Known-good = the whole boot set (my call):** schema-init, schema-ctl,
  the boot-ok check, the deadline service, and a snapshot of
  `/etc/schema-init/services/`, copied to `/var/lib/schema-init/known-good/`
  and run from there (the builder decides how the known-good engine is
  pointed at the snapshot service dir). dpkg-owned files are never
  overwritten, so dpkg's view stays true and the next `apt upgrade` behaves
  normally. `pi enable` refuses if `/var/lib` is on a `noexec` mount.
- **systemd must be there to fall back to (my call):** the package
  `Depends: systemd`, and `pi enable` refuses if `/lib/systemd/systemd` is
  missing. Checked 10-28: both test Pis still have systemd installed, and
  OrangePi still has its systemd units for NetworkManager, ssh and tailscale
  enabled. The hardware test proves a systemd fallback boot is reachable over
  the network on each box.
- Pi firmware `tryboot` (one-shot alternate boot) is a possible stronger
  mechanism; out of scope unless the wrapper approach proves insufficient.

## Testing

- CI: build + `make test` on all three arches; the ARMv6 readelf gate.
- Install test in CI: a `debian:trixie` container per arch installs from the
  freshly built repo and runs `schema-init --version`.
- Fallback test: the VM harness (`tests/livetest/vmtest.sh`) or a QEMU
  aarch64 boot proves both fallback paths (bad binary → known-good; no
  known-good → systemd) before any Pi sees it.
- Hardware: OrangePi then BlakPi move to the dev channel. Each must pass:
  install from repo; enable; reboot to schema-init PID 1; one dev upgrade with
  live re-exec; a deliberately bad binary falls back on the next boot; disable
  returns to systemd. BlakPi's Pi-hole and OrangePi's tailscale serve keep
  working throughout.

## Decided by Jonathan

- Pi parity with Fedora is the goal; day-to-day rough edges are the focus.
- All Pis: Zero W class (32-bit) and 64-bit.
- Hosted builder, like COPR → GitHub Actions + GitHub Pages apt repo (over OBS).
- BlakPi = 64-bit test box; OrangePi = Zero W test box.
- Keep the boot fallback for everyone.
- A Pi that never reaches boot OK reboots itself after ~5 min; after two
  failed boots it falls back, and systemd is one boot only.
- Boot OK = sshd up and every service running; a network address is not
  required (after Greg's review).
- Repo lives at `ajax80.github.io/schema-init-apt` (separate repo), dev keeps
  the last 10 builds.

## Left to the builder

- QEMU vs cross-compile for armhf (subject to the ARMv6 gate).
- Debian packaging details: package split, maintainer-script specifics,
  `conffiles`.
- Repo tooling (reprepro, aptly, or plain `dpkg-scanpackages` + signing).
- Name and exact UX of the enable/disable command.
- How the boot-ok readiness checks are expressed.
