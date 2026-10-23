# schema-dbus live reload — design

2026-10-23. Status: approved design, not built.

## Why

A Fedora package update ran `systemctl reload dbus.service` (imsettings' `%post`) and got
"Unit dbus.service does not support reload". Underneath that message are two real gaps:

1. schema-dbus reads its activation table once at start. A `.service` file a package drops into
   a D-Bus service dir later is not activatable until the next boot. Its `ReloadConfig` method
   rebuilds that table but never the policy.
2. The system-bus policy is dissolved from busconfig XML by `schema-dbus-run.sh` at launch, and
   only there. A package that adds or changes a `system.d/*.conf` gets no effect until reboot.

And one trap: schema-dbus does not handle SIGHUP, so the default action kills it. The obvious
"fix" from the error text, `reload=1` in `dbus.svc`, would make `systemctl reload dbus` SIGHUP the
broker and take down the system bus. Never ship `reload=1` for dbus (see the ReloadConfig route below).

Jonathan's goal: config changes from package updates take effect live, the way stock D-Bus does,
and the bus notices new or changed files **on its own, like dbus-daemon** (his choice over
dbus-broker's "only when asked").

## Anti-goal (his words)

"Losing the system bus mid-update and a dead desktop." Every reload path must be unable to stop,
restart or stall the bus. A reload that fails in any way leaves the bus running on the old config.

## Goals

- **Three triggers, one reload.** SIGHUP, the `org.freedesktop.DBus.ReloadConfig` method, and a
  file change in a watched directory all run the same reload.
- **Reload = activation table + policy.** Re-read the service dirs (as `ReloadConfig` does today,
  mask file included) and, on the system bus, re-dissolve the busconfig and swap the policy in.
- **Live, never a restart.** Same process; every connection, unique name, owned name, match rule,
  pending reply and pending activation survives. No fdstore hand-off, no re-exec.
- **Old config until the new one is proven.** The new policy is built off to the side and only
  swapped in once dissolved, parsed and sanity-checked (below). Any failure (XML error, python3 missing, dissolver crash
  or timeout, empty output, parse failure) logs one line naming the reason and keeps the old
  policy. The activation table is swapped only if the new parse succeeded; a failed rebuild keeps
  the old table.
- **Sanity check before the swap.** A live swap has no boot-time safety net (no stock fallback,
  no flip seatbelt), so a well-formed but wrong policy must not go live. Refuse the swap, keep the
  old policy and log why, if either: (a) the new policy's rule count is under half the current
  one's; or (b) any well-known name currently owned on the bus would be denied `own` by the new
  policy for its current owner's uid. (b) is what keeps login1, NetworkManager, polkit, UDisks2
  and the rest owning their names.
- **New system activation entries go live only through schema's own route.** At boot,
  `schema-import --bus-overrides` has already written the `/usr/local/share` override for every
  `SystemdService=` entry that has a schema `.svc`. Mid-`dnf`, a raw `/usr/share` entry can land
  before its override, and the broker would start the daemon itself as root, outside schema-init:
  the duplicate-daemon / lost-name race the overrides exist to stop. So a live reload adds a *new*
  system-bus entry that has `SystemdService=` only if it comes from an override dir
  (`/usr/local/share/dbus-1/system-services`); a raw one waits for the next boot, which is exactly
  today's behaviour. Entries without `SystemdService=`, changed or removed entries, and all
  override-dir entries apply live. The end-of-transaction `--bus-overrides` trigger writes the
  override, inotify sees it, and the name becomes activatable through schema-init.
- **The bus keeps serving during a reload.** The ~0.6 s python3 dissolve runs in a child process;
  the event loop never blocks on it.
- **Automatic pickup (inotify).** Watch, on the system bus: the system service dirs in use
  (`g_svcdirs`), `/usr/share/dbus-1/system.conf`, `/usr/share/dbus-1/system.d`, `/etc/dbus-1`
  and `/etc/dbus-1/system.d`, and the mask file. On the session bus: its service dirs (including
  `$XDG_RUNTIME_DIR/schema-user/dbus-services`, so schema-user's routed activation files are picked
  up without a ReloadConfig) and the mask file. Same set `schema-dbus-run.sh`'s `cache_fresh`
  treats as config.
- **Debounced.** A `dnf` transaction drops many files. Reload once after the watched set has been
  quiet for a short window (builder picks; ~1 s), not once per file. A change that arrives while a
  dissolve is running marks the reload dirty and runs one more reload after it finishes.
- **`systemctl reload dbus.service` calls `ReloadConfig`, not SIGHUP.** schema-systemd1's
  `ReloadUnit` special-cases `dbus.service`: it calls `org.freedesktop.DBus.ReloadConfig` on the
  system bus (dbus-broker's own `ExecReload=` does the same) and returns its result. No `reload=1`
  in any `dbus.svc`. Why: on an upgraded box the *old* broker keeps running until reboot, and it
  dies on SIGHUP; any path that starts SIGHUPing it before the reboot kills the bus mid-update.
  `ReloadConfig` is safe on every broker version and on stock `dbus-daemon`. The old broker just
  rebuilds its activation table, as today.
- **Session bus:** same broker code. SIGHUP/ReloadConfig/inotify rebuild the activation table.
  The session bus has no dissolved policy (it uses the built-in default), so there is nothing to
  re-dissolve there.

## Non-goals

- Restarting or re-execing the broker for config changes.
- Re-dissolving the session bus policy (there is none).
- Acting on policy changes for connections retroactively beyond what per-message evaluation already
  gives: the policy is evaluated per message against the global `g_policy`, so a swap applies from
  the next message. Nothing needs re-evaluating for messages already routed.
- Changing `dissect_policy.py`'s output format.
- Watching directories that don't exist at start (see "left to the builder").

## Constraints

- The broker is C, single-threaded, epoll + signalfd. SIGCHLD is already in the signalfd (it reaps
  activated children); the dissolve child is reaped the same way and identified by pid.
- How the broker learns how to dissolve: `schema-dbus-run.sh` exports what it already resolved
  (the dissolver path `$DISSECT`, the policy dir, the cache path). The broker runs
  `python3 <dissect> /usr/share/dbus-1/system.conf` with stdout to a temp file in `$POLDIR`, then
  renames it over `$POLDIR/dbus.policy` and refreshes the launcher's cache file only after the
  parse succeeded, so the next boot's cache stays valid. If the broker was started without these
  variables (hand-run, tests, the session bus), a reload only rebuilds the activation table.
- Bound the dissolve: a child still running after a timeout (builder picks; ~30 s) is killed, the
  reload counts as failed, the old policy stays.
- `ReloadConfig` replies after the reload finished: an empty reply on success, a
  `org.freedesktop.DBus.Error.Failed` naming the reason on failure (as dbus-broker does), so a
  scriptlet sees the new config live when the call returns, and a failure is visible to the
  caller as well as in the log (packaged scriptlets already end in `|| :`). While the dissolve
  runs, the reply is held; only the caller waits. Never hold it on anything outside the broker
  (an rpm lock, say): imsettings calls it from inside the transaction.
- A reload must not free anything still referenced. `ReloadConfig` already frees `g_svctab` live;
  the builder must confirm nothing (pending activations in `g_acts`, held messages) keeps pointers
  into the old table or the old policy, and copy or refcount where it does.
- Privilege: `ReloadConfig` stays root/own-uid only, as today. SIGHUP needs the usual kill rights.
- Every reload logs one line to stderr (which lands in the service log): trigger, result, counts,
  e.g. `schema-dbus: reload (inotify): policy 412 rules, 61 activatable services`, or the failure
  reason with "keeping the old policy".

## What he decided

- Notice files automatically, like dbus-daemon (inotify), on top of explicit reload.
- The anti-goal above; it outranks every other goal.

## My calls (Claude's, for him to check)

- One reload path for all three triggers; ReloadConfig replies after completion, error on failure.
- `systemctl reload dbus` → `ReloadConfig` instead of `reload=1` + SIGHUP (builder check found the
  SIGHUP route kills the old broker on upgraded-but-not-rebooted boxes). This replaces the
  "`dbus.svc` gets `reload=1`" in my playback. SIGHUP is still handled, as a safety net.
- Sanity check before the swap: rule count can't halve; owned names keep `own`.
- Raw `SystemdService=` activation entries wait for their override or the next boot.
- Debounce instead of per-event reloads; a "dirty" flag instead of queuing.
- Failed reload = old config, logged; never a fallback to stock dbus-daemon at runtime (that's a
  launch-time decision only).
- The launcher, not the broker, stays the source of truth for paths (exported env).
- No `.svc` change ships `reload=1` for dbus.

## Left to the builder

- Debounce window and dissolve timeout values.
- Whether to watch parent dirs to catch a service dir created after start
  (`/usr/local/share/dbus-1/system-services` may not exist). Stock dbus-daemon doesn't; fine to skip.
- inotify event mask details (`IN_CREATE|IN_DELETE|IN_MOVED_*|IN_CLOSE_WRITE|IN_ATTRIB` is the
  likely set) and handling `IN_Q_OVERFLOW` (treat as "something changed").
- Test structure. Required coverage: SIGHUP doesn't kill the broker and reloads; a new activation
  file becomes activatable without a signal; a changed `system.d` conf changes a policy decision
  live; a broken conf / missing python3 / killed dissolver keeps the old policy and the bus
  answering; ReloadConfig replies after the swap; existing connections and owned names survive a
  reload; a burst of file changes produces one reload; `systemctl reload dbus` goes through
  ReloadConfig (and works against stock dbus-daemon); a new policy that halves the rules or takes
  `own` from a current owner is refused; a raw `SystemdService=` entry without an override does
  not become activatable live, and does once its override appears.
- HW proof on blakbox before the fleet: drop a test `system.d` conf + system service file, watch
  the log line, call the service; then `dnf reinstall imsettings` and see the reload succeed.
