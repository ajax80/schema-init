# schema-dbus Live Reload Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** schema-dbus picks up D-Bus config changes live (policy + activation files), on SIGHUP, `ReloadConfig`, or by itself via inotify, without ever restarting or stalling the bus.

**Architecture:** Pure, unit-tested helpers in a new `sdbus_reload.h` (policy sanity check, live filter for raw `SystemdService=` entries). The broker's event loop gains one reload state machine fed by three triggers (SIGHUP in the existing signalfd, `ReloadConfig`, an inotify fd with a debounce deadline); the system-bus policy is re-dissolved in a child process reaped through the existing SIGCHLD path, then sanity-checked and swapped. The launcher exports the paths it resolved; schema-systemd1 routes `systemctl reload dbus` to `ReloadConfig`.

**Tech Stack:** C (single-threaded epoll/signalfd broker, header-only `sdbus_*.h`), POSIX sh launcher, Python 3 tests and schema-systemd1 (dbus-python).

**Spec:** `docs/superpowers/specs/2026-10-23-schema-dbus-live-reload-design.md`

## Global Constraints

- Anti-goal outranks everything: no reload path may stop, restart, re-exec or block the bus. A failed reload leaves the old policy and old activation table in place.
- No fdstore hand-off, no re-exec for config changes.
- Never ship `reload=1` in any `dbus.svc`.
- `ReloadConfig` stays root/own-uid only; replies empty on success, `org.freedesktop.DBus.Error.Failed` with the reason on failure, and only after the reload finished. Never wait on anything outside the broker before replying.
- Debounce: reload after **1000 ms** with no further inotify event. Dissolve timeout: **30 s**, then SIGKILL the child, reload fails.
- Sanity refusal: new rule count `< old / 2` (when old > 0), or any currently owned well-known name denied `own` for its primary owner's uid/gids.
- Raw `SystemdService=` entries: on the **system bus only**, a live reload never *adds* an entry that has `SystemdService=` unless it was read from `/usr/local/share/dbus-1/system-services` (override dir; env `SCHEMA_DBUS_OVERRIDE_DIR` overrides for tests). Changed/kept/removed entries and entries without `SystemdService=` apply live. Startup parsing is unchanged.
- Every reload logs exactly one stderr line: `schema-dbus: reload (<sighup|ReloadConfig|inotify>): ...` with counts on success, or the reason plus `keeping the old policy` on failure.
- Code style: match the `sdbus_*.h` header-only `static inline` idiom; no docstrings; why-comments only.

## Review Focus

1. **imsettings calls `ReloadConfig` from inside a dnf transaction** → it must return promptly (dissolve ≤ ~1 s) and never deadlock; covered by Task 2's "reply after swap" test with a dissolver that sleeps 0.5 s.
2. **A reload request arrives while a dissolve is already running** (inotify burst + a scriptlet's ReloadConfig) → one more reload after the current one, and every held ReloadConfig caller gets a reply; Task 2 test `concurrent_reload_requests`.
3. **The broker was started by hand / in tests with no `SCHEMA_DBUS_DISSECT`** → reload rebuilds only the activation table and succeeds; Task 2 test `reload_without_dissect_env`.
4. **Watched dir deleted or replaced** (`rm -rf` of a service dir, package removal) → no crash, reload still runs, table drops the removed entries; Task 3 test `watched_dir_removed`.
5. **SIGTERM (planned fdstore restart) while a dissolve child is running or a ReloadConfig reply is held** → the child is killed, held callers get an error before the hand-off, restart still lossless; Task 2 test `sigterm_during_reload`.

---

### Task 1: Pure reload helpers (`sdbus_reload.h`)

**Files:**
- Modify: `sdbus_activate.h` (add fields to `sdbus_svc_ent`, set them in both parse functions, free nothing new: `dir` is borrowed)
- Create: `sdbus_reload.h`
- Test: `tests/test_sdbus_reload.c`; add its compile+run line to the `test:` target in `Makefile` next to `test_sdbus_policy.c`, and add `sdbus_reload.h` to the `schema-dbus:` prerequisites.

**Interfaces:**
- Produces:
  - `sdbus_svc_ent` gains `int systemd_service;` (1 if the file had a non-empty `SystemdService=` line) and `const char *dir;` (the `dirs[di]` string it was read from; borrowed, valid as long as the dirs array).
  - `typedef struct { const char *name; int uid; const int *gids; int n_gids; } sdbus_owner;`
  - `static inline int sdbus_policy_rule_count(const sdbus_policy *p);` — sum of `n_rules` over contexts.
  - `static inline int sdbus_reload_policy_ok(const sdbus_policy *oldp, const sdbus_policy *newp, const sdbus_owner *owners, int n_owners, char *why, size_t why_len);` — 1 = swap allowed; 0 = refused with a one-line reason in `why` (e.g. `rules 412 -> 3`, `would deny own org.freedesktop.login1 to uid 0`). Build the `own` request exactly as `sdbus_driver.h:291` does.
  - `static inline int sdbus_svctab_hold_raw(sdbus_svctab *fresh, const sdbus_svctab *old, const char *override_dir);` — removes from `fresh` (freeing them) entries with `systemd_service && strcmp(dir, override_dir) && !sdbus_svctab_find(old, name)`; returns how many it held back.

- [ ] **Step 1: Write the failing tests** in `tests/test_sdbus_reload.c`, same `check()` style as `tests/test_sdbus_policy.c`:
  - `rule_count`: a policy text with 2 contexts of 3 and 2 rules → 5.
  - `ok_same_policy`: old == new text, owner `{"org.x", 0}` → 1.
  - `refuse_halved`: old 10 rules, new 4 → 0, `why` contains `10 -> 4`.
  - `allow_exactly_half`: old 10, new 5 → 1.
  - `refuse_own_lost`: old allows `own:org.freedesktop.login1` for uid 0, new omits it, owner `{"org.freedesktop.login1", 0}` → 0, `why` contains `org.freedesktop.login1`.
  - `old_empty_any_new_ok`: old 0 rules (no-policy default case), new 1 → 1.
  - `hold_raw_new`: temp dirs `ovr/` and `raw/`; `raw/a.service` has `Name=a`, `Exec=/bin/a`, `SystemdService=a.service`; old table empty; parse `{ovr, raw}` → after `hold_raw(fresh, old, ovr)` `a` is gone, return 1.
  - `keep_raw_known`: same, but old table has `a` → kept, return 0.
  - `keep_override_new`: `ovr/a.service` with `SystemdService=` → kept.
  - `keep_plain_new`: `raw/b.service` without `SystemdService=` → kept.
  - `fields_set`: parsed entry from `raw/` has `systemd_service == 1` and `dir` pointing at the `raw` string.

- [ ] **Step 2: Run to verify it fails**

  Run: `cc -std=c99 -Wall -Wextra tests/test_sdbus_reload.c -o /tmp/t-reload && /tmp/t-reload`
  Expected: compile error (`sdbus_reload.h` / new fields missing).

- [ ] **Step 3: Implement** the fields in `sdbus_activate.h` (parse `SystemdService=` alongside `Name=`/`Exec=`/`User=`) and the three functions in `sdbus_reload.h`.

- [ ] **Step 4: Run `make test`** — Expected: the new binary prints all PASS; every existing sdbus test still passes; 0 warnings.

- [ ] **Step 5: Commit** `schema-dbus: reload helpers (policy sanity check, raw SystemdService hold)`

---

### Task 2: Broker reload engine (SIGHUP + ReloadConfig + dissolve child)

**Files:**
- Modify: `schema-dbus.c` (signal mask, SIGCHLD branch, `ReloadConfig` handler at ~line 666, main-loop deadline, `store_and_exit`)
- Test: `tests/test_dbus_live_reload.py` (new; reuse `marshal`/`parse`/`Client` by importing `tests/test_dbus_fdstore_restart.py` with `importlib`, as `tests/test_systemd1_session.py` loads its module)

**Interfaces:**
- Consumes: Task 1's `sdbus_reload_policy_ok`, `sdbus_svctab_hold_raw`, `sdbus_owner`, `sdbus_policy_rule_count`.
- Produces (env the broker reads at start, used by Task 4's launcher):
  - `SCHEMA_DBUS_DISSECT` — dissolver path; absent ⇒ reloads rebuild the activation table only.
  - `SCHEMA_DBUS_SYSTEM_CONF` — default `/usr/share/dbus-1/system.conf`.
  - `SCHEMA_DBUS_CACHE` — launcher cache file to refresh after a successful swap (optional).
  - `SCHEMA_DBUS_OVERRIDE_DIR` — default `/usr/local/share/dbus-1/system-services`.
  - `SCHEMA_DBUS_POLICY` (existing) — the live policy file; a successful reload renames the new dissolve over it.
- Produces (internal, used by Task 3): `static void reload_request(const char *trigger);` — idempotent while a reload runs (sets dirty); and the main-loop deadline hook.

Behaviour to implement (decisions the tests pin):
- SIGHUP joins SIGCHLD/SIGTERM in the signalfd mask; reading it calls `reload_request("sighup")`.
- `ReloadConfig` (privileged) queues the caller `(conn id, serial)` on a held list and calls `reload_request("ReloadConfig")`; the reply is sent when the reload that *covers* the request finishes (a request arriving during a running dissolve is covered by the dirty follow-up run). Unprivileged → AccessDenied as today.
- A reload: rebuild the activation table into a fresh table; on the system bus apply `sdbus_svctab_hold_raw`. If `SCHEMA_DBUS_DISSECT` is set and this is the system bus: fork, child `execvp("python3", {"python3", dissect, system_conf})` with stdout to `<policy>.reload.tmp`, stderr inherited; record pid + 30 s deadline. When reaped (SIGCHLD branch, matched by pid before the activation lookup): exit 0 and non-empty file → read + `sdbus_policy_parse` → `sdbus_reload_policy_ok` against `g_policy` with owners built from `g_names` primary holders and their conns' uid/gids → swap `g_policy` and `g_svctab`, rename tmp over the policy file, copy to `SCHEMA_DBUS_CACHE` if set; any failure → free the new objects, unlink tmp, keep both old ones. Without a dissolve the table swap happens immediately.
- The activation table is swapped together with the policy (one outcome per reload).
- Timeout: deadline joins the epoll wait computation; expired → SIGKILL the child, fail with `dissolver timed out`.
- `store_and_exit` (SIGTERM): kill + reap a running dissolve child, reply Failed (`broker restarting`) to held callers, then proceed as today.
- Confirm (and fix if not) that no pending activation or held message keeps a pointer into the old `g_svctab` across the free.

- [ ] **Step 1: Write the failing tests** in `tests/test_dbus_live_reload.py` (system-bus broker on a scratch socket, scratch policy file, scratch service dirs, fake dissolver = a small python script in the temp dir that prints a policy text read from a file the test controls, optionally sleeping or exiting 1):
  - `sighup_does_not_kill`: SIGHUP the broker; process still alive; a fresh client can `Hello`; stderr has `reload (sighup)`.
  - `reload_config_replies_after_swap`: dissolver sleeps 0.5 s and emits a policy that additionally denies `send_destination:org.test.Deny`; call `ReloadConfig`; reply is METHOD_RETURN; a send to `org.test.Deny` right after the reply is denied.
  - `existing_connections_survive`: client A owns `org.test.Keep`; reload; A's name still owned (`GetNameOwner`), A can still call the driver.
  - `bad_dissolve_keeps_old`: dissolver exits 1 → `ReloadConfig` returns ERROR `org.freedesktop.DBus.Error.Failed`; old policy decisions unchanged; log has `keeping the old policy`.
  - `missing_python3_keeps_old`: run the broker with `PATH` lacking python3 → same as above, bus answers.
  - `dissolver_timeout_keeps_old`: dissolver sleeps 60 s; set `SCHEMA_DBUS_RELOAD_TIMEOUT_MS=500` (test-only env, default 30000) → Failed, child gone, bus answers.
  - `sanity_refuses_lost_own`: A owns `org.test.Keep` under a policy allowing it; new policy drops that `own` → Failed with `org.test.Keep` in the error; A still owns it.
  - `new_activation_file_after_reload`: drop `org.test.New.service` (no `SystemdService=`) into the svcdir, `ReloadConfig`, then a call to `org.test.New` auto-starts its `Exec=` (a script touching a marker file).
  - `raw_systemd_service_held`: drop `org.test.Raw.service` with `SystemdService=` into a non-override dir → after reload, calling it returns `ServiceUnknown`; then drop the same name into the override dir → after reload, it activates.
  - `reload_without_dissect_env`: broker started without `SCHEMA_DBUS_DISSECT` → `ReloadConfig` succeeds, new activation file picked up.
  - `concurrent_reload_requests`: dissolver sleeps 0.5 s; two clients call `ReloadConfig` 100 ms apart → both get METHOD_RETURN; log shows exactly two reloads.
  - `unprivileged_reload_denied`: unchanged behaviour (skip when the test runs as root, as other tests do).
  - `sigterm_during_reload`: dissolver sleeps 5 s; call `ReloadConfig`, then SIGTERM the broker → caller gets ERROR before the socket closes; no leftover dissolver process.

- [ ] **Step 2: Run to verify it fails** — `python3 tests/test_dbus_live_reload.py`; Expected: `sighup_does_not_kill` FAIL (broker dies), others FAIL/ERROR.

- [ ] **Step 3: Implement** the engine in `schema-dbus.c` per the behaviour list.

- [ ] **Step 4: Run** `make schema-dbus test && python3 tests/test_dbus_live_reload.py && python3 tests/test_dbus_fdstore_restart.py` — Expected: all PASS, 0 compiler warnings.

- [ ] **Step 5: Commit** `schema-dbus: live reload on SIGHUP and ReloadConfig, policy re-dissolved off the event loop`

---

### Task 3: Automatic pickup (inotify + debounce)

**Files:**
- Modify: `schema-dbus.c`
- Test: `tests/test_dbus_live_reload.py` (add cases)

**Interfaces:**
- Consumes: Task 2's `reload_request(const char *trigger)`.
- Produces: env `SCHEMA_DBUS_WATCH_EXTRA` (colon-separated extra paths, test-only, appended to the watch list) and `SCHEMA_DBUS_DEBOUNCE_MS` (default 1000, test-only override).

Behaviour:
- One `inotify_init1(IN_NONBLOCK|IN_CLOEXEC)` fd in epoll. Watch each existing path: every `g_svcdirs[i]`, the mask file's directory (filter events to the mask file's name), and on the system bus `/usr/share/dbus-1/system.conf`'s directory filtered to that name, `/usr/share/dbus-1/system.d`, `/etc/dbus-1`, `/etc/dbus-1/system.d`. Mask: `IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO|IN_CLOSE_WRITE|IN_ATTRIB|IN_DELETE_SELF|IN_MOVE_SELF`. Paths missing at start are not watched (spec: builder's call, skip).
- Any relevant event (and `IN_Q_OVERFLOW`) sets the debounce deadline to now + debounce; further events push it out. When it expires: `reload_request("inotify")`. `IN_IGNORED`/`IN_DELETE_SELF` on a dir: drop that watch, still schedule the reload.
- Inotify-triggered reloads log the same one line with trigger `inotify`.

- [ ] **Step 1: Write the failing tests:**
  - `inotify_new_activation_file`: drop `org.test.Auto.service` into the svcdir, wait 1.5 s with no signal or call → calling `org.test.Auto` activates it.
  - `inotify_policy_change`: with `SCHEMA_DBUS_WATCH_EXTRA` pointing at a temp `system.d` and the fake dissolver reading it, change the policy → within 2 s a previously allowed send is denied.
  - `inotify_burst_one_reload`: write 20 files 50 ms apart → after the quiet window, the log has exactly one `reload (inotify)` line.
  - `watched_dir_removed`: `rm -rf` a watched svcdir → broker alive, one reload, the dir's entries are no longer activatable.
  - `session_bus_inotify`: session-mode broker (`--session`-equivalent startup as `tests/sdbus_session_shim_check.sh` uses) picks up a new activation file without a dissolver.

- [ ] **Step 2: Run to verify they fail** — Expected: FAIL (nothing reloads without a trigger).

- [ ] **Step 3: Implement** the watch setup and debounce deadline in `schema-dbus.c`.

- [ ] **Step 4: Run** `python3 tests/test_dbus_live_reload.py && make test` — Expected: all PASS.

- [ ] **Step 5: Commit** `schema-dbus: notice D-Bus config changes on its own (inotify, debounced)`

---

### Task 4: Launcher exports, `systemctl reload dbus` route, docs

**Files:**
- Modify: `scripts/schema-dbus-run.sh` (before the final `exec "$BROKER" --system`: export `SCHEMA_DBUS_DISSECT="$DISSECT"`, `SCHEMA_DBUS_CACHE="$CACHE"`)
- Modify: `scripts/schema-systemd1.py` (`ReloadUnit`, and `CanReload` / `_read_can_reload` for `dbus.service`)
- Modify: `docs/components.md` (schema-dbus section: one paragraph on live reload, the three triggers, the sanity check and the raw-entry hold), `docs/svc-reference.md` only if it mentions dbus reload
- Test: `tests/test_dbus_gate.py` (launcher), `tests/test_systemd1_reload_dbus.py` (new)

**Interfaces:**
- Consumes: Task 2's env names.
- Produces: `ReloadUnit("dbus.service", mode)` calls `org.freedesktop.DBus.ReloadConfig` on the system bus (`dbus.SystemBus()`), raises the D-Bus error it got on failure, emits the usual job signals on success; `dbus.service` reports `CanReload` true regardless of `reload=` in its `.svc`. Every other unit keeps today's SIGHUP/`reload=1` logic.

- [ ] **Step 1: Write the failing tests:**
  - `test_dbus_gate.py`, in the broker-path block: `launcher exports dissolver and cache` — run the launcher with the existing stub broker that dumps its env; assert `SCHEMA_DBUS_DISSECT` equals the dissolver path the launcher found and `SCHEMA_DBUS_CACHE` equals the test's cache path.
  - `tests/test_systemd1_reload_dbus.py`: load `scripts/schema-systemd1.py` as `tests/test_systemd1_session.py` loads its module; monkeypatch the bus call; assert `ReloadUnit("dbus.service","replace")` calls `ReloadConfig` and never `os.kill`, even when the unit has a pid and no `reload=1`; assert a failing `ReloadConfig` raises; assert `ReloadUnit("foo.service")` without `reload=1` still raises the existing "does not support reload" error.

- [ ] **Step 2: Run to verify they fail.**

- [ ] **Step 3: Implement** the two exports and the `dbus.service` branch; write the docs paragraph.

- [ ] **Step 4: Run** `make test-all` — Expected: exit 0.

- [ ] **Step 5: Commit** `systemctl reload dbus calls ReloadConfig; launcher hands the broker its dissolver`

---

### Task 5: Hardware proof on blakbox (no code)

Runs after the branch is built as a local RPM on blakbox and blakbox has rebooted onto it (the running broker must be the new one; the old one dies on SIGHUP).

- [ ] **Step 1:** `sudo kill -HUP $(pgrep -xo schema-dbus)` with `--system` → broker pid unchanged, log line `reload (sighup): policy N rules, M activatable services`, `busctl --system list` unchanged, Plasma unaffected.
- [ ] **Step 2:** Drop `/etc/dbus-1/system.d/zz-schema-reload-test.conf` allowing `own` of `org.schema.ReloadTest` to root → within 2 s one `reload (inotify)` line; `sudo gdbus call --system ... RequestName org.schema.ReloadTest` style check (or a python one-liner) succeeds; delete the file → reload line, name request denied again.
- [ ] **Step 3:** `sudo dnf reinstall imsettings` → no "does not support reload" line in its scriptlet output; broker log shows `reload (ReloadConfig)` plus at most one `reload (inotify)`.
- [ ] **Step 4:** `sudo schema-ctl status` 0 failed; desktop checked by Jonathan.
- [ ] **Step 5:** Clean up the test conf; record results in NOW.
