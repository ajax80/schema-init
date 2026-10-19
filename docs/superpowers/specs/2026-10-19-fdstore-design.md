# fdstore — PID 1 holds a service's descriptors so a crash or update doesn't drop its clients

Status: design, 2026-10-19. Idea #3 from Greg's list.

## Why

Restarting or upgrading schema-dbus today disconnects every client. The system
broker therefore only changes at reboot, and a broker crash takes down everything
that holds a bus connection. Jonathan has hit both problems. He wants the
**desktop to stay up** when the broker dies, and he wants a **`dnf upgrade` of
schema-dbus to take effect without a reboot**.

The broker already passes unix fds between clients. The missing piece is
something outside the broker that keeps the broker's sockets and bookkeeping
alive while it restarts. PID 1 already does this for `listen=` sockets: it holds
them across restarts and across its own re-exec. fdstore extends that to fds the
service chooses to hand over at runtime, using the systemd `FDSTORE=1` protocol.

## Order (decided with Jonathan)

1. **This project: system bus.** PID 1 gets generic fdstore. schema-dbus learns
   to store its sockets and state and to resume from them. This is proven on
   `schema-dbus --system`, which PID 1 already runs from `dbus.svc`.
2. **Next project: session bus.** The session broker is started by
   `schema-dbus-session-run.sh` from the autologin script, not by PID 1, and that
   wrapper's watchdog ends the session when the broker dies (the SP4 orphaned-kwin
   fix). The per-user supervisor planned next
   reuses the fdstore code from this project. The session broker moves under it,
   and the "broker died → kill session" rule is replaced by resume. Nothing in the
   broker work is built twice.

Jonathan would prefer both buses. The session bus is the one that keeps the
desktop alive, so this project must not paint step 2 into a corner.

## Goals

- **G1 — PID 1 fdstore (generic).** A service that opts in can send fds to the
  notify socket with `FDSTORE=1` (optional `FDNAME=`) and remove them with
  `FDSTOREREMOVE=1` + `FDNAME=`. PID 1 holds them and passes them back on the
  next start after its `listen=` sockets, using `LISTEN_FDS` / `LISTEN_FDNAMES`
  (sd_listen_fds_with_names semantics). Stored fds survive:
  - the service crashing or exiting and being restarted by PID 1
  - `schema-ctl restart <svc>`
  - PID 1's own re-exec (`dnf upgrade` of schema-init) and config reload.
- **G2 — schema-dbus resumes.** On start with stored fds, the broker restores its
  listening socket, every client connection, and its bookkeeping, then carries on.
  Clients see no disconnect: no `NameOwnerChanged` for names that didn't move,
  unique names (`:1.N`) unchanged, match rules intact.
- **G3 — crash safety.** After `kill -9` of the broker at any moment, the restored
  broker is consistent. The cost of a crash is bounded:
  - The message the broker was processing at the instant of the crash may be lost.
    Its caller times out, as it would for any lost reply.
  - Any connection whose byte stream cannot be resumed exactly (e.g. the broker
    had written half of a message to it) is dropped **on its own**, with the
    normal disconnect handling (NameOwnerChanged etc.). Everyone else stays.
  - Outbound messages queued in broker memory but not yet written may be lost.
    Measure how often this matters and record it. Making the queue durable is
    not a goal.
- **G4 — planned restart is lossless.** On SIGTERM the broker always finishes
  its partial writes and leaves the state clean before exiting, with a short
  bounded drain. It does not need to know whether this is a restart or a stop:
  PID 1 keeps the store on restart and drops it on stop. An update loses nothing
  that a crash would lose.
- **G5 — live update path.** `schema-ctl restart dbus` becomes safe on a live
  desktop. The package upgrade restarts the system broker after install, the same
  way PID 1 re-execs today. The hard rule "schema-dbus only at reboot" is relaxed
  only after this has been proven on hardware (see Verification).

## Non-goals

- Session bus support (next project, see Order).
- Making schema-logind crash-proof (a later user of the same fdstore).
- A durable outbound queue, or replay of lost messages.
- Socket activation of the system bus (moving `dbus.svc` to `listen=`). The broker
  may store its own listening socket instead. That is the builder's call (below).
- fdstore for stock `dbus-daemon` (the wrapper's fallback). It ignores stored fds
  and starts fresh, as today.

## Anti-goals (fail even if it works)

- **PID 1 learns anything about D-Bus.** fdstore in PID 1 is generic: fds, names,
  counts. Nothing else.
- **The broker gets noticeably slower or heavier in normal use** just to be ready
  for a crash. Kill lines (Greg's rule: costs boot time, PID 1 size, or
  simplicity → dropped):
  - broker round-trip latency: no measurable regression on the existing
    sub-100 µs bench
  - broker RSS: < +1 MB on a desktop session's worth of connections
  - PID 1 RSS: no change beyond noise when no service uses fdstore
  - boot time: no regression beyond noise (Eli / blakbox, 3 reboots each)
- **A crash loop.** If restored state is what crashes the broker, it must not
  crash forever. After a second crash within a short window of resuming, PID 1
  (or the broker) discards the store and the broker starts fresh, just like today.
  The desktop may drop in that case. That is the floor, not a regression.
- **Leaking dead clients.** A stored client fd whose peer has gone away must not
  be held forever (e.g. a client disconnects while the broker is down).

## Constraints

- C99, the house style of the existing code (header-only `sdbus_*.h` modules,
  `notify.h`, `reexec_state.h`). No new dependencies.
- `notify_recv` currently discards datagrams carrying fds (`notify.h:155`). That
  must stay true for services that have not opted in.
- PID 1 re-exec already carries `listen_fd[]` through `reexec_state.h`
  (`X(FDS, listen_fd)`). Stored fds must ride the same mechanism, with a variable
  count. A field missing from `SVC_RUNTIME_FIELDS` is lost on reload and re-exec
  alike.
- PID 1's `RLIMIT_NOFILE` must allow the cap. The system bus runs well under 256
  connections. The per-uid connection cap is `SDBUS_MAX_CONN_PER_UID` 256.
- schema-dbus is exec'd through `schema-dbus-run.sh` (`exec env … schema-dbus
  --system`), so `LISTEN_PID` is the broker's own pid. Keep that true.
- Never restart session-holding daemons live until proven: every hardware test of a live
  broker restart is announced to Jonathan first.
- Every new in-memory or on-disk layout (stored state, reexec encoding) carries a version, bumped on any change.

## What the broker has to carry (from reading the code)

Per connection (`sdbus_conn`, `sdbus_conn.h`): fd, id, auth state
(`authed`/`auth_ok`/`said_hello`/`negotiated_fd`/`saw_nul`), peer credentials
(uid/gid/pid/groups), unique name, inbound bytes not yet parsed (`in`), received
fds awaiting a whole message (`pending_fds`), outbound queue (`oq`), match rules,
monitor flag + monitor matches.

Global: the name registry with owners and queues (`sdbus_names.h`, including
`next_unique`), the pending-reply table (`sdbus_reply.h`, needed so replies still
route and the requested-reply policy exemption still holds), pending activations
(`sdbus_activate.h`), and the broker's own outgoing serial. Policy is rebuilt from
`/run/schema-init/dbus.policy` as at boot.

Today the broker reads up to 64 KiB at a time (`on_readable`), so partial inbound
messages live in broker memory. That is the main crash-safety problem: bytes taken
off the socket and not yet saved are gone when it crashes.

## Decided (by Jonathan)

- fdstore over in-place re-exec, because crash survival matters ("I would prefer
  the desktop to stay live").
- System bus first, session bus via the per-user supervisor next.

## My calls (Claire) — Jonathan to check

- **Opt-in per service with a cap:** `.svc` key `fdstore=N` (max fds held, default
  0 = off, fds sent anyway are closed as today). `dbus.svc` gets a value sized to
  the connection cap plus headroom.
- **Stop vs restart:** `schema-ctl stop` and a service being removed drop the
  store (closing every fd). Restart, crash-restart, and PID 1 re-exec keep it.
  Matches systemd's default (no FileDescriptorStorePreserve).
- **Dead peers (revised after Greg's review):** PID 1 does *not* poll stored fds.
  It holds them until the service starts again, and drops the whole store when
  the service is stopped, goes dormant, or is excised. The broker, on resume,
  treats a restored connection that is already closed as a normal disconnect.
- **State lives in a memfd that is itself stored** (`FDNAME=state`), so PID 1
  holds the state with no knowledge of its format. The format is versioned. The
  broker refuses an unknown version and starts fresh (closing the stored client
  fds so clients reconnect, which is today's behaviour).
- **Crash-loop guard lives in the broker:** the state header carries a "resumed
  at" stamp and a counter. **The broker bumps the counter as its very first act on
  opening the state, before deserialising anything** (Greg), so a crash caused by
  bad state still counts. Counter ≥ 2 within a short window → close every stored
  fd and start fresh. This keeps PID 1 generic.

- **Format changes across an upgrade:** the broker reads its own state version
  and the one before it (N and N-1), with a migration test for each pair. A
  format two or more versions old is refused → start fresh. Otherwise most
  upgrades would disconnect everyone and G5 would be hollow.
- **Policy changed by the upgrade:** restored owners, queued owners and pending
  replies are kept. The new policy applies to new requests only. That matches
  dbus-daemon's ReloadConfig, which never revokes existing owners. No
  NameOwnerChanged on resume.
- **Which `.svc` files:** `fdstore=N` goes only in `dbus.svc` variants that run
  `schema-dbus-run.sh` (today the blakbox host variant, and any host switched to
  the broker). Stock `dbus-daemon` variants are untouched.
- **No automatic restart in the RPM yet:** the package does not restart the
  broker after install in this project. Live update is `schema-ctl restart dbus`,
  run by hand, until Eli and blakbox prove it. The scriptlet comes in a follow-up,
  and the "schema-dbus only at reboot" rule is relaxed in memory only after that
  proof.
- **PID 1 restart timing:** the first restart after a crash must be immediate
  (inside the existing restart budget, before any dormant backoff). The broker's
  crash-loop guard runs inside that budget. If PID 1 waits longer than the budget
  allows, that is a bug for this project to fix.
- **`fdstore=N` hands out `NOTIFY_SOCKET` without changing readiness:**
  `ready_path` still gates `dbus.svc`. Services without `fdstore=` or `notify=1`
  still get no `NOTIFY_SOCKET`.
- **System shutdown is a stop:** the store is dropped and there's no drain, so
  shutdown time doesn't grow.

## Decided after Greg's review (2026-10-19)

Two PRs (Jonathan):
1. **PR 1:** PID 1 fdstore (G1) + lossless planned restart of the broker (G4) +
   manual live update (G5). Delivers "update schema-dbus without a reboot".
2. **PR 2:** crash resume (G2, G3). Delivers "the desktop survives a broker crash".

Crash safety without a journal (Greg's design; this removes the Open Risk):
- **Control plane only, direct-mapped.** The state memfd holds fixed-slot tables,
  not an append-only log: connection slots (id, fd name, creds, unique name, auth
  done, monitor flag), name owners and queues, match rules, `next_unique`, the
  broker's own serial, **and the pending-reply table** (Claire: without it every
  call in flight at the crash times out, not just one message. One memory write
  per call). Each slot has a valid/generation byte written last, so a crash in
  the middle of an update leaves the old slot, or an empty one, never a torn one.
  The message data path is not mirrored. Memory writes to a shared memfd survive
  the process dying, so there is no fsync or syscall per update.
- **mid_write (Greg):** per-slot byte set before `sendmsg` in `flush_conn` and
  while a chunk is partly sent (`ch->off > 0`), cleared when the chunk completes.
  Resume drops such a connection alone, with normal disconnect signals.
- **mid_read (Claire, gap in Greg's review):** `on_readable` reads 64 KiB at a
  time, so after a crash the broker can hold part of a message the client is
  still sending. Per-slot byte set while `in_len > 0` after a read is processed,
  cleared when the buffer is empty. Resume drops such a connection alone. Whole
  messages read but not yet routed are lost (callers time out). That is within G3.
- **Listening socket:** the broker stores its own (`FDNAME=listen`) and adopts it
  on resume without the unlink+bind in `make_listen_socket`. `dbus.svc` does not
  move to `listen=`, which keeps the stock dbus-daemon fallback in
  `schema-dbus-run.sh` working.

PID 1 details the builder must not miss (verified in code):
- `notify_recv`'s control buffer holds 16 fds (`notify.h:160`); MSG_CTRUNC
  discards the whole datagram. Size it for SCM_MAX_FD (253), or cap the broker's
  batches to fit. Must agree with what the broker sends.
- `service.c:1455`: `close_range(3 + listen_open, …)` before exec would close
  stored fds. Offset by the stored count. `svc_pass_listen`'s
  `names[MAX_LISTEN * 72]` must grow for `LISTEN_FDNAMES`.
- Re-exec: stored fds need FD_CLOEXEC cleared before `fexecve` and restored if
  it fails, as `listen_set_cloexec` does for listen fds (`init.c` reexec_commit),
  plus a variable-length `name:fd` list in `reexec_state.h`.
- The main loop's `struct pollfd fds[5 + MAX_SERVICES * MAX_LISTEN]`
  (`init.c:3849`) is unaffected because PID 1 does not poll stored fds. Keep it
  that way.

## Left to the builder

- Slot table sizes and layout in the memfd (sized from the connection cap and
  the existing name/match/reply limits), and whether the memfd grows or is fixed.
- `LISTEN_FDNAMES` naming scheme for client connections (e.g. `c<id>`), and
  batching fds per notify datagram (SCM_MAX_FD is 253).
- Encoding a variable-length fd list through `reexec_state.h`.
- What happens to activations pending at crash time. Activated children
  outlive the broker. Callers waiting on an activation may time out. Record which.

## Verification (must pass before merge)

1. **Unit:** PID 1 fdstore store/remove/cap/hangup/stop-drops/restart-keeps;
   reexec carry of stored fds; broker state save/restore round-trip including a
   torn last record; crash-loop guard.
2. **Broker integration (scratch socket, no live desktop):** clients connected,
   names owned, matches added → `kill -9` broker at random points during a message
   storm (repeat ≥ 200 times) → restarted broker; assert unique names and owners
   unchanged, matches still deliver, only connections flagged mid-write are
   dropped, no fd leaks. Same with SIGTERM restart: zero drops, zero lost messages.
3. **VM (schema-vmtest):** `schema-dbus --system` under PID 1 with fdstore. Kill -9
   the broker while NetworkManager, schema-logind, polkit, udisks, upower are
   connected. Each still answers on the bus afterwards (`busctl`, `nmcli`,
   `loginctl`), and no service restarts because of the bus drop. Then
   `schema-ctl restart dbus` and a PID 1 re-exec with stores held.
4. **Hardware (Eli first, the test box; announced to Jonathan):** same as 3 on a
   live Plasma desktop. Success = he sees nothing happen on screen, and the
   network/battery/volume applets still work. Then blakbox.
5. **Kill lines** measured and written to a bench file as in the flight recorder
   (latency, broker RSS, PID 1 RSS, boot ×3 per box).

## Open risk

Largely defused by the control-plane-only design: control-plane changes happen
in a handful of places (Hello, Request/ReleaseName, Add/RemoveMatch, connection
add/remove, reply record/consume). If PR 2 still turns out to need wide changes
across the broker, stop and raise it with Jonathan. PR 1 stands on its own.
