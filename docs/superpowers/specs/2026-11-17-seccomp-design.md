# Audit 4.6: syscall_filter= and the seccomp family

## Intent
No seccomp anywhere today. On blakbox 52 units carry `SystemCallFilter=`
(nearly all `@system-service`, often with `~@privileged @resources` or
`~@mount` after it), and the seccomp-based siblings are just as common:
RestrictRealtime 69, LockPersonality 59, SystemCallArchitectures=native
50, RestrictNamespaces 39, RestrictSUIDSGID 35. The importer only warns
"not enforced" for all of them. Static PID 1, so no libseccomp: a
hand-built BPF program.

## Goals
- `syscall_filter=ENTRY ...`: entries are syscall names or `@group`, each
  optionally `:ERRNO` (name like EPERM or a number). The first line decides
  the mode: a plain list is an allowlist, a list starting with `~` a
  denylist. Later lines add to the same set; in allowlist mode a later `~`
  line removes from it, in denylist mode a later plain line removes from the
  denylist (systemd's rules). An empty value clears (drop-in convention).
- `syscall_error_number=ERRNO`: what a blocked syscall returns. Unset, a
  blocked syscall kills the process (SECCOMP_RET_KILL_PROCESS), as in
  systemd. A per-entry `:ERRNO` wins for that entry.
- `syscall_architectures=native`: syscalls from any other ABI kill the
  process (on x86_64 this includes x32 and i386 compat calls).
- `restrict_suid_sgid=1`: chmod/fchmod/fchmodat/fchmodat2 with S_ISUID or
  S_ISGID in the mode, open/openat/creat with O_CREAT and such a mode, and
  mknod/mknodat of such a file return EPERM; openat2 returns ENOSYS (its
  mode is behind a pointer), as systemd does.
- `lock_personality=1`: personality() only with PER_LINUX or the query
  value 0xffffffff, else EPERM.
- `restrict_realtime=1`: sched_setscheduler with SCHED_FIFO, SCHED_RR or
  SCHED_DEADLINE returns EPERM; sched_setattr returns EPERM (its policy is
  behind a pointer). Our own `sched_policy=` is applied before the filter.
- `restrict_namespaces=1`: clone/unshare with any CLONE_NEW* flag and setns
  return EPERM; clone3 returns ENOSYS so libc falls back to clone, whose
  flags can be checked (systemd's approach). Only yes/no.
- Any of these keys implies `no_new_privs=1` unless the process still holds
  CAP_SYS_ADMIN at install time (systemd's rule: root services that keep
  their caps don't need it).
- Installed as the very last step before execve, in the main child and in
  the stop/reload runners (systemd filters ExecStop= too); execve and
  execveat are always allowed, so the exec itself can't trip the filter.
- `schema-ctl status <svc>` shows which seccomp keys are on and the size of
  the allow/deny set.
- Importer maps SystemCallFilter=, SystemCallErrorNumber=,
  SystemCallArchitectures=native, RestrictSUIDSGID=, LockPersonality=,
  RestrictRealtime=, RestrictNamespaces=yes/no; other values of the last
  (lists of namespace types) and other architectures stay warned.

## Mechanism
- `tools/gen-seccomp-tables.sh` turns `systemd-analyze syscall-filter`
  into `seccomp_groups.h` (each group's members, `@refs` kept, the systemd
  version in a comment) and `seccomp_names.h`: every name of `@known`
  wrapped `#ifdef __NR_<name> { "<name>", __NR_<name> }, #endif`, so each
  architecture's build picks its own numbers from the kernel headers and
  drops what it doesn't have. The headers are committed; the script is
  re-run by hand when systemd grows a syscall.
- service_t holds the filter compactly: mode, a bitset over the name table
  (~540 bits), up to 32 per-entry errno overrides, the default errno and
  the sibling flags.
- `seccomp.c` (pure, no service_t) builds the program in the child:
  1. load arch; not the native AUDIT_ARCH → kill (or allow, when
     syscall_architectures is unset); on x86_64 reject the x32 bit.
  2. load nr; the sibling checks (personality, sched_setscheduler, clone,
     unshare, setns, clone3, chmod family, open family, mknod) as small
     argument-inspecting blocks.
  3. the filter set as a balanced binary search tree over sorted syscall
     numbers (~9 compares for 400 entries instead of a linear chain), each
     leaf returning ALLOW or the entry's errno/kill.
  Arguments are checked on their low 32 bits (modes and flags fit there).
  Limit: the kernel's 4096 instructions; the builder refuses past it.
- Installed with prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER) (no libc
  wrapper needed; `seccomp()` with TSYNC is unnecessary, the child is
  single-threaded).

## Non-goals
- Other architectures in `syscall_architectures=` (only `native`).
- RestrictNamespaces= lists of types; RestrictAddressFamilies=,
  SystemCallLog=, SocketBindAllow= (landlock_bind_tcp covers part of it).
- MemoryDenyWriteExecute stays on prctl (4.5).

## Approaches
1. Generated tables + compile-time numbers + BPF tree (picked: no runtime
   dependency, right numbers on every arch, fast filters).
2. Hand-written per-arch number tables: rejected, wrong numbers are the
   classic seccomp bug.
3. Linear jeq chain: rejected, a 400-entry allowlist would cost up to 400
   instructions on every syscall of the service.

## Tests
- C, unprivileged (no_new_privs + filter in forked children): allowlist
  lets listed calls through and returns EPERM/kills on others; denylist
  the inverse; `:ERRNO` per entry; groups expand recursively and `~`
  lines remove; x32 refused (x86_64 only); each sibling's allowed and
  refused cases (chmod 04755 refused, 0755 allowed; personality(PER_LINUX)
  ok, PER_BSD refused; unshare(CLONE_NEWNS) refused, fork ok;
  sched_setscheduler FIFO refused, OTHER ok); the instruction cap.
- Parse: modes, removal rules, drop-in clear, unknown name refused,
  implied no_new_privs.
- Live: a service with `syscall_filter=@system-service` +
  `~@privileged @resources` runs; one calling a denied syscall gets EPERM
  or is killed; blakbox PID 1 with one imported real unit (chronyd shape).
- Importer: each key, the blakbox census (52 SystemCallFilter units
  translate).
