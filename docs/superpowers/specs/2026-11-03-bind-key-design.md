# `bind=` — stop with a service, start again with it

Status: **design, for review (Greg)**. No code yet.
Author: Claire, 2026-11-03. Asked for by Jonathan after the M6 podman work.

## Why

A Quadlet `Pod=` container is `BindsTo=<pod>-pod.service` under systemd, and the
pod unit carries `Wants=<container>.service`. Together:

- the pod stops or dies → systemd stops the container;
- the pod starts again → its `Wants=` starts the container.

schema-init has no equivalent. `schema-import` maps `BindsTo=` to `dep=`, which
only orders the start. Seen live on blakbox (2026-11-03, `evpod`/`ev` and
`m6pod`/`m6`):

- `schema-ctl --user stop <pod>` → `podman pod stop` ends the container's
  conmon → the container's .svc (Quadlet default `Restart=no` → `no_restart=1`)
  goes EXCISED with `excise_reason=exit-code`/`signal`.
- `schema-ctl --user start <pod>` → the pod comes back; the container does not.
  Quadlet runs containers with `--rm`, so `podman pod start` has nothing to
  restart, and the container's .svc stays EXCISED. **Silent.**

Interim (merged with this spec's PR): `schema-import --user` writes a WARN on
every `Pod=` container's .svc saying so.

`fuse=` is not the answer. It trips on the dependency's FRICTION (a pod mid-
restart) and excises terminally until a manual start: right for a quarantine
cascade, wrong for "follow your pod".

## Goal

One .svc key, `bind=<name>`, meaning:

> start after `<name>` is up; stop when it goes down; start again when it comes
> back — unless I was down for my own reasons.

Non-goals: systemd's full `PartOf=`/`Upholds=`/`StopPropagatedFrom=` matrix;
binding to groups (`dep=` can name a group; `bind=` can't, v1); more than one
`bind=` per service (v1: one — see Open questions).

## Semantics

Terms: the **binder** has `bind=`; the **bound** is what it names.
"Up" = the bound is FUNDAMENTAL, SETTLED or PERFECT (as `service_deps_ready`).
"Down" = anything else once it was up: RECOVERY, FRICTION, DORMANT, EXCISED,
or NEW_PROCESS/FULL_TRUST of a restart.

`bind=X` implies `dep=X` (start order, shutdown order, cycle checks). Writing
both is allowed and the same.

| bound does… | binder that is running | binder that is held (see below) | binder down for its own reasons |
|---|---|---|---|
| comes up (first time or again) | — | **started** (NEW_PROCESS, held cleared) | nothing |
| goes down (died, stopped, excised, restarting) | **stopped** with its normal sequence (exec_stop → SIGTERM → grace → kill → exec_stop_post); marked **held** | stays held | nothing |
| is removed from the config (reload) | stopped, held, logged `bind-target-gone` | stays held | nothing |

**Held** is a new runtime flag (`bind_held`), distinct from `SVC_NO_RESTART`:

- set only by the bind stop above;
- while set, the binder is not spawned (checked where `service_deps_ready`
  gates NEW_PROCESS today), shows `state=EXCISED excise_reason=bind`
  (new `EXC_BIND`; shim maps it to `Result=success` — systemd reports a
  BindsTo stop as a clean stop, not a failure);
- cleared when the bound comes up (binder → NEW_PROCESS), or by
  `schema-ctl start|restart|reset <binder>` (which then waits on the bound like
  any `dep=`).

**Down for its own reasons** = the binder was stopped by `schema-ctl stop`, or
excised by its own death/timeout/breaker, before the bound went down. It is
not held, so the bound's return does not start it — as systemd, where a unit
the user stopped is not pulled back by the bound's restart, and a failed one
isn't either unless something `Wants=` it. (Quadlet's pod does `Wants=` its
containers: see Open question 2.)

Strict start: unlike `dep=`, a binder **never** starts while its bound is
EXCISED/DORMANT, critical or not (`dep=` proceeds past a non-critical excised
dep today, `service.c:1958`). `BindsTo=` implies `Requires=`: no pod, no
container.

Restart counting: a bind stop is not a death. It does not touch
`restart_count`, `dormant_count`, the crash-loop breaker or `excise_reason`
beyond `EXC_BIND`. A binder that keeps dying on its own still trips its
breaker normally.

## Interactions

- **Timers/oneshots as bound:** refused at load (`bind=` to a timer or oneshot
  is a load error, like `verify_*` without `notify=1`). Their PERFECT means
  "ran", not "is up". Binder may be anything.
- **`fuse=`:** independent. A binder with both: the fuse wins (terminal), since
  a fuse means "quarantine on any dep trouble".
- **`critical=1` binder:** allowed; bind stops it like any other. Document that
  a critical service bound to a non-critical one inherits its downtime.
- **Shutdown:** `bind` ⊂ `dep`, so the binder already stops first. No bind
  logic runs during `shutdown_ordered` (nothing to restart into).
- **Reload/eviction:** binder evicted → normal eviction. Bound evicted →
  binder stopped + held (`bind-target-gone`). Bound re-added later → binder
  started when it comes up.
- **Re-exec:** `bind_held` joins `SVC_RUNTIME_FIELDS` (U8). `bind=` itself is
  config, re-read from the .svc.
- **Both sides in one tick:** process bound transitions before binder spawns
  in the tick loop, so a bound that goes down and up within a tick still
  restarts the binder (held set then cleared → NEW_PROCESS).
- **schema-user and PID 1:** same engine, same key; nothing mode-specific.

## Where it goes (sketch for sizing, not a plan)

- `service.h`: `char bind_name[64]; int bind_idx; uint8_t bind_held;`
- `service.c`: parse `bind=`, resolve `bind_idx` with `dep_idx`, add to the
  dep list for ordering/cycles; load-time refusal for timer/oneshot bound.
- `engine.c`: in the tick, on the bound's up→down edge (track `prev` state per
  service, already in `inst.prev_state`) stop its running binders and set
  held; on down→up, requeue held binders. Spawn gate honours `bind_held` and
  strict start. `EXC_BIND`.
- `reexec_state.h`: `X(U8, bind_held)`.
- status: `status <name>` line `bound to <X> (held)`; `--kv`
  `service.N.bind=X`, `service.N.bind_held=0|1`.
- `schema-import`: `BindsTo=<svc>` → `bind=<svc>` (one; more → first + WARN).
  Drop the interim Pod= WARN.
- shim: `EXC_BIND` → `Result=success`.

Estimate: one PR, ~250 lines C + tests; risk is all in the tick-loop edge
detection.

## Tests

Unit (`test_engine_conf.c` style, no processes): edge detection — bound up→down
holds a running binder; down→up starts it; user-stopped binder stays down;
breaker-excised binder stays down; held binder cleared by `start`; strict start
vs a non-critical EXCISED bound.

Live (`test_schema_user.sh`, real engine): `pod` (sleeper) + `ctr` (`bind=pod`):
stop pod → ctr stopped with `excise_reason=bind`, its `exec_stop_post` ran;
start pod → ctr running again; `stop ctr`, restart pod → ctr stays down;
kill pod's main (crash → RECOVERY → back) → ctr follows; reload removing pod →
ctr held, `bind-target-gone`.

Re-exec (`test_reexec_state.c`): `bind_held` round-trips.

Real Quadlet on blakbox: pod + container, `stop`/`start` the pod, container
follows; `--kv` shows `bind_held`.

## Open questions (for Greg)

1. **One `bind=` per service, or several?** systemd allows many `BindsTo=`.
   Quadlet only ever emits one. v1 = one keeps the edge logic simple.
2. **Who restarts — the bind, or a `wants=` on the bound?** systemd splits it:
   BindsTo stops, the pod's `Wants=` starts. This spec folds "start again" into
   `bind=` (only for binders it stopped). Alternative: add `wants=` (start these
   when I start) and keep `bind=` stop-only — closer to systemd, two keys, and
   `wants=` would also bring back a container the user stopped. Recommendation:
   folded, because the user-stop case is the one we must not get wrong.
3. **Unresolved `bind=` target** (named service doesn't exist): hold + log
   (proposed, strict like `Requires=`), or ignore like an unresolved `dep=`?
4. **Strict start:** agree a binder never runs while its bound is
   EXCISED/DORMANT even if non-critical? That diverges from `dep=` on purpose.
5. **`excise_reason=bind` + `Result=success`:** or a distinct substate so
   `systemctl is-failed` says no but `status` says "held"? Proposed: EXCISED +
   EXC_BIND, shim reports `inactive (dead)` / `success`.
