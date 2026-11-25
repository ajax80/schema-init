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
3. **Power and session control**: reboot, poweroff, suspend, session lock/unlock,
   and hostname/time/locale setters.
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
| **A5** | a crafted **unit file** that an admin imports | a systemd unit from a third-party package |
| **A6** | physical access to the console | out of scope beyond the session rules below |

Out of scope: an attacker who already has root and holds no boundary the project
claims; the network (nothing here listens, see below); bugs in the software
schema-init launches; the kernel.

## Components and trust boundaries

| Component | Runs as | Untrusted input | Channel |
|---|---|---|---|
| PID 1 (`init.c`, `engine.c`, `service.c`) | root | control commands (A1 in group `schema`, A2); notify messages and passed fds (any local user) | `/run/schema-init.sock` root:schema 0660; `/run/schema-init/notify` 0666 datagram, `SO_PASSCRED` |
| PID 1's `.svc` loader | root | `.svc` files and drop-ins | the filesystem: root-owned, not group/world-writable, or refused |
| `schema-ctl verify` → `verify_root.c` | root | another root filesystem's `init=` binary and `.svc` files (A4) | runs the target's `init= --version` / `--check` |
| `schema-logind.py` (login1, ConsoleKit, hostname1, timedate1, locale1) | root | D-Bus method calls (A1, A2, A3) | the system bus |
| `schema-systemd1.py` (org.freedesktop.systemd1 bridge) | root | D-Bus method calls (A1, A2) | the system bus |
| `schema-dbus` (bus broker) | root | every message from every peer (A1, A2, A3) | `/run/dbus/system_bus_socket` 0777; policy decides who may send what |
| `schema-udev` | root | kernel uevents; rules files | netlink group 1, sender pid 0 only (`schema-udev.c:59`); rules from root-owned dirs |
| `schema-coredump` | root, as `core_pattern` pipe | the core image of any crashing process (A1, A2) | the kernel pipe |
| `schema-user` | the user | that user's own `.svc` files | `$XDG_RUNTIME_DIR/schema-user/services` |
| `schema-import` | root (admin-run) | systemd unit files (A5) | files on disk |
| socket activation (`listen=`) | root binds, hands off | none read by PID 1 | the admin chooses the port; the fd goes to the service unread |

**No network listeners.** PID 1, logind, udev and the broker open no INET socket
of their own: the `AF_INET` sockets in `ns.c` and `schema-udev.c` are interface
ioctl handles. `listen=` binds what the admin configured and passes it on.

## Threats and what holds them

### PID 1 control socket (A1, A2)

- **Who may connect:** root:schema 0660. The uid comes from `SO_PEERCRED`,
  never from the message.
- **Who may change things:** root (and PID 1's own euid) run every verb; members
  of `schema` get read-only verbs only (`engine.c` peer check before `ctl_cmd`).
- **Freezing the supervisor:** a client that asks for output and never reads it
  used to block PID 1 on a full send buffer. Each reply is now built in a buffer
  and sent with `MSG_DONTWAIT`, so PID 1 never waits on a client that doesn't
  read (#479).
- **Residual:** anyone in `schema` can read every service's state and command
  line. Treat `schema` membership as "may see the process table, privileged
  commands included".

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

- **Trust:** a `.svc` must be a regular file owned by root (or the
  supervisor's euid) and not group- or world-writable
  (`service_open_trusted`). Otherwise `REFUSED`. Symlinks under service
  directories are followed only from an admin-owned directory (`dir_trusted`).
- **Sandbox loss on import:** `schema-import` maps systemd sandboxing it
  understands and **warns** about what it drops. A `DynamicUser=` with no `User=`
  stays root with a warning; secrets are no longer copied 0600 → 0644 (#477).
- **`exec=`:** runs through `execv`, with no PATH search. A relative path is a
  warning today and verify shows it as a WARN; a later release refuses it (#596).
- **Residual:** an admin who imports a hostile unit gets a service doing what the
  unit says. The importer is a translator, not a vetting tool.

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
- **Residual:** the target can still read anything root can read on its own
  read-only root. That is the point of the inspection, and nothing leaves except
  the report.

### schema-logind (A1, A2, A3)

- **Who counts as "the user":** root, or the uid of the active local session
  from root-written session files (the session on the foreground VT, else the
  single active local one, else nobody). A lingering user with no session (A3)
  is nobody unless the admin opts in with `/etc/schema-init/logind-run-user-authz`
  (#583).
- **Every state-changing method** (TakeDevice, PowerOff/Reboot, Lock/Unlock,
  hostname/time/locale setters, CreateSession and others) calls `authz_check` as
  its first statement, which fails closed (#470).
- **Bus policy:** each extra name (hostname1, timedate1, locale1, ConsoleKit)
  has its own connection, so their blanket `send_destination` allows don't open
  login1's methods at the bus layer (#572).
- **No polkit:** schema-logind decides in the daemon, as above. Anything polkit
  would have allowed to a non-session uid is denied.
- **Residual:** the active local session's uid has what real logind gives it:
  devices and the power button. A process running as that uid has them too.

### schema-systemd1 bridge (A1, A2)

- Unit-changing methods (Start/Stop/Restart/Reload…Unit and friends) are allowed
  to root. For any other caller the bridge asks polkit's `CheckAuthorization`,
  as systemd does inside its daemon, because the stock bus policy lets anyone
  send them. Any error asking (no uid, polkit unavailable or timing out) is a
  refusal (#469). Interactive authentication only when the client asked for it.
- **Residual:** whatever the box's polkit rules allow, the bridge allows.

### schema-dbus broker (A1, A2, A3)

- **Malformed messages:** validated before forwarding, and header field types
  enforced (#179). Fuzzed in CI on every PR (#318, `tests/fuzz`).
- **Resource pinning:** a peer can pin only a bounded amount: per-connection
  inbound buffering (#472), names, pending replies and match rules (#180, and
  reply-timeout reaping).
- **Policy:** the dbus-daemon policy files decide who may send what; the
  broker's matcher is checked against golden output from the real policy
  (`tests/fixtures/dbus`).
- **Residual:** a policy file that allows too much is honoured as written.

### schema-udev (A1, A2)

- Only kernel uevents (netlink group 1, sender pid 0). Group 2 (libudev
  broadcasts, which any root process can send) is never read.
- Rules come from root-owned directories. `RUN` actions run as root, as with
  systemd-udevd.
- Device ACLs (`uaccess`) go only to the active uid on seat0, as schema-logind
  records it (`uaccess_active_uid`), and are cleared when that changes.

### schema-coredump (A1, A2)

- It reads an attacker-shaped core image from the kernel pipe as root. The
  output goes to a root-only directory, with stale temporary files swept.
- **Residual:** this parser is a classic root-on-attacker-data target. It
  belongs on the external audit list.

### PID 1 availability (everyone)

- PID 1's umask is forced to 022, because an initrd's systemd hands over umask 0,
  which would leave `/run/schema-init`, `/run/udev` and logs world-writable (#204).
- Re-exec on upgrade validates the new image first and rolls back if it fails
  (`reexec_validate`). CI now proves services survive it (#592).
- A wedged shutdown: console writes no longer block PID 1, and the SIGKILL
  broadcast and remount sweep are ordered so the shutdown log survives.

### The next boot (A4, operator error)

- verify (above) judges the next boot, the normal default and the guard
  fallback before reboot, re-exec and `schema-snapshot arm`.
- The boot-success guard: real GRUB is tested in CI (#593). `boot_counter=N`
  gives the new entry N−1 boots, and `arm` refuses N below 2 (#594).

## Fixed vulnerabilities and their regression tests

Every boundary fix needs a test that fails without the fix.

| Fix | Boundary | Regression test |
|---|---|---|
| #179 broker validates messages | D-Bus | `tests/test_sdbus_forward.c`, fuzz harnesses |
| #180, #472 broker resource caps | D-Bus | `tests/test_sdbus_conn.c`, `tests/test_sdbus_driver.c` |
| #204 PID 1 umask 0 from initrd | PID 1 | **missing**: verified in a VM only |
| #390, #393, #394 fd close off PID 1's thread | notify socket | `tests/test_fdclose.c` |
| #469 systemd1 bridge auth (polkit, fail-closed) | D-Bus / systemd1 | `tests/test_systemd1_authz.py` |
| #470 logind setter / lock auth | logind | `tests/test_logind_authz.py` |
| #477 import sandbox loss, secret modes | import | `tests/test_import_units.py` |
| #479 control socket stuck reader, read-only verbs | control socket | `tests/test_ctl_socket.py` |
| #572 logind extra-name bus policy | logind / D-Bus | `tests/test_logind_authz.py` |
| #583 lingering-user authz opt-in | logind | `tests/test_logind_authz.py`, `tests/test_logind_vt.py` |
| #590 verify sandbox | verify | `sudo make verify-sandbox-live` (19 hostile probes), CI job `verify-sandbox` |

Gap: #204 has no test that would catch a regression. That's the first item for
the next security PR.

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
5. schema-coredump's parser.
6. Privileged exec paths: service spawn (user/caps/namespaces/seccomp/Landlock
   setup in `service.c`, `caps.c`, `ns.c`, `seccomp.c`, `landlock.c`).
