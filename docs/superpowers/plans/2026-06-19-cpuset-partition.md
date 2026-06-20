# cpuset_partition= (exclusive / isolated CPU cores) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `.svc` key `cpuset_partition=isolated|root|member` that turns a service's `cpuset=` cores into a cgroup v2 exclusive (and optionally scheduler-isolated) partition.

**Architecture:** New enum field on `service_t`, parsed in both load paths with load-time normalization. At spawn, `cgroup_apply_limits` reserves the cores in `schema-init`'s `cpuset.cpus.exclusive` via read-modify-write of the live sysfs value (kernel resolves the union — no services-table access, swap-safe), sets the service's own exclusive + partition, reads back, and degrades to plain pinning if the kernel reports `invalid`.

**Tech Stack:** C99 (`-std=c99 -Wall -Wextra -O2 -D_GNU_SOURCE`), cgroup v2 cpuset controller, GNU make, QEMU/KVM vmtest harness.

## Global Constraints

- C99, must compile clean under `-Wall -Wextra` (zero warnings — house style).
- No new files in the C tree; modify `service.h` / `service.c` / `README.md` only.
- No docstrings/comments beyond existing density; match surrounding style.
- `member` is the default and MUST be a no-op (existing ~30 services unchanged).
- Never block boot: kernel rejection degrades, it does not fail the service.
- `schema-init` stays a cgroup `member` (remote-partition path only).
- No services-table access from `service.c` (init.c double-buffers `services_a`/`services_b` and swaps the `services` pointer on reload — init.c:1282–1283).
- Spec: `docs/superpowers/specs/2026-06-19-cpuset-partition-design.md`.

---

### Task 1: Data model + parsing + load-time normalization

**Files:**
- Modify: `service.h` (enum near PRIO enum ~lines 26–28; field after `cpuset[64]` line 73)
- Modify: `service.c` (new `parse_partition` helper; parse case in `services_load` ~line 503 and `service_load_one` ~line 662; normalization after each parse loop)

**Interfaces:**
- Produces: `enum { PART_MEMBER = 0, PART_ROOT, PART_ISOLATED };`
- Produces: `int service_t::cpuset_partition;` (default 0 = `PART_MEMBER`, zeroed by the existing `memset(svc, 0, sizeof(*svc))` in both loaders)
- Produces: `static int parse_partition(const char *val);` → returns `PART_ISOLATED`/`PART_ROOT`/`PART_MEMBER` (case-insensitive; unknown → `PART_MEMBER`)

- [ ] **Step 1: Add the enum to `service.h`**

After the priority enum (the `PRIO_CRITICAL` block, ~line 28), add:

```c
enum {
    PART_MEMBER = 0,   /* plain cpuset.cpus pinning (default, no-op) */
    PART_ROOT,         /* exclusive partition, still load-balanced */
    PART_ISOLATED      /* exclusive + removed from scheduler load-balancing */
};
```

- [ ] **Step 2: Add the field to `service_t` in `service.h`**

Immediately after the `cpuset[64]` member (line 73):

```c
    int              cpuset_partition; /* PART_MEMBER/ROOT/ISOLATED; cgroupv2 cpuset.cpus.partition */
```

- [ ] **Step 3: Add the `parse_partition` helper to `service.c`**

Place it just above `service_load_one` (~line 588, file-static, near the other parse helpers). `strcasecmp` is already available via `<strings.h>`/`_GNU_SOURCE` (used by the `priority` parse).

```c
static int parse_partition(const char *val) {
    if (strcasecmp(val, "isolated") == 0) return PART_ISOLATED;
    if (strcasecmp(val, "root") == 0)     return PART_ROOT;
    return PART_MEMBER;
}
```

- [ ] **Step 4: Parse `cpuset_partition` in `services_load`**

In `services_load`, immediately after the `cpuset` case (after line 504):

```c
            } else if (strcmp(line, "cpuset_partition") == 0) {
                svc->cpuset_partition = parse_partition(val);
```

- [ ] **Step 5: Parse `cpuset_partition` in `service_load_one`**

In `service_load_one`, immediately after its `cpuset` case (after line 663):

```c
        } else if (strcmp(line, "cpuset_partition") == 0) {
            svc->cpuset_partition = parse_partition(val);
```

- [ ] **Step 6: Add load-time normalization in both loaders**

In `service_load_one`, immediately after the `while (fgets(...))` parse loop closes (before `fclose(f)`):

```c
    if (svc->cpuset_partition != PART_MEMBER && svc->cpuset[0] == '\0') {
        fprintf(stderr,
                "[schema-init] WARN: '%s' cpuset_partition set without cpuset= "
                "— ignoring (no cores to isolate)\n", svc->name);
        svc->cpuset_partition = PART_MEMBER;
    }
```

In `services_load`, add the identical block at the point where each service file finishes parsing (immediately after that file's `while (fgets(...))` loop closes, before the service is accepted/counted — the same place the `svc` pointer is fully populated).

- [ ] **Step 7: Compile clean**

Run: `make -C ~/projects/schema-init 2>&1 | tail -5`
Expected: builds `schema-init schema-ctl schema-subreaper schema-journal-sink` with **zero warnings**.

- [ ] **Step 8: Commit**

```bash
cd ~/projects/schema-init
git add service.h service.c
git commit -m "service: parse cpuset_partition= + normalize empty-cpuset case

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Partition apply + readback + degrade in `cgroup_apply_limits`

**Files:**
- Modify: `service.c` `cgroup_apply_limits` — append a block after the existing `cpuset.cpus` write (after line 200, before the closing `}` of the function)

**Interfaces:**
- Consumes: `svc->cpuset_partition` (Task 1), `svc->cpuset`, `svc->cgroup_path`, `svc->name`
- Behavior: idempotent across restarts; no-op when `cpuset_partition == PART_MEMBER` or `cpuset` empty; never returns an error (degrades in place)

- [ ] **Step 1: Add the partition block**

Append inside `cgroup_apply_limits`, after the existing `if (svc->cpuset[0]) { ... }` block (after line 200):

```c
    if (svc->cpuset_partition != PART_MEMBER && svc->cpuset[0]) {
        const char *want =
            (svc->cpuset_partition == PART_ISOLATED) ? "isolated" : "root";
        char part[64];
        ssize_t r;
        int ok;

        /* 1. Reserve these cores in schema-init's exclusive union.
         *    Read-modify-write the live value; the kernel resolves the union
         *    and dedups. Strip the trailing newline; no leading comma when
         *    the existing set is empty. */
        {
            char excl[256] = {0};
            char merged[320];
            int rfd = open("/sys/fs/cgroup/schema-init/cpuset.cpus.exclusive",
                           O_RDONLY);
            if (rfd >= 0) {
                r = read(rfd, excl, sizeof(excl) - 1);
                close(rfd);
                if (r > 0) excl[r] = '\0';
            }
            excl[strcspn(excl, "\n")] = '\0';
            if (excl[0])
                snprintf(merged, sizeof(merged), "%s,%s", excl, svc->cpuset);
            else
                snprintf(merged, sizeof(merged), "%s", svc->cpuset);
            fd = open("/sys/fs/cgroup/schema-init/cpuset.cpus.exclusive",
                      O_WRONLY);
            if (fd >= 0) { write(fd, merged, strlen(merged)); close(fd); }
        }

        /* 2. Claim the cores exclusively for this service. */
        snprintf(path, sizeof(path), "%s/cpuset.cpus.exclusive", svc->cgroup_path);
        fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, svc->cpuset, strlen(svc->cpuset)); close(fd); }

        /* 3. Request the partition. */
        snprintf(path, sizeof(path), "%s/cpuset.cpus.partition", svc->cgroup_path);
        fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, want, strlen(want)); close(fd); }

        /* 4. Read back; the kernel appends " invalid (...)" on failure. */
        part[0] = '\0';
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            r = read(fd, part, sizeof(part) - 1);
            close(fd);
            if (r > 0) part[r] = '\0';
        }
        ok = (strstr(part, "invalid") == NULL &&
              strncmp(part, want, strlen(want)) == 0);

        if (!ok) {
            /* Degrade: revert to plain member pinning (cpuset.cpus stands).
             * No parent rollback — a member child claims no cores, so the
             * leftover union entry is inert. */
            fd = open(path, O_WRONLY);                 /* still cpuset.cpus.partition */
            if (fd >= 0) { write(fd, "member", 6); close(fd); }
            snprintf(path, sizeof(path), "%s/cpuset.cpus.exclusive",
                     svc->cgroup_path);
            fd = open(path, O_WRONLY);
            if (fd >= 0) { write(fd, "\n", 1); close(fd); }
            part[strcspn(part, "\n")] = '\0';
            fprintf(stderr,
                    "[schema-init] HAZARD: '%s' cpuset_partition=%s rejected "
                    "(kernel: '%s') — degraded to plain cpuset pinning on %s\n",
                    svc->name, want, part[0] ? part : "?", svc->cpuset);
        }
    }
```

Notes for the implementer:
- `fd`, `path` are already declared at the top of `cgroup_apply_limits` — reuse them; do not redeclare.
- Clearing the service's `cpuset.cpus.exclusive` is `write(fd, "\n", 1)` (an empty line clears it; a 0-byte write is a no-op).
- `want` is a string literal pointer, so `write(fd, want, strlen(want))` avoids any `-Wformat` issue.

- [ ] **Step 2: Compile clean**

Run: `make -C ~/projects/schema-init 2>&1 | tail -5`
Expected: zero warnings.

- [ ] **Step 3: Commit**

```bash
cd ~/projects/schema-init
git add service.c
git commit -m "service: apply cpuset.cpus.partition with readback + degrade

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: vmtest integration gate (the four spec assertions)

**Files:**
- Modify: `~/schema-livetest/vmtest.sh` (local harness, NOT version-controlled) — `-smp 2`→`-smp 4`, add `grep head` applets, add three test svcs, extend the reporter, add assertions

**Interfaces:**
- Consumes: a freshly built `~/projects/schema-init/schema-init` with Tasks 1–2 applied
- Produces: serial-log markers `CPUSET-REPORT … CPUSET-END` + PASS/FAIL verdict

- [ ] **Step 1: Bump SMP and add busybox applets**

In `vmtest.sh`, change the applet loop (line 34) to include `grep` and `head`:

```bash
for a in sh ls cat sleep touch poweroff mount mkdir echo grep head; do
```

And change `-smp 2` (line 73) to `-smp 4`.

- [ ] **Step 2: Add the five test svcs to the initramfs**

After the existing `cp .../test-dependent.svc` line (line 42), add. Core map (guest is `-smp 4`, cores 0–3): `test-iso` takes core 3 isolated, `test-root` takes core 2 root — both are *exclusive*, so the unconstrained sibling `test-share` should end up on `0-1`.

```bash
cat > "$ROOT/etc/schema-init/services/test-iso.svc" <<'EOF'
name=test-iso
exec=/bin/sleep
args=600
cpuset=3
cpuset_partition=isolated
EOF
cat > "$ROOT/etc/schema-init/services/test-root.svc" <<'EOF'
name=test-root
exec=/bin/sleep
args=600
cpuset=2
cpuset_partition=root
EOF
cat > "$ROOT/etc/schema-init/services/test-share.svc" <<'EOF'
name=test-share
exec=/bin/sleep
args=600
EOF
cat > "$ROOT/etc/schema-init/services/test-iso2.svc" <<'EOF'
name=test-iso2
exec=/bin/sleep
args=600
cpuset=3
cpuset_partition=isolated
dep=test-iso
EOF
cat > "$ROOT/etc/schema-init/services/test-noset.svc" <<'EOF'
name=test-noset
exec=/bin/sleep
args=600
cpuset_partition=isolated
EOF
```

- `test-iso` — core 3, `isolated` (happy path).
- `test-root` — core 2, `root` (exclusive but load-balanced; the `root` variant).
- `test-share` — no cpuset; its effective set should lose **both** exclusive cores (2 and 3) → `0-1`.
- `test-iso2` — `dep=test-iso` so it spawns *after* iso forms its partition; also claims core 3 → must lose the race and degrade to `member` + HAZARD.
- `test-noset` — `isolated` with no `cpuset=` → normalized to `member` at load with a WARN (assertion 4).

- [ ] **Step 3: Extend the reporter to dump cpuset state**

In the `vmfinish.sh` heredoc (lines 48–57), add before `echo "===== VMTEST-END ====="`:

```sh
echo "===== CPUSET-REPORT ====="
echo "iso-partition: $(cat /sys/fs/cgroup/schema-init/test-iso/cpuset.cpus.partition 2>&1)"
ISO_PID=$(head -1 /sys/fs/cgroup/schema-init/test-iso/cgroup.procs 2>/dev/null)
echo "iso-affinity: $(grep Cpus_allowed_list /proc/$ISO_PID/status 2>&1)"
echo "root-partition: $(cat /sys/fs/cgroup/schema-init/test-root/cpuset.cpus.partition 2>&1)"
echo "share-effective: $(cat /sys/fs/cgroup/schema-init/test-share/cpuset.cpus.effective 2>&1)"
echo "iso2-partition: $(cat /sys/fs/cgroup/schema-init/test-iso2/cpuset.cpus.partition 2>&1)"
echo "schema-excl: $(cat /sys/fs/cgroup/schema-init/cpuset.cpus.exclusive 2>&1)"
echo "===== CPUSET-END ====="
```

(The empty-cpuset normalization WARN is emitted at load to schema-init's stderr → serial console, so it is already in `$SERIAL` and needs no reporter line.)

- [ ] **Step 4: Add assertions to the verdict block**

After the existing `grep ... SDBOOTED-DIR` assertion (line 89), add — these are the spec's four assertions plus the sibling-exclusivity cross-check:

```bash
grep -Eq "iso-partition: isolated"                  "$SERIAL" || { echo "  MISS: iso partition not isolated"; pass=0; }
grep -Eq "iso-affinity:.*Cpus_allowed_list:[[:space:]]*3$" "$SERIAL" || { echo "  MISS: iso affinity != core 3"; pass=0; }
grep -Eq "root-partition: root"                     "$SERIAL" || { echo "  MISS: root variant did not form partition"; pass=0; }
grep -Eq "share-effective: 0-1"                     "$SERIAL" || { echo "  MISS: sibling still sees an exclusive core"; pass=0; }
grep -Eq "iso2-partition: member"                   "$SERIAL" || { echo "  MISS: overlapping iso2 did not degrade"; pass=0; }
grep -Eq "HAZARD: 'test-iso2' cpuset_partition=isolated rejected" "$SERIAL" || { echo "  MISS: degrade HAZARD not logged"; pass=0; }
grep -Eq "WARN: 'test-noset' cpuset_partition set without cpuset" "$SERIAL" || { echo "  MISS: empty-cpuset normalization warn"; pass=0; }
```

Maps to the spec's four testing items: **(1)** isolated happy path = `iso-partition`/`iso-affinity`; **(2)** degrade = `iso2-partition: member` + HAZARD; **(3)** `root` variant = `root-partition: root` (core 2 exclusive, proven by `share-effective: 0-1`); **(4)** empty-cpuset normalization = the `test-noset` WARN. `share-effective: 0-1` cross-checks that *both* exclusive cores left the shared pool.

- [ ] **Step 5: Run the vmtest**

Run: `~/schema-livetest/vmtest.sh 2>&1 | tail -30`
Expected: the `CPUSET-REPORT` block shows `iso-partition: isolated`, `iso-affinity: … 3`, `share-effective: 0-2`, `iso2-partition: member`; verdict line `>> RESULT: PASS`. If FAIL, the kept workdir + `~/schema-livetest/last-vmtest-serial.log` hold the full serial.

- [ ] **Step 6: Commit (spec-doc note only; harness is not version-controlled)**

No repo files change in this task. If the verdict passed, record it in the next task's PR body. (The harness lives only on Claire/blakbox per the spec.)

---

### Task 4: Documentation — README config table + checklist

**Files:**
- Modify: `README.md` (config table after the `cpuset` row ~line 155; capability checklist line ~767)

**Interfaces:** none (docs only)

- [ ] **Step 1: Add the config-table row**

Immediately after the `cpuset` row (line 155), add:

```markdown
| `cpuset_partition` | `member` | Exclusivity tier for the `cpuset=` cores via cgroupv2 `cpuset.cpus.partition`. `member` (default) = plain pinning, cores stay shared (no-op). `root` = the cores become an *exclusive* partition (no other service may run on them) while still being scheduler-load-balanced. `isolated` = exclusive **and** removed from the scheduler's load balancer — dynamic `isolcpus=`, no kernel cmdline needed; the target for a latency-critical control loop (the Ungulate Leg) or the audio path. Implemented as a cgroup v2 *remote partition*: schema-init reserves the cores in its own `cpuset.cpus.exclusive` and the service forms the partition, so the other services are unaffected. If the kernel rejects the partition (overlapping cores between two isolated services, or no cpuset controller) the service silently degrades to plain `cpuset=` pinning and a `HAZARD` line is logged — boot is never blocked. Requires a non-empty `cpuset=`; setting it alone is ignored with a warning. |
```

- [ ] **Step 2: Update the capability checklist line**

Replace the `cpuset=` clause in the checklist line (line 767) so it reads:

```markdown
- [x] Cgroup resource limits — `cpu_limit=` (1–100, % of one core), `mem_limit=` (MB), `cpuset=` (CPU affinity / core pinning, systemd `AllowedCPUs=` analog), and `cpuset_partition=` (`isolated`/`root` exclusive cores via cgroupv2 partitions — dynamic `isolcpus`) per `.svc`; written via sync-pipe window before child exec; IEC 62304 Class C blast-radius isolation
```

- [ ] **Step 3: Commit**

```bash
cd ~/projects/schema-init
git add README.md
git commit -m "docs: document cpuset_partition= (exclusive/isolated cores)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Done criteria

- `make` builds zero-warning with Tasks 1–2 applied.
- `~/schema-livetest/vmtest.sh` → `RESULT: PASS` with the four CPUSET assertions green.
- README documents the key + checklist.
- Open PR `feat/cpuset-partition` → master; do NOT merge until the vmtest verdict is pasted in the PR body. (Deploy/reboot verification is a follow-up session step, same as PR #19 — the running binary is unaffected until `install` to `/sbin` + reboot.)
