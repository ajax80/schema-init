# schema-udev — Phase 1 design

**Date:** 2026-08-05
**Status:** Approved (Jonathan + Greg), ready for implementation plan
**Tier:** Reclamation Tier 2 flagship — Phase 1 of 3 (see `project_schema_reclamation`)

## Goal

Own the **"device uevent → schema match → action"** engine natively, as a standalone
daemon supervised by schema-init, running **alongside** the real `systemd-udevd`. This is
the first vertical slice of the schema-udev flagship: the same match-and-react shape as
service dependencies, applied to hardware hotplug. It is Jonathan's turf (STM32 / ESP32 /
RFID USB-serial hotplug).

Phase 1 does **not** replace `udevd`. udevd keeps owning the five load-bearing jobs
(symlink farm, persistent net names, uaccess ACLs, `/run/udev/data`, and the libudev
monitor rebroadcast). Retirement is Phases 2–3.

### The three-phase roadmap (context, not scope)

- **Phase 1 (this spec):** ingest kernel uevents via netlink, parse, match `.dev` schema
  rules, execute `on_add`/`on_remove` hooks. Alongside systemd-udevd.
- **Phase 2:** `/dev` symlink management (by-id, by-path, by-uuid) + coldplug synthetic
  uevent replay (`/sys` walker).
- **Phase 3:** `/run/udev/data` state database + libudev monitor-socket broadcasting →
  full systemd-udevd retirement.

## Why this is safe

Kernel uevents multicast on **netlink group 1**. systemd-udevd rebroadcasts its
*processed* events on **group 2**; libudev clients (PipeWire, NetworkManager, KDE, seatd,
Xorg) subscribe to group 2. schema-udev binds **only group 1** — it reads the same raw
kernel stream udevd reads, and never touches the group-2 channel those clients depend on.
Zero interference with the live desktop.

## Architecture

Standalone C binary `schema-udev`, modeled on `schema-journal-sink` (own source file, own
`.svc`, logs to stderr → journal-sink). Not baked into `init.c`.

### Netlink listener
- `socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT)`.
- Bind `struct sockaddr_nl` with `nl_groups = 1` (kernel uevents), `nl_pid = 0`
  (kernel assigns; avoids collision with udevd's own bind).
- `setsockopt(SO_RCVBUFFORCE)` to a large value (e.g. 8–16 MiB) so hotplug/coldplug storms
  don't drop under buffer pressure.
- Enable `SO_PASSCRED` to receive `SCM_CREDENTIALS`.

### Security: reject spoofed events
Any local process can `sendto()` a netlink socket. Genuine kernel uevents come from the
kernel only. For every datagram, verify:
- `SCM_CREDENTIALS` ancillary data present **and** `cred.uid == 0`, **and**
- sender `nl_pid == 0` (kernel).

Drop (log at debug) anything failing either check. This is the same trap systemd-udevd
guards against.

### Parse
Kernel netlink uevent buffer format:
```
<action>@<devpath>\0KEY=VALUE\0KEY=VALUE\0...
```
Parse into a fixed-capacity key→value map (cap 32 keys is ample; the richest observed
event has ~14). The leading `action@devpath` line is redundant with the `ACTION=` /
`DEVPATH=` keys that follow; drive matching off the KEY=VALUE pairs. Skip malformed
buffers (no NUL-terminated keys, missing `ACTION`) with a debug log.

Observed matchable keys on raw kernel netlink (ground-truth capture, USB):
`ACTION, DEVPATH, SUBSYSTEM, DEVTYPE, DRIVER, PRODUCT (vid/pid/rev), MODALIAS, DEVNAME,
MAJOR, MINOR, INTERFACE, TYPE, BUSNUM, DEVNUM, SEQNUM`. The enriched `ID_VENDOR` /
`ID_MODEL` props do **not** appear here — they are udev's own rule output — so rules match
on the kernel-supplied keys above. `SUBSYSTEM` + `PRODUCT` (vid/pid) is a clean fingerprint
for USB-serial bridges (CP210x `10c4`, CH340 `1a86`, FTDI `0403`).

### Rule grammar — `.dev`
One file per rule in `/etc/schema-init/dev/*.dev`, `key=value` per line, same felt style
as `.svc`:

```
# /etc/schema-init/dev/esp32-serial.dev
name=esp32-serial
match_subsystem=tty
match_product=10c4/ea60      # glob ok: 10c4/*
on_add=/usr/local/bin/esp32-up.sh
on_remove=/usr/local/bin/esp32-down.sh
```

Semantics:
- Each `match_KEY=pattern` maps to uevent key `KEY` (strip `match_` prefix, uppercase).
  Value is matched with **`fnmatch(3)`** (POSIX glob: `10c4/*`, `ttyUSB*`).
- A rule **matches** only if **all** its `match_*` conditions match (AND). A `match_*`
  whose uevent key is absent fails the match.
- On match: `ACTION=add` → run `on_add`; `ACTION=remove` → run `on_remove`. Other actions
  (`change`, `bind`, `move`, …) are parsed but have no Phase-1 hook key (extensible later).
- `name=` is for logging/identity only.
- Unknown keys: warn and ignore (forward-compat, mirrors `.svc` parsing tolerance).

### Action execution
- `fork()`. Child: export every uevent key as an environment variable — **including
  `ACTION`** explicitly — alongside `DEVNAME`, `DEVPATH`, `PRODUCT`, etc., then
  `execl("/bin/sh", "sh", "-c", hook, NULL)` (shell so hook strings can be simple commands).
- Parent never blocks on the hook — returns immediately to the poll loop.
- **SIGCHLD reaping:** on the signalfd SIGCHLD event, loop
  `while (waitpid(-1, NULL, WNOHANG) > 0);` to drain **all** finished children — a hotplug
  storm can fire several short-lived hooks concurrently.
- No per-hook timeout guard in Phase 1 (Phase 2 hardening item).

### Event loop
Single-threaded `poll()` over two fds:
1. the netlink socket (readable → drain all pending datagrams, parse, match, fire);
2. a `signalfd` for `SIGHUP` (reload rules), `SIGTERM`/`SIGINT` (clean exit),
   `SIGCHLD` (reap).

`SIGHUP` re-reads `/etc/schema-init/dev/*.dev` in place (same reload philosophy as
`schema-ctl reload`), swapping the rule set atomically (build new list, then replace).

### Logging
stderr, captured by journal-sink like every other service. Levels:
- startup, socket bound, rules loaded (count), reload, shutdown;
- **info = matched rules firing only** (`matched esp32-serial add /dev/ttyUSB0`) — do
  **not** log every uevent (coldplug replays 400+ devices; would flood);
- debug = parse errors, dropped spoofed datagrams, `ENOBUFS`.

## Supervision — `schema-udev.svc`

```
name=schema-udev
exec=/usr/bin/schema-udev
needs_root=1
```

- **Standard priority, not `critical`** — if it dies, real udevd still runs and the
  desktop is fine.
- **No `dep=udevd`** — schema-udev listens to the kernel (group 1) directly; it is
  independent of udevd. Starting order does not matter for correctness (only missed
  hotplugs during the gap, which Phase 2 coldplug will close).

## Deliberate Phase-1 boundaries (YAGNI)

- **No coldplug.** schema-udev reacts to hotplug occurring *after* it starts. Devices
  already present at boot fire `on_add` only on next replug. A global `/sys` "add" replay
  would make **real udevd** reprocess every device and re-apply ACLs on the live desktop —
  unacceptable risk. Deferred to Phase 2 (targeted synthetic replay only schema-udev
  consumes).
- **Not critical / no timeout guard / no `/dev` symlinks / no `/run/udev` db** — all later
  phases.

## Error handling

| Condition | Response |
|---|---|
| Netlink socket/bind fails | log, exit non-zero; service rail logs it. Not critical → no boot impact |
| `recvmsg` returns `ENOBUFS` (kernel dropped events under flood) | log at debug, continue |
| Malformed uevent buffer | skip, debug log |
| Datagram not from kernel (cred/pid check fails) | drop, debug log |
| Hook `exec` fails | child logs + `_exit(127)`; parent reaps normally |
| `/etc/schema-init/dev` missing or empty | run as pure observer (0 rules), fine |

## Testing (no hardware required)

- **Unit** (`tests/`, `make test`, mirrors `test_calendar.c`):
  - `test_uevent_parse.c` — feed captured raw netlink buffers (real ground-truth captures),
    assert the parsed key→value map, including malformed-buffer rejection.
  - `test_dev_match.c` — rule × uevent → match/no-match: glob cases (`10c4/*`, `ttyUSB*`),
    AND semantics, absent-key fails, `add`/`remove` hook selection.
- **Live integration (no unplug):** ship a throwaway `.dev` with
  `on_add=touch /tmp/schema-udev-marker` matching one device known present, then
  `echo add | sudo tee /sys/.../<device>/uevent` to emit a synthetic kernel uevent on
  group 1; assert the marker appears. Proves the full netlink→parse→match→hook path live.
- **vmtest** (`schema-vmtest`): boot schema-init as PID 1 with `schema-udev.svc` in the
  rail; confirm the service supervises clean and binds netlink. No regression.

## Files

| File | Change |
|---|---|
| `schema-udev.c` | **new** — the daemon |
| `Makefile` | build `schema-udev`, `install` to `/usr/local/sbin`, add test targets |
| `services/schema-udev.svc` | **new** — default service unit |
| `distros/fedora-kde/services/schema-udev.svc` | **new** — blakbox copy |
| `tests/test_uevent_parse.c`, `tests/test_dev_match.c` | **new** — unit tests |
| `assets/example.dev` (or `dev/` example) | **new** — commented, **inert** example rule |
| `setup.sh` | install binary + create `/etc/schema-init/dev/` |
| `README.md` | schema-udev section |

**Inert example rule:** ship the example `.dev` fully commented so a boot before hardware
is present spits out no errors for missing hook scripts. The real STM32/ESP32 rule is
authored when the hardware is on the bench.

## Definition of done (Phase 1)

- `make && make test` green (parse + match unit tests pass).
- `schema-udev` binds netlink group 1, drops spoofed datagrams, parses real uevents.
- A live synthetic-uevent integration test fires an `on_add` hook end-to-end.
- `schema-udev.svc` supervises clean under vmtest as PID 1, no regression.
- Runs alongside `udevd` with zero observed impact on PipeWire / desktop hotplug.
