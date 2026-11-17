# Audit 4.5 part 2: private_network, memory_deny_write_execute, umask

## Intent
Same as part 1: units asking for PrivateNetwork=, MemoryDenyWriteExecute= or
UMask= keep that under schema-init instead of an "unenforced" warning.

## Goals
- `private_network=1`: CLONE_NEWNET, loopback up, nothing else. Fds PID 1
  passes in (socket activation) and path-based unix sockets keep working.
- `memory_deny_write_execute=1`: prctl(PR_SET_MDWE, PR_MDWE_REFUSE_EXEC_GAIN)
  last in service_apply_hardening. A kernel without it (< 6.3) fails the
  spawn: HARDENING FAILED, exit 126. Decided with Jonathan: an opt-in
  protection that silently isn't there defeats the purpose.
- `umask=` 0–0777 octal: set in the service child and in the stop/reload
  runner child, so exec_pre, exec and exec_stop*/exec_reload all get it.
  Unset keeps PID 1's 0022.
- Importer maps PrivateNetwork=, MemoryDenyWriteExecute=, UMask= (a line
  only when it asks for something: UMask=0022 emits nothing).
- schema-user: private_network dropped with the mount-ns warning (needs
  CAP_SYS_ADMIN); memory_deny_write_execute and umask kept (unprivileged).

## Non-goals
- `restrict_suid_sgid`: systemd's blocks creating setuid/setgid files, which
  needs a syscall filter (Landlock has no such right; a nosuid remount only
  stops running them, which no_new_privs covers). Moves to 4.6.
- Hardening default: unchanged, all opt-in.
- /sys under private_network still lists host interfaces (no sysfs remount).
- Abstract unix sockets do not cross the network namespace (documented).

## Approaches
1. New bits in ns_kernel (NS_K_NETWORK, NS_K_MDWE), applied in
   service_apply_hardening outside apply_mount_ns, which gets only the
   mount bits (picked: one parse table, one status table).
2. Separate service_t fields per knob: more parse code, same behaviour.
3. memory_deny_write_execute via a seccomp filter (systemd's fallback): that
   is 4.6's machinery; refusing on old kernels covers the gap.

## Also
Part 1's deferred knob-name duplication (service.c / engine.c) is fixed here,
since part 2 adds two names to both tables: one `ns_kernel_names[]` in ns.c.
