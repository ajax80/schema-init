# Service Hardening Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add opt-in `no_new_privs` and capability-allowlist hardening to the service spawn path, applied in the child between fork and exec.

**Architecture:** All raw-syscall capability logic lives in a new dependency-free unit `caps.c`/`caps.h` (name→value table, `keep_caps` parsing, `capset`/`PR_CAPBSET_DROP` application, `no_new_privs`). `service.c` parses the two new `.svc` fields at load time and calls a thin `service_apply_hardening()` wrapper in the child. Opt-in only: a `.svc` requesting neither field spawns byte-for-byte as today. Correctness is proven by a standalone unit test for the capability math plus a vmtest boot-rail marker; a live pilot on blakbox precedes any default-flip.

**Tech Stack:** C (C11, same as the rest of the tree), Linux UAPI `<linux/capability.h>`, `<sys/prctl.h>`, `syscall(SYS_capset)`. No `libcap`, no `libseccomp`.

## Global Constraints

- No external library dependency for capabilities — raw syscalls + `<linux/capability.h>` UAPI only. Keeps PID 1's TCB minimal.
- `SVC_NO_NEW_PRIVS` = `(1 << 7)`; bits `(1 << 0)`–`(1 << 6)` are already allocated.
- Fail-closed: any *requested* hardening step that fails (non-`EINVAL`) → child `_exit(126)`, never exec unhardened.
- `PR_CAPBSET_DROP` returning `EINVAL` = the running kernel's cap ceiling; treat as loop boundary, **not** a failure.
- Child failure logging uses async-signal-safe `dprintf(2, ...)` / `write(2)`, never `fprintf` (post-fork, pre-exec).
- Backward compatible: unset `no_new_privs` and unset `keep_caps` = current behavior, zero syscalls added.
- Unknown `CAP_*` name in `keep_caps` = load error → `service_load_one` returns -1, service skipped (typo must fail loud).
- `keep_caps=` present but empty = drop **all** capabilities (mask 0); `keep_caps` absent = capabilities untouched. The `cap_restrict` flag distinguishes these.
- Capability restriction (`capset`/`capbset`) runs while root, **before** the existing `setuid`; `no_new_privs` runs last, before `execv`.
- Non-root capability *retention* (ambient caps) is out of scope for Phase 1.
- Edit existing files; the only new files are `caps.c`, `caps.h`, `tests/test_cap_parse.c` (approved deviation — makes the capability math unit-testable in isolation).

---

## File Structure

- **Create `caps.h`** — public interface: `cap_name_to_val`, `parse_cap_list`, `apply_capabilities`, `apply_no_new_privs`.
- **Create `caps.c`** — the cap name→value table and the four functions. Depends only on libc + kernel UAPI headers.
- **Create `tests/test_cap_parse.c`** — standalone unit test for `parse_cap_list` / `cap_name_to_val`.
- **Modify `service.h`** — add `cap_keep_mask`, `cap_restrict` fields; add `SVC_NO_NEW_PRIVS`; add `service_apply_hardening` prototype.
- **Modify `service.c`** — `#include "caps.h"`; two field handlers in `service_load_one`; `service_apply_hardening` definition; call site in the child.
- **Modify `Makefile`** — add `caps.o` to the `schema-init` object list.
- **`~/schema-livetest/` (outside repo)** — add `test-hardened.svc` + two reporter assertions. Applied in the harness, noted in the PR, not committed here.

---

### Task 1: `caps` unit — table, name lookup, list parsing (with unit test)

**Files:**
- Create: `caps.h`
- Create: `caps.c`
- Test: `tests/test_cap_parse.c`

**Interfaces:**
- Consumes: nothing (leaf unit).
- Produces:
  - `int cap_name_to_val(const char *name);` — returns the `CAP_*` integer value, or `-1` if the name is unknown.
  - `int parse_cap_list(const char *csv, uint64_t *mask);` — parses a comma list of `CAP_*` names into a bitmask (bit N = `CAP_N`). Returns `0` on success, `-1` if any name is unknown. Empty/whitespace-only input → `*mask = 0`, returns `0`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_cap_parse.c`:

```c
#include "../caps.h"
#include <assert.h>
#include <stdio.h>
#include <linux/capability.h>

int main(void) {
    uint64_t m;

    assert(parse_cap_list("CAP_NET_BIND_SERVICE", &m) == 0);
    assert(m == ((uint64_t)1 << CAP_NET_BIND_SERVICE));

    assert(parse_cap_list("CAP_SYS_TIME,CAP_NET_BIND_SERVICE", &m) == 0);
    assert(m == (((uint64_t)1 << CAP_SYS_TIME) | ((uint64_t)1 << CAP_NET_BIND_SERVICE)));

    assert(parse_cap_list(" CAP_CHOWN , CAP_KILL ", &m) == 0);
    assert(m == (((uint64_t)1 << CAP_CHOWN) | ((uint64_t)1 << CAP_KILL)));

    assert(parse_cap_list("", &m) == 0 && m == 0);

    assert(parse_cap_list("CAP_BOGUS", &m) == -1);

    assert(cap_name_to_val("CAP_CHOWN") == CAP_CHOWN);
    assert(cap_name_to_val("not_a_cap") == -1);

    printf("all cap-parse tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -Wall -o /tmp/test_cap_parse tests/test_cap_parse.c caps.c`
Expected: FAIL — `caps.h`/`caps.c` do not exist yet (compile error).

- [ ] **Step 3: Write the interface**

Create `caps.h`:

```c
#ifndef CAPS_H
#define CAPS_H

#include <stdint.h>

/* CAP_* name -> integer value; -1 if unknown. */
int cap_name_to_val(const char *name);

/* Parse a comma list of CAP_* names into a keep-mask (bit N = CAP_N).
 * Returns 0 on success, -1 if any name is unknown. Empty input -> mask 0. */
int parse_cap_list(const char *csv, uint64_t *mask);

/* Restrict the process to exactly keep_mask: drop every other capability
 * from the bounding set and set permitted/effective/inheritable to keep_mask.
 * Runs while root, before setuid. Returns 0 on success, -1 on real failure. */
int apply_capabilities(uint64_t keep_mask);

/* prctl(PR_SET_NO_NEW_PRIVS). Returns 0 on success, -1 on failure. */
int apply_no_new_privs(void);

#endif
```

- [ ] **Step 4: Implement the table + parsing in `caps.c`**

Create `caps.c` (the `apply_*` functions are stubbed here and filled in Task 2 so this task compiles and its test links):

```c
#define _GNU_SOURCE
#include "caps.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

static const struct { const char *name; int val; } cap_table[] = {
    { "CAP_CHOWN",            CAP_CHOWN },
    { "CAP_DAC_OVERRIDE",     CAP_DAC_OVERRIDE },
    { "CAP_DAC_READ_SEARCH",  CAP_DAC_READ_SEARCH },
    { "CAP_FOWNER",           CAP_FOWNER },
    { "CAP_FSETID",           CAP_FSETID },
    { "CAP_KILL",             CAP_KILL },
    { "CAP_SETGID",           CAP_SETGID },
    { "CAP_SETUID",           CAP_SETUID },
    { "CAP_SETPCAP",          CAP_SETPCAP },
    { "CAP_LINUX_IMMUTABLE",  CAP_LINUX_IMMUTABLE },
    { "CAP_NET_BIND_SERVICE", CAP_NET_BIND_SERVICE },
    { "CAP_NET_BROADCAST",    CAP_NET_BROADCAST },
    { "CAP_NET_ADMIN",        CAP_NET_ADMIN },
    { "CAP_NET_RAW",          CAP_NET_RAW },
    { "CAP_IPC_LOCK",         CAP_IPC_LOCK },
    { "CAP_IPC_OWNER",        CAP_IPC_OWNER },
    { "CAP_SYS_MODULE",       CAP_SYS_MODULE },
    { "CAP_SYS_RAWIO",        CAP_SYS_RAWIO },
    { "CAP_SYS_CHROOT",       CAP_SYS_CHROOT },
    { "CAP_SYS_PTRACE",       CAP_SYS_PTRACE },
    { "CAP_SYS_PACCT",        CAP_SYS_PACCT },
    { "CAP_SYS_ADMIN",        CAP_SYS_ADMIN },
    { "CAP_SYS_BOOT",         CAP_SYS_BOOT },
    { "CAP_SYS_NICE",         CAP_SYS_NICE },
    { "CAP_SYS_RESOURCE",     CAP_SYS_RESOURCE },
    { "CAP_SYS_TIME",         CAP_SYS_TIME },
    { "CAP_SYS_TTY_CONFIG",   CAP_SYS_TTY_CONFIG },
    { "CAP_MKNOD",            CAP_MKNOD },
    { "CAP_LEASE",            CAP_LEASE },
    { "CAP_AUDIT_WRITE",      CAP_AUDIT_WRITE },
    { "CAP_AUDIT_CONTROL",    CAP_AUDIT_CONTROL },
    { "CAP_SETFCAP",          CAP_SETFCAP },
    { "CAP_MAC_OVERRIDE",     CAP_MAC_OVERRIDE },
    { "CAP_MAC_ADMIN",        CAP_MAC_ADMIN },
    { "CAP_SYSLOG",           CAP_SYSLOG },
    { "CAP_WAKE_ALARM",       CAP_WAKE_ALARM },
    { "CAP_BLOCK_SUSPEND",    CAP_BLOCK_SUSPEND },
    { "CAP_AUDIT_READ",       CAP_AUDIT_READ },
#ifdef CAP_PERFMON
    { "CAP_PERFMON",          CAP_PERFMON },
#endif
#ifdef CAP_BPF
    { "CAP_BPF",              CAP_BPF },
#endif
#ifdef CAP_CHECKPOINT_RESTORE
    { "CAP_CHECKPOINT_RESTORE", CAP_CHECKPOINT_RESTORE },
#endif
};

int cap_name_to_val(const char *name) {
    for (size_t i = 0; i < sizeof(cap_table) / sizeof(cap_table[0]); i++)
        if (strcmp(cap_table[i].name, name) == 0)
            return cap_table[i].val;
    return -1;
}

int parse_cap_list(const char *csv, uint64_t *mask) {
    *mask = 0;
    char buf[512];
    strncpy(buf, csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t l = strlen(tok);
        while (l && (tok[l - 1] == ' ' || tok[l - 1] == '\t')) tok[--l] = 0;
        if (!*tok) continue;
        int v = cap_name_to_val(tok);
        if (v < 0) return -1;
        *mask |= (uint64_t)1 << v;
    }
    return 0;
}

int apply_capabilities(uint64_t keep_mask) { (void)keep_mask; return 0; }  /* Task 2 */
int apply_no_new_privs(void) { return 0; }                                 /* Task 2 */
```

- [ ] **Step 5: Run test to verify it passes**

Run: `gcc -Wall -o /tmp/test_cap_parse tests/test_cap_parse.c caps.c && /tmp/test_cap_parse`
Expected: PASS — prints `all cap-parse tests passed`.

- [ ] **Step 6: Commit**

```bash
git add caps.h caps.c tests/test_cap_parse.c
git commit -m "caps: add capability name table and keep_caps list parsing"
```

---

### Task 2: `caps` unit — apply capabilities and no_new_privs

**Files:**
- Modify: `caps.c` (replace the two stubs)

**Interfaces:**
- Consumes: `caps.h` prototypes from Task 1.
- Produces:
  - `int apply_capabilities(uint64_t keep_mask);` — drops all caps except `keep_mask` from the bounding set (`EINVAL` = kernel ceiling, stop), then `capset` permitted/effective/inheritable to `keep_mask`. Returns `0`/`-1`.
  - `int apply_no_new_privs(void);` — `prctl(PR_SET_NO_NEW_PRIVS, 1, ...)`. Returns `0`/`-1`.

- [ ] **Step 1: Replace the stubs in `caps.c`**

```c
int apply_capabilities(uint64_t keep_mask) {
    for (int c = 0; c < 64; c++) {
        if (keep_mask & ((uint64_t)1 << c)) continue;
        if (prctl(PR_CAPBSET_DROP, c, 0, 0, 0) != 0) {
            if (errno == EINVAL) break;   /* past running kernel's max cap */
            return -1;
        }
    }
    struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct data[2];
    memset(data, 0, sizeof(data));
    data[0].permitted = data[0].effective = data[0].inheritable =
        (uint32_t)(keep_mask & 0xffffffffu);
    data[1].permitted = data[1].effective = data[1].inheritable =
        (uint32_t)(keep_mask >> 32);
    if (syscall(SYS_capset, &hdr, data) != 0) return -1;
    return 0;
}

int apply_no_new_privs(void) {
    return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 ? 0 : -1;
}
```

- [ ] **Step 2: Verify the unit still compiles and the parse test still passes**

Run: `gcc -Wall -o /tmp/test_cap_parse tests/test_cap_parse.c caps.c && /tmp/test_cap_parse`
Expected: PASS (the apply_* functions are compiled but not exercised by this test; the boot-rail test in Task 5 exercises them under real PID 1 as root).

- [ ] **Step 3: Root smoke check (optional, confirms no crash as root)**

Run:
```bash
cat > /tmp/caps_smoke.c <<'EOF'
#include "caps.h"
#include <stdio.h>
#include <linux/capability.h>
int main(void){
    if (apply_capabilities((uint64_t)1 << CAP_NET_BIND_SERVICE) != 0) { perror("caps"); return 1; }
    if (apply_no_new_privs() != 0) { perror("nnp"); return 1; }
    printf("applied ok\n"); return 0;
}
EOF
gcc -I. -o /tmp/caps_smoke /tmp/caps_smoke.c caps.c && sudo /tmp/caps_smoke
```
Expected: prints `applied ok` (a non-root run may fail `capset` with `EPERM` — that is expected; the real path always runs as root pre-setuid).

- [ ] **Step 4: Commit**

```bash
git add caps.c
git commit -m "caps: implement bounding-set drop, capset, and no_new_privs"
```

---

### Task 3: Wire the `.svc` fields and the child call site into `service.c`

**Files:**
- Modify: `service.h` (struct fields, flag, prototype)
- Modify: `service.c` (`#include`, field handlers, `service_apply_hardening`, call site)
- Modify: `Makefile` (add `caps.o`)

**Interfaces:**
- Consumes: `parse_cap_list`, `apply_capabilities`, `apply_no_new_privs` from `caps.h`.
- Produces: `int service_apply_hardening(const service_t *svc);` — applies requested hardening in the child; returns `0`, or `-1` after logging (caller `_exit(126)`).

- [ ] **Step 1: Add struct fields, flag, and prototype to `service.h`**

Add the flag beside the existing `SVC_*` defines:

```c
#define SVC_NO_NEW_PRIVS   (1 << 7)  /* prctl(PR_SET_NO_NEW_PRIVS) in child   */
```

Add to `service_t` (near `content_hash`):

```c
uint64_t         cap_keep_mask;    /* keep_caps allowlist; bit N = CAP_N       */
uint8_t          cap_restrict;     /* 1 if keep_caps= was present in the .svc  */
```

Add the prototype beside the other service function decls:

```c
/* apply opt-in hardening in the child, before setuid/execv; -1 -> fail closed */
int service_apply_hardening(const service_t *svc);
```

- [ ] **Step 2: Include `caps.h` and add the field handlers in `service_load_one`**

At the top of `service.c` with the other includes:

```c
#include "caps.h"
```

In the `service_load_one` field chain (after the `needs_root` handler is a natural spot):

```c
else if (strcmp(line, "no_new_privs") == 0 && atoi(val))
    svc->flags |= SVC_NO_NEW_PRIVS;
else if (strcmp(line, "keep_caps") == 0) {
    if (parse_cap_list(val, &svc->cap_keep_mask) != 0) {
        fprintf(stderr, "[schema-init] %s: unknown capability in keep_caps=%s\n",
                svc->name, val);
        return -1;
    }
    svc->cap_restrict = 1;
}
```

- [ ] **Step 3: Define `service_apply_hardening` in `service.c`**

Place it with the other `service_*` definitions (e.g. just above `service_spawn`):

```c
int service_apply_hardening(const service_t *svc) {
    if (svc->cap_restrict) {
        if (apply_capabilities(svc->cap_keep_mask) != 0) {
            dprintf(2, "[schema-init] HARDENING FAILED for %s: capabilities: %d\n",
                    svc->name, errno);
            return -1;
        }
    }
    if (svc->flags & SVC_NO_NEW_PRIVS) {
        if (apply_no_new_privs() != 0) {
            dprintf(2, "[schema-init] HARDENING FAILED for %s: no_new_privs: %d\n",
                    svc->name, errno);
            return -1;
        }
    }
    return 0;
}
```

- [ ] **Step 4: Add the call site in the child, before the uid-drop block**

In `service_spawn`, in the child branch, immediately **before** the `if (svc->run_uid) {` block (currently ~line 411):

```c
if (service_apply_hardening(svc) != 0)
    _exit(126);
```

- [ ] **Step 5: Add `caps.c` to the Makefile**

The Makefile derives objects from `SRCS` (`OBJS = $(SRCS:.c=.o)`) with a generic `%.o: %.c` rule and also lists `$(SRCS)` directly in the `schema-init-static` target. So the only change is to append `caps.c` to the `SRCS` line (line 24) — both the dynamic and static builds pick it up automatically:

```make
SRCS    = init.c schema.c service.c group.c caps.c
```

No explicit `caps.o:` rule is needed — the `%.o: %.c` pattern handles it. (Header changes to `caps.h` won't auto-trigger a rebuild, but `make clean && make` in Step 6 covers that.)

- [ ] **Step 6: Build the whole tree**

Run: `make clean && make`
Expected: clean build, no warnings on `caps.c`/`service.c`, `schema-init` binary produced.

- [ ] **Step 7: Commit**

```bash
git add service.h service.c Makefile
git commit -m "service: opt-in no_new_privs and keep_caps hardening in child"
```

---

### Task 4: Boot-rail proof (vmtest) and PR

**Files:**
- `~/schema-livetest/test-hardened.svc` (harness, outside repo)
- `~/schema-livetest/` reporter/assertion additions (outside repo)

**Interfaces:**
- Consumes: the built branch binary via the vmtest harness.
- Produces: a green vmtest run that asserts the child actually received `NoNewPrivs: 1` and the reduced `CapBnd` under a real `rdinit=/sbin/schema-init` boot.

- [ ] **Step 1: Add the hardened test service to the harness**

Create `~/schema-livetest/test-hardened.svc`:

```
name=test-hardened
exec=/bin/test-hardened.sh
no_new_privs=1
keep_caps=CAP_NET_BIND_SERVICE
```

Create the payload `~/schema-livetest/test-hardened.sh` (packed into the initramfs at `/bin/`, made executable), which prints its own status to the console and stays up:

```sh
#!/bin/sh
echo "test-hardened $(grep NoNewPrivs /proc/self/status) $(grep CapBnd /proc/self/status)" > /dev/console
while :; do sleep 60; done
```

- [ ] **Step 2: Add the two assertions to the vmtest verdict**

Extend the harness PASS check so the serial log must additionally contain:
- `NoNewPrivs:	1` for the `test-hardened` line, and
- `CapBnd:	0000000000000400` (the single-bit mask for `CAP_NET_BIND_SERVICE`, value 10 → `1 << 10` = `0x400`) on that same line.

- [ ] **Step 3: Run vmtest on the branch**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS`, and the serial log shows `test-hardened NoNewPrivs:\t1 CapBnd:\t0000000000000400`. This proves parse → capbset-drop → capset → no_new_privs all compute and apply correctly under real PID 1 as root — the root-staying capset path, which is the one that most needs proving.

- [ ] **Step 4: If FAIL — do not proceed**

Read `~/schema-livetest/last-vmtest-serial.log`, find whether the service failed to spawn (`_exit(126)` → hardening error) or the `CapBnd` value is wrong (bit-math bug in `parse_cap_list`/`apply_capabilities`), fix on the branch, re-run. Do not open the PR until green.

- [ ] **Step 5: Open the PR**

```bash
git push -u origin <branch>
gh pr create --title "service: opt-in no_new_privs + keep_caps hardening (Phase 1)" \
  --body "Phase 1 of secure-by-default service model. Opt-in no_new_privs and keep_caps
capability allowlist, applied in the child before setuid/execv. Fail-closed. No libcap.
vmtest green: test-hardened shows NoNewPrivs:1 CapBnd:0x400. Spec + plan in docs/superpowers.
Note: vmtest harness change (test-hardened.svc + assertions) lives in ~/schema-livetest, not this repo.
Default-flip to opt-out is a separate follow-up PR after the live pilot."
```

---

### Task 5: Live pilot on blakbox (manual verification — no repo commit)

**Files:** one existing pilot `.svc` on the live box (`chrony` or `avahi`), edited for the test only.

- [ ] **Step 1: Pick a pilot and its true capability needs**

`chrony`: needs `CAP_SYS_TIME` (step the clock) and, if it binds 123, `CAP_NET_BIND_SERVICE` — but 123 is >1024 so binding does not need the cap; confirm against the running config. `avahi`: `CAP_NET_BIND_SERVICE` covers mDNS on 5353 only if <1024 (it is not), so avahi may need **no** caps → `keep_caps=`. Verify each daemon's actual needs before editing.

- [ ] **Step 2: Harden the pilot and reload**

Add `no_new_privs=1` and the verified `keep_caps=` line to the pilot's live `.svc`, then reload per the deploy rules (`schema-ctl` reload; do not restart PID 1).

- [ ] **Step 3: Verify the drop applied and the daemon still works**

Run: `grep -E 'NoNewPrivs|CapBnd' /proc/$(pgrep -x <daemon>)/status`
Expected: `NoNewPrivs:	1` and a reduced `CapBnd` matching the kept set. Then confirm function (chrony still tracks; avahi still resolves `.local`).

- [ ] **Step 4: Decide whether to keep the pilot hardened**

If keeping it: commit the pilot `.svc` change on the branch/master. If reverting: restore the original `.svc` and reload. Either way, the pilot result gates the Phase 2 default-flip PR.

---

## Self-Review

**Spec coverage:**
- `no_new_privs` field → Task 3 Step 2 (parse) + Task 1/2 (`apply_no_new_privs`) + Task 3 Step 3 (apply). ✓
- `keep_caps` allowlist + unknown-name load error → Task 3 Step 2. ✓
- `cap_keep_mask` / `cap_restrict` / `SVC_NO_NEW_PRIVS` struct additions → Task 3 Step 1. ✓
- Hand-rolled `capset` v3, no libcap → Task 2 Step 1. ✓
- `PR_CAPBSET_DROP` `EINVAL` = boundary → Task 2 Step 1. ✓
- Fail-closed `_exit(126)` + async-signal-safe `dprintf` → Task 3 Steps 3–4. ✓
- Call site before setuid, no_new_privs last → Task 3 Step 4 (before uid block) + `service_apply_hardening` order (caps then nnp). ✓
- Ambient-cap out-of-scope → reflected in Task 5 Step 1 pilot-need analysis; no code path claims post-setuid retention. ✓
- vmtest proof (NoNewPrivs + CapBnd) → Task 4. ✓
- Live pilot before default-flip → Task 5. ✓
- Default-flip deferred to its own PR → not in this plan (correct). ✓

**Placeholder scan:** No TBD/TODO; every code step has concrete code. ✓

**Type consistency:** `parse_cap_list(const char*, uint64_t*)`, `apply_capabilities(uint64_t)`, `apply_no_new_privs(void)`, `service_apply_hardening(const service_t*)`, `cap_keep_mask`/`cap_restrict`, `SVC_NO_NEW_PRIVS` used identically across Tasks 1–4. ✓
