# cpuset_partition= — exclusive / isolated CPU cores (Phase 2)

**Date:** 2026-06-19
**Status:** Approved (Claire design; Greg reviewed — "fully ready for implementation")
**Builds on:** PR #19 (`cpuset=` per-service CPU affinity / pinning, merged `b421d43`)

## Goal

Let a service reserve CPU cores *exclusively* — and optionally remove them from
the kernel scheduler's load-balancer — using cgroup v2 cpuset **partitions**.
This is dynamic `isolcpus=` (no kernel cmdline needed): the target is giving the
Ungulate Leg control loop and the audio path their own un-contended cores.

Phase 1 (`cpuset=`) pins a service to cores but those cores stay in the shared
pool. Phase 2 adds the exclusivity tier on top of the existing `cpuset=` cores.

## Config surface

One new `.svc` key, pairs with the existing `cpuset=`:

```
cpuset=10-11
cpuset_partition=isolated      # isolated | root | member   (default: member)
```

| value | meaning |
|-------|---------|
| `member` (default) | **Phase 1 behavior, unchanged.** Plain `cpuset.cpus` pinning, cores stay shared. Zero change for existing services. |
| `root` | Cores become an exclusive partition; still scheduler-load-balanced *within* the partition. For cpu-bound exclusive workloads. |
| `isolated` | Exclusive **and** removed from scheduler load-balancing. The Leg / audio RT target. |

`cpuset=` continues to name *which* cores. `cpuset_partition=` only names the
exclusivity tier. Author states intent explicitly — no auto-selection.

## Mechanism — remote partition (schema-init stays a `member`)

We use the cgroup v2 **remote partition** path so the `/sys/fs/cgroup/schema-init`
parent does not itself have to become a partition root (which would constrain all
~30 sibling services). schema-init remains `member`; only the named cores get
reserved down the chain.

On spawn of a service with `cpuset_partition != member`, in `cgroup_apply_limits`,
**after** `cpuset.cpus` is written for that service:

1. **Reserve cores at the parent via read-modify-write of the live sysfs value.**
   - Read `/sys/fs/cgroup/schema-init/cpuset.cpus.exclusive`.
   - Write back `"<existing>,<this svc's cores>"` — concatenated.
   - The kernel's cpuset parser resolves the union itself and dedups
     (`"10-11,10-11"` → `10-11`), so this is idempotent across restarts and needs
     no range-parsing in C, and **no access to the `services[]` table.**
2. Write the svc's cores to `<svc>/cpuset.cpus.exclusive`.
3. Write `isolated` (or `root`) to `<svc>/cpuset.cpus.partition`.
4. **Read back** `<svc>/cpuset.cpus.partition`. The kernel reports the requested
   value on success, or `"<value> invalid (...)"` on failure.

### Why no services-table access

`init.c` double-buffers the service table (`services_a` / `services_b`) and swaps
the active `services` pointer on every reload (init.c:1282–1283). `service.c` has
no global table pointer and `service_spawn(svc)` receives only one service. Letting
the **live cgroupfs value be the union source of truth** (step 1) avoids passing
the array down *and* avoids a global getter that could race the buffer swap. No
signatures change.

## Failure mode — degrade to plain pinning

If the step-4 readback contains `invalid` (overlapping cores between two isolated
services, exclusivity conflict, or cpuset controller absent), degrade in this exact
order:

1. svc `cpuset.cpus.partition` ← `member`
2. svc `cpuset.cpus.exclusive` ← `""` (empty)
3. leave svc `cpuset.cpus` = cores (plain Phase-1 pinning stands)
4. log one `HAZARD` line naming the service and the kernel reason

The service runs; boot is never blocked. Matches the existing cpuset philosophy
(empty = unconstrained, no-op if unsupported).

**No parent rollback.** A stale entry in schema-init's `cpuset.cpus.exclusive` is
inert: an exclusive reservation only removes cores from siblings' `effective` set
when a child *actively forms a partition*. A degraded (member) child claims
nothing, so the leftover union entry has zero effect — no cleanup needed on
degrade or on later service removal/excise. First isolated service to claim a core
wins; a second service requesting the same core degrades to pinning.

## Data model

`service.h`: add

```c
int cpuset_partition;   /* PART_MEMBER=0, PART_ROOT, PART_ISOLATED */
```

with an enum. Parsed in **both** load paths, same as `cpuset`:

- `services_load` (boot)
- `service_load_one` (live `schema-ctl add <path>`)

Accepted values: `member` / `root` / `isolated` (case-insensitive); anything else →
`member`.

### Load-time normalization

If `cpuset_partition != member && cpuset[0] == '\0'`, force `cpuset_partition = member`
and emit a one-line warning. A partition with no CPUs is a config error the kernel
would reject anyway; catching it at parse keeps the spawn path simple and the
message clear, instead of surfacing as a runtime HAZARD.

## Testing (vmtest, no hardware reboot)

QEMU `-smp 4`:

1. **Happy path** — an `isolated` svc (e.g. `cpuset=3`, `cpuset_partition=isolated`):
   - `<svc>/cpuset.cpus.partition` == `isolated` (not `invalid`)
   - core 3 removed from a sibling service's `cpuset.cpus.effective`
   - svc process `Cpus_allowed_list` == `3`
2. **Degrade path** — two services both claiming core 3 as `isolated`: second comes
   up `member` with core 3 still in `cpuset.cpus`, and a `HAZARD` line is logged.
3. **`root` variant** — partition reads `root`, core exclusive but still in the
   load-balancer.
4. **Empty-cpuset normalization** — `cpuset_partition=isolated` with no `cpuset=`
   loads as `member` + warning, no partition attempted.

README config table + the post-deploy checklist updated with `cpuset_partition=`.

## Scope held (YAGNI)

- No load-time overlap pre-validation between services — the runtime degrade path
  covers it.
- No per-core accounting / introspection API.
- No `root`-vs-`isolated` auto-selection — author states intent.
- No parent `cpuset.cpus.exclusive` cleanup logic (inert by construction).

## Files touched

- `service.h` — `cpuset_partition` field + enum.
- `service.c` — parse in `services_load` + `service_load_one` (with empty-cpuset
  normalization); partition setup + readback + degrade in `cgroup_apply_limits`.
- `README.md` — config table row + checklist line.
- `vmtest.sh` (Claire/blakbox-local, not version-controlled) — the four assertions.
