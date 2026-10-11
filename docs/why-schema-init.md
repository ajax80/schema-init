# Why schema-init

A modern Linux desktop boots into far more than its init. Next to PID 1 sit `journald`, `logind`, `udevd`, `resolved`, a D-Bus broker and a set of timers. Every one of them stays resident, holds memory and wakes the CPU on its own schedule, whether you need it or not. Most people never see any of this, and most people never need to.

schema-init is for the people who do care about it. Here are the reasons someone would want it.

## Your machine is older than the software expects

Daemons cost the same on every machine, so they take a bigger share of a small one. On a 4 GB laptop the stack around PID 1 decides whether you can keep a browser and one more program open, or whether the machine starts swapping.

On the same laptop with the same desktop, a stock Fedora 44 KDE install converted to schema-init draws the desktop **27.5 s sooner** and carries **439 MB less RAM** at idle. On an older i3 laptop, idle swap dropped from hundreds of MB to zero. The method and raw data for both are in [Numbers](benchmarks.md).

The author's own desktop was about to be retired. It wasn't retired, and it now does just about everything ([the story](../README.md#my-old-machine-got-better)).

## You want to read the thing that runs your computer

PID 1 is the most trusted process on the system. schema-init is one static binary of about 1.2 MB once stripped, and it runs a single thread. Each daemon it replaces is a separate, single-purpose program that you can read from top to bottom in an afternoon:

- `schema-logind`: sessions, seats, power, plus the `hostname1`, `timedate1`, `locale1` and `systemd1` D-Bus surfaces
- `schema-udev`: device management, authoritative over `/dev`
- `schema-journal-sink`: accepts journald-style logging and writes plain text files, with no journal database
- built-in `.svc` timers, which replace cron and systemd `.timer` units
- `schema-dbus`: a third D-Bus broker, alongside `dbus-daemon` and `dbus-broker`

Services are described in small key=value [`.svc` files](svc-reference.md). The supervisor is a [state machine](how-it-works.md) you can draw on a napkin. When a service misbehaves, `schema-ctl` tells you which state it is in and why it got there.

## You want to own the layer, not adopt a platform

systemd keeps growing. Each release takes on more jobs: user records, home directories, network configuration, boot loading. That is a reasonable design, but it means the init layer is a project you follow, not a part you choose.

With schema-init you pick each piece. Every replacement is opt-in, runs only if it is listed, and comes out with one reboot. You can run a lean PID 1 on top of the stock daemons, replace all of them, or stop anywhere in between. Your systemd boot entry stays in the GRUB menu the whole time.

## You need a quiet machine

Once every service is stable, schema-init's main loop sleeps until something actually happens. No timer wakes it. On an idle i3 laptop with a full desktop, the CPU spent 92–99% of its time in its deepest sleep state (C10). The whole chip drew about 1.25 W, read from the hardware's own energy counters, and the load average sat at 0.03, compared with 0.10–0.20 under systemd on the same hardware.

That matters on battery. It matters even more on embedded hardware. schema-init was built as the operating layer for a robotic leg controller, where a deterministic boot and a hardware watchdog matter more than features. It has [cgroup limits, CPU pinning and a hardware watchdog](roadmap.md) for that kind of node, and it cross-compiles for aarch64 and ARMv6.

## Who it is not for

- **Anyone who needs it to be proven.** It is young. `schema-dbus` and `schema-logind` sit on the system's authorization boundary and have not had an outside security review. Don't put them on a machine other people log into until they have.
- **Servers bought on a power-savings promise.** The power figures come from one laptop. An init's own draw is a tiny slice of a server's total, so don't multiply them by a fleet size. At scale, the reasons to consider it are density, footprint, attack surface and determinism.
- **Distros beyond the tested paths.** The packaged path is Fedora KDE (COPR, the installer ISO, the in-place migrator and wizard). Other distros work from source with [`setup.sh`](install.md#build-from-source-any-distro), but you will be porting. See [Porting](porting.md).

## Trying it costs one reboot

- **No risk at all:** `git clone`, `make`, `make test`. You only need a compiler; no root, no VM.
- **Watch it boot:** run it in a VM. Your real bootloader and `/dev` are never touched.
- **On Fedora KDE:** `dnf copr enable ajax80/schema-init`, then `schema-migrate` or the `schema-wizard` GUI. Either one adds a non-default boot entry, and one command removes it.

Every step is in [Installing](install.md). If something breaks, [open an issue](https://github.com/ajax80/schema-init/issues). That is the most useful thing a new user can do.
