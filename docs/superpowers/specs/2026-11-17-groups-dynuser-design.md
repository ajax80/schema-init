# Audit 4.10: supplementary_groups=, dynamic_user=

## Intent
Imported units keep their privilege model. Today `SupplementaryGroups=`
is only warned about, and `DynamicUser=yes` without `User=` runs as root
(earlyoom, fwupd-refresh, chrony-wait on blakbox).

## Goals
- `supplementary_groups=G ...`: names or numeric gids, space-separated,
  up to 16 in total; later lines add, an empty value clears (drop-in
  convention). Applied after `initgroups()`, so the service has its user's
  /etc/group memberships plus these. Applies to a root service as well
  (systemd does), so a service with only `supplementary_groups=` set still
  calls setgroups. An unknown name fails the start (exit 126,
  "UID DROP FAILED ... supplementary_groups: <name>"), like `group=`. The
  list is getgroups(0) + 16 long (a user can already be in many groups),
  deduplicated. The stop/post-stop/reload runners (svc_runner_drop) drop the
  same way as the main process.
- `dynamic_user=1` (system services only; schema-user ignores it with a log
  line, its services already run as the user):
  - With `user=NAME` where NAME exists in passwd: that static user is used
    (systemd's rule) — everything below but the allocation still applies
    (implied keys, private dirs). Otherwise a uid is allocated.
  - Allocation (PID 1, at the first spawn): start at
    61184 + FNV-1a(name) % 4336, probe upward with wraparound, skipping any
    number `getpwuid()` or `getgrgid()` knows (gid = uid, so a static group
    collides too) and any uid another loaded service holds. No free uid refuses the start. The uid rides SVC_RUNTIME_FIELDS
    (reload and re-exec keep it); the hash keeps it stable across boots.
  - Implies, unless set explicitly: `no_new_privs=1`, `private_tmp=1`,
    `protect_system=full`, `protect_home=1`.
  - Env: `USER`/`LOGNAME` = the service name, `HOME=/`.
  - State, cache and logs dirs live at `<base>/private/<dir>` (`private`
    root 0700), owned by the uid; `<base>/<dir>` is a symlink to
    `private/<dir>`. In the service's mount namespace a 0755 tmpfs covers
    `<base>/private` and only its own dirs are bound into it, so the
    symlink resolves for the service while another service that is later
    handed the same uid can never reach them. The tmpfs hides the host's
    `private/` by path, so the child opens each of its dirs O_PATH first,
    mounts the tmpfs, then binds each back through /proc/self/fd (the
    private_devices pattern). A dir whose owner is not the uid (it changed
    after a collision) is chowned recursively at start, as systemd does:
    O_NOFOLLOW|O_DIRECTORY opens, fchownat(AT_SYMLINK_NOFOLLOW), and no
    descent across a different st_dev, so a planted link or mount can't
    turn root's chown onto host files. A plain `<base>/<dir>` left from
    before dynamic_user (fwupd-refresh's cache) is renamed into
    `private/<dir>` and replaced by the symlink. Runtime dirs stay in /run, owned by the uid, removed on
    stop as today.
- Importer: `SupplementaryGroups=` → `supplementary_groups=`;
  `DynamicUser=yes` → `dynamic_user=1` (with `User=` kept), no more "runs
  as root" warning.
- `schema-ctl status <svc>` shows `dynamic uid N` once allocated.

## Non-goals
- An NSS module: `getpwuid()` inside the service returns nothing for a
  dynamic uid (systemd ships nss-systemd for this). Documented.
- `RestrictSUIDSGID=`, `RemoveIPC=`: the seccomp tier (4.6).
- Dynamic users for schema-user services.

## Approaches
1. Hash + probe in PID 1, private dirs behind a tmpfs in the service's
   namespace (picked: systemd's isolation, built on the mount-ns code
   4.4/4.5 already use).
2. Plain chown of `<base>/<dir>` to the uid, no private parent: rejected,
   a recycled uid could read another service's state.
3. Write dynamic users into /etc/passwd: rejected, PID 1 editing passwd at
   runtime is the opposite of dynamic.

## Tests
- C: supplementary_groups parse (names, gids, 16 cap, drop-in clear);
  allocation (hash start, probe past a passwd uid and a taken one, wrap,
  exhaustion); dynamic_user implies the four keys unless explicit; static
  user= wins.
- Live, PID 1 only (cgroups/uids need root; schema-user can't): a
  dynamic service sees uid in range, `id -G` with the supplementary group,
  writes its state dir; `/var/lib/<dir>` is a symlink into a 0700 private
  root; a second dynamic service cannot read the first's state; re-exec
  keeps the uid. Run on blakbox PID 1 like 4.4–4.7.
- Importer: earlyoom/fwupd-refresh/wsdd shapes, SupplementaryGroups=.
