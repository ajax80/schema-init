# `bind=` — stop with a service, start again with it

Status: **design, rev 2 — Greg's review (2026-11-03) folded in.** No code yet.
Author: Claire, 2026-11-03. Asked for by Jonathan after the M6 podman work.
Rev 2 changes are listed at the end.

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
> back — unless I was stopped on purpose or failed on my own.

Non-goals: systemd's full `PartOf=`/`Upholds=`/`StopPropagatedFrom=` matrix;
binding to groups; more than one `bind=` per service.

## Terms

- **binder**: the service with `bind=`. **bound**: the service it names.
- **up**: FUNDAMENTAL, SETTLED or PERFECT (as `service_deps_ready`).
- **down**: anything else, once it was up.
- **held**: new runtime flag `bind_held`, distinct from `SVC_NO_RESTART`. A held
  binder is in `STATE_EXCISED` with `excise_reason=bind` (new `EXC_BIND`).
  EXCISED is a no-op in the tick (`engine.c:1470`), so nothing moves a held
  binder except the bind logic below.

## Load-time rules

- One `bind=` per .svc; a second is a load error ("more than one bind= line").
- `bind=X` implies `dep=X` for start order, shutdown order and cycle checks, and
  takes one `dep` slot: a `dep=X` already present (or added later in the file)
  is not counted twice against `MAX_DEPS` (`service.c:2440` doesn't dedupe
  today — the bind code must).
- The bound must not be a timer or oneshot (load error): their PERFECT means
  "ran", not "is up".
- **Unresolved bound** (no .svc by that name after load or reload): the binder is
  held and logs `bind-target-missing <X>` once; it never runs without its bound.
  Not a load refusal, so it stays visible in `status` and starts if `<X>`
  appears on a later reload.

## Semantics

### 1. Stopping the bound stops its binders first (proactive)

Any stop *request* for the bound — `schema-ctl stop|restart <bound>`, the
bound's own `stop_main()`, eviction of the bound — first stops each **running**
binder with its normal sequence (exec_stop → SIGTERM → grace → kill →
exec_stop_post) and sets `bind_held`. The bound's own stop then proceeds; it is
not delayed waiting for the binders (systemd orders them, but the binder's stop
is independent of the bound's and both are bounded by their own grace).

This closes the podman race (review #1): with `podman pod stop` the
container's conmon exits *during* the pod's exec_stop, i.e. before the pod is
down. Because the binder was already marked held when the stop was requested,
that exit is a bind stop, not "its own death".

### 2. The bound goes down unrequested (crash) — deferred classification

A crash has no request to hook. Order of reaps is not deterministic: when a
pod's infra dies, a container can be reaped before or after the pod's main.

- At the binder's reap: if its bound is already not up, or has a stop in
  progress (`stop_pid > 0` phase 1, `ctl_killed`, or `SVC_NO_RESTART` set by a
  `stop`), the exit is a **bind stop** (§3).
- Otherwise it is the binder's own exit, handled as today (death → RECOVERY,
  or `no_restart` → EXCISED with its exit reason) — **but** the engine records
  `bind_died_at` (monotonic). If the bound goes down within `BIND_GRACE_MS`
  (2000) of that, the binder is reclassified: `bind_held=1`,
  `excise_reason=bind`, and any restart bookkeeping its death caused is undone
  (restart_count/dormant_count restored from a snapshot taken at the reap).
  Past the grace, its exit stands as its own.

### 3. Reaping a binder whose stop was a bind stop (review #3)

In `reap()`, a child of a service with `bind_held` set:

- `STATE_EXCISED`, `excise_reason = EXC_BIND`;
- cgroup torn down as the `76-no-restart` branch does today;
- `stop_cmds_begin(svc, 2)` (exec_stop_post);
- **no** `restart_budget_refresh`, no failsafe, no RECOVERY, no breaker /
  `dormant_count` change. A bind stop is not a death.

### 4. The bound comes back up — requeue held binders (review #4, #5)

On the bound's down→up edge, each binder with `bind_held`:

- if its stop has fully finished (`child_pid == 0 && stop_pid == 0`): clear
  `bind_held`, set `inst.state = STATE_NEW_PROCESS` explicitly (EXCISED would
  otherwise sit forever), `restart_count` untouched;
- if it is still stopping: set `bind_requeue`; when its last stop process is
  reaped (`child_pid == 0 && stop_pid == 0`) and the bound is still up, do the
  above. If the bound went down again meanwhile, clear `bind_requeue` and stay
  held.

The spawn then goes through the normal NEW_PROCESS gate, with **strict start**:
a binder never spawns while its bound is EXCISED or DORMANT, critical or not
(`dep=` lets a non-critical excised dep through, `service.c:1958`; the bind
check overrides that for `bind_idx`).

### 5. Not held — down on purpose or on its own

- `schema-ctl stop <binder>` sets `SVC_NO_RESTART` as today and clears
  `bind_held`/`bind_requeue`: the bound's return does not start it. This is the
  case that must not go wrong.
- A binder that died on its own and was not reclassified (§2) is not held.
- `schema-ctl start|restart|reset <binder>` clears held/requeue and starts it
  through the normal gate (so it waits on its bound).
- `schema-ctl restart <bound>`: §1 stops the running binders (held), the bound
  restarts, §4 brings them back. Binders the user had stopped stay stopped.

### 6. Edge detection (review #2)

Not `inst.prev_state`: it is only written in `schema.c` (NEW_PROCESS
evaluation) while `engine.c` assigns `inst.state` directly in 36 places.
Instead each service carries `bind_was_up`, refreshed for every service at the
**tail** of each tick pass; a bound's edge is `was_up != up(now)`, computed at
the **head** of the next pass before any binder is spawned, so a bound that goes
down and up between two passes still yields (held → requeue). Only services
that are some binder's bound need the flag consulted; computing it for all is
one compare each.

## Interactions

- **`fuse=`:** independent. A binder with both: the fuse wins (terminal).
- **`critical=1` binder:** allowed; bind stops it like any other.
- **Shutdown:** `bind` ⊂ `dep`, so binders already stop first. No bind logic
  in `shutdown_ordered` (no held/requeue; nothing restarts).
- **Reload/eviction:** binder evicted → normal eviction. Bound evicted → §1
  first, then binders held with `bind-target-missing`; bound re-added later →
  §4 when it comes up.
- **Re-exec:** `bind_held`, `bind_requeue`, `bind_was_up`, `bind_died_at` (as
  remaining-ms) join `SVC_RUNTIME_FIELDS`.
- **schema-user and PID 1:** same engine, same key.

## Visibility

- `status <name>`: `bound to <X>` plus `(held)` / `(requeued)`.
- `--kv` (keys by name, as today): `service.<name>.bind=<X>`,
  `service.<name>.bind_held=0|1`.
- `--json`: `"bind"`, `"bind_held"`.
- Rail events: `bind-stop`, `bind-requeue`, `bind-start`, `bind-reclassified`,
  `bind-target-missing`.
- systemd1 shim: EXCISED with `excise_reason == 'bind'` →
  `ActiveState=inactive`, `SubState=dead`, `Result=success`
  (`schema-systemd1.py:250` maps EXCISED to `failed` unless recently stopped —
  bind becomes the other exception). `is-failed` says no.

## Importer

`BindsTo=<svc>` → `bind=<svc>` (one; more → first + WARN, rest as `dep=`). The
interim Pod= WARN is dropped once `bind=` ships.

## Where it goes (sizing)

- `service.h`: `char bind_name[64]; int bind_idx; uint8_t bind_held,
  bind_requeue, bind_was_up; struct timespec bind_died_at;`
- `service.c`: parse (one only), dedupe against `dep=`, resolve with `dep_idx`,
  timer/oneshot refusal.
- `engine.c`: §1 hook in `stop_main()` / ctl stop+restart / eviction; §2 at
  reap + reclassify on the bound's down edge; §3 branch in `reap()`; §4 at the
  head of the tick; strict start in the NEW_PROCESS gate; `EXC_BIND`.
- `reexec_state.h`, status/--kv/--json, rail events, shim, importer.

Estimate: one PR, ~350 lines C + tests. The risk is §1/§2 ordering and §4's
requeue-after-stop; the test plan targets exactly those.

## Tests

Unit (no processes, `test_engine_conf.c` style): edge detection across two
passes incl. down+up between passes; reclassification inside vs outside
`BIND_GRACE_MS`; restart bookkeeping restored on reclassify; strict start vs a
non-critical EXCISED bound; one-bind and timer/oneshot load refusals; dep dedupe.

Live (`test_schema_user.sh`, real engine), `pod` (sleeper whose exec_stop kills
`ctr`'s main first, like `podman pod stop`) + `ctr` (`bind=pod`, `no_restart=1`):

1. stop pod → ctr EXCISED `excise_reason=bind`, its exec_stop_post ran, no
   restart counted (**the podman race, review #1**);
2. start pod → ctr running again;
3. `stop ctr`, restart pod → ctr stays down;
4. kill pod's main (crash) with ctr dying first → reclassified, comes back
   with the pod;
5. ctr killed on its own, pod stays up past 2 s → not held, stays down;
6. pod restarts while ctr's exec_stop is still running (slow exec_stop) → ctr
   not spawned until its old process and stop runner are gone (**review #4**);
7. reload removing pod → ctr held, `bind-target-missing`.

Re-exec (`test_reexec_state.c`): the four fields round-trip, incl. a requeue
in flight.

Real Quadlet on blakbox: pod + container, `stop`/`start`/`restart` the pod,
crash the infra conmon; container follows each time.

## Decisions (from review)

1. One `bind=` per service; a second is a load error.
2. "Start again" stays folded into `bind=` (no reverse `wants=` tracking);
   `restart <pod>` brings its running binders back, user-stopped ones stay down.
3. Unresolved bound: hold + log (not ignore, not load refusal).
4. Strict start: agreed, overrides `dep=`'s non-critical skip for `bind_idx`.
5. `EXCISED + EXC_BIND`; shim reports `inactive`/`dead`/`success`.

## Rev 2 changes (Greg's review)

- #1 podman stop race → §1 proactive stop on any bound stop request + §2
  deferred reclassification for crashes.
- #2 `prev_state` is not maintained by engine.c → §6 explicit `bind_was_up`.
- #3 bind-stopped exit in `reap()` → §3.
- #4 fast restart while stopping → §4 `bind_requeue`.
- #5 EXCISED is a tick no-op → §4 explicit move to NEW_PROCESS.
- Minor: `--kv` keys are by name; `bind` must dedupe against `dep=`.
- New, not in the review: §2's reclassification window (`BIND_GRACE_MS`) — the
  crash path has no stop request to hook, so #1's fix alone doesn't cover it.
