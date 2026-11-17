# Audit 4.5 part 1: kernel-surface mount-namespace knobs

## Intent
Imported systemd units that ask for ProtectKernelTunables=, ProtectKernelModules=,
ProtectControlGroups=, PrivateDevices= or ProtectHostname= today lose that
confinement with an "unenforced" warning. Give each a native .svc key so the
sandbox survives the import.

## Goals
- `protect_kernel_tunables=1`: read-only binds of /proc/sys, /sys,
  /proc/sysrq-trigger, /proc/latency_stats, /proc/acpi, /proc/apm, /proc/asound,
  /proc/bus, /proc/fs, /proc/irq, /proc/scsi (missing ones skipped).
- `protect_kernel_modules=1`: CAP_SYS_MODULE dropped from the bounding set
  (with or without keep_caps); /usr/lib/modules and /lib/modules hidden.
- `protect_control_groups=1`: read-only /sys/fs/cgroup.
- `private_devices=1`: tmpfs /dev (nosuid, noexec, mode 0755) holding null,
  zero, full, random, urandom, tty bound from the host; a new devpts instance
  with /dev/ptmx -> pts/ptmx; /dev/shm, /dev/mqueue, /dev/log bound from the
  host; stdin/stdout/stderr/fd symlinks. CAP_MKNOD and CAP_SYS_RAWIO dropped
  from the bounding set.
- `protect_hostname=1`: CLONE_NEWUTS.
- Importer maps the five directives natively; schema-user strips them.
- `schema-ctl status` shows them.

## Non-goals
- `device_allow=` / DevicePolicy=: needs a cgroup device BPF program, a new
  mechanism. Its own later item.
- private_network, restrict_suid_sgid, memory_deny_write_execute, umask: part 2.
- Joining the hardening default: these stay opt-in.

## Constraints
- All in the forked child inside apply_mount_ns, before caps drop; no new deps.
- An explicit knob that hides the service's exec or ready_path refuses the
  load (ns_hides, as today).

## Approaches
1. Extend apply_mount_ns with a flags word for the new knobs (picked: one
   mount pass, one failure path, one `step` name per knob).
2. A separate apply_kernel_ns after apply_mount_ns: a second unshare path and
   duplicate propagation setup for no gain.
3. mknod device nodes in the private /dev (systemd's way): needs a tmpfs
   without nodev; binding host nodes onto empty files needs nothing new and
   keeps host permissions.

## Known limit
/dev/log is bound by inode. When schema-journal-sink restarts it unlinks and
rebinds /dev/log, so a private_devices service's syslog() goes nowhere until
the service restarts (stdout/stderr still reach its log). The fix (socket in
/run, /dev/log a symlink) touches journal-sink and waits.

## Left to the builder
Exact flag layout in service_t, test harness shape (unshare-based tests as in
the existing ns tests).
