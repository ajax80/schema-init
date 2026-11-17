# watch_path= Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Services started by filesystem conditions (`watch_path=KIND:PATH`), so `.path` units import.

**Architecture:** A self-contained `watch.c` owns path walking, inotify arming, event classification and level evaluation, testable without the engine. `service.c` parses the keys into `watch_t`s; `engine.c` arms them on its existing `ino_fd`, routes drained events, and drives the service like a timer (born armed in PERFECT, fire → NEW_PROCESS, back to PERFECT after each run). The importer folds `.path` + its service into one .svc like `timer_to_svc`.

**Tech Stack:** C99 (`-D_GNU_SOURCE`), inotify, glob(3); Python 3 importer; sh live tests through schema-user.

**Spec:** `docs/superpowers/specs/2026-11-17-watch-path-design.md`

## Global Constraints
- Kinds: `exists`, `exists_glob`, `dir_not_empty`, `changed`, `modified`; PATH absolute, < 256 bytes; up to 8 (`MAX_WATCH`) per service.
- Glob characters (`*?[`) only in the last component of an `exists_glob` PATH.
- Defaults: `directory_mode=0755`, `trigger_limit_burst=200`, `trigger_limit_interval_sec=2`.
- `watch_path=` with `listen=` or any timer key (`on_boot_sec`, `on_active_sec`, `on_calendar`) refuses the load.
- `make_directory=1` creates PATH for `dir_not_empty`, dirname(PATH) for `exists_glob`, nothing for other kinds; parents 0755; owner = creating process (never chown to user=).
- One inotify mask for every `inotify_add_watch` on `ino_fd`; IN_DONT_FOLLOW only when adding the leaf.
- Rail lines: `path-fire KIND:PATH`, `path-trigger-limit`; excise reason name `trigger-limit-hit`.
- No docstrings; why-comments only.

## Review Focus
1. A watched path on a filesystem that sends no events (bind mount, NFS) — level kinds still fire within READY_BACKSTOP_MS. Test: Task 3, `backstop` case (`watch_backstop_tick` flips a level watch whose wd is -1).
2. The watched directory is deleted and recreated while armed — the watch re-arms on the new inode and still fires. Test: Task 1, `test_dir_recreated`.
3. A service that writes its own `changed` path every run — parked by the trigger limit, not a CPU-burning loop. Test: Task 3 live, `selfloop` service reaches `trigger-limit-hit` within 5 s.
4. A busy directory (many writes to siblings of PATH) — no fire, no ready_path recheck storm. Test: Task 1, `test_sibling_noise` (100 sibling writes → `WATCH_EV_NONE` each).
5. PATH longer than the buffer or relative — load refused with a message naming the key, not truncated. Test: Task 2, refuse table.

---

### Task 1: watch.c — parse, arm, classify, evaluate

**Files:**
- Create: `watch.h`, `watch.c`
- Test: `tests/test_watch.c`
- Modify: `Makefile` (add `watch.c` to `SRCS`, `watch.o` to `ENGINE_OBJS`, every test recipe that links `service.c`, and a `test_watch` line beside `test_svc_sched`)

**Interfaces:**
- Produces (watch.h):
```c
#define MAX_WATCH 8
enum { WATCH_EXISTS, WATCH_EXISTS_GLOB, WATCH_DIR_NOT_EMPTY, WATCH_CHANGED, WATCH_MODIFIED };
#define WATCH_INO_MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | \
                        IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)
typedef struct {
    uint8_t kind;
    uint8_t leaf_seen;   /* leaf existed at the last arm: absent→present fires edge kinds */
    int     wd_parent;   /* deepest existing ancestor dir (dirname for glob), -1 none */
    int     wd_leaf;     /* PATH itself, -1 when absent or kind is exists/exists_glob */
    int     depth;       /* components between the wd_parent dir and the leaf: 1 = direct parent */
    char    path[256];
} watch_t;
#define WATCH_EV_NONE  0
#define WATCH_EV_FIRE  1   /* edge kind matched, or a level kind re-evaluated true */
#define WATCH_EV_REARM 2   /* a component appeared/vanished or a watched inode went away */
int  watch_parse(const char *val, watch_t *w);              /* 0 / -1; wds set to -1 */
const char *watch_kind_name(uint8_t kind);
int  watch_is_level(const watch_t *w);
void watch_arm(int ino_fd, watch_t *w);                     /* first arm: sets wds, depth, leaf_seen; never fires */
int  watch_rearm(int ino_fd, watch_t *w);                   /* arm again; WATCH_EV_FIRE if edge kind and leaf went absent→present */
int  watch_level_true(const watch_t *w);                    /* exists / glob matches / dir has an entry */
int  watch_event(const watch_t *w, const struct inotify_event *ev); /* WATCH_EV_* bits */
int  watch_make_dir(const watch_t *w, mode_t mode);         /* 0 / -1 errno; no-op for kinds without a dir */
```

- [ ] **Step 1: Write the failing tests** in `tests/test_watch.c` (mkdtemp under /tmp, own `inotify_init1(IN_NONBLOCK)` fd, a `drain(fd, w)` helper that reads all events and ORs `watch_event` results, calling `watch_rearm` whenever REARM is set and ORing its result):
  - `test_parse`: `exists:/a` ok kind EXISTS; each kind name ok; refuse `exists:rel`, `bogus:/a`, `exists:`, `/a`, `exists_glob:/a*/b`, a 300-byte path.
  - `test_changed_once`: arm `changed:$T/f` (f exists); `write+close` f → across the drained batch exactly one `watch_event` call returns FIRE (the parent ignores content events; the leaf reports CLOSE_WRITE).
  - `test_modified_write`: `modified` fires on a write without close (IN_MODIFY on leaf).
  - `test_rename_over`: write `$T/f.new`, `rename` onto f → FIRE; after rearm `wd_leaf` names the new inode (stat ino of path matches `inotify` re-add returning the wd it holds: assert a later write to f fires again).
  - `test_symlink_leaf`: f is a symlink to a; `symlink` b to `$T/f.tmp` + rename onto f (what `ln -sfn` does) → FIRE; a write to `a` → no FIRE (link watched, not target).
  - `test_deep_parent`: watch `changed:$T/a/b/c/file` with only `$T` existing; mkdir a, a/b, a/b/c draining after each (REARM each, no FIRE); create file → FIRE once.
  - `test_symlink_ancestor`: `$T/real` dir, `$T/link -> real`; `exists:$T/link/x`; create `$T/real/x` → FIRE.
  - `test_level`: `exists`, `exists_glob:$T/d/*.q`, `dir_not_empty:$T/e` false then true after create; `watch_level_true` matches.
  - `test_dir_recreated`: `dir_not_empty:$T/e`; rmdir e, mkdir e, drain (REARM), create e/x → FIRE.
  - `test_sibling_noise`: `changed:$T/f`; 100 writes to `$T/g` → every `watch_event` NONE.
  - `test_make_dir`: `dir_not_empty:$T/m/n` mode 0700 → n 0700, m 0755; `exists_glob:$T/g/*.q` → `$T/g` exists, no `*.q` entry; `changed:` → nothing created.

- [ ] **Step 2: Run** `make test 2>&1 | grep -A3 watch` — Expected: compile failure (watch.h missing).

- [ ] **Step 3: Implement `watch.c`.** Arming walks PATH from `/` with `stat` (follows symlinks) to the deepest existing directory, adds it with `WATCH_INO_MASK` (no IN_DONT_FOLLOW), records `depth`; if the leaf exists and kind is changed/modified/dir_not_empty, adds it with `WATCH_INO_MASK | IN_DONT_FOLLOW`. `watch_event` rules:
  - `IN_Q_OVERFLOW` (wd -1): REARM, plus FIRE for edge kinds.
  - On `wd_parent` with depth 1: name must equal the leaf; structural masks (CREATE/DELETE/MOVED_TO/MOVED_FROM) → REARM, plus FIRE for edge kinds (never content masks — the leaf wd reports those). For exists_glob: any structural event in the dir → FIRE if `watch_level_true`.
  - On `wd_parent` with depth > 1: name must equal the next component; structural → REARM.
  - On `wd_leaf`: DELETE_SELF/MOVE_SELF/IN_IGNORED → REARM (+FIRE edge); CLOSE_WRITE/ATTRIB → FIRE for changed and modified; MODIFY → FIRE for modified; for dir_not_empty, CREATE/MOVED_TO → FIRE if `watch_level_true`.
  - Level kinds: any FIRE is gated by `watch_level_true`.

- [ ] **Step 4: Run** `make test 2>&1 | grep -E 'watch|FAIL'` — Expected: `all watch tests passed`, no FAIL.

- [ ] **Step 5: Commit** `git add watch.[ch] tests/test_watch.c Makefile && git commit -m "watch.c: path watches for watch_path= (audit 4.8)"`

### Task 2: .svc keys and validation

**Files:**
- Modify: `service.h` (fields, `SVC_PATH (1 << 10)`, `EXC_TRIGGER_LIMIT` at the end of the excise enum), `service.c` (parser beside `parse_sched_key`, cross-key check in `svc_finalize`, defaults in `svc_init_defaults`, `svc_finalize` sets `SVC_PATH` when `watch_count`)
- Test: `tests/test_svc_watch.c`, `Makefile`

**Interfaces:**
- Consumes: `watch_parse`, `watch_t`, `MAX_WATCH`.
- Produces (service_t): `watch_t watch[MAX_WATCH]; uint8_t watch_count; uint8_t make_directory; uint16_t directory_mode; int trigger_burst; int trigger_interval_sec;` runtime: `uint8_t path_due; uint8_t path_rerun; int trigger_count; struct timespec trigger_window; time_t path_last_fire;`

- [ ] **Step 1: Write failing tests** `tests/test_svc_watch.c` (same `load()` helper shape as `tests/test_svc_sched.c`):
  - `watch_path=exists:/run/x` → `watch_count==1`, `flags & SVC_PATH`, defaults `directory_mode==0755`, `trigger_burst==200`, `trigger_interval_sec==2`.
  - 8 watch_path lines ok; a 9th refuses.
  - empty `watch_path=` in a later line clears the list (drop-in reset, like other list keys).
  - `make_directory=1`, `directory_mode=0700`, `trigger_limit_burst=4`, `trigger_limit_interval_sec=1800` parse.
  - refuse: `watch_path=exists:rel`, a 300-byte path, `directory_mode=0999`, `trigger_limit_burst=0`, `watch_path=` + `listen=stream:/run/s`, `watch_path=` + `on_boot_sec=5`, `+ on_active_sec=5`, `+ on_calendar=03:00`.
  - Each refusal prints a line containing the key name (capture stderr via a pipe/dup2, `strstr`).

- [ ] **Step 2: Run** `make test 2>&1 | grep -A3 svc-watch` — Expected: FAIL / no such fields.

- [ ] **Step 3: Implement** parsing (`parse_watch_key(svc, key, val)` → 0/-1, same dispatch as `parse_sched_key`) and the finalize check (`"%s: watch_path= cannot be combined with listen= or timers"`).

- [ ] **Step 4: Run** `make test 2>&1 | grep -E 'svc-watch|FAIL'` — Expected: pass.

- [ ] **Step 5: Commit** `git commit -am "watch_path=, make_directory=, directory_mode=, trigger_limit_*= keys"` (add the new test file).

### Task 3: engine — arm, route, fire, rerun, limit

**Files:**
- Modify: `engine.c` (ready_watch mask + `ready_wd`; `ready_inotify_drain` → `ino_drain`; arm in `engine_arm_timers`, `handle_reload`, the `schema-ctl add` path; PERFECT case; death path at `timer_rearm` call ~1165; condition-skip at ~1599; `wake_min` loop ~3093; backstop at the `ready_recheck_all` call; excise name table ~543; status detail), `service.h` (`int ready_wd`), `reexec_state.h` (runtime fields), the systemd1 shim's excise→Result map (`trigger-limit-hit`)
- Test: `tests/test_watch_path_live.sh` (shape of `tests/test_exec_dirs_live.sh`), `Makefile` (after `test_exec_dirs_live.sh`)

**Interfaces:**
- Consumes: Task 1 `watch_*`, Task 2 fields.
- Produces: `static void path_arm(service_t *svc)` (make_directory, `watch_arm` each, then `path_due = any level true`); `static void path_fire_check(service_t *svc)` (trigger window/limit, sets EXCISED+`EXC_TRIGGER_LIMIT`+rail `path-trigger-limit` or NEW_PROCESS+rail `path-fire KIND:PATH`); rail and status strings per Global Constraints.

Behaviour to implement (the spec's rules, pinned):
- `ready_watch` uses `WATCH_INO_MASK` and stores the wd in `svc->ready_wd`; the drain schedules the ready recheck only for an event on a `ready_wd` or `IN_Q_OVERFLOW`.
- `ino_drain` walks each event over every SVC_PATH service's watches; FIRE → `path_due=1` (or `path_rerun=1` if `child_pid > 0`); REARM → `watch_rearm`, OR its result.
- PERFECT + SVC_PATH + `path_due` → `path_fire_check`; clear `path_due`.
- On death (beside the timer branch): state PERFECT, rail `path-done`/`path-failed`, then `path_due = path_rerun || any level watch true`; `path_rerun = 0`.
- Condition-skip of a path service returns to PERFECT, same as a timer.
- Trigger window: `trigger_window` start + `trigger_interval_sec`; `trigger_count > trigger_burst` inside it → park. `schema-ctl start`/`restart` clears count and window and re-arms.
- Dependents see an armed path service as PERFECT (extend the `SVC_TIMER` check at ~3756 to `SVC_TIMER | SVC_PATH`).
- Backstop tick: every SVC_PATH service with a level watch whose `wd_parent < 0` or every READY_BACKSTOP_MS → `watch_rearm` all, re-evaluate level.
- reexec_state.h: `X(U8, path_due) X(U8, path_rerun) X(INT, trigger_count) X(TS, trigger_window) X(TIME, path_last_fire)`. wds are never carried (new fd).
- Status: one line per watch `  watch  KIND:PATH`, plus `last fire` when set.

- [ ] **Step 1: Write the failing live test** `tests/test_watch_path_live.sh` (schema-user, XDG under `$T`, skip as root): services
  - `ex` `exists:$T/flag` oneshot writes a count file and deletes the flag → after `touch flag`, count 1; touch again → count 2.
  - `dne` `dir_not_empty:$T/q` + `make_directory=1` `directory_mode=0700` → `$T/q` is 0700 at start; a file dropped in is consumed (service `rm`s it) once.
  - `ch` `changed:$T/conf` → one `printf > conf` gives exactly one run (count file == 1 after 1 s settle).
  - `slow` `changed:$T/s`, service sleeps 2 s: two writes while running → exactly 2 runs total.
  - `selfloop` `changed:$T/loop`, service appends to `$T/loop`, `trigger_limit_burst=5` → status shows `trigger-limit-hit` within 5 s; `rm $T/loop` → still excised; `schema-ctl --user start selfloop` → runs again.
  - `ready` with `ready_path=$T/rp/ready` alongside `ch2` `changed:$T/rp/other`: 200 writes to other don't flap `ready`.
  - reload: `schema-ctl --user reload` while `slow` runs with a pending rerun → the rerun still happens.
  - backstop: `ex2` `exists:$T/bm/flag` where `$T/bm` doesn't exist at start; `mkdir -p $T/bm && touch $T/bm/flag` → fires (REARM path through the parent).

- [ ] **Step 2: Run** `make schema-user schema-ctl && sh tests/test_watch_path_live.sh` — Expected: FAIL (key unknown / never ran).

- [ ] **Step 3: Implement** the engine glue above.

- [ ] **Step 4: Run** `make test && XDG_RUNTIME_DIR=/run/user/$(id -u) sh .github/ci-python-suite.sh` — Expected: all green, `test_watch_path_live: ok`.

- [ ] **Step 5: Commit** `git commit -am "engine: watch_path= services (audit 4.8)"` (add the live test).

### Task 4: importer and shim

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-import.py` (`path_to_svc` beside `timer_to_svc`; system import branch at ~2545 for `.path`; user loop at ~3087 and `_user_translate`/`_user_activates`; `_UNIT_SUFFIXES` already has `.path`), `systemctl_shim.h` (drop `.path` from `unit_supported`; `unit_queue_name` strips `.path` like `.timer`)
- Test: `tests/test_import_units.py`, `tests/test_import_user.py`, `tests/test_systemctl_shim.c`

**Interfaces:**
- Produces: `path_to_svc(name, path_sections, svc_sections, known=None, unit=None) -> str` (raises `Skip`).

- [ ] **Step 1: Write failing tests:**
  - `PathExists=/run/x` + `x.service` → `.svc` contains `watch_path=exists:/run/x` and the service's exec.
  - Each of PathExistsGlob/PathChanged/PathModified/DirectoryNotEmpty maps to its kind; `MakeDirectory=yes` → `make_directory=1`; `DirectoryMode=0700`; `TriggerLimitBurst=4`, `TriggerLimitIntervalSec=30 minutes` → `trigger_limit_interval_sec=1800`.
  - `Unit=other.service` is used; missing service → skipped `"path's other.service not found"`.
  - `%v` in a Path*= → skipped, reason mentions `%v`.
  - target service is socket-activated (sibling .socket) or has a .timer → skipped `"watch_path cannot combine with socket or timer activation"`.
  - `systemd-ask-password-console.path` → skipped (denied list).
  - user: drkonqi shape (`PathExistsGlob=%C/drkonqi/sentry-envelopes/*`) → `%C` expanded to the user's cache dir.
  - shim: `unit_supported("foo.path") == 1`; `enable foo.path` queues `foo`.

- [ ] **Step 2: Run** `python3 -m pytest tests/test_import_units.py tests/test_import_user.py -q` and `make test 2>&1 | grep shim` — Expected: new cases FAIL.

- [ ] **Step 3: Implement** `path_to_svc` (reuse `_specifiers`, the timer's condition merge, and `unit_to_svc` for the service body), the import branches, and the shim change.

- [ ] **Step 4: Run** both commands plus the full python suite — Expected: green.

- [ ] **Step 5: Commit** `git commit -am "schema-import, shim: take .path units as watch_path= (audit 4.8)"`

### Task 5: docs, draft PR, review, live PID 1

**Files:** `docs/svc-reference.md` (every new key), `docs/roadmap.md` (tick the watch_path item), `docs/superpowers/specs/2026-11-12-full-audit-spec.md` (mark 4.8 done)

- [ ] **Step 1:** Write the docs; commit `docs: watch_path= reference`.
- [ ] **Step 2:** `git push -u origin feat/watch-path && gh pr create --draft` (body ends with the Claude Code attribution line).
- [ ] **Step 3:** One `/code-review`; fix what survives; `make test` + python suite green before each push.
- [ ] **Step 4:** Live on blakbox PID 1: build, `sudo schema-ctl reexec <branch binary>`, `schema-ctl add` one test .svc per kind under `/run/wp-test/`, exercise each, `schema-ctl status` shows the watches, then remove them and `sudo schema-ctl reexec` back to packaged; confirm 67 healthy.
- [ ] **Step 5:** Wait for Jonathan's word to merge. After merge: `gh run list --workflow deb.yml -L1` green.
