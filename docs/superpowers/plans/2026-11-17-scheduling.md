# Scheduling keys (audit 4.7) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans.

**Goal / Spec:** `docs/superpowers/specs/2026-11-17-scheduling-design.md`

**Architecture:** service_t gains `int8_t nice_val; uint8_t nice_set; int8_t io_class /* -1 unset, 0 none,1 rt,2 be,3 idle */; int8_t io_prio /* -1 */; int8_t sched_policy /* -1 */; uint8_t sched_prio;`. Parse in service.c beside `priority`; finalize check (sched_priority with non-RT policy → refuse). `int svc_apply_sched(const service_t *svc, const char **what)` in service.c, called in the spawn child right after the priority nice block (before uid drop); nonzero → SCHEDULING FAILED, _exit(126).

## Review Focus
1. nice= with priority=critical: nice= wins (test).
2. io_priority without io_class → best-effort (test).
3. Unprivileged spawn asking nice=-5 fails the spawn, nice=5 succeeds (test, non-root).
4. sched_policy=fifo actually applied as root (sudo test / live).
5. Importer IOSchedulingClass=2 numeric form (test).

### Task 1: parse + validation (tests in tests/test_svc_sched.c, Makefile line)
- loads: nice=-20/19, io_class names, io_priority 0..7, sched_policy names, sched_priority 1..99 with fifo
- refuses: nice=20, nice=x, io_class=foo, io_priority=8, sched_policy=deadline, sched_priority=0/100, sched_priority=5 + sched_policy=batch
### Task 2: apply in child
- non-root: spawn exec=/bin/sh -c "cat /proc/self/stat field 19 (nice) > f": nice=5 → 5; priority=critical+nice=3 → 3; nice=-5 → spawn child exits 126; io_class=idle → `ionice -p self` shows idle (or ioprio_get syscall in a helper); sched_policy=batch → /proc/self/sched policy 3. Root (sudo run): fifo prio 10 → policy 1, rt_priority 10.
### Task 3: status, importer, docs; full suite; commit
### Task 4: draft PR, /code-review, live blakbox, merge on his word
