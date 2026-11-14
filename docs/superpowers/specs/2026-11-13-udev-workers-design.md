# schema-udev hotplug workers (PR J, audit 3.3)

Status: design, 2026-11-13. Supersedes the fix sketch in
`2026-11-12-full-audit-spec.md` §3.3 where they differ (noted below).

## Why

schema-udev handles every kernel uevent inline in one loop
(`schema-udev.c` `main()` → `dispatch()`). Anything slow inside one event
stops every event behind it:

- `PROGRAM=` / `IMPORT{program}` run through `udev_run_capture()`
  (`udev_exec.h`), which blocks up to `UDEV_EXEC_TIMEOUT` = 180 s.
- The in-process builtins (`ata_id`, `cdrom_id`, `v4l_id`, blkid probing)
  issue ioctls and reads on the device. A spinning disk in standby spins up
  first (seconds); a wedged USB bridge can block far longer.

Jonathan's priority (2026-11-13): **one stuck device must never freeze
hotplug for everything else.** Boot time is secondary.

### What blakbox shows today (recon 2026-11-13)

- No evidence of a stall yet: zero `ENOBUFS` drops across ~16 boots of
  `udevd.log`; traced live events take ~75 ms (disk change) and ~35 ms
  (input device). The rare front-USB miss shows only one USB reset in the
  kernel log, nothing udev-side.
- But live events are not timed or logged, so a stall would leave no trace.
- Coldplug is serial, 3.0–4.6 s per boot, almost all on five SATA disks.
- `RUN=` children are fire-and-forget (`run_ctx_runs()`), unlike udevd,
  which runs them in the worker and waits.

## Goals

1. An event that blocks (helper, builtin ioctl, disk spin-up) delays only
   later events for **the same device or its ancestors/descendants**.
   Unrelated devices are handled at today's latency.
2. Per-device ordering is preserved: a device's `add` → `change` → `remove`
   are processed in kernel order, and a parent's event finishes before a
   child's starts (and vice versa), as udevd does.
3. Every event has a hard deadline; a worker past it is killed and the
   device path is logged. Nothing can hold a device's queue forever.
4. Live events gain timing: a "slow" log line, so the next stall is visible.
5. `verify-rules-live` parity and the flip tests are unchanged.

## Non-goals

- Coldplug stays serial in this PR (**Claude's call**: keeps PR J to
  hotplug, which is the stated priority; the audit spec proposed pooling it
  too, that becomes a follow-up once the worker path has run on hardware).
- The `bash -c '[[ …'` syntax error seen twice per boot and the
  `pktsetup`/`pktcdvd` noise: separate small fixes.
- No pre-forked long-lived workers, no control socket protocol like udevd's
  `udevadm control`; fork-per-event is enough.

## Anti-goals (a failure even if it "works")

- Two workers corrupting shared state: a `/dev/disk/by-*` link pointing at
  the wrong device, a lost ACL, a half-written db record.
- Reordering a single device's events (e.g. `remove` handled before its
  `add` finishes, leaving a stale node or link).
- Losing kernel events because the main loop stopped reading netlink.
- Regressing schema-udev's role as the session-critical daemon: it is
  restartable live (the never-restart rule covers logind/dbus, not udev),
  but a crash still loses hotplug until restart.

## Design (decided)

### Main loop

- Keeps reading netlink and the signalfd exactly as now, but never runs
  `dispatch()` itself for live events. It appends each received datagram
  (raw bytes + length, not a parsed `struct uevent`, which is ~73 KB) to an
  in-memory FIFO.
- After every wakeup it starts as many queued events as it may (below).
- Coldplug (`subsystems_coldplug()` and `coldplug_walk_root()`) still runs
  inline before the loop, unchanged, and the ready marker still means
  "coldplug complete".

### Workers

- One `fork()` per event. The child parses the datagram and runs today's
  `dispatch()` unmodified in behaviour, then `_exit()`s. The parsed ruleset
  is shared copy-on-write.
- `dispatch()` gains a mode flag: worker mode vs inline (coldplug).
  Coldplug keeps today's behaviour exactly, including fire-and-forget RUN
  (**Claude's call**: waiting there would slow boot and has no per-event
  deadline to rescue it).
- `RUN=` programs, worker mode: the worker runs them in order and waits for each
  (udevd parity), each bounded by the per-helper timeout. Previously they
  were fire-and-forget from the single loop. (**Claude's call**: waiting is
  what makes "the event is done" true for ordering and for monitor
  listeners; it is safe now because waiting no longer blocks other devices.)
- The monitor broadcast (`monitor_broadcast()`, group 2) moves to the very
  end of `dispatch()`, after `run_rules`, the uaccess ACL apply/clear and
  the legacy `.dev` symlinks/hooks, in both modes. Today it fires mid-way,
  so kwin/libinput can see an `add` before the seat ACL exists; udevd
  announces only after everything (**Claude's call**; the flip tests check
  keyboard/mouse/DRM ACLs on the seat because of it).
- Worker stdout/stderr stay on the daemon's log, so helper output
  (`hdparm`) keeps appearing in `udevd.log`.

### Scheduling

- Two events *conflict* when any of these hold (udevd's rule):
  - DEVPATHs equal, or one an ancestor of the other (prefix test on `/`
    boundaries; `move` events use both `DEVPATH` and `DEVPATH_OLD`);
  - same device number (`MAJOR`:`MINOR` and block/char): the db record,
    `/dev/{block,char}` link, claims `devid` and ACL node are all keyed by
    it, so a stick replugged into another port (same `sdb`, new DEVPATH)
    must not run its `add` beside the old `remove`;
  - same `IFINDEX` (net devices: db records are `n<ifindex>`).
- An event may start when it conflicts with no running worker **and** with
  no earlier event still queued (so per-device queue order holds).
- Events without DEVPATH (rare) are serialised against everything.
- Concurrency cap: `max(4, min(2 * nproc, 16))` workers (**Claude's call**;
  the audit's `min(nproc, 4)` lets four sleeping disks stall everything).
- Queue cap: 4096 events. On overflow, log once per burst and drop the
  oldest *unstarted* event for a device that already has a newer queued
  event of the same action, else drop the newest; never block netlink reads.
  (The builder may pick a simpler bounded policy if it keeps the log line.)

### Timeouts

- Per helper (`PROGRAM`, `IMPORT{program}`, each worker-mode `RUN`): 30 s,
  then SIGKILL, logged with the command and DEVPATH. This replaces the
  180 s in `udev_run_capture()` in both modes, so a hung helper during
  coldplug now costs 30 s instead of 180 s.
- Per event: 180 s (udevd's `event_timeout`), enforced by the main loop on
  the worker pid: SIGKILL, log `[schema-udev] event TIMED OUT after 180s
  <ACTION> <DEVPATH>`, release the device so its queue moves on. Keep the
  existing `(TIMED OUT)` wording style.
- Builtin ioctls are only covered by the per-event deadline (they cannot be
  interrupted from inside); that is the point of moving them off the loop.
- A killed worker may leave that one device half-updated (some claims
  withdrawn, db written but no ACL). Accepted, as udevd does, and logged
  (**Claude's call**). The claims registry and db converge on the device's
  next event; a killed `remove` can leave stale links/records until the
  same device number reappears or reboot. Remove-path rules almost never
  run helpers, so this needs a wedged builtin on a vanished device.

### Logging

- `[schema-udev] slow <ms>ms <ACTION> <DEVPATH>` for live events over
  1000 ms (end-to-end, measured by the main loop from dequeue to reap).
- Reap status: a worker that dies on a signal other than our timeout kill is
  logged with the signal and DEVPATH.

### Shared state under concurrency

Each worker is a separate process, so in-memory state is not shared; what
matters is files. Audit of what `dispatch()` touches:

| State | Per-device? | Action |
|---|---|---|
| `/run/udev/data/<id>` db (`udev_db_write_full`) | yes, tmp+rename with pid | none |
| `/dev/char`, `/dev/block` farm (`node_symlink_farm`) | yes | none |
| node owner/mode (`node_apply_perms`) | yes | none |
| `/run/schema-udev-links` claims + `/dev/disk/*` resolve (`rule_links_update`) | **no**: two devices can claim one name | `flock()` a lock file in the claims dir around the whole call |
| uaccess record + ACL apply/clear | per node (devnum rule), but reads seat state | `flock()` one uaccess lock around record/apply/clear |
| NIC rename (`net_rename`) | **no**: temp-name collisions between NICs | serialise all `net` subsystem `add` events (scheduler rule) or a lock |
| legacy `.dev` rules symlinks/hooks (`symlink_apply`, `run_hook`) | names can collide | same lock as the claims registry |

Locks are taken only around those sections, never across a helper run.

### SIGCHLD, HUP, TERM

- SIGCHLD: the main loop reaps workers by pid (not `waitpid(-1)` blindly
  dropping status) to free their device slots. Grandchildren (legacy hooks)
  are the worker's problem; a worker that `_exit()`s leaves them to PID 1
  as today.
- SIGHUP: reload rules in the main process; workers forked afterwards use
  the new set, in-flight ones finish on the old.
- SIGTERM/INT: stop dequeuing, give in-flight workers up to 5 s, SIGKILL
  the rest, exit.

## Testing (required)

- Pure scheduler unit test (new `tests/test_udev_sched.c`): conflict rule
  (equal / ancestor / descendant / sibling / `/` boundary like `sda` vs
  `sda1` vs `sdab`; same devnum on different DEVPATHs; block vs char with
  equal numbers do not conflict; same IFINDEX), queue-order holding, cap,
  `move` with `DEVPATH_OLD`, no-DEVPATH serialisation.
- Daemon-level test with synthetic events (the builder adds a test-only
  event source, e.g. an env-selected socketpair or file the loop reads in
  place of netlink, compiled in always but inert unless set): a rule whose
  `PROGRAM` sleeps 5 s on device A; an event for unrelated device B
  completes in < 1 s; a second A event waits for the first; a 2 s
  test-overridden per-event deadline kills a hung worker, logs it, and A's
  next event runs.
- `make test`, `.github/ci-python-suite.sh`, `verify-rules-live` parity,
  the QEMU harness (`~/schema-livetest/vmtest.sh`) PASS.
- Hardware, in order: **DBox flip-test first** (installer-style schema-udev
  flip, plug/unplug USB storage and a keyboard, check `/dev/disk/by-*`,
  ACLs on the seat, NIC names), then blakbox. Only then merge.

## Left to the builder

- Data structures for the queue and running set; where the test event
  source hooks in.
- Whether the scheduler lives in a new header (`udev_sched.h`, preferred for
  testing) or in `schema-udev.c`.
- Exact lock file names and whether NIC renames use the scheduler rule or a
  lock.
- Queue overflow policy details, within the constraints above.
