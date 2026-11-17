# Kernel-surface part 2 (audit 4.5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** private_network=, memory_deny_write_execute=, umask= as .svc keys, mapped by the importer.

**Architecture:** NS_K_NETWORK 0x20 and NS_K_MDWE 0x40 join ns_kernel; `ns_kernel_names[]` in ns.c drives parsing and status. service_apply_hardening: private_net() before apply_mount_ns (which gets `ns_kernel & NS_K_MOUNT_BITS`), MDWE last. `int16_t umask_val` (-1 unset) applied in the spawn child and svc_runner_child.

**Spec:** `docs/superpowers/specs/2026-11-16-kernel-surface-2-design.md`

## Global Constraints
- MDWE unavailable → spawn fails (exit 126), never runs unprotected.
- No new deps; static PID 1.
- Importer: a line only when the unit asks for something.

## Review Focus
1. A socket-activated service with private_network=1 still accepts on its passed fd. Test in Task 2 (listening socket made before unshare, connect from parent).
2. MDWE on the main exec, not just the child: test execs a helper that tries an RWX mmap (Task 2).
3. umask reaches exec_stop runners, not only exec: test in Task 3 via service_stop_spawn writing a file.
4. umask=0 (valid, 0000) distinguished from unset. Test in Task 1.
5. User-mode import keeps umask/MDWE, drops private_network. Test in Task 4.

---

### Task 1: parse + name table
**Files:** ns.h/ns.c (`NS_K_NETWORK 0x20`, `NS_K_MDWE 0x40`, `NS_K_MOUNT_BITS 0x1f`, `extern const char *const ns_kernel_names[7]` indexed by bit), service.h (`int16_t umask_val`), service.c (parse_ns_field uses the table; `umask` key parse: 1–4 octal digits, ≤0777, else load error; default -1 in the service init), engine.c status uses the table and prints `umask=%04o` when set.
- [ ] Tests (test_mountns_parse.c): private_network=1 → NS_K_NETWORK; memory_deny_write_execute=1 → NS_K_MDWE; umask=0027 → 027; umask=0 → 0; unset → -1; umask=0888, umask=1777, umask=abc, umask= → load error. RED, implement, GREEN, commit.

### Task 2: network + MDWE in the child
**Files:** ns.c/ns.h (`int private_net(void)`: unshare(CLONE_NEWNET), SIOCGIFFLAGS/SIOCSIFFLAGS IFF_UP on "lo"; `int apply_mdwe(void)`: the prctl, defines if headers lack them), service.c service_apply_hardening (`HARDENING FAILED for %s: private_network: %d` / `memory_deny_write_execute: %d`).
- [ ] Tests (test_kernel_ns.c): private_net → getifaddrs lists only "lo", IFF_UP, bind+connect on 127.0.0.1 works; a TCP listener opened before private_net still accepts a connect from the parent; apply_mdwe then execve(/proc/self/exe, "rwx-probe") → probe's RWX mmap fails (MAP_FAILED). RED, implement, GREEN (userns + sudo), commit.

### Task 3: umask in children
**Files:** service.c (spawn child after svc_child_log; svc_runner_child start).
- [ ] Tests (test_svc_stopcmds.c or new case in test_mountns_parse.c): svc with umask=0077 and exec_stop=/bin/sh -c "touch $DIR/f"; service_stop_spawn, wait, stat mode == 0600; same svc main spawn path covered live. RED, implement, GREEN, commit.

### Task 4: importer, user mode, docs
**Files:** schema-import.py (_hardening: PrivateNetwork/MemoryDenyWriteExecute → key=1 when true; UMask → `umask=%04o` when valid and != 0022, bad value warns; remove the three from _UNENFORCED and UMask from _UNENFORCED_DEFAULT; `private_network=` into _USER_NO_HARDEN), docs/svc-reference.md rows (+ MDWE refuses on kernels < 6.3, abstract sockets, /sys), audit spec 4.6 gains restrict_suid_sgid.
- [ ] Tests: import_units: each directive maps; UMask=0022 → no line; UMask=0077 → umask=0077; UMask=bogus → warn, no line; import_user: PrivateNetwork dropped with warning, UMask/MemoryDenyWriteExecute kept. RED, implement, GREEN, make test + all Python scripts, commit.

### Task 5: draft PR, /code-review, live on blakbox PID 1, merge on his go (he said "merge when ready" for part 1 only).
