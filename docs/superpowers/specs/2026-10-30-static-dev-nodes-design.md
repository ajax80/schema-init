# Static device nodes at boot (kmod-static-nodes parity) — design

**Status:** draft, 2026-10-30. Option B chosen by Jonathan. Reviewed by Greg 2026-10-30 (all catches folded in).

## Why

A systemd boot runs `kmod-static-nodes` + `systemd-tmpfiles-setup-dev` before
udev: every module that declares a fixed device node (`modules.devname`) gets
that node created up front, so the first `open()` autoloads the module.
schema-init never does this. A node exists only if its module is already
loaded or built in.

Found 2026-10-30 on podman test 1 (DBox, 0.4.21): `/dev/net/tun` absent →
pasta can't set up rootless networking → the Quadlet container crash-looped to
DORMANT. With `modprobe tun` by hand the podman e2e passed 11/11. blakbox only
passed on 10-27 because its VPN already loads tun.

Missing across the fleet today: blakbox `uinput vhost-net ppp cuse`; Optiplex
`vhost-net ppp cuse`; Eli `uinput vhost-net ppp cuse`; DBox `net/tun uinput
vhost-net vhost-vsock ppp cuse`. uinput breaks Steam Input / ydotool /
KDE Connect input; vhost-net silently slows QEMU networking.

The Artix port already does this in `distros/artix/scripts/artix-boot`
(`kmod static-nodes` + `etmpfiles --prefix=/dev --boot`). The Fedora rail
(`schema-sysprep.sh`) does not, and blakbox/Optiplex don't run sysprep at all —
so the fix belongs in PID 1, where every box gets it.

## Goals

1. On first boot (not re-exec), right after `mount_pseudo()` and before
   `load_configured_modules()`, PID 1 creates every node listed in
   `/lib/modules/$(uname -r)/modules.devname` that does not already exist.
2. Then applies the permissions in `static-nodes-permissions.conf`, so e.g.
   `/dev/net/tun` and `/dev/fuse` are 0666, `/dev/kvm`, `/dev/vhost-net` and
   `/dev/vhost-vsock` are 0666 group kvm, `/dev/snd/seq` 0660 group audio —
   exactly as on systemd. Paths are literal (the shipped file has no globs;
   no glob support).
3. No fork, no exec, no kmod dependency. A missing file is a no-op.
4. One summary line in the boot log naming what was created.

## Non-goals

- General tmpfiles support (`d`, `L`, `D!` …). Only the static-node job.
- Changing `schema-sysprep.sh`, `artix-boot`, or schema-udev. Artix's own
  `kmod static-nodes` run afterwards finds the nodes existing — harmless.
- Loading modules. Nodes are created; the kernel autoloads on first open.

## Anti-goals

- Never touch an existing node's type or device number (an existing node is
  kernel/devtmpfs truth). Only the permission pass may chmod/chown it — that is
  what systemd's `z` lines do too.
- Never block or fail boot: every error is logged and skipped.

## Design

**Code:** new header `static_nodes.h` (pure, testable, the `disk_links.h`
pattern), called from `init.c`'s first-boot block.

**modules.devname** — release from `uname(2)`; file
`/lib/modules/<rel>/modules.devname`, falling back to
`/usr/lib/modules/<rel>/modules.devname`. kmod's format, one per line:
`<module> <devname> <c|b><major>:<minor>`; `#` lines and blanks skipped;
malformed lines skipped. Devnames containing `..` or starting with `/` are
rejected (root-owned file, but PID 1 doesn't trust a path blindly). For each:
create the *parent* directories of `/dev/<devname>` (never the leaf) mode 0755;
then `mknod(path, (type=='c' ? S_IFCHR : S_IFBLK) | 0600, makedev(maj,min))`
(`<sys/sysmacros.h>`; kmod's tmpfiles output uses 0600 — parity). `EEXIST` →
skip silently.

**Permissions** — file `static-nodes-permissions.conf`, systemd's tmpfiles
search order: `/etc/tmpfiles.d/` → `/run/tmpfiles.d/` →
`/usr/local/lib/tmpfiles.d/` → `/usr/lib/tmpfiles.d/` (first found wins, whole
file). Only `z` lines whose path starts with `/dev/` and contains no `..` are
used; fields `path mode user group`, `-` = leave as is (tmpfiles semantics, not
udev's "group without mode ⇒ 0660"); mode is plain octal (anything else → skip
the mode, keep owner/group). User/group: all-digits → numeric id, else
`getpwnam`/`getgrnam` (PID 1 already does this in static glibc); unknown name →
skip that chown, log it. Applied to every listed path that exists, created by
us or not (systemd parity). Apply exactly like schema-udev's
`node_apply_perms`: `open(O_PATH|O_NOFOLLOW|O_CLOEXEC)` (no device open, so no
autoload; no symlink follow), `fstat` → char/block only, change via the
`/proc/self/fd/N` alias. `ENOENT` is normal (e.g. no `/dev/kvm` without virt)
and silent. Not a call to `node_apply_perms` itself: its udev mode default
differs, and PID 1 shouldn't include the 550-line udev header for 30 lines.

**Log:** `[schema-init] static nodes: created 6 (net/tun uinput …)`; errors as
`[schema-init] static node <path>: <strerror>`.

**Re-exec:** inside the existing `reexec_fd < 0` block, so it runs once per
boot.

## Testing

- `tests/test_static_nodes.c` (wired into `make test`): devname parsing (valid
  char/block, comment, blank, malformed, `..`/absolute rejected); perms parsing
  (`z` used, other types ignored, non-`/dev` ignored, `-` fields, bad mode);
  precedence (/etc over /usr/lib). The create step takes a root prefix; the
  mknod assertions run only as root, the rest always.
- vmtest (schema-vmtest skill): boot, `tun` not loaded → `/dev/net/tun` exists
  `crw-rw-rw-`, `/dev/uinput` exists; open of `/dev/net/tun` autoloads `tun`.
- Hardware: DBox on the dev build → reboot → no `modprobe tun` → podman test 1:
  container up with nobody logged in, `network-online` spawn→done ≈ 0 s,
  LAN curl 200. Same nodes check on blakbox.

## Builder's calls (mine, flag if wrong)

- Permission pass applies to pre-existing nodes too (systemd does).
- Unknown or unsupported mode syntax is skipped per field, not per line.
- Done before `load_configured_modules()` so a `modules-load.d` entry and a
  static node never race.

## Seen in passing — not this change

`mount_pseudo()` uses decimal `1777` in `mkdir("/dev/shm", 1777)`,
`mkdir("/run/lock", 1777)`, `mkdir("/run/shm", 1777)` (should be `01777`).
`/dev/shm` is masked by its tmpfs `mode=1777`; DBox shows `/run/shm` at 1341.
Separate one-line PR.
