# Threat model

What schema-init defends, against whom, where the trust boundaries are, and what
holds them. Written against master `1431e6f` (2026-11-25). When a boundary below
changes, this file changes in the same PR (see the checklist at the end).

schema-init is PID 1 and also replaces logind, udev and the D-Bus broker. A bug in
any of them can hand out root, a device, or the power button. Its users are
desktops and small servers; none of it is meant to face the network.

## Assets

1. **Root on the box**: code execution as uid 0, or with capabilities.
2. **Devices**: input (keyloggers), DRM/GPU, sound, cameras and USB, which logind
   and udev grant to the active session.
3. **Power and session control**: reboot, poweroff, session lock/unlock, linger,
   and hostname/time/locale setters. (There is no suspend or hibernate:
   `CanSuspend` answers `na`, by design.)
4. **Service integrity**: which services run, how, as whom and with what sandbox,
   and that a service can't be killed, frozen or impersonated by someone else.
5. **The next boot**: that what verify says about the next boot holds, and that a
   bad deploy falls back to a snapshot (the boot-success guard).
6. **Availability of PID 1**: a hung or crashed PID 1 is a hung or panicked box.

## Attackers

| | Starts with | Typical example |
|---|---|---|
| **A1** | a local unprivileged account, logged in (not necessarily at the console) | an ssh user; a second desktop user |
| **A2** | code running as a service's user, or as root with a reduced capability set | a compromised daemon; a buggy service |
| **A3** | a lingering user with no session | background jobs left behind after logout |
| **A4** | a crafted **boot entry or root filesystem** that verify is asked to inspect | a snapshot entry, or a foreign root on another disk |
| **A5** | a crafted **unit file** in any installed package | a systemd unit from a third-party RPM: the package's file trigger imports it as root with no admin step |
| **A6** | physical access to the console | out of scope beyond the session rules below |

Out of scope: an attacker who already has root and holds no boundary the project
claims; the network (nothing here listens, see below); bugs in the software
schema-init launches; the kernel.

## Components and trust boundaries

| Component | Runs as | Untrusted input | Channel |
|---|---|---|---|
| PID 1 (`init.c`, `engine.c`, `service.c`) | root | control commands (A1 in group `schema`, A2); notify messages and passed fds (any local user) | `/run/schema-init.sock`, root:schema 0660 when a `schema` group exists, else 0600; `/run/schema-init/notify` 0666 datagram, `SO_PASSCRED`; `/dev/shm/schema-init` 0644 (read-only export) |
| PID 1's `.svc` loader | root | `.svc` files and drop-ins | the filesystem: each file must be root-owned and not group/world-writable, or it is refused |
| `schema-ctl verify` → `verify_root.c` | root | another root filesystem (A4): the kernel mounts it; its `init=` runs; its `--check` report is parsed | mount; the #590 sandbox; the report pipe |
| `schema-logind.py` (login1, ConsoleKit, hostname1, timedate1, locale1) | root | D-Bus method calls (A1, A2, A3) | the system bus |
| `schema-systemd1.py` (org.freedesktop.systemd1 bridge) | root | D-Bus method calls (A1, A2), including `AttachProcessesToUnit` | the system bus; `/run/systemd/private` 0600 (root only, no polkit) |
| `schema-dbus` (bus broker) | root | every message from every peer (A1, A2, A3); activation requests by bus name | `/run/dbus/system_bus_socket` 0777; policy decides who may send what |
| `schema-journal-sink` | root | log lines and passed fds from any local user (A1, A2) | `/dev/log`, `/run/systemd/journal/socket` and `stdout`, all 0666 |
| `schema-recorder` | root | every process's `/proc` strings; coredump records | its socket is 0600; it publishes a 0644 shm |
| `schema-udev` | root | kernel uevents; rules files | netlink group 1, sender pid 0 only (`schema-udev.c:59`); rules from root-owned dirs |
| `schema-coredump` | root, as `core_pattern` pipe | a crashing process's `/proc/<pid>/comm`, `exe`, `cmdline` and `cgroup` (A1, A2); the core itself is streamed into zstd unparsed | the kernel pipe |
| `schema-user` | the user | that user's own `.svc` files | `$XDG_RUNTIME_DIR/schema-user/services` |
| `schema-import` | root: run by an admin, **and by RPM file triggers** whenever any package installs a unit or a D-Bus service file | systemd unit files and D-Bus `.service` files (A5) | files on disk |
| passwordless sudo grants | root | argv to `schema-flip-apply` (ISO, the login user, 20 subcommands) and `schema-migrate` (`%wheel`, 10 flags), plus the state they read | `/etc/sudoers.d/schema-flip`, `/etc/sudoers.d/schema-wizard` |
| socket activation (`listen=`) | root binds, hands off | none read by PID 1 | the admin chooses the port; the fd goes to the service unread |

**No network listeners.** PID 1, logind, udev and the broker open no INET socket
of their own: the `AF_INET` sockets in `ns.c` and `schema-udev.c` are interface
ioctl handles. `listen=` binds what the admin configured and passes it on.

## Threats and what holds them

### PID 1 control socket (A1, A2)

- **Who may connect:** root:schema 0660 when a `schema` group exists, else root
  only (0600). The uid comes from `SO_PEERCRED`, never from the message.
- **Who may change things:** root (and PID 1's own euid) run every verb; members
  of `schema` get read-only verbs only (`engine.c` peer check before `ctl_cmd`).
- **Freezing the supervisor:** a client that asks for output and never reads it
  used to block PID 1 on a full send buffer. Each reply is now built in a buffer
  and sent with `MSG_DONTWAIT`, with a 250 ms deadline per command line, so PID 1
  never waits on a client that doesn't read (#479).
- **Residual:** anyone in `schema` can read every service's state and command
  line. And **every** local user can read service names, states and pids from
  PID 1's `/dev/shm/schema-init` export (0644). Treat the service list as public
  on the box.

### Notify socket (any local user)

- **Impersonating a service:** the sender's pid from `SCM_CREDENTIALS` must
  resolve to a service's main pid or cgroup (`MAINPID=` checks the cgroup). A
  message without credentials is dropped.
- **Passed fds:** anyone may send fds. PID 1 never `close()`s one on its own
  thread. A FUSE fd whose release blocks would otherwise hang PID 1 forever. A
  bounded set of closer children does the close (#390, #393, #394), and reads
  pause when too many are parked.
- **FDSTORE:** only from the service itself, capped per service and in total.
- **Residual:** the socket is a cheap way to make PID 1 do a little work per
  datagram; there is no per-uid rate limit.

### `.svc` files and the loader (A2, A5)

- **Trust, per file:** a `.svc` must be a regular file owned by root (or the
  supervisor's euid) and not group- or world-writable
  (`service_open_trusted`). Otherwise `REFUSED`. A `.svc` symlink is followed
  (no `O_NOFOLLOW`), and only the target's owner and mode decide.
- **Trust, per directory:** a service directory, or a parent, that someone
  other than root could write gets a WARN and still loads; only `schema-ctl
  reload` is refused there (`service.c` `services_load`). `dir_trusted` is a
  different check: it governs symlinks PID 1 meets while creating state, runtime
  and listen directories for a service.
- **Sandbox loss on import:** `schema-import` maps systemd sandboxing it
  understands and **warns** about what it drops (`RemoveIPC=`,
  `CapabilityBoundingSet=`, `RestrictAddressFamilies=` and others; the list is
  `_UNENFORCED` in `schema-import.py`). An explicit `RestrictSUIDSGID=` maps to
  `restrict_suid_sgid=1`. `DynamicUser=` maps to `dynamic_user=1`, an allocated
  uid (#540), with a warning that the two settings it implies, RemoveIPC and
  RestrictSUIDSGID, are not added. Secrets are no longer copied 0600 → 0644 (#477).
- **`exec=`:** runs through `execv`, with no PATH search. A relative path is a
  warning today and verify shows it as a WARN; a later release refuses it (#596).
- **Residual:** the RPM file trigger imports every unit a package ships, as root,
  with no admin step. A package that ships a hostile unit gets a service doing
  what the unit says, as it would under systemd. The importer is a translator,
  not a vetting tool.

### `schema-ctl verify` and the boot entries it inspects (A4)

The boundary normal boot doesn't have: verify inspects entries the operator may
never boot, so their `init=` is untrusted.

- The target's root is mounted read-only in a private mount namespace. The
  target's `init=` runs **sandboxed** (#590): user, mount, pid, net, ipc, uts and
  cgroup namespaces; chroot into the read-only root; uid 1 (host 2147483646, which
  owns nothing) with only `CAP_DAC_OVERRIDE` and `CAP_DAC_READ_SEARCH`, both
  scoped to the user namespace; a private `/dev` with 5 nodes; `/proc` with
  `subset=pid`; no_new_privs; caps on file size, process count and address space.
- If the sandbox can't be built, the result is UNRESOLVED; the binary never runs
  any other way.
- A 30 s deadline kills the whole process group, which the sandbox stays in.
- Outside the sandbox, root parses the target's `--check` report, as text with
  bounded size, and `stat`s the `schema-bootok.exec=` path it names, resolved
  inside the target.
- **Residual:** the target can still read anything root can read on its own
  read-only root. That is the point of the inspection, and nothing leaves except
  the report. The kernel's filesystem code mounts a possibly crafted image,
  read-only, with log replay refused. A filesystem driver bug is the kernel's,
  but it is reachable from here.

### schema-logind (A1, A2, A3)

- **Who counts as "the user":** root; for a per-session method, that session's
  own root-written owner uid, even on a background VT, as with real logind;
  otherwise the uid of the active local session from root-written session files (the session on the foreground VT, else the
  single active local one, else nobody). A lingering user with no session (A3)
  is nobody unless the admin opts in with `/etc/schema-init/logind-run-user-authz`
  (#583).
- **State-changing methods fail closed on the same model.** The 15 methods
  gated in #470 (hostname/time/locale setters, Lock/Unlock, ReleaseDevice and
  others) call `authz_check` first. TakeDevice, TakeControl, PowerOff, Reboot
  and ConsoleKit Restart/Stop call `caller_authorized()` directly, the same rule,
  older code, and so does `Seat.SwitchTo`, which changes the active VT.
  `CreateSession` is root only (PAM's uid). `SetUserLinger` has its own
  `linger_allowed()` rule.
- **Ungated because empty today:** timedate1 `SetTime`, `SetNTP`, `SetLocalRTC`;
  `TerminateSession/User/Seat`, `ScheduleShutdown`, `SetWallMessage`, `Inhibit`,
  `Session.SetType`. They do nothing. **Giving any of them a body means adding the
  gate in the same change.**
- **Bus policy:** each extra name (hostname1, timedate1, locale1, ConsoleKit)
  has its own connection, so their blanket `send_destination` allows don't open
  login1's methods at the bus layer (#572).
- **No polkit:** schema-logind decides in the daemon, as above. Anything polkit
  would have allowed to a non-session uid is denied.
- **Residual:** the active local session's uid has what real logind gives it:
  devices and the power button. A process running as that uid has them too. A
  second user's background session can use its own session's TakeDevice,
  TakeControl and Lock without being in front, as with real logind.

### schema-systemd1 bridge (A1, A2)

- `AttachProcessesToUnit` writes pids into cgroups as root for non-root callers;
  `attach_validate` lets a caller move only processes wholly its own, into its
  own `user@` unit's slice. The 0600 private socket (`/run/systemd/private`) is
  root's by its file mode and skips polkit.
- Unit-changing methods (Start/Stop/Restart/Reload…Unit and friends) are allowed
  to root. For any other caller the bridge asks polkit's `CheckAuthorization`,
  as systemd does inside its daemon, because the stock bus policy lets anyone
  send them. Any error asking (no uid, polkit unavailable or timing out) is a
  refusal (#469). Interactive authentication only when the client asked for it.
- **Residual:** whatever the box's polkit rules allow, the bridge allows.

### schema-dbus broker (A1, A2, A3)

- **Malformed messages:** validated before forwarding, and header field types
  enforced (#179). Fuzzed in CI on every PR (#318, `tests/fuzz`).
- **Resource pinning:** pre-auth floods and match rules (#180), per-connection
  inbound buffering (#472), and 512 names per connection (ac11c28) are capped.
  **Pending replies have no count cap**; they are reaped after 120 s. A peer
  can pin that many until the reaper runs.
- **Policy:** the dbus-daemon policy files decide who may send what. CI checks
  the broker's matcher against a synthetic policy with verdicts from an
  independent oracle (`tests/fixtures/dbus`). The real-corpus comparison, `make
  verify-dbus-conformance`, runs locally only.
- **Activation:** any peer the policy lets send to an activatable name makes
  the broker (or PID 1) start that service. That is D-Bus semantics; the input
  is the bus name.
- **Residual:** a policy file that allows too much is honoured as written.

### schema-udev (A1, A2)

- Only kernel uevents (netlink group 1, sender pid 0). Group 2 (libudev
  broadcasts, which any root process can send) is never read.
- Rules come from fixed paths in `/etc` and `/usr`. `RUN` actions run as root,
  as with systemd-udevd. **There is no owner/mode check on rules files** like
  the `.svc` loader's; it holds because those directories are root-owned.
- Device ACLs (`uaccess`) go only to the active uid on seat0, as schema-logind
  records it (`uaccess_active_uid`), and are cleared when that changes.

### schema-coredump (A1, A2)

- The core image is streamed into zstd unparsed, into a 0700 root directory,
  with stale temporary files swept.
- As root it reads the crashing process's `/proc/<pid>/comm`, `exe`, `cmdline`
  and `cgroup`, all strings the crasher controls. They are cleaned and written
  to the `.meta` file, syslog and the recorder socket.
- **Residual:** attacker-controlled strings travel through root into three
  sinks. Greg's 11-04 audit left coredump unaudited, so it's on the external
  audit list.

### schema-journal-sink (A1, A2)

- It owns `/dev/log`, `/run/systemd/journal/socket` and `stdout`, all 0666, so
  any local user can send it log lines and fds. Passed fds are read only when
  they are sealed memfds or on tmpfs, then closed through the detached closer.
- **Residual:** log forging and disk fill by any local user, as with journald.
  There is no per-uid rate limit.

### schema-recorder (everyone, read side)

- It runs as root and reads every process's `/proc` strings. Its control socket
  is 0600, but its shm summary is 0644, readable by every local user, like
  `/proc` itself.

### Passwordless sudo grants (A1 in the granted group)

- The ISO's `schema-flip` grant (the login user, `schema-flip-apply`, 20 fixed
  subcommands) and the wizard's `schema-wizard` grant (`%wheel`, `schema-migrate`,
  10 fixed flags) name exact commands, not wildcards (#478).
- **Residual:** each listed subcommand is a root action anyone in that group can
  trigger without a password. A bug in how those programs read their argv or
  state is root for that group.

### PID 1 availability (everyone)

- PID 1's umask is forced to 022, because an initrd's systemd hands over umask 0,
  which would leave `/run/schema-init`, `/run/udev` and logs world-writable (#204).
- Re-exec on upgrade validates the new image first and rolls back if it fails
  (`reexec_validate`). CI now proves services survive it (#592).
- A wedged shutdown: console writes no longer block PID 1, and the SIGKILL
  broadcast and remount sweep are ordered so the shutdown log survives.

### The next boot (A4, operator error)

- verify (above) judges the next boot, the normal default and the guard
  fallback before re-exec and `schema-snapshot arm`, which both refuse on
  failure. `reboot`/`poweroff` are gated only from a terminal, and they go ahead
  when the verifier is unavailable, so a box that must go down is never stranded
  (`docs/operations.md`).
- The boot-success guard: real GRUB is tested in CI (#593). `boot_counter=N`
  gives the new entry N−1 boots, and `arm` refuses N below 2 (#594).

## Fixed vulnerabilities and their regression tests

Every boundary fix needs a test that fails without the fix.

| Fix | Boundary | Regression test |
|---|---|---|
| #179 broker validates messages | D-Bus | `tests/test_sdbus_forward.c`, fuzz harnesses |
| #180 broker pre-auth flood, BEGIN without AUTH, match limit | D-Bus | `tests/test_sdbus_auth.c`, `tests/test_sdbus_driver.c` |
| #472 per-connection inbound cap | D-Bus | `tests/test_sdbus_conn.c` |
| #204 PID 1 umask 0 from initrd | PID 1 | `tests/qemu-ci/pid1-boot.sh`: boots through an `rdinit` that sets umask 000 and execs PID 1; a service's new file must be 0644 |
| #390, #393, #394 fd close off PID 1's thread | notify socket | `tests/test_fdclose.c` |
| #469 systemd1 bridge auth (polkit, fail-closed) | D-Bus / systemd1 | `tests/test_systemd1_authz.py` |
| #470 logind setter / lock auth | logind | `tests/test_logind_authz.py` |
| #477 import sandbox loss, secret modes | import | `tests/test_import_units.py` |
| #479 control socket stuck reader, line deadline | control socket | `tests/test_ctl_socket.py` |
| read-only verbs for group `schema` (7ceeaad) | control socket | `tests/test_ctl_gate.c` (the verb allowlist, anchored); `tests/qemu-ci/pid1-boot.sh`: the socket is 0660 root:schema, a uid-1000 member of `schema` runs `status` and is refused `stop` |
| broker per-uid connection cap | D-Bus | `tests/test_dbus_conn_cap.py`: the live broker keeps 256 connections from one uid, closes the 257th, frees a slot on close |
| #572 logind extra-name bus policy | logind / D-Bus | `tests/test_logind_authz.py` |
| #583 lingering-user authz opt-in | logind | `tests/test_logind_authz.py` (`linger_case`) |
| #590 verify sandbox | verify | `sudo make verify-sandbox-live` (18 hostile probes and an unbuildable-sandbox case), CI job `verify-sandbox` |

The three added in #600 were each checked to fail with their fix removed. `test_ctl_gate.c`
covers only the allowlist; the engine calling it is covered by the QEMU boot, so a run without
KVM doesn't see that half. `test_dbus_conn_cap.py` skips as root (root is exempt from the cap);
CI runs it as an unprivileged user.

## Checklist for a change to privileged code

For any change to PID 1, schema-logind, schema-systemd1, schema-dbus,
schema-udev, schema-coredump, verify, or anything else running as root on
another party's input:

1. **Name the boundary.** Which row of the components table does this touch?
   Who is on the other side (A1–A5)?
2. **Identity from the kernel only:** `SO_PEERCRED`, `SCM_CREDENTIALS`, D-Bus
   `GetConnectionUnixUser`, cgroups. Never a uid, pid or name from the message.
3. **Fail closed.** An error or an unknown caller is a denial, and an unknown
   input is a refusal or UNRESOLVED, never a fallback to the unprotected path.
4. **No blocking on the other side.** PID 1 never does a blocking read, write or
   `close()` on an fd an unprivileged party controls. Bounded buffers, timeouts,
   `MSG_DONTWAIT`.
5. **Bounded resources per peer:** memory, fds, names, children.
6. **Files:** created with explicit modes, never through a symlink in a
   directory someone else can write, and read only if root-owned and not
   group/world-writable where trust depends on it.
7. **Test the attack, not only the feature:** a regression test that fails
   without the fix, added to the table above.
8. **Update this file** in the same PR when a boundary, an attacker or a
   residual changes.
9. **Keep exploit detail out of public PR text** until the fix is released
   (see SECURITY.md).

## For the external audit

When it happens, start with the components that are most exposed and least
covered by tests:

1. schema-dbus broker: message parsing, policy matching, resource limits.
2. schema-logind: the authz model and the session-file trust.
3. The notify socket and fd handling in PID 1.
4. `verify_root.c`: mount handling and the sandbox.
5. schema-coredump and schema-journal-sink: root reading strings and fds that
   any local user controls.
6. Privileged exec paths: service spawn (user/caps/namespaces/seccomp/Landlock
   setup in `service.c`, `caps.c`, `ns.c`, `seccomp.c`, `landlock.c`).
7. The passwordless sudo targets (`schema-flip-apply`, `schema-migrate`) and
   schema-import as run by RPM file triggers.
