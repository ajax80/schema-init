# cgroup efficiency batch — design

**Date:** 2026-08-01
**Author:** Claire + Greg (design), Jonathan (approval)
**Status:** approved — ready for TDD

## Goal

Extend per-service cgroup setup with four cgroup v2 controls that improve
resource isolation, keyed off the existing `prio_t` priority enum
(`PRIO_PERIPHERAL` / `PRIO_STANDARD` / `PRIO_CRITICAL`). This is a focused
additive change to one function (`cgroup_apply_limits()` in `service.c`) plus
two pure helpers in `service.h`, mirroring the `reclaim_target` pattern from the
2026-08-01 mem-pressure reclaim work.

Motivating symptom: PipeWire audio crackle under heavy disk load (frigate /
torrents). PipeWire, WirePlumber, pipewire-pulse are already `priority=critical`,
so an `io.weight` tiering keyed off priority shields them automatically.

## Policy

| sysfs file | PERIPHERAL | STANDARD | CRITICAL | condition |
|---|---|---|---|---|
| `memory.oom.group` | `1` | `1` | `1` | always — service's procs die as a unit, no orphans |
| `cpu.idle` | `1` | — | — | peripheral only; **replaces** its `cpu.weight` write (kernel rejects `cpu.weight` when `cpu.idle=1`) |
| `cpu.weight` | *(skipped)* | `100` | `1000` | non-peripheral only (unchanged tiering) |
| `io.weight` | `10` | `100` | `1000` | always — shields CRITICAL (audio/display) disk I/O from peripheral hogs |
| `memory.high` | `0.9 × max` | `0.9 × max` | `0.9 × max` | only when `mem_limit` set — gentle reclaim buffer 10% below the hard cap; pairs with the PSI reclaim executive |

### Judgment calls (blessed)
1. **`memory.high` = 90%×`memory.max`, only when `mem_limit_mb > 0`.** No magic
   absolute floor on peripheral (would throttle legit heavy background tasks).
   Nearly a no-op until services opt in with `mem_limit` — acceptable; it's the
   correct, safe mechanism.
2. **`io.weight` is a safe no-op on plain blk-mq**, and has teeth under BFQ /
   `io.cost`. Harmless to always write; vital where enforced.

## Pure helpers (service.h, beside `reclaim_target`)

```c
typedef struct {
    int cpu_weight; /* 100 STANDARD, 1000 CRITICAL; 0 when cpu_idle set */
    int io_weight;  /* 10 PERIPHERAL, 100 STANDARD, 1000 CRITICAL */
    int cpu_idle;   /* 1 PERIPHERAL, 0 otherwise */
} cgroup_tier_t;

static inline cgroup_tier_t cgroup_tiering(prio_t priority) {
    cgroup_tier_t tier = {0};
    switch (priority) {
    case PRIO_PERIPHERAL:
        tier.cpu_idle = 1; tier.cpu_weight = 0; tier.io_weight = 10; break;
    case PRIO_CRITICAL:
        tier.cpu_idle = 0; tier.cpu_weight = 1000; tier.io_weight = 1000; break;
    case PRIO_STANDARD:
    default:
        tier.cpu_idle = 0; tier.cpu_weight = 100; tier.io_weight = 100; break;
    }
    return tier;
}

static inline long mem_high_bytes(long mem_limit_mb) {
    if (mem_limit_mb <= 0) return 0;
    return (mem_limit_mb * 1024L * 1024L * 9L) / 10L;
}
```

`cgroup_apply_limits()` replaces its inline cpu.weight block with a
`cgroup_tiering(svc->priority)` call, then writes: `memory.oom.group=1` always;
`cpu.idle=1` when `tier.cpu_idle` else `cpu.weight=tier.cpu_weight`;
`io.weight=tier.io_weight` always; `memory.high=mem_high_bytes(svc->mem_limit_mb)`
when `> 0`. Each write is best-effort (open O_WRONLY, write, close; ignore
missing files — e.g. `io.weight` absent if io controller not on the cgroup).

## Testing

- **Unit:** `tests/test_cgroup_tiering.c` — assert `cgroup_tiering()` mapping for
  all three priorities (cpu_idle/cpu_weight/io_weight), and `mem_high_bytes()`
  edge cases: `0` and negative → `0`; `100 MB → 94371840`; large value no
  overflow (long math). Wire into the Makefile test target next to
  `test_reclaim`.
- **Boot:** `schema-vmtest` — PID 1 boots clean with the new writes, all services
  reach steady state; grep for write errors in the init log.
- **Live:** one reboot on blakbox. This reboot also activates the staged
  `init.c` `make-rshared /` fix and the mem-pressure reclaim. Keep the
  pre-session binary backup for rollback (`/usr/bin/schema-init.bak-<date>`).

## Non-goals (YAGNI / deferred)
- Dynamic postures (PERF/BALANCED/IDLE) — own design, deferred.
- Absolute per-priority `memory.high` floors — only via explicit `mem_limit`.
- No new `.svc` fields; all behavior derives from existing `priority=` + `mem_limit=`.
