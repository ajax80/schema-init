# Service Hardening — Phase 3: Mount-Namespace Isolation

**Date:** 2026-09-26
**Status:** Approved design (2026-09-26) — Greg review folded in, ready for implementation plan
**Scope:** One PR. Three opt-in `.svc` fields that give a service its own view of the
filesystem, applied in the service child between fork and exec.

## Motivation

Phase 1 (#68) limited *what a service may do* (capabilities, `no_new_privs`). Phase 2 proved it
live on chronyd. Neither limits *what a service can see and write*. A compromised root daemon
today can still rewrite `/usr/bin`, drop a backdoor in `/etc`, read `~/.ssh`, and plant files
in the shared `/tmp` that other programs trust.

Mount namespaces close that: each opted-in service gets a private copy of the mount table in
which `/usr` is read-only, `/home` is an empty wall, and `/tmp` is its own. PID 1 and every
other service keep the real view.

(Naming note: the Phase 1 spec called mount-ns "Phase 2". Phase 2 became the chrony rollout,
so mount-ns is Phase 3 and seccomp is Phase 4.)

## Non-goals

- `protect_system=strict` (whole filesystem read-only except allowlisted paths) — needs a
  writable-paths field; later.
- `ReadWritePaths` / `InaccessiblePaths` / `BindPaths` equivalents.
- PID, network, user, or IPC namespaces.
- seccomp (Phase 4). Default-flip (own spec).
- Any new library. Raw syscalls only, same as `caps.c`.

## New `.svc` fields

All default to unset = current behavior. Any one of them being set makes the child create a
mount namespace. An explicit `0` is accepted as a no-op (clean for templating/overrides).

| Field | Values | Effect inside the service |
|-------|--------|---------------------------|
| `private_tmp` | `0` / `1` | Fresh empty `tmpfs` on `/tmp` and `/var/tmp` (mode 1777, `nosuid,nodev`). Discarded when the service exits. |
| `protect_system` | `0` / `1` / `full` | `1`: `/usr`, `/boot`, `/efi` read-only (recursive, so `/boot/efi` is covered). `full`: also `/etc`. |
| `protect_home` | `0` / `1` | Empty read-only `tmpfs` over `/home`, `/root`, `/run/user`. |

Parsing goes through small helpers in `ns.h` (`int parse_ns_bool(const char *val, uint8_t *out)`,
`int parse_protect_system(const char *val, uint8_t *out)`) so the unit test calls them
directly. Unknown values (e.g. `protect_system=yes`) are a **load error**; the service is skipped and
logged. A typo must not silently give *less* protection than intended.

### `service_t` additions (`service.h`)

```c
uint8_t ns_private_tmp;     /* 1 = private /tmp + /var/tmp        */
uint8_t ns_protect_system;  /* 0 none, 1 = /usr /boot /efi, 2 = +/etc */
uint8_t ns_protect_home;    /* 1 = hide /home /root /run/user     */
```

### Both parsers

`services_load()` (the boot path) and `service_load_one()` (`schema-ctl add`) each carry their
own field chain. Phase 1 shipped a field in only one and vmtest caught it. The new fields go in
both, and a new unit test (below) loads the same file through both paths and compares the
resulting structs, so the next field added to only one fails `make test` instead of vmtest.

### Load-time conflict checks (load error, service skipped)

- `private_tmp=1` with `ready_path` under `/tmp/` or `/var/tmp/`: PID 1 checks readiness in
  the *host* view and would never see the file → the service would hang in "starting".
- `private_tmp=1` with `exec` under `/tmp/` or `/var/tmp/`: the fresh tmpfs hides the binary → 127.
- `protect_home=1` with `ready_path` under `/home/`, `/root/`, `/run/user/`: the child can't
  write it (read-only tmpfs) and PID 1 couldn't see it anyway.
- `protect_home=1` with `exec` under `/home/`, `/root/`, `/run/user/`: the exec would fail with 127.

## Mechanism — new `ns.c` / `ns.h`

Mirrors `caps.c`: one small unit, raw syscalls, unit-testable parse helpers.

Headers: `<sys/mount.h>` only — **never also `<linux/mount.h>`** (the pair redefines on glibc).
With the Makefile's existing `-D_GNU_SOURCE`, `<sys/mount.h>` + `<fcntl.h>` + `<sys/syscall.h>`
provide `struct mount_attr`, `MOUNT_ATTR_RDONLY`, `AT_RECURSIVE` and `SYS_mount_setattr`.
Verified by test compile 2026-09-26 on blakbox (glibc 2.43, x86_64) and the Pi Zero (glibc
2.41, armv6l, native build). No hand-rolled fallback definitions: declaring our own
`struct mount_attr` would itself collide with glibc's. If a future toolchain lacks them, the
build fails loudly, which is the right outcome.

```c
/* Build the service's private mount view. Returns 0, or -1 with errno set
 * on any failure of a requested step. */
int apply_mount_ns(const service_t *svc);
```

Steps, in order:

0. If none of the three fields is set, return 0 immediately — unhardened services get no
   namespace and no extra syscalls.
1. `unshare(CLONE_NEWNS)`.
2. `mount(NULL, "/", NULL, MS_REC | MS_SLAVE, NULL)` — mounts made on the host after this
   still flow *in* (a USB stick plugged later is visible), but nothing the service mounts
   flows *out*. `MS_SLAVE`, not `MS_PRIVATE`, matching systemd.
3. **private_tmp:** for `/tmp`, `/var/tmp`: `mount("tmpfs", p, "tmpfs",
   MS_NOSUID | MS_NODEV, "mode=1777")`.
4. **protect_system:** for each path in the set:
   `mount(p, p, NULL, MS_BIND | MS_REC, NULL)` (makes it a mount point even when it's just a
   directory on `/`), then `mount_setattr(AT_FDCWD, p, AT_RECURSIVE, &{ .attr_set =
   MOUNT_ATTR_RDONLY }, sizeof attr)`. `mount_setattr` (kernel ≥ 5.12) makes the whole subtree
   read-only in one call; the old `MS_REMOUNT|MS_RDONLY` route is not recursive and would leave
   `/boot/efi` writable. Called via `syscall(SYS_mount_setattr, ...)` for the armv6 cross-build.
   Fleet kernels: 7.0 (x86), 6.18 (Pis) — all fine.
5. **protect_home:** for `/home`, `/root`, `/run/user`: `mount("tmpfs", p, "tmpfs",
   MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0755")`.

A path that doesn't exist on this machine (`ENOENT` — e.g. no `/efi`, no `/var/tmp` on a
minimal image) is **skipped**, not an error. Any other errno fails the step.

### Call site

`service_apply_hardening()` gains the mount step **first**:

```
apply_mount_ns      ← needs CAP_SYS_ADMIN, so before caps are dropped
apply_capabilities  ← existing
apply_no_new_privs  ← existing
(return) → uid drop → execv
```

Mounting after `keep_caps` would fail with EPERM for every hardened service that doesn't keep
`CAP_SYS_ADMIN` — which is all of them. The log file is already open before this point, so its
fd survives even though `/var/log` is untouched anyway.

PID 1 itself never calls `unshare`; only the forked child does.

### `protect_home` and `XDG_RUNTIME_DIR`

The uid-drop block runs after hardening and does `mkdir("/run/user/<uid>")` + `setenv
("XDG_RUNTIME_DIR")`. Under `protect_home=1`, `/run/user` is a read-only empty tmpfs, so the
mkdir fails (return ignored today) and the env var would point at nothing. Intended behavior:
a `protect_home` service has **no** runtime dir — skip the mkdir/chown/setenv when
`svc->ns_protect_home` is set, so the service sees an honest unset `XDG_RUNTIME_DIR` rather
than a dangling one. Services that need a per-user runtime dir (pipewire, wireplumber) don't
opt in. wsdd (runs as `wsdd`) doesn't use it.

## Failure policy: fail-closed

Same as Phase 1: a failed requested step writes one `dprintf(2, "[schema-init] HARDENING FAILED
for %s: mount_ns: <step>: %d\n", ...)` line to the service log and `_exit(126)`. The service
never runs with less isolation than its `.svc` claims. The existing restart backoff handles a
service that fails this way repeatedly.

## What will break (who must NOT opt in)

- **Anything that mounts for the system** (`mount-home`, `mount-ocean*`, `mount-efi`, udev
  automount helpers, `nfs-server`): its mounts stay inside its namespace and the host never
  sees them. With `MS_SLAVE` this is silent — the worst kind. Documented in the field table.
- **Anything that writes `/etc`** under `protect_system=full`: NetworkManager (`/etc/resolv.conf`, connection
  profiles), tailscaled (DNS config — unverified), accounts-daemon, polkitd rules updates.
- **Desktop session pieces** that share sockets in `/tmp` (`/tmp/.X11-unix`, `.ICE-unix`) with
  `private_tmp`: sddm, display stack, pipewire.
- **Package managers / updaters** with `protect_system`.
- **crond**: user jobs need `/home` and the real `/tmp`.
- **Ordering edge:** with `MS_SLAVE`, a host mount that lands on `/home` *after* a
  `protect_home` service started propagates into its namespace and may appear above or below
  the tmpfs. vmtest must cover it (start the checker, then mount on `/home` from the host,
  re-check). Until proven, `protect_home` services should `dep=` on `mount-home`.

## Testing

**Build:** `ns.c` → `SRCS`, `ns.h` → `CORE_HDRS`; `test_service_env` link line gains `ns.c`;
`test_mountns_parse` added to the `test:` target.

**Unit (`make test`):**
- `tests/test_mountns_parse.c` — field parsing: valid values, `protect_system=full` → 2,
  bad values → load error, both conflict checks.
- Parser parity: write one `.svc` with every hardening field, load via `service_load_one()`
  and via `services_load()` on a temp dir, `memcmp` the hardening fields.

**vmtest (the gate before any hardware):** new `test-mountns.svc` with all three fields runs a
small checker (`tests/livetest/test_mountns.c`) that asserts, from inside the service:
- write to `/usr/.probe` → `EROFS`; write to `/etc/.probe` → `EROFS` (full);
  write to `/boot/efi/.probe` → `EROFS` if `/boot/efi` exists (proves recursion);
- `/home` and `/root` list empty;
- writes `/tmp/mountns-sentinel`.

The vmtest script then asserts from the **host** that `/tmp/mountns-sentinel` does *not*
exist, that `/proc/<pid>/ns/mnt` differs from `/proc/1/ns/mnt`, and that PID 1's `/usr` is
still writable.

**Red/green proof (Phase 2 lesson — false green):** run the same checker from a copy of the
`.svc` *without* the three fields and require it to **fail** each assertion. A checker that
passes both ways is testing nothing.

## Rollout

1. PR with code + tests; vmtest green; red/green proof recorded in the PR.
2. Pilot on blakbox, one service at a time, `kill -HUP` deploy (never `restart`):
   - **wsdd** — network-facing, runs as `wsdd`, needs none of the protected paths:
     `private_tmp=1`, `protect_system=full`, `protect_home=1`.
   - **chronyd** — already Phase-2 hardened; writes only `/var/lib/chrony` and `/run/chrony`:
     same three fields.
   Verify each: running, working (`chronyc tracking` Leap Normal; wsdd visible from Dolphin
   on Optiplex), `HARDENING FAILED` absent from its log, `/proc/<pid>/mountinfo` shows the
   ro/tmpfs mounts.
3. Then candidates: avahi, rpcbind, smbd (`protect_home` off — shares may live under
   `/home`), ollama. Each its own verify.

Rollback per service: remove the three lines, `kill -HUP`.

## Decisions (Jonathan, 2026-09-26)

1. `private_tmp` is plain tmpfs; `/var/tmp` contents don't survive a restart. Accepted.
2. Pilot pair: wsdd + chronyd.
