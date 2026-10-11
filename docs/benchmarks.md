# Numbers

## What it gives back

A stock systemd boot isn't just PID 1 — it's a constellation of always-on daemons: `journald`, `systemd-logind`, `systemd-resolved`, resident `udevd` workers, timers firing on their own schedule, with `dbus-broker` alongside. Each one holds RAM and wakes the CPU whether or not you're using it. schema-init replaces PID 1 with a single static binary and **does none of that** — no journal database, no background event loops. (Socket activation is built into PID 1 itself; see [Socket activation](svc-reference.md#socket-activation).) What that machinery was holding comes back to you.

**Your RAM comes back.** On identical hardware running the identical desktop, schema-init frees roughly **half a gigabyte of RAM** that systemd's daemon stack was sitting on (~1.1 GB used at desktop vs ~1.6–2.0 GB — see [Real numbers](#real-numbers)), and idle swap drops from hundreds of MB to **zero**. In lived terms that is the difference between *a few browser tabs plus one other program before the machine starts thrashing* and **two or three browsers with ~20 tabs each and a game running at the same time** — same RAM, no upgrade. The computer you already own effectively gets bigger.

**Your power comes back.** With no ambient timer wakeups holding the cores awake, the CPU actually reaches its deepest hardware sleep state: measured **92–99% C10 residency** and **~1.25 W full-SoC package draw** at a working desktop, idle load average **0.03** (vs 0.10–0.20 under systemd). Those figures are read from Intel RAPL hardware energy counters, not estimated. Per machine it is a small, honest number — but it is *structural*, paid back every second of every idle hour. schema-init's part is simply removing the constant wakeups that keep silicon out of deep sleep in the first place.

> **On extrapolating this:** don't. These are single-node measurements on one i3 laptop. An init system's own power draw is a tiny slice of a server's total, so multiplying a per-node idle delta by a fleet size produces a number that will not survive contact with anyone who runs real hardware. If schema-init saves money at scale, the levers are **density, footprint, boot time, attack surface and determinism** — not init power draw.

**The machine goes quiet, not just lean.** One PID-1 thread instead of 20–30. A tick loop that sleeps *indefinitely* once services are stable — nothing wakes it on a schedule. No journal flush, no D-Bus polling, no watchdog chatter. The hardware is allowed to actually rest.

This isn't theory or a benchmark rig — it's a salvaged Dell Inspiron (Intel i3, 4 GB) that swapped constantly under systemd and now runs a full desktop with room to spare under schema-init. Older and low-RAM machines benefit the most: the daemons you delete are the exact ones a small machine can least afford.

---

## Real numbers

Tested on Dell Inspiron 3542 (Intel Core i3, 4GB RAM) running full Cinnamon desktop:

| Metric | schema-init | systemd (same hardware, Fedora) |
|--------|-------------|----------------------------------|
| PID 1 RSS | see [PID 1 RSS — every measurement](#pid-1-rss--every-measurement) | *(not measured on this machine)* |
| PID 1 threads | **1** | 20–30+ |
| RAM used at desktop | **~1.1 GB** | ~1.6–2.0 GB |
| Swap used | **0 MB** | 200–500 MB |
| Time to desktop | **~20.7s** | slower |

The gap is structural. schema-init spawns your services and then sleeps until something needs it. There is no journal daemon, no dbus-broker, no unit file parser running in the background.

Boot timing breakdown (Dell Inspiron 3542, Debian Bookworm, kernel 6.1.0-49, times relative to PID1 start):

```
kernel → PID 1:    6.968s
dbus               1.761s   (ready: /run/dbus/system_bus_socket)
elogind            2.739s   (ready: /run/systemd/seats)
polkitd            3.447s
udev               3.197s
network           10.505s   (oneshot)
network-manager   11.757s
getty-tty1        10.755s
sshd              10.755s
display-manager   13.760s   ← LightDM login screen visible
```

total kernel → login screen: **~20.7s**

`schema-ctl timing` produces this output.

### Fedora 44 KDE, installed from the ISO: systemd vs schema-init

Measured 2026-09-25 on an HP 15-bs2xx (Celeron N4000, 4 GB RAM, 5400 rpm HDD): a fresh v0.3.1 ISO install with the schema-udev flip done, fully `dnf upgrade`d, kernel 7.2.7 on every run. Both sides autologin into the same Plasma 6 session. Each run is a warm reboot into the named GRUB entry (`grub2-reboot`); readings are taken 4 minutes after boot by [`tests/livetest/boot-metrics`](../tests/livetest/boot-metrics). Times are seconds from kernel start to the process being spawned, read from `/proc/<pid>/stat`, so firmware and bootloader (~16.5 s here, identical for both) are not included. Medians of 3 runs; raw data in [`tests/livetest/dbox-bench-20260925.txt`](../tests/livetest/dbox-bench-20260925.txt).

| | systemd (stock Fedora) | schema-init (as installed) | schema-init + host tuning | + schema-dbus |
|---|---|---|---|---|
| kwin_wayland spawned | 45.3 s | 45.5 s | 45.6 s | **38.4 s** |
| plasmashell spawned | 64.3 s | 59.1 s | 58.9 s | **53.6 s** |
| desktop drawn (desktop.so kioworker) | 92.2 s | 87.8 s | 84.0 s | **76.9 s** |
| xdg-desktop-portal-kde spawned | 72.9 s | 60.1 s | 58.9 s | **53.6 s** |
| RAM used at idle | 1477 MB | 1104 MB | **1036 MB** | 1058 MB |
| processes | 219 | 165 | 163 | 171 |
| load average (1 min) at 4 min | 1.40 | 0.29 | 0.24 | 0.28 |
| IO pressure (`some` avg300) | 11.4 | 8.5 | 7.7 | 7.7 |

With the stock D-Bus daemon, both inits reach the compositor at the same moment, because on a 5400 rpm disk that part is bound by reading files. schema-init's own gain comes after that: the shell and portal are up sooner, and the booted machine carries 373 MB less RAM and about 54 fewer processes. "Host tuning" is per-machine configuration, not part of schema-init: zswap off (zram only), `noatime`, Wi-Fi power save off, file indexer and unused autostart entries disabled, KWin blur/contrast off. It does not move the shell start, but it draws the desktop about 4 s sooner and trims another ~70 MB.

The last column (measured 2026-09-27, medians of 4 runs) adds schema's own message bus, [schema-dbus](components.md#the-system-bus-itself-schema-dbus), on both the system and session bus. To isolate it, the same box with the same tuning was measured with the switch rolled back to the stock daemon (3 runs: kwin 46.0 s, plasmashell 61.1 s, desktop 83.5 s, 1050 MB — matching the 09-25 column), then switched back. schema-dbus alone brings the compositor up 7.6 s sooner and draws the desktop 6.6 s sooner, at the same RAM; the gain lands before the compositor starts. Raw data in [`tests/livetest/dbox-bench-20260927.txt`](../tests/livetest/dbox-bench-20260927.txt). End to end, the desktop is drawn about 15 s sooner than on stock systemd.

These are one laptop's numbers. The run-to-run spread was small (plasmashell 57.7–59.7 s on schema-init vs 63.5–64.4 s on systemd), but a different disk, CPU or desktop will give different figures.

### Fedora 44 KDE, converted in place: stock vs migrated

Measured 2026-10-09 on the same HP laptop (Celeron N4000, 3.7 GB RAM, 5400 rpm HDD), reinstalled from the official Fedora 44 KDE Live ISO and fully `dnf upgrade`d (kernel 7.2.9 on every run). The stock side was measured first; then `dnf install schema-init-wizard` from the COPR and three clicks through the wizard (deploy, schema-udev, schema-dbus), with no tuning and no hand fixes. Both sides autologin into Plasma 6 (the stock side through a temporary `plasmalogin` autologin, removed before migrating). Same method as above: warm reboots, readings 4 minutes after boot by [`tests/livetest/boot-metrics`](../tests/livetest/boot-metrics), seconds from kernel start, medians of 3 runs after one discarded boot per side. Raw data in [`tests/livetest/dbox-bench-20261009.txt`](../tests/livetest/dbox-bench-20261009.txt).

| | systemd (stock Fedora) | schema-init (migrated) |
|---|---|---|
| kwin_wayland spawned | 44.6 s | **28.7 s** |
| plasmashell spawned | 65.2 s | **43.1 s** |
| desktop drawn (desktop.so kioworker) | 98.6 s | **71.1 s** |
| xdg-desktop-portal-kde spawned | 72.5 s | **43.9 s** |
| RAM used at idle | 1872 MB | **1433 MB** |
| processes | 239 | 207 |
| load average (1 min) at 4 min | 0.69 | 0.58 |
| IO pressure (`some` avg300) | 12.2 | 7.8 |

On a box nobody tuned, the migrated install draws the desktop 27.5 s sooner and carries 439 MB less RAM. Spread was small on both sides (desktop drawn 98.5–103.7 s stock, 68.9–71.7 s migrated). The stock baseline differs from the table above (Live-ISO install instead of netinst, a newer kernel and Plasma), so compare columns within one table, not across tables.

### PID 1 RSS — every measurement

One table, every number, each naming the machine, the build and the service count it came from. Anything not listed here is not a measurement we have.

| PID 1 | RSS | Machine / conditions | Measured |
|-------|-----|----------------------|----------|
| schema-init | **1.2 MB** | minimal static boot, QEMU/KVM 512 MB / 2 vCPU | 2026-06-14 |
| schema-init | **2.6 MB** | live desktop, QEMU/KVM 512 MB / 2 vCPU | 2026-06-14 |
| schema-init | **3.3–4.0 MB** | Fedora 44, KDE Plasma + Docker/podman, **47 services**, v0.1.0 — six consecutive boots | 2026-07-16 → 07-24 |
| systemd | **20.1 MB** | Fedora Cloud Base 44 clean idle, 15 running units, same kernel, same QEMU profile | 2026-06-14 |

**On the same kernel and QEMU profile that is 8–17× lighter.** The desktop and the Cloud Base figures are *not* a fair pair — one runs KDE, the other is headless — so they are not presented as one. The only apples-to-apples comparison here is minimal-boot schema-init vs clean-idle systemd.

Two numbers this README used to carry, and why they're gone:

- **"892 KB"** was real, but it was an earlier and smaller build on the Dell. Current builds measure 1.2 MB minimal and 3.3–4.0 MB at a full desktop; the init has grown (timers, cpuset, cgroup delegation, container support). Leading with the lowest figure ever recorded, from a binary you can no longer download, isn't a footprint claim — it's cherry-picking. **892 KB was also never the binary's size on disk** (see [Binary size](#binary-size)).
- **"40 MB – 120 MB" for systemd's PID 1** was never measured by this project. The measured figure is 20.1 MB, above.

### Binary size

`make` produces an unstripped static binary. Measured on the v0.1.0 build (`601b18ac`, 2026-07-24):

| | Bytes | |
|---|---|---|
| as built (static, with debug info) | 5,607,840 | **5.6 MB** |
| after `strip schema-init` | 1,199,144 | **1.2 MB** |
| `.text` alone | 1,119,086 | 1.1 MB |

`.text` alone is 1.1 MB, so no build of this binary has ever been under 1 MB on disk. Reproduce with `ls -l`, `size` and `strip`. Note that plain `make` does **not** strip — quote 5.6 MB for what you build yourself, 1.2 MB only for a stripped binary. `make release` produces the stripped set in `release/` alongside a `SHA256SUMS` file; that is what release assets ship.

### Architectural efficiency

Live measurements from a 9-hour uptime session (Fedora 44, KDE Plasma, GreyBox — note this node runs an older, smaller build; its RSS is not comparable to the current one):

| Metric | systemd | schema-init | Architectural elimination |
|--------|---------|-------------|--------------------------|
| Idle CPU consumption | Constant ambient timer wakeups | ~0.03ms/min (1.06s over 9h) | CPU reaches deeper C-states — hardware idle, not just low-utilization idle |
| State tracking | D-Bus event loops, logging daemons | Direct POSIX shared memory / binary flag probes | Removes IPC serialization and deserialization bottlenecks entirely |
| Session tracking | utmp/logind infrastructure | Ghost sessions — `who`/`w` show 0 users | Zero inode contention on `/var/run/utmp`; `who` and `w` are zero-overhead no-ops under concurrent logins |

The load average on an idle system with schema-init as PID 1 sits at 0.03. On the same hardware with systemd, ambient timer wakeups hold it at 0.10–0.20 at idle. The difference is structural: schema-init's tick loop sleeps indefinitely once all services are stable. Nothing wakes it.

`turbostat` on Eli (Dell Inspiron 3542, Intel i3-4005U, Fedora 44, full Cinnamon desktop):

```
C10%: 92–99%    ← deepest available C-state; CPU hardware-verified
C6%:  0.00%     ← skipped; CPU goes straight to C10
Busy: 0.21–0.38%
PkgWatt: 1.23–1.32W   ← entire SoC including iGPU, read via Intel RAPL
GFX%rc6: 99.67%        ← integrated GPU in deepest sleep state
```

C10 is the deepest sleep state on Haswell silicon. Reaching it requires the CPU to sit undisturbed long enough to flush caches and power-gate internal voltage rails — typically blocked by the constant timer wakeups from systemd's watchdog, journal flush, and D-Bus polling infrastructure. At 92–99% C10 residency with a full desktop running, schema-init is generating near-zero ambient noise. The 1.25W package figure is read directly from Intel RAPL hardware energy counters, not estimated. Services with `ready_path` set promote the instant the path exists — no blind timer. `stable_secs` (default 10s) is the fallback. The remaining ~10s cluster is network/getty/sshd with no readiness path.
