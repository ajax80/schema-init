# Verify-notify Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A verify FAIL reaches the desktop as one schema-doctor notification within seconds; desktop Restart stays ungated, and the docs say why.

**Architecture:** `verify_run` writes `/run/schema-init/verify-status` (real runs only). schema-doctor gains a `verify` check that reads it, with a per-result colour override (RED for FAIL, AMBER for CHANGE/UNRESOLVED). A `changed:` watch service runs `schema-doctor --verify-only`, which touches only the verify entry; both doctor runs serialize on a flock.

**Tech Stack:** C99 (verify.c, schema-ctl), Python 3 (schema-doctor), .svc, Markdown.

**Spec:** `docs/superpowers/specs/2026-11-27-verify-notify-design.md`

## Global Constraints

- Status file: `/run/schema-init/verify-status`, mode 0644, written temp + `rename`, `/run/schema-init` created if missing.
- Contents, one per line: `exit=N`, `ts=<epoch>`, `result=<the Result: line, without "Result: ">`, then up to 5 `fail=<text of a [FAIL] line>`.
- Exit 1 → RED (popup). Exit 2, 3 → AMBER (no popup). Exit 0 or no file → GREEN.
- Desktop Restart/Shut Down never blocked or delayed; no new periodic verify.
- The service copies `schema-doctor-periodic.svc`'s flags exactly.
- No docstrings; why-comments only.

## Review Focus

1. Verify run as a normal user: no status file written, no error. (Task 1 test)
2. Garbage or truncated status file (no `exit=`): doctor reads it as GREEN-with-note, never crashes the whole doctor run. (Task 2 test)
3. `--verify-only` while another check is RED in flap state: verify going GREEN must not send "all clear" for the system. (Task 3 test)
4. Two FAIL runs in a row (same RED): one popup, not two. (Task 3 test)
5. `--accept` after a CHANGE: status flips to `exit=0`, doctor goes GREEN. (Task 1 test)

---

### Task 1: verify records its result

**Files:**
- Modify: `verify.h:9-14` (field), `verify.c:84-110` (`verify_opts_env`), `verify.c:~880-917` (write after the accept block, before return)
- Test: `tests/test_verify_cli.sh`

**Interfaces:**
- Produces: `const char *status_path` in `verify_opts_t`. `verify_opts_env` default: `"/run/schema-init/verify-status"` in a shipped build; in a `-DSCHEMA_VERIFY_TESTHOOKS` build, `SCHEMA_VERIFY_STATUS` or NULL (never the host file). `static void status_write(const char *path, int code, const char *result, char *const *fails, int nfails)`.

- [ ] **Step 1: Failing tests in `tests/test_verify_cli.sh`** (reuse existing fixtures that already produce exit 0, 1 and 2):
  - `SCHEMA_VERIFY_STATUS=$T/st/verify-status` (dir `$T/st` absent) with the FAIL fixture → file exists, `grep -qx 'exit=1'`, a `^ts=[0-9]+$` line, `^result=1 fail` line, at least one `^fail=` line; mode `644`.
  - the CHANGE fixture → `exit=2`; then `--accept` → `exit=0` and `result=` contains `(accepted)`.
  - unset `SCHEMA_VERIFY_STATUS` → no file anywhere under `$T` named `verify-status`.
- [ ] **Step 2: Run** `make test-verify` (or `sh tests/test_verify_cli.sh build/test/schema-ctl-testhooks` after building it). Expected: FAIL on the first new check.
- [ ] **Step 3: Implement.** Collect the `[FAIL]` texts and the Result line as they are printed; compute the exit code once, call `status_write` when `o->status_path` is set and (shipped build) `geteuid()==0`, then return the code. In the testhooks build skip the euid test (the CLI tests are unprivileged). Write failure is silent: the verdict on stdout and the exit code are what gates read.
- [ ] **Step 4: Run** the same command. Expected: PASS, and the rest of `test_verify_cli.sh`/`test_verify_gate.sh` still PASS.
- [ ] **Step 5: Commit** `verify: record each real run's result in /run/schema-init/verify-status`.

### Task 2: doctor `verify` check with a colour override

**Files:**
- Modify: `scripts/schema-doctor.py` (`Finding`, `CheckResult`, `run_checks` reported branch, `result_color`, new check class + `REGISTRY.append`)
- Test: Create `tests/test_doctor_verify.py` (pattern: `tests/test_doctor_notify.py`, `DOCTOR_ROOT` tmpdir); add to the Makefile line that runs the other `test_doctor_*.py`.

**Interfaces:**
- Produces: `Finding.color: str = ""`, `CheckResult.color: str = ""`; `result_color(r)` returns `r.color` when non-empty. `class VerifyStatus(Check)`: `name = "verify"`, `grade = DEFERRED`, `detect()` returns None (GREEN) or `Finding(detail, healable=False, color=RED|AMBER)`. `VERIFY_STATUS = "run/schema-init/verify-status"` (relative to `ROOT`).

- [ ] **Step 1: Failing tests** in `tests/test_doctor_verify.py`, each writing `$DOCTOR_ROOT/run/schema-init/verify-status` then `r = sd.run_checks([sd.VerifyStatus()], heal=False, force=set())[0]`:
  - no file → `sd.result_color(r) == sd.GREEN`
  - `exit=0` → GREEN
  - `exit=1` + `fail=next boot: init= missing` → RED, `"init= missing" in r.detail`, `"schema-ctl verify" in r.detail`
  - `exit=2` → AMBER; `exit=3` → AMBER
  - `"garbage\n"` → GREEN, no exception
  - an existing DEFERRED check without a color still reports AMBER (override is opt-in)
- [ ] **Step 2: Run** `python3 tests/test_doctor_verify.py`. Expected: FAIL (`VerifyStatus` missing).
- [ ] **Step 3: Implement.** Pass `f.color` into the reported `CheckResult`. RED detail: first `fail=` line, then `— run schema-ctl verify`. AMBER detail: the `result=` text. `summary = "the last verify run found nothing that breaks the next boot"`.
- [ ] **Step 4: Run** it plus `tests/test_doctor_status.py`, `tests/test_doctor_engine.py`. Expected: PASS.
- [ ] **Step 5: Commit** `schema-doctor: verify check (RED on a FAIL, AMBER on change/unresolved)`.

### Task 3: `--verify-only`, the lock, the boot reset

**Files:**
- Modify: `scripts/schema-doctor.py` (`main` arg parsing ~1562, the run block ~1598-1623, `FlapState.load` ~230, `notify_transitions` ~1511)
- Test: `tests/test_doctor_verify.py`

**Interfaces:**
- Consumes: `VerifyStatus` (Task 2).
- Produces: CLI `schema-doctor --verify-only`. `FlapState.locked()`: context manager taking `fcntl.flock(LOCK_EX)` on `ROOT/var/lib/schema-init/doctor-state.lock` around load→notify→save; `--periodic` and `--verify-only` both use it. `notify_transitions(results, flap, enabled, partial=False)`: with `partial=True` the overall is computed from every stored `last_state` after updating the given results, and `last_overall` is left untouched unless that overall changed.

- [ ] **Step 1: Failing tests** (monkeypatch `sd.notify_send` to append to a list):
  - stored `checks["power"].last_state = RED`, `last_overall = RED`; verify RED then `--verify-only`-style call with verify GREEN → popups are `["schema-doctor: verify"]` then nothing titled `all clear`; `last_overall` still RED.
  - all others GREEN: verify RED → one popup; second identical RED run → no new popup; verify GREEN → `schema-doctor: all clear`.
  - `--verify-only` leaves `doctor-status.json`'s other entries in place (seed it with a `power` entry; after the run it is still there and a `verify` entry exists), and does not create/modify `var/log/schema-init/doctor-report.txt`.
  - boot reset: `last_state` RED for verify under `boot_id` A; change boot_id to B; `FlapState.load()` → verify `last_state == GREEN`, and a GREEN run sends no popup.
  - lock: `FlapState.locked()` acquired in the test, a child `python3 schema-doctor.py --verify-only` started with `DOCTOR_ROOT`; it is still running after 0.5 s; releasing lets it exit 0.
- [ ] **Step 2: Run** `python3 tests/test_doctor_verify.py`. Expected: FAIL.
- [ ] **Step 3: Implement.** `--verify-only` runs `run_checks([VerifyStatus()], heal=False, ...)`, patches the verify entry into the existing status JSON/text (rebuild text from the patched JSON's checks), skips `write_report`, and prints its one result. Boot reset sets `last_state = GREEN` only for `"verify"` (its file lives in `/run`).
- [ ] **Step 4: Run** all `tests/test_doctor_*.py`. Expected: PASS.
- [ ] **Step 5: Commit** `schema-doctor: --verify-only, flock on flap state, verify resets per boot`.

### Task 4: the watch service, install lists, docs

**Files:**
- Create: `distros/fedora-installer/rail/services/schema-doctor-verify.svc`
- Modify: `distros/fedora-installer/migrate/schema-migrate.py:115` (`DOCTOR_TIMERS`), `distros/fedora-installer/migrate/prevent-set.list:36`, `schema-init.spec` (wherever `schema-doctor-periodic.svc` is installed/listed, if it is), `docs/operations.md` (the gates paragraph's last sentence), `docs/security/threat-model.md` (logind power methods), the schema-doctor docs page that lists checks (grep for `schema-doctor-periodic` in `docs/`)
- Test: `tests/test_migrate_advanced_flags.py:131-133`

**Interfaces:**
- Consumes: `schema-doctor --verify-only` (Task 3).

- [ ] **Step 1: Failing tests**: default migrate installs `schema-doctor-verify.svc`; `no-doctor-timers` does not. Plus `./schema-init --check` on the new file loads clean (add to the same test or run in step 4).
- [ ] **Step 2: Run** `python3 tests/test_migrate_advanced_flags.py`. Expected: FAIL.
- [ ] **Step 3: Implement.** The .svc: `name=schema-doctor-verify`, `exec=/usr/local/bin/schema-doctor`, `args=--verify-only`, `watch_path=changed:/run/schema-init/verify-status`, then `schema-doctor-periodic.svc`'s flags verbatim from `needs_root=1` down. Docs: operations.md replaces "The desktop menu's shutdown goes through schema-logind and is not gated." with: not gated on purpose (Plasma ends the session before it calls `Reboot`/`PowerOff`, so a refusal would protect nothing and strand the user; verify is too slow for `CanReboot`), and instead every real verify run records its result and schema-doctor posts a desktop notification on a FAIL within seconds (AMBER for change/unresolved shows in `schema-doctor --status` only; `notify=no` in doctor's config turns popups off). Threat model: one line under logind's power methods pointing at that paragraph.
- [ ] **Step 4: Run** the migrate test, `./schema-init --check distros/fedora-installer/rail/services/schema-doctor-verify.svc` (expected: loads, no warning), then `make test`. Expected: all PASS.
- [ ] **Step 5: Commit** `schema-doctor-verify.svc: notify on a verify FAIL; document the ungated desktop restart`.

### Task 5: end to end on blakbox (no reboot)

- [ ] Install the branch's `schema-doctor` and `schema-doctor-verify.svc` by hand; `sudo schema-ctl start schema-doctor-verify` is not needed (the watch arms at load: `sudo schema-ctl reload` or the documented reload path; never restart session daemons).
- [ ] Write a fake `exit=1` status as root (`printf 'exit=1\nts=0\nresult=1 fail\nfail=test\n' | sudo tee /run/schema-init/verify-status.tmp && sudo mv ... verify-status`): a notification appears within ~5 s; `schema-doctor --status` shows verify RED. Then `sudo schema-ctl verify --quiet` (real run) rewrites it: "all clear" appears if it passes.
- [ ] Record the result in the PR description.
