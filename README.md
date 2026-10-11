# schema-init

## Documentation

| | |
|---|---|
| [Installing](docs/install.md) | ISO, in-place conversion, building from source, GRUB, filesystem setup |
| [How it works](docs/how-it-works.md) | The state machine, `.svc` files, states, shutdown, the shared-memory interface |
| [Service file reference](docs/svc-reference.md) | Every `.svc` key |
| [The reclaimed daemons](docs/components.md) | schema-udev, schema-doctor, schema-logind, schema-dbus, running packaged software |
| [Running a schema-init box](docs/operations.md) | `schema-ctl`, the recovery console, debugging, crashes, logs |
| [Numbers](docs/benchmarks.md) | Every boot, RAM, power and footprint measurement, with method and raw data |
| [Porting and distributions](docs/porting.md) | New distros, the shipped distro configs, repository layout |
| [Roadmap](docs/roadmap.md) | What's done and what's next |
| [Why schema-init](docs/why-schema-init.md) | Who it is for, and who it is not for |

**License:** AGPL-3.0-or-later. A [commercial license](#license) is available for embedding in proprietary systems.

---

<p align="center">
  <a href="assets/schema-init-trailer.mp4"><img src="assets/schema-init-trailer.gif" alt="schema-init — 30-second trailer" width="640"></a>
</p>
<p align="center"><sub>▶ 30-second trailer — <a href="assets/schema-init-trailer.mp4">watch in full resolution, with sound</a></sub></p>

## My machine got more useful after schema-init

I wrote schema-init as the operating layer for Project Daedalus, the controller for the Ungulate Leg (more in [Origin](#origin)). It had to be a PID 1 I could read end to end, with none of systemd's ideas about what a computer is for. In late May 2026 I tried it on my own desktop, a Ryzen 5 1600 with 16 GB of RAM and a GTX 1050 Ti. I was already shopping for a replacement, and that box was about to become the TV computer.

Under systemd it ran out of memory with four Firefox tabs open and Cyberpunk running. Under schema-init it runs fifteen browser tabs and Cyberpunk. Or Control plus Obsidian, a password manager, two terminals, a file manager, Steam, KDE Connect and a TV app, all at once, with 7.6 GB of the 16 still free. (That last figure was measured with Control running. The rest is my experience on one machine, not a benchmark.)

I never bought the new machine.

The measured numbers come from a test laptop: a 2017 HP with a Celeron, 4 GB of RAM and a 5400 rpm disk. On a stock Fedora 44 KDE install, converted in place with three clicks and no tuning, **the desktop is drawn 27.5 s sooner and the machine carries 439 MB less RAM at idle**. That's the median of 3 boots per side, with the method and raw data in [Numbers](docs/benchmarks.md#fedora-44-kde-converted-in-place-stock-vs-migrated).

## What it is

A minimal PID 1 init system for Linux that supervises services through a weight-state machine instead of unit files and dependency graphs — and, increasingly, a native, auditable replacement for the systemd daemons around it.

No systemd. No OpenRC. No journal daemon. At its core, just a statically linked binary that mounts your filesystems, spawns your services in dependency order, and watches them — then gets out of the way.

**It doesn't stop at PID 1.** schema-init ships small, single-purpose, native replacements for systemd's satellite daemons, which you opt into one at a time: `schema-logind` (sessions, power, seats, plus the `hostname1`/`timedate1`/`systemd1` D-Bus surfaces), `schema-udev` (device management, authoritative over `/dev`), `schema-journal-sink` (a journald-shaped endpoint that drains to a plain logfile), and built-in `.svc` timers that retire `cron` and systemd `.timer` units. Alongside them sits `schema-dbus`, a third D-Bus broker for both the system and session bus. Each is a program you can read end to end, runs only if you list it, and backs out with a single reboot. The point isn't only *less*; it's an init layer you can actually **read, top to bottom, and own**.

**PID 1 footprint: 1.2 MB RSS on a minimal boot, 3.3–4.0 MB running a 47-service KDE desktop — one thread, in every case.** Every figure names the machine, the build and the service count it was measured on: see [PID 1 RSS — every measurement](docs/benchmarks.md#pid-1-rss--every-measurement).

## Try it

Your systemd boot entries stay in the GRUB menu, and you can back out with one reboot at every step.

| You want to… | Do this | What it touches |
|---|---|---|
| Try it without risking a disk | [Build, test, boot it in a VM](docs/install.md#build-from-source-any-distro) — or boot the installer ISO in a VM | Nothing on your machine |
| Put it on a spare machine | [Install from the ISO](docs/install.md#fresh-install--the-installer-iso) | The disk you pick in the installer |
| Convert your Fedora KDE install | [COPR + `schema-migrate`](docs/install.md#convert-fedora-kde-in-place) | Adds a non-default boot entry; one command undoes it |
| Run it on another distro | [`setup.sh` from source](docs/install.md#build-from-source-any-distro) | Adds a non-default boot entry |

On Fedora KDE the short version is:

```sh
sudo dnf copr enable ajax80/schema-init
sudo dnf install schema-init-wizard
```

then run `schema-wizard`. It walks you through three reversible stages with a rollback at each one.

## Known limitations

These are real gaps, not future features being teased:

- **Young, and not independently audited.** `schema-dbus` and `schema-logind` sit on the system's authorization boundary — who may own a bus name, who may power off, who gets device access. Both are fuzzed and covered by tests, but neither has had an outside security review. Weigh that before running them on a machine other people log into.
- **Log rotation is not scheduled by default.** The `logrotate` config ships, and an example timer (`services/logrotate.svc.example`) ships alongside it, but nothing fires the rotation until you enable that timer. See [Logs](docs/operations.md#logs).
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
