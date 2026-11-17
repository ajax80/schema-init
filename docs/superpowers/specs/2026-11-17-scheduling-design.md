# Audit 4.7: scheduling keys

## Intent
nice=, io_class=/io_priority=, sched_policy=/sched_priority= as .svc keys, so
imported Nice=/IOScheduling*/CPUScheduling* units keep their scheduling (today
the importer drops them without a word), and the Ungulate Leg loop can run FIFO.

## Goals
- `nice=-20..19`: replaces the nice `priority=` would set; cgroup weight still
  follows priority.
- `io_class=realtime|best-effort|idle|none`, `io_priority=0..7` (default 4;
  io_priority alone implies best-effort). ioprio_set on the child.
- `sched_policy=other|batch|idle|fifo|rr`, `sched_priority=1..99` (fifo/rr
  only, default 1; given with another policy refuses the load).
- Applied in the spawn child before the uid drop (RT needs CAP_SYS_NICE).
- A failure to apply any explicit key fails the spawn (systemd: 213/214),
  "SCHEDULING FAILED for <svc>: <key>: <err>", exit 126. priority='s own nice
  stays a warning.
- Importer maps Nice=, IOSchedulingClass= (names and 0–3), IOSchedulingPriority=,
  CPUSchedulingPolicy=, CPUSchedulingPriority=; schema-user keeps them.
- schema-ctl status shows the set ones.

## Non-goals
- CPUSchedulingResetOnFork=, CPUAffinity= (cpuset= exists), RT group budgets
  (blakbox kernel has no CONFIG_RT_GROUP_SCHED).

## Approaches
1. Explicit keys, failures fatal (picked: a Leg loop silently not FIFO is the
   failure that matters; matches systemd and tonight's MDWE call).
2. Warn on failure like priority=: rejected, silent loss.
3. Extend priority= tiers: rejected, three levels can't carry a policy.
