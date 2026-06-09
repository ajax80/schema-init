# Design Review: CPU Priority Weight & Scheduling Fallbacks

This document reviews the changes proposed in the `feat/cpu-priority-weight` branch of `schema-init`. We assess the use of cgroup v2 `cpu.weight`, the aggressiveness of the critical weight, and the addition of a `setpriority` fallback.

---

## 🔍 Core Analysis & Architecture

The implementation uses the existing cgroup v2 architecture to enforce CPU prioritization, utilizing `cpu.weight` to set proportional shares during CPU saturation.

### 1. Synchronization and Race Conditions
Because the cgroup population and configuration occur in the parent side of `service_spawn` while the child process is blocked on a read barrier `sync` pipe, we are guaranteed that:
- The child process's PID is successfully moved into the cgroup via `cgroup_assign`.
- The cgroup limits and weight are written via `cgroup_apply_limits`.
- The child only proceeds to drop privileges and `execv` once these scheduling contexts are fully established.

This structure is highly robust and avoids any startup races.

---

## ⚖️ Weight Tuning: 10000 vs. 1000

The original implementation set the weight for `PRIO_CRITICAL` to the maximum allowed value of `10000` (compared to `100` for standard, and `10` for peripheral).

While a weight of `10000` guarantees the compositor gets immediate CPU attention, it results in a **100:1 ratio** against standard services. Under CPU saturation (e.g., a massive compile job or heavy disk sync), standard services get only **0.99%** of the CPU. This near-total starvation can trigger timeouts on other critical service loops, loggers, or watchdog timers.

### Proportional Share Comparison

| Priority Class | Weight = 10000 (Original) | Weight = 1000 (Adjusted) | Relative Ratio (vs. Standard) |
| :--- | :--- | :--- | :--- |
| **Critical** | **99.0% CPU** | **90.9% CPU** | 10x |
| **Standard** | 0.9% CPU | 9.1% CPU | 1x |
| **Peripheral** | 0.1% CPU | 0.9% CPU | 0.1x |

### Rationale for Tuning to 1000:
- **Starvation Prevention**: A `90.9%` allocation gives the display compositor (`KWin`/`Xorg`) and real-time control loops all the CPU they need during a render frame, but leaves standard services (logging, socket keepalives) with enough share (`9.1%`) to avoid timeout cascading failures.
- **Symmetric Spacing**: Weight values `10` / `100` / `1000` form a clean, base-10 logarithmic progression.

---

## 🔄 Design Fork: The `setpriority()` / `nice` Fallback

On embedded hardware (e.g., certain Raspberry Pi configs) or custom kernel builds, cgroup v2 controllers (especially the `cpu` controller) might be compiled out or missing from the hierarchy. 

### Why a Fallback is Critical
Without a fallback:
- If `cgroup_assign` fails or `cpu.weight` cannot be written, the priority setting silently does nothing.
- The compositor runs with default priority and stuttering returns under load.

### Implementation Details
We added a surgical fallback inside the child process of `service_spawn` using `setpriority()`.

1. **Header Inclusion**: Included `<sys/resource.h>` in `service.c`.
2. **Privilege timing**: The fallback is executed **before** we drop privileges via `setuid()` to ensure we can assign negative nice values for critical processes.
3. **Nice Mappings**:
   - `PRIO_CRITICAL` $\rightarrow$ Nice `-10` (standard for high-priority display and audio servers, e.g., Pipewire/X11).
   - `PRIO_STANDARD` $\rightarrow$ Nice `0` (default).
   - `PRIO_PERIPHERAL` $\rightarrow$ Nice `10` (low priority).
4. **Error Handling**: The return value of `setpriority()` is checked. If it fails (e.g., lack of privileges, container namespaces, or EPERM), it prints a warning to `stderr` (which redirects to the service log file) to assist debugging without blocking `execv`.

```c
        if (svc->priority == PRIO_CRITICAL) {
            if (setpriority(PRIO_PROCESS, 0, -10) < 0) {
                fprintf(stderr, "[schema-init] Warning: failed to set critical priority for %s: %s\n", svc->name, strerror(errno));
            }
        } else if (svc->priority == PRIO_PERIPHERAL) {
            if (setpriority(PRIO_PROCESS, 0, 10) < 0) {
                fprintf(stderr, "[schema-init] Warning: failed to set peripheral priority for %s: %s\n", svc->name, strerror(errno));
            }
        }
```

### Double-Benefit:
Even when cgroup weights *are* active, nice values still apply. They dictate priority *within* the cgroups or if child processes get spawned outside, providing a dual-layered scheduling safety net.

---

## 🛠️ Verification Build

To compile the branch with the updated changes:
```bash
make clean && make
```
The build completes successfully without errors or warnings related to the scheduler modifications.
