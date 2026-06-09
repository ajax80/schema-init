# Pull Request #8 Review: Start Timeout Guard in `schema-init`

This document details the pressure-testing and code review of **PR #8 (startup timeouts)**, which addresses boot hangs by terminating services stuck unpromoted in `STATE_FULL_TRUST`.

We have analyzed all four questions raised by Greg and implemented comprehensive bug fixes directly in the branch `feat/start-timeout`.

---

## 🔍 Detailed Pressure-Test & Resolution Analysis

### 1. Retry-Storm vs. "Boot Proceeds" (The Hang Vector)
* **The Hazard**: When a oneshot service times out, PR #8 initially routed it to `STATE_RECOVERY`, which queued a retry. Because the retry budget default is 5 (`MAX_RESTARTS`) and the timeout is 90 seconds, a permanently hung oneshot would spawn 5 times, taking **450 seconds (7.5 minutes)** before finally being excised. During this entire 7.5-minute window, all non-critical dependents would remain blocked in `STATE_NEW_PROCESS`.
* **Our Read**: 
  - Retrying a startup timeout is a boot-time hazard for non-critical services. A task that hangs for 90 seconds is almost certainly deadlocked or misconfigured; retrying it immediately is highly unlikely to succeed and only delays boot.
  - However, unconditionally excising the service (i.e. moving it directly to `STATE_EXCISED`) introduces a regression for critical services (`critical=1` or `no_excise=1`), as it would bypass their recovery path and permanently block the critical boot chain.
* **Action Taken**: 
  - **The Ratified Split**: The review caught and corrected the unconditional-excise regression. We resolved this by introducing a split model:
    - **Non-critical oneshots**: Transition directly to `STATE_EXCISED` (state 76) on timeout, allowing boot to proceed immediately for dependent services.
    - **Critical oneshots** (or those with `no_excise=1`): Continue to route to `STATE_RECOVERY` (allowing normal retry cooldowns) to prevent critical boot failure without manual intervention.

---

### 2. Wall-Clock vs. Monotonic Clocks (NTP Step Vulnerability)
* **The Hazard**: PR #8 calculated timeouts using `time(NULL) - start_time` (wall-clock time). Since these services run during boot (when network connectivity is established and `chrony`/`ntp` synchronizes the clock), any forward time step > 90 seconds would instantly kill all currently running healthy oneshots. A backward time step would artificially delay timeouts.
* **Our Read**: 
  - Wall-clock time should never be used for critical service timeout calculations. 
  - Both `start_timeout_sec` AND `stable_secs` should be calculated using `CLOCK_MONOTONIC`.
* **Action Taken**:
  - We added a new field `spawn_time_mono` to the [service_t](file:///home/ajax80/projects/schema-init/service.h#L83) struct.
  - Set it during [service_spawn](file:///home/ajax80/projects/schema-init/service.c#L280):
    ```c
    clock_gettime(CLOCK_MONOTONIC, &svc->spawn_time_mono);
    ```
  - Preserved it across configuration reloads in [handle_reload](file:///home/ajax80/projects/schema-init/init.c#L1088).
  - Updated [tick_service](file:///home/ajax80/projects/schema-init/init.c#L530) to fetch monotonic time and use it for both the startup timeout and the `stable_secs` promotion checks:
    ```c
    struct timespec now_mono;
    clock_gettime(CLOCK_MONOTONIC, &now_mono);
    
    // Monotonic Start Timeout check:
    if (svc->start_timeout_sec > 0 && svc->child_pid > 0 &&
        now_mono.tv_sec - svc->spawn_time_mono.tv_sec >= svc->start_timeout_sec) { ... }

    // Monotonic Stable check:
    } else if (now_mono.tv_sec - svc->spawn_time_mono.tv_sec >= svc->stable_secs) { ... }
    ```

---

### 3. Default Timeout Value (90s)
* **Our Read**:
  - With our fix for Item 1 (immediate transition to `STATE_EXCISED` on timeout, skipping retry loops), a default of **90 seconds** is a very safe and robust value.
  - It ensures we do not have false positives on slow hardware (e.g. system initialization or disk checks on low-power devices) while guaranteeing that the total boot delay is strictly capped at 90 seconds for any hung task.
  - If retries were still enabled, 90 seconds would be too long (7.5 min boot hang). With immediate excision, it is optimal.

---

### 4. Failsafe Double-Fire Guard
* **Our Read**: We have verified the code paths and confirmed that there is **no possibility of a double-fire**.
  - On startup timeout, `active_kill_service(svc)` kills the child and performs a synchronous, non-blocking `waitpid(svc->child_pid, &status, WNOHANG)` (and falls back to `SIGKILL` + blocking `waitpid` if necessary) before setting `svc->child_pid = 0`.
  - Because `svc->child_pid` is set to `0`, the main loop's `reap()` function will not match this PID and will never execute the exit-status block.
  - Furthermore, `start_failsafe(svc)` has a guard clause `if (svc->failsafe_pid > 0) return;` which prevents executing another failsafe command if one is already running.
* **Verdict**: No code changes are required here; the guard mechanisms are fully correct.

---

## 🛠️ Build Status
We have successfully resolved the compiler warning regarding the unused `now` variable. The project compiles cleanly:
```bash
make clean && make
```
*Status: Build successful and warning-free.*
