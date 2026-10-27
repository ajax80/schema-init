# Rootless podman + Quadlet on schema-user — design

Date: 2026-10-27. Roadmap: "Per-user supervisor, phase 2" — this spec takes the
rootless-podman part of it. GNOME, desktop per-app scopes and a native session
`systemd1` move to a later phase.

## Intent

Jonathan wants to run rootless podman containers on schema-init boxes, with no
systemd. The way they get there is Quadlet: he drops a `.container` file in
`~/.config/containers/systemd/`, and the container comes up on its own and
stays up — at boot when the user lingers, before anyone logs in.

His requirements, in his words: "come up on its own and stay up", "restart on
crash", "limits that actually hold", "being reachable from LAN". Boot with
lingering: "good".

## What exists today (recon on blakbox, 2026-10-27)

- podman 5.8.9, crun, conmon, netavark, passt, Quadlet
  (`/usr/libexec/podman/quadlet`, `/usr/lib/systemd/user-generators/podman-user-generator`) are installed.
- Plain `podman run` as the user works. podman detects it cannot get a scope
  (`schema-systemd1-session` refuses transient scopes) and falls back to
  `cgroup_manager=cgroupfs`.
- **Limits are silently lost.** `podman run --memory 64m` from a login shell
  ran with `memory.max = max`: the shell lives in root-owned
  `session-1.scope`, so podman cannot create the container cgroup and drops
  the limit without an error.
- schema-user already runs in delegated, user-owned
  `user@1000.service/` (all controllers enabled), moves itself into
  `init.scope`, and gives each service its own cgroup below that
  (`user_cgroup_root()` in `schema-user.c`). It is a child subreaper.
- PID 1 / schema-user accept `READY=1` from **any** pid in the service's cgroup
  (`notify.h`) — equivalent to `NotifyAccess=all`.
- Nothing runs Quadlet's generator: `schema-import --user` reads
  `~/.config/systemd/user` (+ distro user units), so a `.container` file never
  becomes a service.
- Quadlet's output for a minimal `.container` (`Memory=64m`, `WantedBy=default.target`):

  ```
  [Unit]
  Wants=podman-user-wait-network-online.service
  After=podman-user-wait-network-online.service
  RequiresMountsFor=%t/containers
  [Service]
  Environment=PODMAN_SYSTEMD_UNIT=%n
  KillMode=mixed
  ExecStop=/usr/bin/podman rm -v -f -i systemd-%N
  ExecStopPost=-/usr/bin/podman rm -v -f -i systemd-%N
  Delegate=yes
  Type=notify
  NotifyAccess=all
  ExecStart=/usr/bin/podman run --name systemd-%N --replace --rm --cgroups=split --memory 64m --sdnotify=conmon -d <image> <cmd>
  ```

  Fed through today's `unit_to_svc`, it translates mostly right (`notify=1`,
  args, `%N` expanded) but: `%n` is left literal in `env=`, `ExecStop`/
  `ExecStopPost` are dropped, and nothing follows the main PID.
- `podman run -d --sdnotify=conmon` exits once the container is up; conmon
  stays and sends `MAINPID=<conmon>` then `READY=1`. schema-init does not
  understand `MAINPID=`, so it would treat the launcher's exit as the service
  ending.

## Goals

1. A `.container` (and the `.network` / `.volume` / `.pod` / `.image` /
   `.kube` files Quadlet handles, since they come through the same generator)
   in the user's Quadlet dirs becomes a schema-user service with no extra step.
2. With lingering on, it starts at boot before login; without, at login.
3. It is supervised as the container: crash → restart (when the file asks for
   restart, as on systemd), stop → container removed.
4. Resource limits in the file (`Memory=`, `PodmanArgs=--cpus`, …) are really
   enforced by the kernel.
5. Published ports (`PublishPort=8080:80`) answer from another LAN host.
6. Editing a `.container` and running `systemctl --user daemon-reload` (the
   existing shim) picks the change up.

## Non-goals (this phase)

- GNOME, desktop per-app scopes, a native session `systemd1` with
  `StartTransientUnit` — later phase. podman keeps using `cgroupfs` here.
- Rootful/system Quadlet (`/etc/containers/systemd`, `podman-system-generator`).
  Out unless Jonathan pulls it in; noted as a small follow-on once this works.
- Lowering `net.ipv4.ip_unprivileged_port_start`. Rootless containers publish
  on ports ≥ 1024 (e.g. 8080, not 80). Jonathan agreed to leave the limit alone.
- Containers surviving a schema-user restart/crash/upgrade (Jonathan: accept
  the blip for now). Today logind kills what schema-user leaves behind and the
  containers come back via `--replace`. Re-adopting running services across a
  schema-user restart goes on the roadmap.
- `podman generate systemd` output, auto-update timers (`podman-auto-update`)
  — not asked for.

## Anti-goals

- A limit that is set but silently not enforced. If a limit can't be applied,
  the service must fail loudly (log + not start), never run unlimited quietly.
- Containers left behind after stop/crash that block the next start.
- Any change that alters behaviour of existing system services under PID 1
  beyond the new, opt-in keys.

## Success test (on blakbox, real hardware, after a vmtest)

One real `.container` with `PublishPort=8080:80`, `Memory=64m`,
`[Service] Restart=always`, `[Install] WantedBy=default.target`, lingering on:

1. Boot → container running before any login (`podman ps`, `schema-ctl --user status`).
2. Log in → still one container, not two.
3. `kill -9` conmon's container process → service restarts, container back.
4. A memory hog inside the container is OOM-killed at 64 MB
   (`memory.max` = 67108864 in the container's cgroup).
5. `curl http://<blakbox-ip>:8080` from another LAN box answers.
6. `systemctl --user stop <name>` → `podman ps -a` shows no leftover container.
7. Edit the file + `systemctl --user daemon-reload` + `systemctl --user restart <name>`
   → change applied (systemd semantics: reload alone leaves running services
   alone — *Claire's call*).

## Design (decided)

### 1. Run Quadlet during user import

`schema-import --user` runs the user generator
(`/usr/lib/systemd/user-generators/podman-user-generator`, or any executable in
the user-generators dirs — builder's call how general) into a per-user output
dir under `$XDG_RUNTIME_DIR/schema-user/`, before reading units. Generated
units are treated as distro-level units; units the generator enables via its
`.wants` symlinks count as enabled, as `~/.config` `.wants` links do today.
Runs on every import, so the existing `daemon-reload` shim path re-generates.
The 10 s import cap stays; Quadlet is fast.

### 2. `MAINPID=` support (engine — PID 1 and schema-user)

Opt-in, so no existing service changes: a new key `main_pid_notify=1` (needs
`notify=1`; the importer sets it for `Type=notify` + `NotifyAccess=all`, which
is what Quadlet emits). Without the key, `MAINPID=` stays ignored as today.
*(Claire's call — the anti-goal rules out a global change.)* Such a service may
send `MAINPID=<pid>`. Accept it only when that pid is
alive and inside the service's cgroup — the same rule `pid_file_read()` uses.
Then supervise that pid as the main process exactly as `pid_file` adoption
does (`fork_adopt` path): the launcher's exit 0 is not a death; the adopted
pid's death enters the recovery arc; stop signals it. A launcher that exits
non-zero before READY is a failed start.

### 3. Stop commands

New `.svc` keys `exec_stop=` and `exec_stop_post=` (repeatable, same argv form
as `exec_pre`), run on stop: `exec_stop` before signalling the main process,
`exec_stop_post` after the service is gone (also after a crash, matching
systemd's `ExecStopPost`). The importer maps `ExecStop=` / `ExecStopPost=`
(with the `-` ignore-failure prefix). Bounded by `stop_timeout_sec`.

### 4. Importer fixes

- Expand `%n` (unit name with suffix) wherever `%N` already is, and audit the
  other specifiers Quadlet emits (`%t`, `%N`, `%n`, `%h`, `%U`, `%S`).
- `Restart=` from the `.container`'s `[Service]` is already mapped; check the
  Quadlet output carries it through.
- `podman-user-wait-network-online.service` (calls `systemctl is-active
  network-online.target`) → map to the existing `dep=network-online` wait
  instead of importing it.
- `KillMode=mixed`, `Delegate=yes` → see §5.

### 5. Limits that hold

With `--cgroups=split`, podman (as cgroupfs manager) creates the container's
cgroup **below the service's own cgroup**, which schema-user already makes
user-owned. For limits to apply, that service cgroup must have the needed
controllers (`memory`, `cpu`, `pids`, `io`, `cpuset`) enabled in its
`cgroup.subtree_control`, and its processes must not sit in the cgroup's
root (cgroup v2 no-internal-process rule) — the launcher/conmon must be in a
leaf. A `delegate=1` `.svc` key (from `Delegate=yes`) turns this on for the
service. Exact layout (e.g. service cgroup → `supervisor/` leaf for
launcher+conmon, podman's container cgroup beside it) is the builder's call,
as long as success test 4 passes and a failure to enable controllers fails the
start instead of running unlimited.

### 5b. Verify the limits landed (Jonathan: yes)

Enabled controllers don't guarantee podman/crun wrote the limit — rootless
crun can skip cgroup setup silently (what recon saw). So after READY,
schema-user finds the container's cgroup under the service cgroup and
compares `memory.max`, `cpu.max` and `pids.max` with what the unit asked for
(`--memory`, `--cpus`, `--pids-limit` in the podman argv, which Quadlet's
`Memory=`/`PodmanArgs=` become). A mismatch logs the expected and actual
values and fails the service (recovery arc), never runs on unlimited. How
the expected values reach schema-user (importer writes `.svc` keys vs
schema-user parsing argv vs `podman inspect`) is the builder's call. Own PR.

### 6. LAN reachability

Rootless port publishing goes through pasta (installed). It binds on all
interfaces by default, so nothing schema-specific is expected. firewalld is
not running on blakbox. Verify only; fix only if test 5 fails.

### 7. Image pulls at boot

A cold `podman run` pulls before READY. Daemons have no start timeout by
default (`start_timeout_sec=0`), so a slow pull just stays starting; the
importer must not map a short `TimeoutStartSec` that would kill a pull. The
success test also runs once with the image removed first. *(Claire's call.)*

## Constraints

- Session-holding daemons are never `restart`ed live: swap schema-user onto a
  new binary with SIGKILL (logind restarts it), never SIGTERM; schema-logind
  via `kill -HUP`.
- Engine changes touch PID 1 → `make test` + `/schema-vmtest` boot test before
  any hardware reboot.
- PRs open as **draft**; one `/code-review` pass; bump spec before tag.
- Works on Fedora 44 x86_64 first. aarch64 (Eli) is a nice-to-have.

## Left to the builder

- PR split (suggested: importer generator+specifiers → engine MAINPID →
  exec_stop → delegate/limits → limit verification → end-to-end test).
- Cgroup layout under the service (§5), generator-dir layout, how general the
  generator runner is.
- Test container image for the success test (a small web server serving on
  80 inside, published on 8080).
- Whether `exec_stop*` also serves `schema-ctl --user stop` and shutdown, and
  how the `-` prefix is represented.
