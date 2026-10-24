# schema-init

<p align="center">
  <a href="assets/schema-init-trailer.mp4"><img src="assets/schema-init-trailer.gif" alt="schema-init — 30-second trailer" width="640"></a>
</p>
<p align="center"><sub>▶ 30-second trailer — <a href="assets/schema-init-trailer.mp4">watch in full resolution, with sound</a></sub></p>

## Documentation

| | |
|---|---|
| [Installing](docs/install.md) | ISO, in-place conversion, building from source, GRUB, filesystem setup |
| [How it works](docs/how-it-works.md) | The state machine, `.svc` files, states, shutdown, the shared-memory interface |
| [Service file reference](docs/svc-reference.md) | Every `.svc` key |
| [The reclaimed daemons](docs/components.md) | schema-udev, schema-doctor, schema-logind, schema-dbus, running packaged software |
| [Running a schema-init box](docs/operations.md) | `schema-ctl`, the recovery console, debugging, crashes, logs, the per-user supervisor `schema-user` |
| [Numbers](docs/benchmarks.md) | Every boot, RAM, power and footprint measurement, with method and raw data |
| [Compared](docs/compared.md) | schema-init next to systemd, sysvinit, OpenRC, runit, s6, dinit, Shepherd and BusyBox init, with head-to-head boot numbers |
| [Porting and distributions](docs/porting.md) | New distros, the shipped distro configs, repository layout |
| [Roadmap](docs/roadmap.md) | What's done and what's next |
| [Why schema-init](docs/why-schema-init.md) | Who it is for, and who it is not for |

**License:** AGPL-3.0-or-later. A [commercial license](#license) is available for embedding in proprietary systems.

---

## My old machine got better

I wrote schema-init as the operating layer for Project Daedalus, the controller for the Ungulate Leg (more in [Origin](#origin)). It had to be a PID 1 I could read end to end, with none of systemd's ideas about what a computer is for. In late May 2026 I tried it on my own desktop, a Ryzen 5 1600 with 16 GB of RAM and a GTX 1050 Ti. I was already shopping for a replacement, and that box was about to become the TV computer.

Under systemd it was sluggish, slow to boot, and ran out of memory all the time. Under schema-init it became useful. schema-init frees about 400 MB of RAM or more, and with its memory-pressure handling I can run far more at once than I ever could on stock systemd.

I never bought the new machine.

The measured numbers come from a test laptop: a 2017 HP with a Celeron, 4 GB of RAM and a 5400 rpm disk. On a stock Fedora 44 KDE install, converted in place with three clicks and no tuning, **the desktop is drawn 27.5 s sooner and the machine carries 439 MB less RAM at idle**. That's the median of 3 boots per side, with the method and raw data in [Numbers](docs/benchmarks.md#fedora-44-kde-converted-in-place-stock-vs-migrated).

## What it is

A minimal PID 1 init system for Linux that supervises services through a weight-state machine instead of unit files and dependency graphs — and, increasingly, a native, auditable replacement for the systemd daemons around it.

No systemd. No OpenRC. No journal daemon. At its core, just a statically linked binary that mounts your filesystems, spawns your services in dependency order, and watches them — then gets out of the way.

**It doesn't stop at PID 1.** schema-init ships small, single-purpose, native replacements for systemd's satellite daemons, which you opt into one at a time: `schema-logind` (sessions, power, seats, plus the `hostname1`/`timedate1`/`systemd1` D-Bus surfaces), `schema-udev` (device management, authoritative over `/dev`), `schema-journal-sink` (a journald-shaped endpoint that drains to a plain logfile), and built-in `.svc` timers that retire `cron` and systemd `.timer` units. Alongside them sits `schema-dbus`, a third D-Bus broker for both the system and session bus. Each is a program you can read end to end, runs only if you list it, and backs out with a single reboot. The point isn't only *less*; it's an init layer you can actually **read, top to bottom, and own**.

**Services can be fenced in by the kernel, one key at a time.** Besides the usual `no_new_privs`, `private_tmp` and `protect_system`, a service can be held to the paths it lists (`landlock_ro=`/`landlock_rw=`) and to the TCP ports it lists: `landlock_bind_tcp=80,443` and `landlock_connect_tcp=5432` let a web server listen only on 80/443 and reach only its database, enforced by Landlock (kernel 6.7+), with no firewall and no eBPF. See [Hardening](docs/svc-reference.md#hardening).

**PID 1 footprint: 1.2 MB RSS on a minimal boot, 3.3–4.0 MB running a 47-service KDE desktop — one thread, in every case.** Every figure names the machine, the build and the service count it was measured on: see [PID 1 RSS — every measurement](docs/benchmarks.md#pid-1-rss--every-measurement).

## Limp mode

The Ungulate Leg has a get-home mode: when a joint fails it doesn't fall over, it limps. schema-init does the same for services.

When a service fails, the recovery arc doesn't only retry it. It plays the **cards** the service's `.svc` declares, cheapest first, one per retry:

```ini
name=camera-stream
exec=/usr/bin/camera-stream
card=exec:/usr/bin/camera-reset --usb      # run as root before the next try
card=fallback:camera-stream-lowres         # give up for now, run the degraded service instead
```

- **`card=exec:CMD`** runs a fix-up command (reset a device, clear a stale cache) in its own process, and the service waits for it before trying again.
- **`card=fallback:SVC`** names a standby service that does the job in a reduced way. It sits idle until the card is played, then runs while the main service is parked, and is stopped again before the main service gets its next try. If the main service never comes back, the standby keeps going.

Each card is played once; the deck starts over after the service has run for 5 minutes. Nothing here can lose work behind your back: cards are only what the service file declares.

**The board** (`schema-board`) shows it live: every service's state in the leg's LED colours, which card it is on, which service is limping and which one is standing in for it. It runs on its own console and keeps working when the desktop has frozen. See [`card=` in the service file reference](docs/svc-reference.md) and [the recovery console](docs/operations.md#recovery-console).

## Flight recorder

When something dies without a service failing (a runaway process pins the CPU, the kernel runs out of memory, the desktop session drops you to a console), every service can still look green. `schema-recorder` keeps a short rolling record of what happened and the board shows the latest line, for example `desktop session ended - CPU pinned 20 min by cosmic-player x8`. It is opt-in. PID 1 only sends it a short message per event. On a real desktop the recorder uses about 0.2–0.3 MB (PSS) and doesn't wake up while nothing is happening; boot time and PID 1 memory didn't change across 6 reboots on two machines:

```sh
sudo cp /usr/share/schema-init/services/schema-recorder.svc.example /etc/schema-init/services/schema-recorder.svc && sudo schema-ctl add schema-recorder
schema-ctl trace                 # what happened
schema-ctl trace --since 21:00   # from a time (or an epoch) onward
```

To record earlyoom kills too, run it with `-N /usr/local/bin/schema-recorder-earlyoom`. See [the flight recorder in the operations guide](docs/operations.md#flight-recorder).

## Restarting the D-Bus broker

`schema-dbus` hands its sockets and client connections to PID 1 before it stops, then picks them up again, so `sudo schema-ctl restart dbus` no longer drops every client on the bus. Any service can do the same with `fdstore=N`. See [the descriptor store in the operations guide](docs/operations.md#descriptor-store-fdstore).

Mostly you won't need to restart it: like `dbus-daemon`, it picks up new or changed D-Bus config on its own, so a `dnf` transaction that installs a service's bus policy takes effect without a restart or a reboot. See [config changes apply live](docs/components.md).

## Try it

Your existing boot entries (systemd, or the stock init on Devuan, Void or Artix) stay in the GRUB menu, and you can back out with one reboot at every step.

| You want to… | Do this | What it touches |
|---|---|---|
| Try it without risking a disk | [Build, test, boot it in a VM](docs/install.md#build-from-source-any-distro) — or boot the installer ISO in a VM | Nothing on your machine |
| Put it on a spare machine | [Install from the ISO](docs/install.md#fresh-install--the-installer-iso) | The disk you pick in the installer |
| Convert your Fedora KDE install | [COPR + `schema-migrate`](docs/install.md#convert-fedora-kde-in-place) | Adds a non-default boot entry; one command undoes it |
| Run it on Devuan (no systemd) | [`distros/devuan`](distros/devuan/README.md): `make` + `install.sh`, beside sysvinit | Adds a non-default boot entry; `install.sh --remove` undoes it |
| Run it on Void (no systemd) | [`distros/void`](distros/void/README.md): `make` + `install.sh`, beside runit | Adds a non-default boot entry; `install.sh --remove` undoes it |
| Run it on Artix (no systemd) | [`distros/artix`](distros/artix/README.md): `make` + `install.sh`, beside dinit, s6 or OpenRC | Adds a non-default boot entry; `install.sh --remove` undoes it |
| Run it on another distro | [`setup.sh` from source](docs/install.md#build-from-source-any-distro) | Adds a non-default boot entry |

On Fedora KDE the short version is:

```sh
sudo dnf upgrade --refresh && sudo reboot
sudo dnf copr enable ajax80/schema-init
sudo dnf install schema-init-wizard
```

then run `schema-wizard`. It walks you through three reversible stages with a rollback at each one. Upgrade and reboot first: on a fresh-from-the-image install the wizard's Qt dependency pulls half of KDE forward, and that partial upgrade can fail on Fedora package conflicts; the reboot puts you on the new kernel, which is the one the schema boot entry is made for.

## Known limitations

These are real gaps, not future features being teased:

- **Young, and not independently audited.** `schema-dbus` and `schema-logind` sit on the system's authorization boundary — who may own a bus name, who may power off, who gets device access. Both are fuzzed and covered by tests, but neither has had an outside security review. Weigh that before running them on a machine other people log into.
- **`schema-logind.py` reimplements a subset of `org.freedesktop.login1`.** It models multiple concurrent sessions and seats — a session registry with one object per session, per-seat membership, and active-session tracking — which is enough for a Wayland compositor to take KMS and hand it back on a VT switch (see [Recovery console](docs/operations.md#recovery-console)) and for `uaccess` device ACLs to follow the active session. It is a targeted reimplementation, not the full daemon. It also does not set `KDSKBMODE = K_OFF`, deliberately — if the daemon died while `K_OFF` were set the console keyboard would stay dead, and `K_OFF` would also disable the kernel's ctrl-alt-F<n> VT switch that is the recovery-console escape hatch — so keystrokes still reach the tty underneath a compositor. They are neutralised rather than blocked: when the session VT is handed to the compositor, echo is turned off and pending input is flushed, and no getty runs on the session VT to read what arrives.

---

## Origin

schema-init was built by Jonathan Ayers in 2026 as the operating layer for the **Ungulate Leg** — an exoskeleton controller that needs a deterministic, schema-driven PID 1 carrying none of systemd's assumptions about what a computer is for. (A sister project, the **Samara Wing**, applies the same architecture to flight.) The reference hardware that proved it — a pawn-shop Dell Inspiron named Eli — booted to a full desktop under schema-init on May 30th, 2026. It was written alongside Claire, an AI (Claude, by Anthropic) that held continuity across the work.

---

## License

**AGPL-3.0-or-later** for open source use — full text in [LICENSE](LICENSE).  
Commercial license available for embedding in proprietary systems — open an issue or contact via GitHub.

Contributions are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md). The people behind it are in [CONTRIBUTORS.md](CONTRIBUTORS.md).

---

*Built by Jonathan Ayers. The schema was written before the code.*
