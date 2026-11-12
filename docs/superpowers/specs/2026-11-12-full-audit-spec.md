# Full audit spec — 2026-11-12

Read-only audit of master `92ccc33` (every C source in the tree, the four
Python daemons' authorization paths, the Pi kit scripts, the shim, the
importer's directive table, CI). Nothing below is implemented. Each item has
an evidence line, a fix sketch and the test that proves it. Work the tiers in
order; every tier is a separate PR (draft until reviewed, one `/code-review`
pass each, spec bump before tag).

Tier 0 is bugs a box can hit today. Tier 1 is security. Tier 2 is memory /
RAM behaviour. Tier 3 is speed. Tier 4 is features that systemd units carry
and the importer currently drops with a warning. Tier 5 is polish.

---

## Tier 0 — bugs

### 0.1 `on_calendar=` typo turns a timer into a boot-time daemon
- `service.c:2685`: a value `parse_calendar()` rejects is logged as WARN and
  the line is *ignored*. Without `SVC_TIMER|SVC_ONESHOT` the service becomes
  an ordinary daemon: `exec=` runs at boot, exits, enters the recovery arc,
  burns 5 restarts, goes DORMANT. A nightly backup job with `on_calendar=25:00`
  runs five times at boot.
- Fix: `return -1` (service not loaded) like every other malformed key.
- Test: `tests/test_engine_conf.c` — a `.svc` with a bad `on_calendar` loads 0
  services and prints the WARN.

### 0.2 PID 1 can block forever reaping a killed card
- `engine.c:1211`: `monitor_failsafes()` does `kill(SIGKILL)` then
  `waitpid(card_pid, &st, 0)` — a blocking wait. A card stuck in D state
  (NFS, FUSE, a wedged SPI driver on the Leg) stops the main loop, the
  hardware watchdog goes unfed, the box reboots. Every other kill in PID 1 is
  already non-blocking for this exact reason (see `active_kill_service`).
- Fix: `WNOHANG`; on 0 mark `card_pid` as killed and let `reap()` finish it
  (`card_done` already handles the status).
- Test: unit test with a fake card that ignores SIGKILL is not possible; add a
  `test_card_timeout_nonblocking` under `tests/` that spawns a `sleep 60` card
  with `CARD_TIMEOUT_SECS` overridden and asserts the loop keeps ticking.

### 0.3 `schema-ctl reload` ignores a changed or removed `listen=`
- `engine.c:2913`: the shadow table takes `listen_fd[]`/`listen_open` from the
  live entry via `svc_runtime_copy` and nothing re-validates them.
  `service_listen_open()` returns early while `listen_open` is set, so a
  changed address keeps the old socket until reboot; a *removed* `listen=`
  leaves `listen_open > 0` with `listen_count == 0`, the fds leak and
  `svc_pass_listen` still hands them to the child. Re-exec gets this right in
  `init.c:790 listen_adopt()`; reload never calls it.
- Fix: move `listen_adopt` into engine.c and call it per adopted service in
  `handle_reload`, after `svc_runtime_copy`.
- Test: python integration (`tests/test_ctl_socket.py` style): reload with a
  changed `listen=` path → new socket exists, old one gone; reload with
  `listen=` removed → `/proc/1/fd` no longer holds it.

### 0.4 Duplicate `name=` across two `.svc` files is not detected
- `service.c:2371`: `name=` overrides the filename. `services_load()` never
  checks for a second entry with the same name (only `schema-ctl add` does).
  Both spawn, share one cgroup directory and one log file; every by-name
  lookup (`ctl stop`, deps, bind=) hits the first.
- Fix: after the load loop, refuse the second occurrence with a REFUSED line
  and bump `svc_refused` so reload is rejected too.
- Test: `test_engine_conf.c`.

### 0.5 `name=` accepts `/` and `..`
- `service.c:2371`: name feeds `/sys/fs/cgroup/schema-init/<name>`,
  `/var/log/schema-init/<name>.log`, the timer stamp path and the ctl protocol
  (space-separated). Root-only config, so defence in depth, but a `name=` with
  a space also breaks `status --kv` parsing for every consumer.
- Fix: allow `[A-Za-z0-9._@-]` only; refuse otherwise.
- Test: `test_engine_conf.c`.

### 0.6 Resource keys accept garbage silently
- `service.c:2646` `cpu_limit=atoi` (value > 10000 → `cpu_max_quota` returns 0
  = unlimited; `cpu_limit=50%` → 50 by accident, `cpu_limit=abc` → unlimited),
  `service.c:2658` `mem_limit=atol` (`mem_limit=512M` → 512 MB by luck,
  `mem_limit=1G` → 1 MB). Same for `watchdog_sec`, `stable_secs` negatives.
- Fix: route them through `cg_key_int()` like `pids_max`/`cpu_weight` already
  are; accept an optional `M`/`G` suffix on `mem_limit`.
- Test: `test_engine_conf.c`.

### 0.7 Service load order is directory order
- `service.c:2898`: `readdir` order decides table indices, so the start order
  among independent services, `status` output order and the shm table differ
  per filesystem and per boot. Makes `analyze` numbers noisy.
- Fix: `scandir` + `alphasort` (locale-free `strcmp`, as `svc_dropins.h` does).
- Test: existing suites keep passing; `status` order becomes stable.

---

## Tier 1 — security

### 1.1 No hardening flags on the binaries
- `Makefile:7`: `CFLAGS ?= -O2`. The RPM gets Fedora's `%{optflags}` for the
  dynamic helpers only; the deb build and `make` get nothing; PID 1 (static)
  gets nothing anywhere. No `_FORTIFY_SOURCE`, no stack protector, no RELRO,
  no PIE, no `-fstack-clash-protection`, no `-fcf-protection`.
- Fix: default
  `CFLAGS ?= -O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fcf-protection`
  (last one x86 only; guard by arch), `LDFLAGS ?= -Wl,-z,relro,-z,now`;
  dynamic helpers `-fPIE -pie`; PID 1 `-static-pie` (glibc ≥ 2.27, Fedora and
  Trixie both fine; keep plain `-static` for the armv6 Pi build if the
  toolchain refuses). Check `checksec --file` in CI for every binary in `release/`.
- Test: a `tests/test_hardening_flags.sh` running `checksec` (or `readelf -d`
  + `objdump` for the canary) over the built binaries.

### 1.2 journal-sink: any local user can hang system logging
- `schema-journal-sink.c:206`: `drain_passed_fd()` does a blocking `read()`
  on whatever fd a client passed over `/run/systemd/journal/socket` (0666).
  Pass a pipe with no writer and the single-threaded sink blocks forever;
  `/dev/log` and every syslog(3) caller lose logging until reboot.
- Fix: `fstat` the fd and accept only a regular file (memfd / O_TMPFILE), as
  journald does; `F_GET_SEALS` with `F_SEAL_WRITE` for memfds; cap at
  `MAX_FD_BYTES` with `pread`.
- Test: `tests/test_journal_sink_fd.sh` passing a pipe fd, asserting the next
  datagram still lands in the log within 1 s.

### 1.3 logind's "active uid" falls back to 1000 and ignores the real seat
- `scripts/schema-logind.py:95`: no `/run/user` entry → the active uid is
  1000; several entries → `min()`. So at the login screen (nobody logged in)
  any process running as uid 1000 passes `caller_authorized()` for
  PowerOff/Reboot/TakeDevice/SetHostname/SetTime; on a two-user box uid 1000
  is "active" while 1001 sits at the console. The trusted source the comment
  asks for now exists: `schema-session-register` writes `VTNR=` and
  `ACTIVE=` under `/run/systemd/sessions`, and `/sys/class/tty/tty0/active`
  is kernel truth.
- Fix: active uid = uid of the session file whose `VTNR` equals tty0/active
  (and `TYPE` is not tty-less), else the single session with `ACTIVE=1`, else
  **none** (deny). Drop the 1000 fallback.
- Test: `tests/test_doctor_session.py`-style fixture dir; the authz unit test
  (`tests/test_logind_*`) gains the no-session and two-session cases.

### 1.4 `TakeDevice` without a session controller
- `scripts/schema-logind.py:1449`: any process of the active uid gets evdev
  fds. Real logind hands them only to the session's controller
  (`TakeControl` first). With Yama `ptrace_scope=1` (Fedora default) same-uid
  processes cannot read each other's input otherwise, so this is a real
  widening: a keylogger in the user's session.
- Fix: require `TakeControl` from the same sender (store `(unique name, pid)`
  per session; `ReleaseControl`/disconnect clears it). `TakeControl` itself
  stays gated by `caller_authorized`.
- Test: logind unit tests: TakeDevice before TakeControl → AccessDenied.

### 1.5 `systemctl disable` deletes hand-written `.svc` files
- `systemctl_shim.h:588`: `disable` unlinks `<svc_dir>/<name>.svc` whatever
  wrote it. `sudo systemctl disable sshd` (muscle memory, or a package
  `%preun`) permanently removes a hand-tuned `sshd.svc`; systemd semantics
  would leave the unit file.
- Fix: unlink only files that carry a schema-import marker
  (`# schema-import:`/`# generated by schema-import`); for anything else
  rename to `<name>.svc.disabled` and print what happened. `enable` of a
  name with a `.disabled` twin renames it back.
- Test: `tests/test_systemctl_shim.*` — disable on a marked and on an
  unmarked file.

### 1.6 `systemctl mask`/`unmask` are silent no-ops
- `systemctl_shim.h` has no case for them; `systemctl mask foo` returns 0 and
  does nothing, so a hardening step an admin or a kickstart runs looks done
  and isn't.
- Fix: `mask` = write `<name>.svc.masked` (empty) and stop; the loader
  (`services_load`) skips a `.svc` whose `.masked` twin exists; the importer
  refuses to regenerate it; `unmask` removes the twin. `is-enabled` prints
  `masked`.
- Test: shim tests + `test_engine_conf.c`.

### 1.7 tmpfs mounts without a size
- `init.c:295` `/run`, `/dev/shm` (just above it), and `ns.c:56` every
  `private_tmp` tmpfs: no `size=`, so each defaults to 50 % of RAM. A hardened
  service with `private_tmp=1` can fill half the machine's RAM through its
  own /tmp; `/run` likewise. systemd: `/run` `size=20%,nr_inodes=800k`,
  `/dev/shm` default, PrivateTmp = a per-service subdir of the *host* /tmp
  (so the host quota applies).
- Fix: `/run` → `size=20%,nr_inodes=800k,mode=0755`; `/dev/shm` keep the
  default but add `nr_inodes=1m`; `private_tmp` → bind a fresh
  `/tmp/schema-private/<svc>` (host tmpfs or disk) onto `/tmp` instead of a
  new tmpfs, wiped at spawn and at stop. Document the change in
  `svc-reference.md`.
- Test: `tests/test_ns_private_tmp.sh` (exists? extend): df inside the
  namespace shows the host /tmp filesystem.

### 1.8 schema-dbus runs the dissolver through `PATH`
- `schema-dbus.c:1051`: `execlp("python3", …)` as root. PATH is PID 1's
  fixed one today, but the broker also runs under `schema-dbus-run.sh` and
  as a session bus with the user's PATH.
- Fix: `/usr/bin/python3` (fall back to `/usr/local/bin/python3`), and
  `SCHEMA_DBUS_DISSECT` must be absolute.
- Test: trivial; covered by the live reload test.

### 1.9 Control-socket client can hold PID 1 for 250 ms per connection
- `engine.c ctl_poll()`: a `schema` group member connects and sends no
  newline; PID 1 blocks up to `CTL_RECV_MS` reading one byte at a time. Four
  such connections (the backlog) in a loop keep the loop busy ~25 % of the
  time and delay every reap and pet.
- Fix: accept non-blocking, keep per-client line buffers, read in the poll
  loop, drop a client after 250 ms without a newline; or simply refuse the
  second concurrent non-root client. Keep the one-shot reply model.
- Test: `tests/test_ctl_socket.py`: a dribbling client does not delay a
  concurrent `status`.

### 1.10 PID 1 pages can be swapped out
- Nowhere in `init.c`: no `mlockall`. Under a swap storm PID 1 (which feeds
  the hardware watchdog, see 2.5) can stall on a page-in; with the watchdog
  open that is a reboot. The static binary plus BSS is ~4 MB.
- Fix: `mlockall(MCL_CURRENT | MCL_FUTURE)` after `engine_load`, before the
  first spawn; log the failure and carry on if it fails (containers).
- Test: `grep VmLck /proc/1/status` in `tests/livetest`.

---

## Tier 2 — memory / RAM

### 2.1 Protect CRITICAL services from reclaim and swap
- `service.c:317` writes `memory.max/high/oom.group` only. A CRITICAL
  service (pipewire, the compositor, the Leg loop) has no `memory.low` and
  can be swapped out under pressure — the audio crackle the PSI executive
  exists to prevent.
- Fix: new keys `mem_low=` (MB, → `memory.low`) and `swap_max=` (MB or 0 →
  `memory.swap.max`); default for `priority=critical`: `memory.swap.max=0`
  and `memory.low` = `mem_low` if given. Importer maps `MemoryLow=`,
  `MemoryMin=`, `MemorySwapMax=` (today in `_CG_UNMAPPED`,
  `schema-import.py:256`).
- Test: `test_cgroup_tiering.c` pure function + a root-only live check like
  #510's.

### 2.2 zram sized by RAM, with a memory limit
- `scripts/zram-swap.sh:46`: `disksize=16G` regardless of RAM and no
  `mem_limit`. On a 4 GB box zram can hold the whole of RAM as compressed
  pages before the OOM path runs; that is the thrash the service was meant
  to end. zram-generator's default is `min(RAM/2, 4G)`.
- Fix: `disksize = min(2×RAM, 8G)`, `mem_limit = RAM/2` (write
  `/sys/block/zram0/mem_limit`), both overridable by env from the `.svc`.
- Test: shell test with `SCHEMA_ZRAM_RAM_KB` injected.

### 2.3 Per-service memory in `schema-ctl status`
- Nothing reads `memory.current`/`memory.peak`/`cpu.stat` back today; the
  roadmap's `schema-top` is still open. Cheapest useful step: `status`
  gains a `mem=` column (MiB from `memory.current`) and `status <svc>` prints
  `memory.peak`, `memory.swap.current`, `cpu.stat usage_usec`,
  `memory.events` (oom_kill count). Zero cost to the loop: read on request.
- Test: `test_ctl_socket.py` on a box with cgroups.

### 2.4 Proactive reclaim of idle PERIPHERAL services (optional)
- Today reclaim runs only inside a pressure episode. A `reclaim_idle_sec=`
  per service (or a global default for `priority=peripheral`) writing
  `memory.reclaim` once the service has been FUNDAMENTAL that long and the
  box is idle (`avg10 < 1`) keeps cold daemon pages out of RAM before the
  pressure hits. Make it opt-in: it costs one timer wake per interval.
- Test: `test_reclaim.c` pure helper + live check.

### 2.5 Hardware watchdog is opened unconditionally
- `init.c:1726`: any `/dev/watchdog0` gets opened and fed. Every Intel box
  with iTCO loaded (blakbox included) runs with a 30 s hardware reboot armed
  behind PID 1, and PID 1 wakes every `timeout/3` for the rest of uptime (so
  the "sleeps indefinitely" claim is false on those boxes). systemd leaves
  the watchdog closed unless `RuntimeWatchdogSec=` is set.
- Fix: opt-in — open it when `schema.watchdog=1` is on the cmdline,
  `/etc/schema-init/watchdog` exists, or at least one loaded `.svc` sets
  `watchdog_timeout_ms` (the Leg). Log which rule armed it.
- Test: `test_engine_conf.c` for the decision function; livetest on blakbox
  that `/proc/1/fd` no longer holds the watchdog.

---

## Tier 3 — speed

### 3.1 `modules-load.d` runs serially inside PID 1 before anything spawns
- `init.c:1723`: `load_configured_modules()` forks `modprobe` per line and
  `waitpid`s each, before the control socket, the watchdog and the first
  service. `nvidia`, `vboxdrv`, `v4l2loopback` can take seconds each.
  systemd runs this as a parallel oneshot.
- Fix: one child running `modprobe -a -b <all names>` (kmod handles the
  list), not waited for; PID 1 reaps it like any orphan. Modules a `.svc`
  needs early declare `dep=modules-load` via a built-in pseudo-service, or
  the Pi kit keeps its own ordering.
- Test: `tests/test_pi_wrapper.sh`-style with a fake modprobe that sleeps:
  first spawn no longer waits for it.

### 3.2 fsck runs one filesystem at a time
- `scripts/schema-mount-fstab` `check()`: sequential `fsck -a` per entry,
  and `wait_dev` polls `blkid -l` ten times a second for up to 90 s. systemd
  fscks distinct devices in parallel.
- Fix: first pass collect entries with `passno > 0`, run their fscks in
  parallel per *device* (`&` + `wait`), then mount in order. Poll `blkid`
  every 0.5 s after the first second.
- Test: `tests/test_mount_fstab.sh` with fake `fsck.*` that sleep: total
  wall time ≈ max, not sum.

### 3.3 schema-udev runs `PROGRAM=`/`IMPORT{program}` synchronously, 180 s each
- `udev_exec.h:60` and the single event loop in `schema-udev.c`: one slow
  helper (a hung `hdparm`, a USB device that stalls `usb_id`) stops every
  hotplug event behind it; coldplug is a serial sysfs walk with the same
  cost per device. udevd runs a worker pool.
- Fix: a small fork-per-event worker pool (`workers = min(nproc, 4)`) for
  the coldplug walk and the live loop, events for the same device serialised
  by `DEVPATH` hash; per-helper timeout 30 s (udev's `event_timeout` is for
  the whole event, 180 s). Keep the `(TIMED OUT)` log line.
- Test: `tests/test_coldplug.c` + a rules fixture whose PROGRAM sleeps:
  coldplug time ≈ max, not sum; `verify-rules-live` parity unchanged.

### 3.4 Fixed one-second sleep on every shutdown
- `init.c:1827` `kill(-1, SIGKILL); sleep(1);`. Poll `waitpid(-1, WNOHANG)`
  plus `/proc` for non-child survivors at 20 ms, cap at 1 s. Saves ~1 s per
  reboot on every box.
- Test: shutdown transcript shows `SIGKILL broadcast` sooner; vmtest.

### 3.5 `shm_update()` every pass
- `engine.c:1692`: 128 × (memcpy + two O(n) scans) and a `seq++` on every
  loop wake even when nothing moved, so every reader (board, shim
  `is-active`) sees a changing seq. Gate on `pass_changed || any state/pid
  changed`, or compare before writing. Micro, but it is the hot loop.
- Test: `seq` stays constant across an idle minute.

### 3.6 Reload tooling (not perf, but the same review found it)
- `timer_catchup_tick` hard-codes the name `mount-fstab`; make it "every
  service with `ready_path` under /var" or a `provides=var` marker. Low.

---

## Tier 4 — features the importer drops today

The importer's `_UNMAPPED` list (`schema-import.py:1180`) names what real
units carry that the engine cannot express. Ordered by how often Fedora's
units use them.

### 4.1 `exec_reload=` and `schema-ctl reload <svc>`
- Today `systemctl reload nginx` (shim) becomes `restart`: dropped
  connections. Add `exec_reload=` (command list like `exec_stop`, `$MAINPID`
  expanded) and `reload_signal=HUP` (default when `exec_reload` is absent and
  the key is present). `schema-ctl reload <svc>` and the systemd1 shim's
  `ReloadUnit` use it; importer maps `ExecReload=`. Keep bare
  `schema-ctl reload` meaning config reload.

### 4.2 `restart=` policy and exit-status classes
- Engine restarts every non-oneshot death the same way; systemd's default is
  `Restart=no` and most daemons ship `on-failure`. Add
  `restart=always|on-failure|on-abnormal|no` (default `always` to keep
  today's behaviour; the importer emits the unit's), `success_exit_status=`
  and `restart_prevent_exit_status=` lists. Clean exit 0 under `on-failure`
  → PERFECT, not the recovery arc.

### 4.3 `kill_mode=` and `kill_signal=`
- `kill_mode=process` (sshd: sessions survive `stop`), `mixed`, and the
  default `cgroup`; `kill_signal=INT|QUIT|TERM`, `final_kill_signal=`.
  Today `stop` always sweeps the cgroup and always sends TERM.

### 4.4 `runtime_dir=` / `state_dir=` / `cache_dir=` / `logs_dir=`
- Importer currently fakes `RuntimeDirectory=` with an `exec_pre=/bin/sh -c
  "install -d …"` line (`schema-import.py:1311`), which costs one of the 8
  `exec_pre` slots and never cleans up. Native keys: created before spawn,
  owned by `user=`/`group=`, mode from `*_dir_mode=`, runtime one removed on
  stop; exported as `RUNTIME_DIRECTORY=` etc.

### 4.5 Kernel-surface hardening knobs
- `protect_kernel_tunables=1` (ro bind of `/proc/sys`, `/sys`,
  `/proc/sysrq-trigger`, `/proc/latency_stats`, `/proc/acpi`, …),
  `protect_kernel_modules=1` (drop `CAP_SYS_MODULE` + hide
  `/usr/lib/modules`), `protect_control_groups=1` (ro `/sys/fs/cgroup`),
  `private_devices=1` (tmpfs `/dev` with null/zero/full/random/urandom/
  tty/ptmx/pts + `device_allow=` list), `private_network=1`
  (`CLONE_NEWNET` + loopback up), `umask=`, `protect_hostname=1`
  (`CLONE_NEWUTS`), `restrict_suid_sgid=1` (Landlock ABI 6 or the
  `nosuid` remount), `memory_deny_write_execute=1`. All of these reuse
  `apply_mount_ns` and need no new mechanism beyond what `ns.c` has.
  Gate each with the same `ns_hides()` safety check and the hardening
  default stays what it is (nnp, private_tmp, protect_system, protect_home).

### 4.6 `syscall_filter=` (seccomp)
- No seccomp anywhere. A hand-built BPF (no libseccomp in a static PID 1)
  with systemd's named groups (`@system-service`, `@mount`, `@privileged`,
  …) as deny or allow lists, plus `syscall_architectures=native`. Importer
  maps `SystemCallFilter=`/`SystemCallArchitectures=`. This is the largest
  item in the list; do it last in the tier and spec it on its own.

### 4.7 Scheduling
- `nice=` (today only via `priority=`), `io_class=`/`io_priority=`
  (`ioprio_set`), `sched_policy=fifo|rr|batch|idle` + `sched_priority=`
  (the Leg loop wants FIFO), `cpu_affinity=` already exists as `cpuset=`.

### 4.8 `watch_path=` (systemd `.path` units)
- The shim refuses `.path` units. PID 1 already holds an inotify fd for
  `ready_path`; `watch_path=KIND:PATH` (`exists`, `changed`, `modified`,
  `dir_not_empty`) that arms a NEW_PROCESS when the event fires is a small
  addition and lets the importer take `.path` units.

### 4.9 Timers
- `randomized_delay_sec=` (all `on_calendar` timers fire at the same
  second today), `on_unit_inactive_sec=`, and the richer calendar forms
  in `docs/timers-design.md`.

### 4.10 `supplementary_groups=`, `dynamic_user=1`
- Importer skips `SupplementaryGroups=`; `initgroups` already runs, so the
  key is a list appended after it. `dynamic_user` allocates a uid from
  61184–65519 for the run and owns the `*_dir=` directories; lower
  priority.

---

## Tier 5 — polish and test coverage

- 5.1 Sanitizer run in CI: `make test` under `-fsanitize=address,undefined`
  for the C unit tests (the fuzz job already has clang + compiler-rt).
- 5.2 `make test` writes binaries to `/tmp/schema-test-*`: use a
  `$(TESTDIR)` under the build tree (CI shares `/tmp`).
- 5.3 `schema-ctl log [-f] <svc>` (roadmap) — tail of
  `/var/log/schema-init/<svc>.log` with the rail lines for that service
  interleaved.
- 5.4 Size-capped service logs: a `log_max_bytes=` that PID 1 enforces by
  `ftruncate`+copytruncate on the timer tick, for SD-card boxes between
  logrotate runs.
- 5.5 `schema-ctl status` colour on a tty (roadmap).
- 5.6 Docs: `svc-reference.md` gains every new key; `operations.md` gains
  the watchdog opt-in and the `.masked`/`.disabled` conventions.

---

## Order of work (suggested PR cut)

1. **PR A — Tier 0 all seven** (small, same files, one review).
2. **PR B — 1.1 build flags + 5.1 sanitizers + 5.2** (CI-only risk).
3. **PR C — 1.2 journal-sink, 1.8, 1.9, 1.10** (C daemons, no config change).
4. **PR D — 1.3 + 1.4 logind** (Python, its own test suite).
5. **PR E — 1.5 + 1.6 shim** (needs the loader to learn `.masked`).
6. **PR F — 1.7 tmpfs sizes** (needs a reboot test on blakbox + a Pi).
7. **PR G — 2.5 watchdog opt-in + 2.1 mem_low/swap_max + 2.2 zram + 2.3 status**.
8. **PR H — 3.1 modules-load + 3.4 shutdown sleep + 3.5 shm gate** (boot/shutdown timing; vmtest before and after).
9. **PR I — 3.2 parallel fsck** and **PR J — 3.3 udev workers** (each its own; J is the risky one, flip-tested on DBox first).
10. **Tier 4 one key family per PR**, in the order listed; 4.6 gets its own design spec first.

Every PR: spec section referenced in the description, `make test` green,
`/code-review` once, CI *and* `deb.yml` green after merge (lesson from
#506–#513), release notes line added to the pending list in NOW.

---

## Handoff — switching me back to Opus to carry this out

The audit was done on Fable 5.1; the implementation is Opus work by his
standing choice. In the Claude Code prompt:

```
/model opus
```

(or `/model` and pick Opus 5.5; it is saved as the default for new
sessions). Context survives the switch, but a `/clear` after it does not, so
the kickoff prompt below names the spec explicitly:

```
Read docs/superpowers/specs/2026-11-12-full-audit-spec.md and work it tier
by tier, starting with PR A (Tier 0, all seven items). One branch per PR as
the "Order of work" section cuts them, draft PR, /code-review once, fix
findings, merge when CI and deb.yml are green. Confirm the fix for 0.3 and
0.2 against the live behaviour before writing tests. Stop and ask before
Tier 4.6 (seccomp) and before 3.3 (udev workers); both need their own
design spec first.
```

What the Opus session should know that is not in the spec:
- Tier 0.2 and 1.10 are the two that can turn a stall into a hardware
  reboot on blakbox (watchdog is open there today: `/dev/watchdog0` exists,
  iTCO). Do them before any long livetest.
- blakbox runs `hardening-default = on`; 1.7's private_tmp change touches
  every hardened service on it. Test in VM 202 first.
- The Pi kit (`schema-init-pi`, `scripts/schema-pi-*`) is deb-only; 3.1
  must keep its ordering (`pi-early` → modules) — read
  `docs/superpowers/specs/2026-10-28-pi-service-kit-design.md` before
  touching `load_configured_modules`.
