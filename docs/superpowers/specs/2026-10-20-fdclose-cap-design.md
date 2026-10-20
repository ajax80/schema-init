# fdclose: a hard closer cap, and no outside kill at the cap

Follow-up to #390, from the #391 code review. Three issues, fixed in this order.

## 1. Pre-6.16: a full closer cap stops all notify traffic

Without `SO_PASSRIGHTS` (kernels before 6.16) PID 1 cannot refuse fds. At
32 in-flight closers `fdclose_full()` stays 1, the notify socket leaves the
poll set, and `notify_poll` reads nothing. Any local user can make that
permanent by sending 32 fds whose `close()` never returns: a FUSE file with a
hung flush, or a TCP socket with SO_LINGER set.

While reads are paused:
- READY never arrives, so notify services sit in start until `start_timeout`
  kills them.
- WATCHDOG never arrives, so `service_watchdog_tick` aborts watchdog services,
  and `watchdog_pet` (init.c:392) **stops petting `/dev/watchdog`, so the
  hardware reboots the machine.**

### No complete fix exists before 6.16

Every fd PID 1 receives must be closed somewhere, and any close can block
forever. A bounded closer pool can always be filled. An unbounded pool is a
fork bomb on PID 1. Truncating the control message does not help: the kernel
fputs the overflow fds in the receiver's own context, and `release` can block
too (SO_LINGER). The only real fix is 6.16's `SO_PASSRIGHTS`.

### What this PR does: take away the damage, keep the stall visible

While notify reads are paused by the closer cap (`fdclose_full()` returns 1):
- **Watchdog judgment is frozen.** Each armed service's `last_pet` is held at
  now, the same treatment a frozen service already gets. No service is
  aborted, and `/dev/watchdog` keeps getting pets.
- **`start_timeout` is frozen** for notify services still waiting on READY:
  it is not checked while paused, and on resume their spawn time moves
  forward by the pause.
- **On resume every running service's pet clock restarts**, so the pause is
  not counted against a service whose READY had not been read yet.
- **One log line when reads pause and one when they resume**, naming the uid
  and pid of the senders whose fds the stuck closers hold. The admin can see
  who is doing it.

The attack goes from "kill services and reboot the box" to "stall readiness,
logged with the culprit." 6.16+ is unchanged: there `fdclose_full()` is 0 at
the cap and reads never pause.

## 2. The cap is soft: one message can start 253 closers

`fdclose_full()` is checked between messages, but `fd_close_detached` spawns
one closer per fd. One message can carry 253 fds (`NOTIFY_MAX_FDS`), and on
6.16+ messages queued before the shut still arrive.

**Fix: 32 per-fd closers plus at most one overflow closer.**
- Under the cap, each fd gets its own closer, as now.
- At the cap, fds join a parked list held by PID 1. When no overflow closer
  is running, one is spawned with the whole list. It shares the fd table and
  closes the list in order. Its memory is a copy taken at clone, so PID 1
  clears its own list right away. Fds that arrive while it runs park for the
  next one.
- The parked list holds up to 4096 (PID 1's hard NOFILE), so any batch
  fits. Notify reads pause once more than 1024 - 253 are parked, so the next
  message is sure to fit; that is section 1's pause. Before 6.16 reads now go
  on at the cap until then, instead of stopping at 32 closers.

There are never more than 33 closers. A hung fd in the overflow batch holds
back the fds behind it in that batch, but they are capped by the headroom.
On 6.16+ the parked list holds only what was already queued.

## 3. realloc failure leaves a closer untracked

With at most 33 closers, `fdclose_pids` becomes a static array of 33. There
is no realloc left to fail, so every closer is tracked and `fdclose_quiesce`
waits for every one.

## Found on the way (not in this PR)

- A notify message is attributed to its sender's cgroup by reading
  `/proc/<pid>/cgroup`. A message from a short-lived helper (`systemd-notify`
  from a shell script) that has exited by the time PID 1 reads it is dropped.
  Any read delay, including this pause, loses it. The main pid is matched
  directly, so daemons that notify themselves are unaffected.

## Not in this PR

- A closer stuck in D state can survive `fdclose_quiesce` (review item 4).
  That is the next PR.

## Tests

`make test` (tests/test_fdclose.c):
- At the cap, a 253-fd message spawns exactly one overflow closer, and
  `fdclose_n` never goes past 33.
- A second message at the cap parks while the overflow closer runs, then goes
  in the next batch once it is reaped.
- With the cap reached, `fdclose_full()` is 0 until a full message might not
  park, then 1.
- A closer records the pid and uid of the sender.

VM, `tests/livetest/notifycap-vmtest.sh` (host kernel 7.0, softdog as
`/dev/watchdog`, margin 10 s). Uid 65534 sends 33 SO_LINGER sockets (25 s),
then 1000 more fds. `wdog` is notify with watchdog_sec=3 and
watchdog_timeout_ms=4000. `late` sends READY at 8 s, with start_timeout_sec=10
and stable_secs=600. `old` simulates pre-6.16 with a seccomp wrapper that fails
setsockopt(SO_PASSRIGHTS).

| binary | kernel | result |
|---|---|---|
| master | old | wdog watchdog-abort, late start-timeout + excised, **softdog reboot at 20 s** |
| this PR | old | paused 24 s, logged `pid 69 uid 65534`; late ready on resume; 0 missed pets; max 33 closers; clean poweroff |
| master | new | no pause, late ready at 8 s, max 32 closers |
| this PR | new | same as master |
