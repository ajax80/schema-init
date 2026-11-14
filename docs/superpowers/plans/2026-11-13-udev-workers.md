# schema-udev Hotplug Workers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Live uevents are handled in forked per-event workers with udevd-style conflict ordering and deadlines, so one stuck device never freezes hotplug for the rest.

**Architecture:** The main loop queues raw netlink datagrams with metadata extracted at enqueue, starts a worker per event when it conflicts with nothing running or earlier-queued, and enforces a per-event deadline through a dynamic `poll()` timeout. Workers run `dispatch()` in worker mode (waits for RUN, broadcasts at the tail, flocks shared registries). Coldplug stays inline.

**Tech Stack:** C99 + `_GNU_SOURCE`, header-only modules (repo convention), `fork`/`waitpid`/`flock`/`pidfd_open`, Python 3 for the daemon-level test.

**Spec:** `docs/superpowers/specs/2026-11-13-udev-workers-design.md`

## Global Constraints

- Coldplug (`subsystems_coldplug`, `coldplug_walk_root`) keeps today's behaviour, including fire-and-forget RUN; ready marker still means coldplug complete.
- Per-helper timeout 30 s (`PROGRAM`, `IMPORT{program}`, each worker-mode `RUN`), both modes; per-event deadline 180 s (live workers only).
- Concurrency cap `max(4, min(2 * nproc, 16))`; queue cap 4096, tail drop with a rate-limited log line.
- Slow-event log threshold 1000 ms: `[schema-udev] slow <ms>ms <ACTION> <DEVPATH>`.
- Timeout log: `[schema-udev] event TIMED OUT after 180s <ACTION> <DEVPATH>`.
- Never a blocking `waitpid()` on a killed worker; SIGTERM grace 5 s.
- Locks only around the claims registry, uaccess record/apply/clear, and legacy `symlink_apply`/`symlink_clear`; never across a helper or `run_hook()`.
- Before every push: `make test` AND `sh .github/ci-python-suite.sh`, one heavy job at a time. Draft PR, one `/code-review`, merge only after DBox then blakbox hardware checks.

## Review Focus

1. A USB stick replugged into another port while its old `remove` is still running: same devnum, new DEVPATH, must wait (Task 1 test `replug_same_devnum`).
2. A worker hung with no other activity: the deadline must still fire on an idle daemon (Task 4 test `idle_deadline_fires`).
3. A burst of events for one device (`add`, several `change`, `remove`) during a slow helper: order preserved, nothing reordered past the queued head (Task 1 test `queue_order_same_dev`, Task 4 test `same_device_ordered`).
4. Keyboard/mouse added while a session runs: the monitor broadcast only after the seat ACL is applied (Task 3 code; checked on DBox in Task 5).
5. SIGTERM with a worker in a long helper: daemon exits within ~5 s, not 30/180 s (Task 4 test `term_with_hung_worker`).

---

### Task 1: Pure scheduler (`udev_sched.h`)

**Files:**
- Create: `udev_sched.h`
- Create: `tests/test_udev_sched.c`
- Modify: `Makefile` (test target: add the compile+run line next to `test_udev_ruleset.c`, line ~339)

**Interfaces:**
- Consumes: `struct uevent`, `uevent_get()` from `schema-udev.h`.
- Produces:
  - `struct sched_meta { char devpath[UE_VAL_MAX]; char devpath_old[UE_VAL_MAX]; char action[16]; char subsystem[64]; unsigned major, minor; int has_devnum; int is_block; int ifindex; };`
  - `void sched_meta_from(const struct uevent *ev, struct sched_meta *m);` — devnum from `MAJOR`/`MINOR` (`has_devnum` only when both present), `is_block` = `SUBSYSTEM=="block"`, `ifindex` from `IFINDEX` (0 = none).
  - `int sched_conflict(const struct sched_meta *a, const struct sched_meta *b);` — 1 when: either has empty devpath; devpaths (and each side's `devpath_old`) equal or ancestor/descendant on `/` boundaries; same devnum with same `is_block`; same nonzero ifindex; both `subsystem=="net"`.
  - `int sched_cap(long nproc);` → `max(4, min(2*nproc, 16))`.
  - `int sched_pick(const struct sched_meta *queued, int nq, const struct sched_meta *running, int nr);` → index of the first queued entry that conflicts with no running entry and no earlier queued entry, else -1. (Caller enforces the cap.)

- [ ] **Step 1: Write the failing test** `tests/test_udev_sched.c` with a helper `meta(devpath, subsystem, major, minor, ifindex)` and these cases as asserts:
  - `prefix_boundary`: `/devices/b/sda` vs `/devices/b/sda/sda1` → 1; `/devices/b/sda` vs `/devices/b/sdab` → 0; equal → 1; siblings `/devices/b/sda` vs `/devices/b/sdb` → 0.
  - `replug_same_devnum`: block 8:16 at `/devices/usb1/1-1/.../sdb` vs block 8:16 at `/devices/usb2/2-3/.../sdb` → 1.
  - `block_vs_char_same_numbers`: block 8:16 vs char 8:16, unrelated paths → 0.
  - `same_ifindex`: two devpaths, ifindex 3 both → 1; net vs net with different ifindex → 1 (net serialised); net vs block → 0.
  - `move_old_path`: a with `devpath_old=/devices/x/eth0`, b with devpath `/devices/x/eth0` → 1.
  - `no_devpath`: empty devpath vs anything → 1.
  - `queue_order_same_dev`: running `{}`, queued `[sda change, sda remove, sdb add]` → pick 0; running `[sda change]`, queued `[sda remove, sdb add]` → pick 1 (sdb), never 0.
  - `held_behind_queued`: running `[/devices/b/sda/sda1 add]`, queued `[/devices/b/sda change, /devices/b/sdc add]` → queued[0] conflicts with the running descendant, queued[1] does not → pick 1.
  - `cap`: `sched_cap(1)==4`, `sched_cap(4)==8`, `sched_cap(32)==16`.
  - `meta_from`: an event with `MAJOR=8 MINOR=16 SUBSYSTEM=block IFINDEX` absent → `has_devnum==1, is_block==1, ifindex==0`.

- [ ] **Step 2: Run to verify it fails**
  Run: `make test 2>&1 | grep -E 'udev_sched|udev-sched'`
  Expected: compile error, `udev_sched.h` missing.

- [ ] **Step 3: Implement `udev_sched.h`** (header-only, `static inline`, include guard). Prefix test: `a` is ancestor of `b` iff `strncmp(a,b,strlen(a))==0 && (b[strlen(a)]=='/' || b[strlen(a)]=='\0')`.

- [ ] **Step 4: Run to verify it passes**
  Run: `make test 2>&1 | grep -E 'sched'`
  Expected: the `udev-sched` binary runs and prints `test_udev_sched: ok`; `make test` exits 0.

- [ ] **Step 5: Commit** `git add udev_sched.h tests/test_udev_sched.c Makefile && git commit -m "schema-udev: pure worker scheduler (conflict rule, pick, cap)"` (+ Co-Authored-By trailer).

---

### Task 2: Helper timeout 30 s and bounded child wait

**Files:**
- Modify: `udev_exec.h:59-61` (default timeout), `udev_exec.h:131` (log on timeout)
- Modify: `udev_exec.h` (add wait helper)
- Test: `tests/test_udev_executor.c`, `Makefile` (second compile of the executor test with `-DUDEV_EXEC_TIMEOUT=1`, binary `executor-t1`)

**Interfaces:**
- Produces:
  - `UDEV_EXEC_TIMEOUT` default `30` (seconds, still `#ifndef`-overridable).
  - On capture timeout: `fprintf(stderr, "[schema-udev] helper TIMED OUT after %ds: %s\n", UDEV_EXEC_TIMEOUT, cmd)` before returning -1.
  - `int udev_wait_child(pid_t pid, int timeout_s, const char *what);` → exit status (0–255), -1 on signal/err; on timeout SIGKILL, blocking `waitpid` (a helper is not the D-state worker case; the worker's own deadline covers that), log `[schema-udev] helper TIMED OUT after %ds: %s`, return -1. Use `pidfd_open` + `poll`; if `pidfd_open` fails (ENOSYS), poll `waitpid(WNOHANG)` every 10 ms.

- [ ] **Step 1: Write the failing tests** in `tests/test_udev_executor.c`, guarded `#if UDEV_EXEC_TIMEOUT == 1` so only `executor-t1` runs them:
  - `udev_run_capture("/bin/sleep 5", out, sizeof out)` returns -1 in < 3 s (measure with `CLOCK_MONOTONIC`).
  - fork a child that `execl("/bin/sleep","sleep","5",NULL)`; `udev_wait_child(pid, 1, "sleep 5")` returns -1 in < 3 s and `kill(pid,0)` then fails with ESRCH.
  - fork a child that `_exit(7)`; `udev_wait_child(pid, 1, "x") == 7`.
- [ ] **Step 2: Run to verify it fails:** `make test 2>&1 | grep -A3 executor-t1` → undefined `udev_wait_child` (or capture taking 180 s if run without the define: the define must be on the new Makefile line).
- [ ] **Step 3: Implement** the default change, the timeout log, and `udev_wait_child` in `udev_exec.h`.
- [ ] **Step 4: Run to verify it passes:** `make test 2>&1 | grep -E 'executor'` → both executor binaries run clean, the t1 one in a few seconds.
- [ ] **Step 5: Commit** `udev_exec.h tests/test_udev_executor.c Makefile`: "schema-udev: 30 s helper timeout, logged; bounded child wait".

---

### Task 3: `dispatch()` worker mode, tail broadcast, locks

**Files:**
- Modify: `schema-udev.c:140-216` (`run_ctx_runs`), `schema-udev.c:290-418` (`dispatch`), `schema-udev.c:244` (`run_hook` untouched), callers at `:466`/`:477`.
- Modify: `disk_links.h` only if the lock helper lives there; prefer a small `udev_lock()` in `schema-udev.h`.

**Interfaces:**
- Consumes: `udev_wait_child()` (Task 2).
- Produces:
  - `enum dispatch_mode { DISPATCH_INLINE, DISPATCH_WORKER };`
  - `static void dispatch(struct uevent *ev, enum dispatch_mode mode);` — coldplug paths pass `DISPATCH_INLINE`; Task 4's worker passes `DISPATCH_WORKER`.
  - `static void run_ctx_runs(const struct dev_ctx *rc, enum dispatch_mode mode);` — worker mode waits for each RUN child (and the kmod builtin child) with `udev_wait_child(pid, UDEV_EXEC_TIMEOUT, rc->runs[i])`, in order; inline mode unchanged.
  - `static inline int udev_lock(const char *path);` → fd holding `flock(LOCK_EX)` on `path` (created `O_CREAT|O_CLOEXEC`, 0600), -1 on failure (caller proceeds unlocked, as today); `udev_unlock(int fd)` closes. Lock files: `SCHEMA_UDEV_LINK_CLAIMS "/.lock"` for claims + legacy symlinks, `SCHEMA_UACCESS_DIR "/.lock"` for uaccess.

- [ ] **Step 1: Thread the mode** through `dispatch()`/`run_ctx_runs()`; coldplug and the current live call pass `DISPATCH_INLINE` for now. Build: `make schema-udev` clean, no warnings.
- [ ] **Step 2: Move `monitor_broadcast(&shadow_ev)`** from inside the `if (g_live)` block (~line 356) to the last statement of `dispatch()`, after the uaccess apply/clear and after the legacy `.dev` rules loop. Keep a `struct uevent` copy (or flag + pointer) so the broadcast still sends the post-rules `shadow_ev`, only when `g_live` and `dev_ctx_init` succeeded, in both modes.
- [ ] **Step 3: Wrap** `rule_links_update(...)` with the claims lock; `uaccess_record`/`uaccess_apply` (add/change) and `uaccess_clear`/`ua_clear_node` (remove) with the uaccess lock; `symlink_apply`/`symlink_clear` in the legacy loop with the claims lock. `run_hook()` stays outside any lock.
- [ ] **Step 4: Worker-mode RUN wait** in `run_ctx_runs` as specified in Interfaces.
- [ ] **Step 5: Verify parity:** `make test` passes; `make verify-rules-live && sudo -n ./verify-rules-live` (or `tests/verify_rules_live.sh`) reports the same divergence count as on master (record master's number first).
- [ ] **Step 6: Commit** "schema-udev: dispatch modes, broadcast after ACLs, flock shared registries".

---

### Task 4: Worker main loop, deadlines, test feed

**Files:**
- Modify: `schema-udev.c:487-592` (`main`), new static helpers in `schema-udev.c`
- Create: `tests/test_udev_workers.py`
- Modify: `docs/roadmap.md` (one line under schema-udev: hotplug worker pool)

**Interfaces:**
- Consumes: `struct sched_meta`, `sched_meta_from`, `sched_pick`, `sched_cap` (Task 1); `dispatch(ev, DISPATCH_WORKER)` (Task 3).
- Produces (test hooks, inert unless set; daemon refuses them when `geteuid()==0`, logs and exits 1):
  - `SCHEMA_UDEV_TEST_FEED=<path>`: bind a `AF_UNIX`/`SOCK_DGRAM` socket at path and read it in place of netlink; skip subsystem coldplug and the sysfs walk; force dry-run.
  - `SCHEMA_UDEV_TEST_RULES=<dir>`: the only rules dir (instead of `RULE_DIRS`).
  - `SCHEMA_UDEV_EVENT_TIMEOUT_MS=<n>`: per-event deadline (default 180000).
  - `SCHEMA_UDEV_SLOW_MS=<n>`: slow-log threshold (default 1000); 0 logs every event.
  - Startup line when the feed is active: `[schema-udev] test feed on <path>` (test waits for it).

Main loop behaviour (spec §Main loop, §Scheduling, §Timeouts, §SIGCHLD/HUP/TERM): queue entry = `{ char *raw; size_t len; struct sched_meta m; double enq_ms; }`, FIFO array of 4096; running table of `cap` slots `{ pid_t pid; struct sched_meta m; double start_ms; double deadline_ms; int killed; const char *action/devpath copies }`. After every wakeup: drain SIGCHLD (`waitpid(-1,&st,WNOHANG)` loop, unknown pids ignored, slot freed, abnormal exit logged unless `killed`), kill expired (`SIGKILL`, log TIMED OUT, free slot immediately, keep pid in a small "killed, unreaped" list only so its later reap is recognised and not logged), then start workers while `nr < cap` and `sched_pick` ≥ 0. Enqueue on a full queue drops the new datagram and logs `[schema-udev] event queue full (4096), dropping events` at most once per second, with a count of drops since the last line. `poll` timeout = ms to earliest deadline, -1 if none. Slow log on reap when `now - enq_ms >= SLOW_MS`. Child after fork: empty sigmask, `SIG_DFL` for HUP/TERM/INT/CHLD, close netlink + signalfd + feed fds, keep `g_monfd`, `uevent_parse`, `dispatch(&ev, DISPATCH_WORKER)`, `_exit(0)`. TERM/INT: stop starting, SIGTERM running, 5 s of WNOHANG reaps at 50 ms, SIGKILL the rest, exit. HUP: reload as today.

- [ ] **Step 1: Write the failing test** `tests/test_udev_workers.py` (skip with a message and exit 0 when run as root or when `./schema-udev` is missing). Helper `ev(action, devpath, seq, **kv)` builds `f"{action}@{devpath}\0ACTION={action}\0DEVPATH={devpath}\0SUBSYSTEM=...\0SEQNUM={seq}\0..."`. Rules fixture in a temp dir, `90-test.rules`:
  ```
  ENV{TESTSLEEP}=="?*", PROGRAM="/bin/sleep $env{TESTSLEEP}"
  ENV{TESTRUN}=="?*", RUN+="/bin/sleep $env{TESTRUN}"
  ```
  Start `./schema-udev` with the four env vars (`SLOW_MS=0`), wait for `test feed on`, then cases; parse `slow <ms>ms <ACTION> <DEVPATH>` lines with their arrival time:
  - `unrelated_not_blocked`: A (`/devices/t/a`, TESTSLEEP=5) then B (`/devices/t/b`); B's slow line arrives < 1.5 s after send, A's ≥ 5 s.
  - `same_device_ordered`: A add (TESTSLEEP=2) then A change; change's line arrives after add's.
  - `replug_waits`: block 8:16 at `/devices/u1/sdb` remove (TESTSLEEP=2) then 8:16 at `/devices/u2/sdb` add; add's line after remove's.
  - `run_waited`: C with TESTRUN=2 → its slow line reports ≥ 2000 ms.
  - `idle_deadline_fires`: restart with `EVENT_TIMEOUT_MS=2000`; D with TESTSLEEP=20, send nothing else; `event TIMED OUT` line for D within 4 s; then D change (no sleep) is handled within 1 s after that.
  - `term_with_hung_worker`: with D-style worker running (TESTSLEEP=20), send SIGTERM; process exits within 7 s.
- [ ] **Step 2: Run to verify it fails:** `python3 tests/test_udev_workers.py` → fails waiting for `test feed on` (hooks not implemented).
- [ ] **Step 3: Implement** the hooks and the worker loop in `schema-udev.c` as described; coldplug untouched.
- [ ] **Step 4: Run to verify it passes:** `make schema-udev && python3 tests/test_udev_workers.py` → all PASS. Then `make test` and `sh .github/ci-python-suite.sh` → both exit 0 (the new test is picked up by the `tests/test_*.py` loop).
- [ ] **Step 5: Mutation checks** (revert each after): make `sched_conflict` ignore devnum → `replug_waits` fails; make `poll` use -1 → `idle_deadline_fires` fails; skip the child sigmask reset → `term_with_hung_worker` fails (or document why it cannot, and keep the reset regardless).
- [ ] **Step 6: Commit** "schema-udev: per-event workers with deadlines; test feed" plus the roadmap line.

---

### Task 5: VM, PR, review, hardware gate

**Files:** none new (harness edits stay in `~/schema-livetest/`, outside the repo).

- [ ] **Step 1: QEMU:** `cd ~/schema-livetest && ./vmtest.sh` → `>> RESULT: PASS` (schema-udev is not on the VM rail; this proves the PID 1 build is unaffected).
- [ ] **Step 2: Push + draft PR** on `pr-j-udev-workers` (spec + plan + code), body lists the seven design calls and the Greg/builder-check folds; then one `/code-review`, fix findings, re-run Task 4 Step 4.
- [ ] **Step 3: DBox (ask Jonathan before touching it):** install the branch's `schema-udev`, restart `udevd` service (live restart is allowed for schema-udev), then: plug/unplug USB storage twice in different ports → `/dev/disk/by-id|by-uuid` links correct, no stale ones (`ls -l /dev/disk/by-id`); keyboard + mouse replug → seat ACL present (`getfacl /dev/input/eventN` shows the session user) and input works in the session; `ip link` NIC names unchanged; `grep -E 'TIMED OUT|slow' udevd.log` reviewed. Reboot DBox once and repeat the checks after coldplug.
- [ ] **Step 4: blakbox (ask first):** same checks, plus the five SATA disks' `/dev/disk/by-*` links and `hdparm` APM lines in `udevd.log`.
- [ ] **Step 5: Merge** (squash) only with CI + deb green and Steps 3–4 passed; update memory (`project_schema_audit_20261112.md`) and the release-notes owed list.
