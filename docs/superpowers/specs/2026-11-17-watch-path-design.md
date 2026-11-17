# Audit 4.8: watch_path= (systemd .path units)

## Intent
A service started by a filesystem condition instead of a clock or a socket,
so the importer and the shim can take `.path` units (today both refuse them),
in PID 1 and schema-user alike (one engine).

## Goals
- `watch_path=KIND:PATH`, up to 8 per service, any one fires it:
  - `exists` — PATH exists (follows symlinks, like `access(F_OK)`).
  - `exists_glob` — the glob matches something; glob characters only in the
    last component (the directory is what gets watched).
  - `dir_not_empty` — PATH is a directory with an entry besides `.`/`..`.
  - `changed` — PATH was closed after a write, created, deleted, renamed onto
    or away, or had its attributes changed.
  - `modified` — `changed` plus every write.
  PATH is absolute; anything else refuses the load.
- Level kinds (`exists`, `exists_glob`, `dir_not_empty`) fire whenever the
  condition holds and the service is not running: at arm time, on an event,
  and again after each run while it still holds (systemd semantics).
- Edge kinds (`changed`, `modified`) fire on the event. An event while the
  service is running is remembered once and runs it again after it exits (the
  file changed after the service read it). Never fired at arm time.
- `make_directory=1`, `directory_mode=` (default 0755): PID 1 (or schema-user,
  as the user) creates the PATH of `dir_not_empty`/`exists_glob` watches at
  arm, parents 0755.
- `trigger_limit_burst=` (default 200), `trigger_limit_interval_sec=`
  (default 2): more fires than that in the window parks the service
  (state EXCISED, reason "trigger limit", rail line `path-trigger-limit`)
  until `schema-ctl start`/`restart`. Stops a service that touches its own
  watched path from looping.
- A path service is born armed like a timer: PERFECT while waiting, counted
  as satisfied by dependents, `path-fire KIND:PATH` on the rail when it
  starts, back to armed after each run whatever the exit code.
- `schema-ctl start` runs it now; status shows the watches and the last fire.
- Importer (system and user): a `.path` plus the service it starts (`Unit=`,
  else the same name) becomes one .svc, the way `timer_to_svc` folds a timer.
  Maps PathExists=, PathExistsGlob=, PathChanged=, PathModified=,
  DirectoryNotEmpty=, MakeDirectory=, DirectoryMode=, TriggerLimitBurst=,
  TriggerLimitIntervalSec=. Specifiers through `_specifiers()` (so `%v` is
  skipped with a reason: it would freeze the kernel version at import time).
- Shim: `.path` leaves `unit_supported()`'s refusals; `enable foo.path`
  queues foo like a timer.

## Mechanism
- One inotify fd, the one ready_path already uses. Every `inotify_add_watch`
  on it uses one shared mask (the union of what ready_path and watch_path
  need, with IN_DONT_FOLLOW on the leaf): watches on the same inode share a
  wd, and a narrower mask from a later caller would replace the earlier one.
  Watches are never removed by hand; the kernel drops them with the inode
  (IN_IGNORED).
- Per watch: the wd of the deepest existing ancestor directory, and the wd of
  PATH itself when it exists (needed for `dir_not_empty` and the self events
  of `changed`/`modified`). When an event shows a missing component appeared
  or a watched inode went away, the watch is re-armed by walking PATH again.
  A symlink leaf is watched as the link (IN_DONT_FOLLOW), so replacing
  /etc/localtime by rename shows as IN_MOVED_TO name=localtime on /etc.
- The drain stops discarding events: each event is matched by wd (and, on a
  parent watch, by name == the next component) to every watch that holds that
  wd. Level watches are re-evaluated with stat/glob/readdir; edge watches fire
  when the event mask is in their set. ready_path keeps its one debounced
  recheck per batch.
- IN_Q_OVERFLOW: every watch re-arms, level kinds re-evaluate, edge kinds
  fire once (a spurious run beats a missed one).
- The READY_BACKSTOP_MS sweep also re-evaluates level watches and re-arms,
  for what inotify can't see (bind mounts, network filesystems).
- Re-exec: the fd is CLOEXEC, so the new image re-arms everything and
  re-evaluates level kinds; edge events in that window are lost. Reload:
  re-arm from the new config on the same fd.

## Non-goals (v1)
- watch_path= together with listen= or a timer key: refuses the load (cups
  keeps its socket activation; its .path is skipped with that reason).
- Globs in directory components, recursive watches, fanotify.
- systemd-ask-password-*.path: skipped by the importer (boot password
  prompts need their own look).
- TriggerLimit*= on non-path units.

## Approaches
1. Event-mapped watches on the existing fd (picked: `changed`/`modified`
   need the events themselves; one fd keeps poll and re-exec simple).
2. Poll with stat on the tick: rejected, misses writes that keep mtime in the
   same second and wakes an idle box.
3. One inotify fd per service: rejected, more fds in poll and re-exec for no
   gain; the shared-wd problem is solved by the shared mask.

## Tests
- Unit (C): parse/refuse table; level fire at arm; edge fire on write,
  rename-onto, symlink replace; event-while-running reruns once; trigger
  limit parks; make_directory modes; a missing parent created later arms
  the watch; ready_path still debounces with a path watch on the same dir.
- Importer (python suite): each Path*= key, Unit=, specifiers (%v skipped),
  listen/timer conflict, ask-password skip, user units.
- Live: blakbox PID 1 re-exec into the branch, a test path .svc for each
  kind, then back to packaged; schema-user with drkonqi-sentry-postman's
  shape (exists_glob + make_directory under XDG cache).
