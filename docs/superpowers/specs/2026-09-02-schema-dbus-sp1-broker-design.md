# schema-dbus SP1 — the real bus (C broker), v1.0 design

**Status:** design, approved 2026-09-02. Successor to SP0 (the passive learner +
felt-policy fidelity gate). SP0 closed with a 0/0 organic gate run over a
14,979-message corpus captured from real blakbox use.

**Scope of this document:** v1.0 of `schema-dbus`, the C system-bus broker that
replaces `/usr/bin/dbus-daemon --system`. Service activation is explicitly
deferred to v1.1 (see "The v1.0 / v1.1 line"). The felt wire is SP5, out of
scope here — SP1 is **wire-compatible dbus** so every existing client (the
`schema-logind.py` / `schema-systemd1.py` shims and all of KDE) connects
unchanged.

---

## 1. Decisions locked (with rationale)

1. **v1.0 = router + names + policy; activation deferred to v1.1.**
   Smallest correct broker that boots the box and runs the desktop, provided the
   activatable-service inventory (§7) confirms nothing boot-critical is on-demand.

2. **libdbus is a codec only.** Linked purely for `dbus_message_demarshal` /
   `dbus_message_marshal` and header-field access. SP1 owns the event loop,
   connection lifecycle, auth, name registry, routing, reply-tracking, and
   policy. libdbus is the freedesktop *reference wire codec* — known knowledge,
   not one of systemd's annexed daemons (that is `sd-bus`, which we do NOT use).

3. **Policy XML stays in Python.** `felt_policy.py` already consumes the
   *dissolved* policy (output of `dissolve_tree`). The Python dissolver remains
   the single source of truth and emits a canonical pre-dissolved policy file at
   build/boot; `policy.c` loads that. No XML parsing in C; the C engine and the
   Python fidelity oracle read the identical policy text.

4. **Auth: SASL EXTERNAL, uid/gid from `SO_PEERCRED` + `getgrouplist(3)`.** We
   run the EXTERNAL line handshake ourselves and take the authoritative
   `(pid, uid, gid)` from `SO_PEERCRED` at accept, then resolve the full
   supplementary group set via `getgrouplist(uid)`. This matches the corpus's
   `gids` derivation (dbus `GetConnectionCredentials.UnixGroupIDs`, itself
   `getgrouplist`-based), keeping the oracle valid.

5. **Cutover is reboot-only and reversible.** Swap `dbus.svc`'s `exec=` to
   `/usr/local/bin/schema-dbus`; keep the stock `dbus-daemon` line commented as
   instant fallback. Never hot-swap the system bus.

6. **Single-threaded `epoll` broker.** dbus is single-threaded by design — no
   locking, matches the reference, simplest correct model.

---

## 2. Module decomposition

One file, one purpose; each testable in isolation.

| Module | Owns |
|---|---|
| `bus.c` | main + `epoll` loop over listen socket and all client fds; non-blocking I/O; outbound-buffer flush on writable |
| `conn.c` | per-connection state: fd, auth-phase vs active, unique name `:1.N`, `SO_PEERCRED` uid/gid/pid, cached supplementary gids, in/out byte buffers, its match rules, pending-reply serials |
| `auth.c` | SASL EXTERNAL line handshake incl. `NEGOTIATE_UNIX_FD`; capture `SO_PEERCRED` at accept; resolve `getgrouplist` |
| `codec.c` | thin wrapper over libdbus demarshal/marshal; extract type/serial/reply_serial/sender/dest/path/iface/member/sig; **stamp verified SENDER** |
| `names.c` | well-known→owner registry + ownership queue; RequestName/ReleaseName flag semantics; emits `NameOwnerChanged`/`NameAcquired`/`NameLost` |
| `route.c` | unicast (resolve dest well-known→unique), signal broadcast by match rules, directed-signal unicast, reply routing via `reply_serial`→caller |
| `driver.c` | the `org.freedesktop.DBus` object (method table below); `StartServiceByName` → `ServiceUnknown` (v1.1 stub) |
| `match.c` | parse/store/evaluate AddMatch rules for signal delivery |
| `policy.c` | **the crown** — C port of `felt_policy.py`; gates every `send_destination` and `own` |

**Driver methods (v1.0):** Hello, RequestName, ReleaseName, ListNames,
ListActivatableNames (static list, no activation), NameHasOwner, GetNameOwner,
AddMatch, RemoveMatch, GetConnectionUnixUser, GetConnectionUnixProcessID,
GetConnectionCredentials, GetId, Ping, Peer. StartServiceByName → error.

---

## 3. Names, routing & reply semantics

### Name registry (`names.c`)
- `unique → conn` (every connection gets `:1.N` at Hello; N monotonic).
- `well-known → owner unique` (current owner).
- per-well-known **ownership queue** (FIFO of waiters).

RequestName honors `ALLOW_REPLACEMENT`, `REPLACE_EXISTING`, `DO_NOT_QUEUE`
exactly as the reference; return codes `PRIMARY_OWNER` / `IN_QUEUE` / `EXISTS` /
`ALREADY_OWNER`. ReleaseName pops the queue and promotes the next waiter. Every
ownership transition emits `NameOwnerChanged(old,new)` plus directed
`NameAcquired`/`NameLost`. On disconnect: release all owned names, drain from all
queues, fire the transitions.

### Routing (`route.c`) — `deliver(msg)`
- **method_call / return / error with a destination** → resolve: `:1.x` direct;
  well-known → owner lookup. No owner → synthesize
  `org.freedesktop.DBus.Error.ServiceUnknown` to sender (unless it is a reply,
  which is dropped silently, per spec).
- **signal with a destination** → directed unicast to that owner (skip broadcast).
- **signal without a destination** → broadcast to every connection whose stored
  match rules accept it. Driver signals (`NameOwnerChanged` etc.) use this path.
- Delivery = marshal into target conn's **outbound buffer**; epoll flushes on
  writable. **Backpressure:** outbound buffer over cap → disconnect the client
  (reference behavior; a stuck reader must not wedge the bus).

### Security invariant — SENDER stamping
`codec.c` **overwrites** the SENDER header with the connection's verified `:1.N`
on every relayed message. A client's claimed sender is never trusted.

### Reply tracking (`conn.c` + `route.c`)
When a method_call is routed, record `(serial, caller-unique)` on the **callee**
connection. A method_return/error carrying `reply_serial` is delivered only if it
matches a pending entry on that callee→caller pair; the entry is then consumed.
This is the same fact policy leans on for the requested-reply exemption (§4).
Unmatched replies are dropped (prevents reply-spoofing). **On disconnect, purge
all pending-reply entries where the conn is caller or callee** — no leaked slots,
no stale-serial collisions.

### Unix fd passing (`SCM_RIGHTS`)
KDE and `schema-logind` pass fds over the bus (inhibitor locks, seat control).
`conn.c` uses `recvmsg`/`sendmsg` with `cmsg` buffers: collect ancillary fds on
receive and hand them to the codec alongside the bytes (libdbus `DBusMessage`
carries the fds); extract and re-send them via `cmsg` on delivery. Auth must
support `NEGOTIATE_UNIX_FD`. This is a known hang-source and gets dedicated
hardening + a live interop test.

---

## 4. Policy port & the conformance oracle

### Port (`policy.c`) — line-for-line semantics of `felt_policy.py`
Each invariant traces to a Python line and must survive translation:

- **Context ordering** default → group → user → mandatory, last-match-wins;
  start verdict `deny` (`evaluate`, felt_policy.py:170–184).
- **`send_destination` = set-membership** against the destination's owned
  well-known names (from the live registry), raw-destination fallback for
  unresolved; `*` = any non-empty set (lines 86–102).
- **Requested-reply exemption**: `method_return`/`error` with `reply_serial`
  short-circuits to `allow` before the context sweep (lines 159–171).
- **own / own_prefix** gate only `op="own"`; exact or prefix (lines 76–85).
- **user/group applicability** by uid / gid-membership, numeric-or-name
  selector; selectors resolved once at policy-load (static per boot) (lines
  122–157).
- **Unknown predicate → never matches** (line 114); surfaces as divergence, not
  a silent allow.

### Conformance oracle — how the C port is *proven*
The 14,979-message corpus + `felt_policy.py` are the golden oracle:

1. **`policy_conformance_test`** feeds every corpus record's request-tuple
   through **both** engines (Python `evaluate()` and C `policy.c`) over the
   identical dissolved policy, asserting **byte-identical verdicts on all
   14,979**. Any single divergence fails the build.
2. **Live re-gate**: `verify_dbus_policy_live.py` runs again against a fresh
   SP1-brokered capture to confirm 0/0 on the real bus — the same ritual that
   closed SP0.

Policy-correctness is earned against real adjudicated traffic, not asserted by
inspection. This is the payoff of SP0.

**Fidelity rider:** the C side must derive `gids` the same way the learner did —
`getgrouplist(uid)`, matching dbus `GetConnectionCredentials.UnixGroupIDs`
(schema_dbus_learn.py:96–99) — or the oracle drifts.

---

## 5. Auth & connection lifecycle

1. `accept()` → read `SO_PEERCRED` (pid, uid, primary gid); `getgrouplist(uid)`
   → cache full supplementary set on the conn struct.
2. SASL EXTERNAL line handshake: `AUTH EXTERNAL <hex uid>`, optional
   `NEGOTIATE_UNIX_FD`, `BEGIN`. Reject anything else.
3. First message must be `Hello` → assign `:1.N`, reply with it, mark active.
4. Active: demarshal inbound (with any cmsg fds) → stamp sender → policy-gate →
   route. Marshal outbound (+cmsg fds) into target buffers.
5. Disconnect: release names, drain queues, purge reply entries, fire
   transitions, close fd.

---

## 6. Cutover

`dbus.svc`: `exec=/usr/bin/dbus-daemon` → `exec=/usr/local/bin/schema-dbus`,
same `args=--system`, same `ready_path=/run/dbus/system_bus_socket`. Stock line
kept commented as instant fallback. Reboot-only flip (hard rule: never
`restart`, deploy via reboot for the bus). `schema-doctor` gains a check: system
bus socket present + a `Ping` to `org.freedesktop.DBus` answers.

---

## 7. Activatable-service inventory (deferral safety net)

Before the flip, enumerate all `/usr/share/dbus-1/system-services/*.service`
(67 present 2026-09-02), cross-reference the schema-init rail, produce three
buckets:

- **(a) on the rail already** → fine.
- **(b) not railed but boot/desktop-critical** → rail-pin in v1.0 so nothing
  essential goes dark.
- **(c) not railed, non-critical on-demand** → accept dark until v1.1.

Known on-demand pokes from tonight's reject log: `org.freedesktop.nm_priv_helper`,
`org.freedesktop.intel_lpmd` (both have real `.service` files; NetworkManager
itself is railed, so core networking survives). `StartServiceByName` returns
`ServiceUnknown` until v1.1 (clients treat as unavailable, do not hang).

---

## 8. Test tiers (each gates the next)

1. **Unit** — names (queue/flags/transitions), match-rule parse/eval, codec
   round-trip, auth handshake, reply-table purge.
2. **Policy conformance** — the 14,979-corpus dual-engine byte-identical gate.
   Build-breaking.
3. **Live interop** — SP1 on a scratch socket; `busctl`/`dbus-send` exercise
   Hello, RequestName contention, ListNames, GetNameOwner, signal broadcast,
   call→reply round-trip, and an **fd-passing** call. Point the real
   `schema-logind.py` / `schema-systemd1.py` shims at it; confirm they register.
4. **vmtest** — boot schema-init with swapped `dbus.svc` under QEMU; assert
   graphical reach + both shims own their names + a fresh live-gate capture reads
   0/0.
5. **Hardware** — reboot blakbox, doctor green, then a real organic live-gate
   run (same proof ritual as SP0 close).

---

## 9. The v1.0 / v1.1 line

- **v1.0 ships:** listener + auth (`SO_PEERCRED`/`getgrouplist`) + codec + names
  + routing + reply-tracking + driver methods + policy port + fd-passing. Boots
  and runs the desktop.
- **v1.1 fast-follow:** `StartServiceByName` + `.service` parsing + activation
  queue (buffer calls to a not-yet-started name, deliver on ownership); plus the
  rail-pin sweep if inventory bucket (b) is non-empty.

---

## 10. Risks

- **Policy port drift** → mitigated by the 14,979-corpus dual-engine gate
  (build-breaking) + live re-gate.
- **fd-passing bugs** → dedicated hardening + live interop test; a silent break
  hangs logind/KDE.
- **gid derivation mismatch** → pinned to `getgrouplist`/`UnixGroupIDs`.
- **Activation deferral** → audited by the §7 inventory, not assumed.
- **Backpressure / stuck reader wedging the bus** → outbound-cap disconnect.
- **Sender spoofing** → mandatory SENDER stamping.
