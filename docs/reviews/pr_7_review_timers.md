# Pull Request #7 Review: Timer Services in `schema-init`

This document provides a detailed code review of **PR #7 (periodic services/timers)** for `schema-init`. 

We have pressure-tested the implementation across the four specific scenarios requested and have successfully checked in fixes to the local branch `feat/timers` to resolve the identified bugs.

---

## 🔍 Detailed Pressure-Test & Resolution Analysis

### 1. Dependent-on-a-Timer Hazard
* **The Hazard**: A timer service periodically transitions through the lifecycle: `STATE_PERFECT` → `STATE_NEW_PROCESS` → `STATE_FULL_TRUST` → `STATE_PERFECT`. Because [service_deps_ready](file:///home/ajax80/projects/schema-init/service.c#L337) only treats `STATE_FUNDAMENTAL`, `STATE_SETTLED`, and `STATE_PERFECT` as ready, any service depending on a timer (e.g. `dep=my-timer`) will have its dependency satisfied *only* when the timer is idle. During each fire window, the dependency goes unsatisfied.
* **Our Read**: 
  - Yes, this hazard exists. If a dependent service crashes or tries to restart while the timer is executing, it will be transiently blocked in `STATE_NEW_PROCESS` until the timer completes.
  - Architecturally, depending on a timer service is an anti-pattern. A timer service is transient and does not expose persistent endpoints or APIs for other services to consume.
* **Action Taken**: 
  - We documented that services **should not depend on timer services**.
  - To prevent developer mistakes, we added an active validation check in [validate_and_resolve](file:///home/ajax80/projects/schema-init/init.c#L1048) in [init.c](file:///home/ajax80/projects/schema-init/init.c). If a service declares a dependency on a timer, a startup/reload warning is logged:
    ```c
    if (svc_table[j].flags & SVC_TIMER) {
        printf("[schema-init] WARNING: service '%s' depends on timer service '%s'. Dependents of timer services may be transiently blocked when the timer fires.\n",
               svc_table[s].name, svc_table[j].name);
    }
    ```

---

### 2. Reload Re-Arming
* **The Hazard**: During a `schema-ctl reload` event:
  1. **Existing Timers**: The `timer_next` deadline field was not merged from active services to shadow services in [handle_reload](file:///home/ajax80/projects/schema-init/init.c#L1088). Consequently, the shadow service received `timer_next.tv_sec = 0` (via `memset`), causing it to satisfy the fire condition immediately on the first tick post-reload.
  2. **New Timers**: Brand-new timer services added to the configuration started in `STATE_NEW_PROCESS` (the default) and fired immediately, completely ignoring their `on_boot_sec` delay.
* **Our Read**: This behavior is undesirable. Configuration reloads should not trigger a stampede of immediate timer executions.
* **Action Taken**:
  - We modified [handle_reload](file:///home/ajax80/projects/schema-init/init.c#L1088) to preserve the `timer_next` state for existing timers across reloads:
    ```c
    shadow_services[i].timer_next = services[j].timer_next;
    ```
  - We added a post-swap arming loop in [handle_reload](file:///home/ajax80/projects/schema-init/init.c#L1088) that detects newly added timers (where `timer_next.tv_sec == 0`) and arms them to `STATE_PERFECT` with `timer_next = tnow + timer_boot_sec` (respecting `on_boot_sec` just like boot-time initialization):
    ```c
    struct timespec tnow;
    clock_gettime(CLOCK_MONOTONIC, &tnow);
    for (i = 0; i < svc_count; i++) {
        if (services[i].flags & SVC_TIMER) {
            if (services[i].timer_next.tv_sec == 0) {
                services[i].inst.state = STATE_PERFECT;
                services[i].timer_next = tnow;
                services[i].timer_next.tv_sec += services[i].timer_boot_sec;
            }
        }
    }
    ```

---

### 3. Group-State Flap
* **The Hazard**: If a timer service is a member of a group, every time the timer fires, its state transitions to `STATE_NEW_PROCESS` / `STATE_FULL_TRUST`. The group's state-aggregation logic in [aggregate](file:///home/ajax80/projects/schema-init/group.c#L20) maps these states to a group state of `STATE_NEW_PROCESS`, causing the group's state to flap from `STATE_FUNDAMENTAL` (or `STATE_PERFECT`) to `STATE_NEW_PROCESS` and back, resulting in console/log spam.
* **Our Read**: Flapping group states clutter the logs and may cause transient startup blocks for group-dependent services. A running timer is executing its normal, scheduled behavior and should not cause the group to be reported as "starting up" or unstable.
* **Action Taken**:
  - In the main loop of [init.c](file:///home/ajax80/projects/schema-init/init.c), we intercepted the state array population passed to [groups_update](file:///home/ajax80/projects/schema-init/group.c#L49).
  - If a service is a timer (`SVC_TIMER`) and is currently running (`STATE_NEW_PROCESS` or `STATE_FULL_TRUST`), we project its state as `STATE_PERFECT` for group aggregation purposes. If the timer fails (e.g. `EXCISED`, `FRICTION`, or `DORMANT`), the aggregate state continues to correctly reflect the failure:
    ```c
    for (i = 0; i < svc_count; i++) {
        uint8_t s = services[i].inst.state;
        if ((services[i].flags & SVC_TIMER) && (s == STATE_NEW_PROCESS || s == STATE_FULL_TRUST)) {
            svc_states[i] = STATE_PERFECT;
        } else {
            svc_states[i] = s;
        }
    }
    ```

---

### 4. tv_sec-Only Comparison (`_tn.tv_sec >= svc->timer_next.tv_sec`)
* **The Hazard**: Comparing only seconds ignores nanoseconds. Because `reap()` sets `timer_next` relative to the current monotonic time (`clock_gettime`), a seconds-only comparison introduces a severe bug for short-interval or sub-second boundaries.
* **Our Read**: 
  - This is a critical bug. Suppose a timer with a `1s` interval fires at `sec 10, nsec 900M` and re-arms to `sec 11, nsec 900M`.
  - Just 100ms later, the clock ticks to `sec 11, nsec 0`. With the seconds-only comparison, `11 >= 11` is true, and the timer fires again immediately!
  - The timer effectively runs twice within 100ms, completely violating the `1s` interval.
* **Action Taken**:
  - We replaced the seconds-only check in [tick_service](file:///home/ajax80/projects/schema-init/init.c#L530) with a full `timespec` comparison:
    ```c
    if (_tn.tv_sec > svc->timer_next.tv_sec ||
        (_tn.tv_sec == svc->timer_next.tv_sec && _tn.tv_nsec >= svc->timer_next.tv_nsec))
    ```
  - This ensures that timers always respect their exact scheduled interval (to within the 250ms tick frequency) without premature double-firing.
