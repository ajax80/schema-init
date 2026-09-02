# schema-dbus SP0 — the Learner, the corpus, the felt-policy model

**Date:** 2026-09-01
**Branch:** `feat/schema-dbus-learner` (new)
**Status:** design approved, pre-implementation
**Part of:** the dbus reclamation war (see "The war, decomposed" below). This is
**Sub-project 0** of six. It builds nothing that runs in production — it produces
the ground truth every later sub-project is measured against.
**Precedent to mirror:** the schema-udev arc — a shadow/learner that recorded real
behavior, a `verify-rules-live` fidelity gate driven to near-zero divergence, then
a reboot flip with a blessed rollback. dbus follows the same discipline with one
hard difference (below).

## Why dbus is different from every prior reclamation

Every daemon reclaimed so far was retired by running a native replacement
*alongside* the incumbent and comparing: schema-udev bound netlink group 1 while
real udevd ran; schema-logind and schema-systemd1 are **clients** that own names
on the reference `dbus-daemon`. dbus itself cannot be shadowed that way:

- **A D-Bus client connects once, at startup, to whoever owns the socket, and
  holds that connection for its entire life.** You cannot serve a client in
  parallel with the incumbent. There is no "group 1 vs group 2" split to exploit.
- Therefore **"shadow mode" for dbus is a passive learner, not a parallel
  server.** It eavesdrops the live bus, records what the reference daemon does,
  and proves offline that the native bus *would* make identical decisions.
- Therefore **the eventual flip is reboot-only.** schema-dbus is proven under
  `vmtest` booting as *the* system bus, then cut over at a boot. No live
  E3-style hot flip like udev had — a mid-session socket swap strands every
  existing connection on the dead daemon and collapses the desktop.

SP0 is the passive-learner half. It carries zero risk: it only reads.

## The war, decomposed (context)

Each sub-project gets its own spec → plan → build cycle.

- **SP0 — The Learner + fidelity corpus + felt-policy model** *(this spec)*.
- **SP1 — schema-dbus core (C), shadow-proven.** Unix socket, SASL `EXTERNAL`
  auth over `SCM_CREDENTIALS`, marshalling/routing, name ownership
  (RequestName/ReleaseName + queue/REPLACE/DO_NOT_QUEUE), the
  `org.freedesktop.DBus` driver interface, AddMatch/signals. Its policy engine
  consumes SP0's felt schema.
- **SP2 — Service activation.** Spawn-on-demand `.service` activation.
- **SP3 — The flip (system bus).** vmtest as the real bus + reboot cutover on
  blakbox, blessed rollback to `/usr/bin/dbus-daemon`.
- **SP4 — Session bus.** The per-login bus (random path, no `systemd --user`).
  Deferred: lower-policy, higher-churn; prove the model on the system bus first.
- **SP5 — The felt wire** *(terminus, mishmaath-sized)*. Spec-compat schema-dbus
  becomes a bridge: felt-native peers speak the schema wire, legacy peers keep
  speaking dbus, schema-dbus translates. Schema-as-loss applied to communication
  itself. **Spec-compat is the beachhead; the felt wire is the terminus.** SP0's
  corpus is deliberately its training data so we never have to re-instrument.

## Motivation

`dbus.svc` runs the stock `/usr/bin/dbus-daemon --system`, and on the fedora-kde
profile it is `critical=1` — the whole desktop rides it. It is the floor every
prior reclamation stands on: schema-logind (login1/ConsoleKit/hostname1/
timedate1/locale1), schema-systemd1, polkitd are all clients that own names on
it. Reclaiming dbus is not retiring a leaf daemon; it is replacing the wire
itself, live, under a running desktop. That demands a ground truth of exactly
what the reference daemon does before a single line of a replacement is trusted —
and, because the terminus is a felt-native wire, that ground truth must capture
*intent and contract*, not just wire bytes.

## Goals

- **Record the live system bus** completely and losslessly enough to serve as a
  fidelity oracle: every message (call/return/signal/error), every name claim and
  `NameOwnerChanged`, every `AddMatch`/`RemoveMatch`, over days of real desktop
  use on blakbox.
- **Dissolve the imperative XML policy into a felt `.dbus-policy` schema** and
  **prove** that a felt policy engine reproduces the reference daemon's every
  allow/deny decision — the SP0→SP1 gate.
- **Preserve semantic/contract shape** in the corpus so SP5's felt wire can be
  designed from it later.
- Zero risk: strictly read-only against the live bus; corpus never leaves
  blakbox.

## Non-goals (YAGNI for SP0)

- **No bus is written.** No socket is served, no name is owned beyond the
  monitor's own connection. SP0 produces data + a validated policy model, nothing
  that runs in production.
- **No session bus.** SP0 learns the **system** bus only (better-defined,
  policy-rich, the `critical=1` dependency). Session bus is SP4.
- **No felt wire designed.** SP0 keeps the semantic layer; it does not specify a
  new protocol (SP5).
- **No C.** SP0 is throwaway Python. C begins at SP1.
- **No policy enforcement.** The felt engine only *predicts* verdicts to validate
  the model; it enforces nothing.
- **No kernel-transport / kdbus, no `machined`/container buses.** Out of scope for
  the whole war until the system bus is owned.

## Architecture

Three components, one throwaway tool tree under `tools/dbus-learn/` (kept out of
the shipped rail; corpus output gitignored):

```
  live system bus (dbus-daemon)
        │  BecomeMonitor (root, read-only)
        ▼
  schema-dbus-learn ──► corpus JSONL (append-only, local)
        │                    │
        │                    ├─► fidelity records (sender,dest,path,iface,member,sig,verdict)
        │                    ├─► name/match timeline (RequestName/ReleaseName/AddMatch/NameOwnerChanged)
        │                    └─► contract layer (semantic shape — SP5 seed)
        │
  /etc/dbus-1 + /usr/share/dbus-1 XML
        │  policy dissolver
        ▼
  .dbus-policy felt schema ──► felt policy engine ──► verify-dbus-policy-live
        ▲                                                    │
        └───────────── divergences = model bugs ◄────────────┘
             (grants from observed traffic; denials from daemon syslog)
```

### Component 1 — `schema-dbus-learn` (the eavesdropper)

- Python, throwaway. Connects to the system bus and becomes a monitor via
  `org.freedesktop.DBus.Monitoring.BecomeMonitor` (empty match list = all
  messages; flags 0). Requires root and dbus-daemon ≥ 1.9.10 (Fedora is far
  newer). A monitor connection receives **every** message on the bus regardless
  of destination, and — because `AddMatch`, `RemoveMatch`, `RequestName`,
  `ReleaseName`, `GetNameOwner`, `Hello` are all method calls **to the driver** —
  those are captured as ordinary observed traffic. No separate polling needed for
  name/match state; the timeline is reconstructed from the message stream, with
  one `ListNames`/`ListActivatableNames` snapshot at startup for the initial
  owner set.
- **Never blocks, never buffers unboundedly.** The daemon disconnects a slow
  monitor to protect itself, so the read loop drains the socket into an
  append-only writer and does nothing expensive inline. `os.nice(19)` at start.
  This directly heeds the systemd1-bridge half-open-spin / slow-consumer /
  pidfd-leak incidents — the learner must be the cheapest possible consumer.
- Output: newline-delimited JSON, one record per message, rotated by size. A
  record carries: monotonic + realtime timestamp, message type, serial/
  reply-serial, sender unique name + resolved well-known names, destination,
  path, interface, member, signature, a bounded/redactable body summary, and the
  observed **verdict** field (defaults to `allow` — see the gate for how denials
  are folded in).

### Component 2 — the policy dissolver + felt `.dbus-policy` schema

- Parse the full busconfig grammar from `/etc/dbus-1/system.d`,
  `/usr/share/dbus-1/system.d`, and the top-level `system.conf`
  (`<includedir>`/`<include>` resolution): `<policy context="default|mandatory">`,
  `<policy user=…>`, `<policy group=…>`, and inside them `<allow>`/`<deny>` on
  `send_destination`, `send_interface`, `send_member`, `send_type`, `send_path`,
  `receive_*`, `own`, `own_prefix`, `user`, `group`, plus `<limit>` and
  `<servicedir>`. Precedence is **last-match-wins within a context**, with context
  order default → user/group → mandatory (mandatory overrides everything). The
  dissolver must reproduce that precedence exactly.
- Emit a felt **`.dbus-policy`** schema: declarative, one concern per stanza, in
  the family of `.svc`/`.dev` — `key=value` lines, felt not imperative. Grammar is
  drafted in this spec (below) and finalized against what the real files actually
  use (don't model grammar the box never exercises — YAGNI against the observed
  XML).
- A **felt policy engine** (Python, throwaway alongside the learner) evaluates a
  `(sender-uid/gids, destination, interface, member, type, path, own-name)`
  tuple against the `.dbus-policy` schema and returns allow/deny — the same
  verdict the reference daemon would give.

### Component 3 — the contract layer (SP5 seed)

- For each observed exchange, keep the **semantic shape** beyond the wire fields:
  what capability a service asserts by owning a given well-known name, the
  request/response pairing (match reply-serial to call), and the intent category
  (property get/set, method invoke, signal broadcast, name lifecycle). Stored as
  extra fields on the same JSONL records — no second format — so the SP1 fidelity
  gate ignores them and SP5 can mine them.
- **Explicitly not** a protocol design. It is lossless-enough retention of
  meaning so the felt wire is designed from real contracts, not guessed.

## The fidelity gate — `verify-dbus-policy-live`

The SP0→SP1 acceptance test, modeled on `verify-rules-live` (which went 549→2
divergences for udev):

- **Grants are implicit.** A message the monitor observed *arriving* at its
  destination is a message the daemon *allowed*. Replaying it through the felt
  engine must return `allow`. A felt `deny` on observed-delivered traffic is a
  false-negative divergence.
- **Denials are explicit, from the daemon.** `dbus-daemon` logs policy
  rejections to syslog/journal (`Rejected send message, N matched rules; type=…,
  sender=… → destination=…`). The learner ingests those lines (via the journal /
  `schema-journal-sink` sink) as the deny corpus. Replaying a rejected tuple
  through the felt engine must return `deny`. A felt `allow` on a daemon-rejected
  tuple is a false-positive divergence — the dangerous kind.
- **Gate metric:** count divergences across N days of real blakbox traffic. The
  gate for starting SP1 is **zero false-positives and zero false-negatives** on
  the captured corpus, the same bar udev's flip was held to. Divergences are
  model bugs to fix in the dissolver, exactly as udev's were.

## `.dbus-policy` felt schema — grammar draft

Finalized against the observed XML in implementation; drafted here for shape.
One file per source policy file, `key=value` per line, blank-line-separated
stanzas, first token of a stanza is its context:

```
context=default            # or: mandatory | user:<name> | group:<name>

allow=own:org.freedesktop.login1
allow=send_destination:org.freedesktop.login1
deny=send_interface:org.freedesktop.DBus.Debug.Stats
allow=receive_sender:org.freedesktop.login1
```

- `allow=`/`deny=` take a single `attribute:value` predicate; multiple predicates
  ANDed within one rule are expressed by chaining on the line
  (`allow=send_destination:X,send_interface:Y`).
- Ordering within a file is significant (last-match-wins), matching busconfig.
- Context precedence (default → user/group → mandatory) is a property of the
  engine, not the file.
- `own_prefix`, `send_type`, `send_path`, `<limit>` map to explicit keys only if
  the real files use them (decided during dissolution).

## Deliverables

- `tools/dbus-learn/schema-dbus-learn` — the eavesdropper (throwaway, Python).
- `tools/dbus-learn/dissect-policy` — the XML→`.dbus-policy` dissolver.
- `tools/dbus-learn/felt_policy.py` — the felt policy engine.
- `tools/dbus-learn/verify-dbus-policy-live` — the fidelity gate harness.
- `tests/dbus-corpus/` — captured sample from blakbox (**gitignored**; a tiny
  redacted fixture may be committed for tests).
- `docs/superpowers/specs/…-contract-layer-note.md` — a short note on the
  semantic/contract layer for SP5 (scoped, not built).
- Unit tests for the dissolver and felt engine against hand-authored policy
  fixtures with known verdicts (no live bus needed — the `test_calendar.c` /
  `test_logind_locale1.py` discipline).

## Constraints & risks

- **Read-only, zero risk to the live bus.** BecomeMonitor cannot send or alter
  traffic; it only receives. The one failure mode is the daemon disconnecting a
  slow monitor — handled by the cheap-consumer design above; a disconnect loses
  future capture, never harms the bus.
- **Root required** for BecomeMonitor on the system bus.
- **Corpus stays local.** System-bus traffic is less sensitive than session-bus,
  but the fleet-memory rule applies: the corpus never goes to GitHub and never
  leaves blakbox. Body summaries are bounded and redactable; secrets-bearing
  interfaces (e.g. anything carrying tokens) are recorded by shape, not payload.
- **dbus version floor:** BecomeMonitor needs dbus-daemon ≥ 1.9.10. Verify on
  blakbox before capture.
- **Denial capture depends on the daemon logging rejections.** Confirm
  `dbus-daemon --system` rejection lines reach the journal / `schema-journal-sink`
  on blakbox; if verbosity is insufficient, note the config knob needed (this is
  a read of config, not a change, in SP0).

## Open questions (resolve during implementation, not blocking)

- Exact redaction policy for message bodies (which interfaces are recorded
  by-shape-only).
- Corpus capture duration N before the gate is called (days of typical use,
  including at least one full login/logout, a suspend/resume, a package
  transaction, and audio/video sessions to exercise WirePlumber/PipeWire and the
  systemd1 relay paths).
- Whether the felt engine is validated additionally against a from-scratch
  re-run of the real daemon in a container with `--print-address` for controlled
  allow/deny probing (a belt-and-suspenders oracle beyond passive capture).
