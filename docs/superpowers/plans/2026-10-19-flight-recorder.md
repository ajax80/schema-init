# Flight Recorder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When something takes the desktop down or chokes the box, `schema-board` shows one arrow line naming what happened and who did it, and `schema-ctl trace` shows the story.

**Architecture:** A new opt-in daemon `schema-recorder` (C) receives one-line datagrams from PID 1, schema-coredump, the autologin script and earlyoom, runs its own PSI triggers with async `/proc` hog sampling, correlates, and publishes a small arrow header + event ring in its own shm segment `/schema-recorder`, plus daily event files. Pure logic lives in header-only `recorder.h` / `recorder_logic.h` (the `coredump.h` / `notify.h` pattern) so it is unit-tested with injected time.

**Tech Stack:** C99 + `_GNU_SOURCE`, no new libraries; bash for the autologin/earlyoom senders; tests are standalone C programs wired into `make test`, plus shell integration scripts.

**Spec:** `docs/superpowers/specs/2026-10-18-flight-recorder-design.md`

## Global Constraints

- No `schema_shm_t` layout bump — `SCHEMA_SHM_MAGIC` stays `0x53434d03`.
- PID 1 never blocks on or depends on the recorder: `SOCK_DGRAM|SOCK_NONBLOCK`, `MSG_DONTWAIT`, failures ignored silently.
- Zero recorder wakeups while calm: `poll(..., -1)`; `timerfd` disarmed when no episode is active.
- Socket `/run/schema-init/recorder.sock`, `AF_UNIX`/`SOCK_DGRAM`, mode 0600.
- Shm `/schema-recorder`, `fchmod(fd, 0644)` after `shm_open`.
- PSI: CPU `"some 250000 1000000"`, memory `"some 200000 1000000"`, written with the trailing NUL (`sizeof`, not `strlen`).
- Hog sampling: snapshot 1 → arm 1 s timerfd → snapshot 2; re-sample every 20 s during an episode; episode ends after 30 s with no trigger fire.
- CPU/memory episode earns an arrow only if it lasts ≥ 120 s or overlaps a crash, kill, service death or session end.
- Correlation window: 20 s before a session exit.
- Arrow: bright while live and for 12 h after `resolved_ts`; grey until 36 h; then off the board. Board computes this from `now - resolved_ts`.
- Precedence: live problem > session crash > critical-service death > minor failure; newest wins within a tier.
- Events files `/var/lib/schema-init/events-YYYY-MM-DD.log`, uncompressed, recorder prunes > 7 days; reload last 36 h into the ring, 7 days into culprit counts.
- Culprit identity: `comm` for hogs/crashes/kills, service name for PID 1 events.
- Opt-in: ship `services/schema-recorder.svc.example`; enable on blakbox, Eli, Optiplex only.
- Rail log lines dated: `[YYYY-MM-DD HH:MM:SS]`.
- No comments/docstrings beyond the surrounding code's density.

## Review Focus

1. Hostile process names/cmdlines (spaces, newlines, `=`, empty — earlyoom warns `EARLYOOM_NAME` is user-controlled) must not inject fields or split a datagram; `msg=` is always last and runs to end of line.
2. Recorder dead, socket missing, or receive buffer full: PID 1's send fails silently with no stall, no rail spam.
3. Wall clock jumps (NTP step at boot, RTC wrong): `now < resolved_ts` shows as held-bright, never negative durations; episode durations use `CLOCK_MONOTONIC`.
4. Truncated/corrupt last line in an events file (power loss mid-write): loader skips that line and keeps the rest.
5. Recorder restarted mid-episode: an episode left "live" by the old instance is marked resolved at reload time, so no arrow stays red forever.

---

### Task 1: Shared types, wire format, send helper — `recorder.h`

**Files:**
- Create: `recorder.h`
- Create: `tests/test_recorder.c`
- Modify: `Makefile` (`test:` target — add the line, same pattern as `test_reclaim.c`)

**Interfaces:**
- Produces:
  - `#define REC_SOCK_PATH "/run/schema-init/recorder.sock"`, `REC_SHM_NAME "/schema-recorder"`, `REC_SHM_MAGIC 0x53434652u` ("SCFR"), `REC_RING 128`, `REC_EVENTS_DIR "/var/lib/schema-init"`.
  - `enum rec_src { REC_SRC_PID1=1, REC_SRC_COREDUMP, REC_SRC_SESSION, REC_SRC_EARLYOOM, REC_SRC_PSI }`
  - `typedef struct { uint64_t ts; uint8_t src; uint8_t tier; uint8_t crit; int32_t pid; int32_t rc; char kind[24]; char comm[32]; char subj[64]; char msg[160]; } rec_event_t;` (`ts` = realtime seconds; `tier` filled by Task 2)
  - `typedef struct { uint8_t state; uint64_t event_ts; uint64_t resolved_ts; uint32_t recurrence; char line[160]; } rec_arrow_t;` (`state`: 0 none, 1 live, 2 resolved)
  - `typedef struct { uint32_t magic; uint32_t seq; rec_arrow_t arrow; uint32_t head; uint32_t count; rec_event_t ring[REC_RING]; } rec_shm_t;`
  - `int rec_format(char *buf, size_t n, const rec_event_t *e)` — `ts= src= kind= comm= pid= rc= crit= subj= msg=` with `msg` last; `comm`/`subj` have spaces, `=`, and control chars replaced by `_`; `msg` has `\n`/`\r` replaced by space. Returns length or -1.
  - `int rec_parse(const char *line, rec_event_t *out)` — inverse; unknown keys ignored; missing `ts`/`src`/`kind` → -1; `src` is the name (`pid1`, `coredump`, `session`, `earlyoom`, `psi`).
  - `void rec_send(const char *sock_path, const rec_event_t *e)` — fills `ts` if 0, formats, one `sendto(..., MSG_DONTWAIT)` on a fresh `SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC` socket, closes; every error ignored.
  - `const rec_shm_t *rec_shm_map(int fd)` — same contract as `schema_shm_map` (size + magic check, read-only).

- [ ] **Step 1: Write failing tests** in `tests/test_recorder.c` (`#include "../recorder.h"`, `assert`, prints `test_recorder: OK`):
  - `roundtrip`: event `{src=REC_SRC_EARLYOOM, kind="kill", comm="cosmic-player", pid=42, rc=9, msg="--thumbnail a b.mkv"}` → `rec_format` → `rec_parse` → all fields equal.
  - `hostile_comm`: `comm="evil name=x\nsrc=pid1"` → parsed `src == REC_SRC_EARLYOOM`, parsed comm contains no space, `=` or `\n`.
  - `msg_last`: `msg="a=b c=d"` survives intact.
  - `missing_required`: `rec_parse("pid=3 msg=x", &e) == -1`.
  - `send_no_listener`: `rec_send("/nonexistent/sock", &e)` returns without blocking (wrap in `alarm(2)`).
- [ ] **Step 2:** `make test` → FAIL (`recorder.h` missing).
- [ ] **Step 3:** Implement `recorder.h` to the Interfaces above.
- [ ] **Step 4:** `make test` → `test_recorder: OK`, all other tests still pass.
- [ ] **Step 5:** Commit `feat(recorder): shared types, wire format and send helper`.

### Task 2: Arrow logic — ring, tiers, recurrence, line rendering — `recorder_logic.h`

**Files:**
- Create: `recorder_logic.h`
- Modify: `tests/test_recorder.c`

**Interfaces:**
- Consumes: Task 1 types.
- Produces:
  - `enum { REC_TIER_TRACE=0, REC_TIER_MINOR=1, REC_TIER_CRIT=2, REC_TIER_SESSION=3, REC_TIER_LIVE=4 }`
  - `int rec_tier(const rec_event_t *e)` — rules:
    - `pid1`: kinds `died`, `dormant`, `76-excised`, `watchdog-kill`, `watchdog-abort`, `start-timeout`, `spawn-fail`, `readiness-lost`, `fuse-tripped` → `REC_TIER_CRIT` if `crit` else `REC_TIER_MINOR`; `survival-enter` → `REC_TIER_MINOR`; anything else → TRACE.
    - `session` kind `plasma-exit`: `rc != 0` → SESSION; `rc == 0` → TRACE (Task 3 may upgrade by correlation).
    - `coredump`: SESSION if `comm` is in the session set (`kwin_wayland`, `plasmashell`, `ksmserver`, `startplasma-way`, `plasma_session`, `Xwayland`) or `subj` (owning service) non-empty → CRIT; else TRACE. (My call: plain app crashes like obsidian's SIGBUS at shutdown would otherwise light the board constantly.)
    - `earlyoom` kind `kill` → MINOR.
    - `psi`: set by Task 3.
  - `void rec_ring_push(rec_shm_t *m, const rec_event_t *e)` — seqlock: `seq++` (odd) → write → `seq++` (even), with `__atomic` release ordering.
  - `int rec_snapshot_arrow(const rec_shm_t *m, rec_arrow_t *out)` / `int rec_snapshot_ring(const rec_shm_t *m, rec_shm_t *out)` — retry ≤ 100 while seq odd or changed; -1 if never stable.
  - `typedef struct { char key[64]; uint64_t ts[32]; int n; } rec_culprit_t;` and `int rec_culprit_count(rec_culprit_t *tab, int *ntab, int max, const char *key, uint64_t ts, uint64_t now)` — records `ts`, drops entries older than `now - 7*86400`, returns count in window.
  - `void rec_render(char *buf, size_t n, const rec_event_t *e, uint32_t recurrence)` — `"▶ HH:MM <what> — <detail>"` + `" (2nd time this week)"` when recurrence ≥ 2 (`3rd`, `4th`… ordinal). `<what>`: session → `desktop session ended`; coredump → `<comm> crashed (SIG<x>)`; earlyoom → `earlyoom killed <comm>`; pid1 → `<subj> <kind>`; psi → `CPU pinned`/`memory pressure` + ` <N> min by <comm> ×<count>`.
  - `int rec_arrow_pick(const rec_arrow_t *cur, const rec_event_t *cand, uint64_t now)` — 1 if `cand` should replace `cur`: higher tier wins; equal tier → newer wins; `cur` older than 36 h after resolution always loses.
  - `int rec_arrow_shade(const rec_arrow_t *a, uint64_t now)` — 0 none, 1 bright, 2 grey: live → 1; resolved and `now < resolved_ts` → 1; `< 12*3600` → 1; `< 36*3600` → 2; else 0.

- [ ] **Step 1: Failing tests:**
  - `tier_app_crash_trace`: coredump `comm="obsidian"`, empty subj → `REC_TIER_TRACE`.
  - `tier_kwin_session`: coredump `comm="kwin_wayland"` → `REC_TIER_SESSION`.
  - `tier_logout`: session `plasma-exit rc=0` → TRACE; `rc=1` → SESSION.
  - `pick_precedence`: CRIT arrow at t=100, MINOR candidate at t=200 → 0; SESSION candidate at t=50 → 1.
  - `shade`: resolved at 1000 → shade at 1000+11h59m = 1, +12h01m = 2, +36h01m = 0; `now = 500` (clock went back) → 1.
  - `recurrence`: `cosmic-player` at day 0, 2, 5 → third call returns 3; at day 8 → 3 (day 0 dropped: 2, 5, 8); at day 13 → 2 (8, 13).
  - `render_example`: psi event, comm `cosmic-player`, count 8, 20 min, 21:42 local → line equals `"▶ 21:42 CPU pinned 20 min by cosmic-player ×8"` (set `TZ=UTC` in the test and build ts accordingly).
  - `seqlock`: push 300 events → `count == REC_RING`, newest at `(head-1) % REC_RING`, `seq` even.
- [ ] **Step 2:** `make test` → FAIL.
- [ ] **Step 3:** Implement `recorder_logic.h`.
- [ ] **Step 4:** `make test` → PASS.
- [ ] **Step 5:** Commit `feat(recorder): arrow tiers, recurrence, rendering, seqlock ring`.

### Task 3: Episodes, hog ranking, correlation — `recorder_logic.h`

**Files:**
- Modify: `recorder_logic.h`, `tests/test_recorder.c`

**Interfaces:**
- Produces:
  - `typedef struct { int32_t pid, ppid; char comm[32]; uint64_t ticks; } rec_proc_t;` (utime+stime from `/proc/PID/stat`)
  - `typedef struct { char comm[32]; int count; uint64_t ticks; int32_t top_pid; } rec_hog_t;`
  - `int rec_hogs(const rec_proc_t *a, int na, const rec_proc_t *b, int nb, rec_hog_t *out, int max)` — delta ticks per pid present in both (pid+comm match), grouped by comm, sorted by ticks desc; `top_pid` = biggest pid in the group.
  - `typedef struct { uint8_t active; uint8_t kind; uint64_t start_mono, last_fire_mono, start_wall; rec_hog_t top; char chain[96]; char cmdline[96]; uint8_t overlapped; } rec_episode_t;` (`kind` 1 cpu, 2 mem)
  - `void rec_episode_fire(rec_episode_t *ep, int kind, uint64_t mono, uint64_t wall)` — starts or extends.
  - `int rec_episode_check(rec_episode_t *ep, uint64_t mono)` — 1 when `mono - last_fire_mono >= 30` (ended), else 0.
  - `int rec_episode_arrow(const rec_episode_t *ep, uint64_t end_mono)` — 1 if `end_mono - start_mono >= 120` or `overlapped`.
  - `int rec_correlate(const rec_shm_t *m, const rec_episode_t *ep, const rec_event_t *session_exit, char *why, size_t n)` — scans ring for coredump/earlyoom/pid1 events with `ts` in `[exit.ts-20, exit.ts]`, or an active/just-ended episode overlapping that window; writes the cause text (`"CPU pinned 20 min by cosmic-player ×8"`, `"kwin_wayland crashed (SIGSEGV)"`, `"earlyoom killed plasmashell"`); returns 1 if found. A found cause upgrades a `rc=0` exit to `REC_TIER_SESSION` and sets the episode's `overlapped`.

- [ ] **Step 1: Failing tests:**
  - `hogs_group`: 8 `cosmic-player` pids each +135 ticks and one `kwin_wayland` +20 → `out[0]` = `{cosmic-player, 8, 1080}`; pid reused with different comm is excluded.
  - `episode_short_no_arrow`: fire at mono 0..90 every 1 s, check at 121 → ended, `rec_episode_arrow` = 0.
  - `episode_long_arrow`: fire 0..1200, ended at 1231 → arrow 1.
  - `episode_overlap_arrow`: 60 s episode with `overlapped=1` → arrow 1.
  - `correlate_kwin`: ring has coredump kwin_wayland at T-5; session exit rc=0 at T → returns 1, why contains `kwin_wayland`.
  - `correlate_none_logout`: nothing in window (crash at T-25) → 0.
  - `correlate_episode`: active cpu episode, session exit inside it → why starts `CPU pinned`.
- [ ] **Step 2:** `make test` → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make test` → PASS.
- [ ] **Step 5:** Commit `feat(recorder): episodes, hog ranking, session correlation`.

### Task 4: The daemon — `schema-recorder.c`

**Files:**
- Create: `schema-recorder.c`
- Create: `tests/test_recorder_daemon.sh`
- Modify: `Makefile` (`BINS`, a `schema-recorder:` rule like `schema-board:`, `test:` runs the script after building), `clean:`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces: binary `schema-recorder` with modes:
  - `schema-recorder` — daemon.
  - `schema-recorder send key=val ...` — joins args into one wire line, validates with `rec_parse`, `rec_send`. Exit 0 always (senders must not fail).
  - Env overrides for tests only: `SCHEMA_RECORDER_SOCK`, `SCHEMA_RECORDER_SHM`, `SCHEMA_RECORDER_DIR`, `SCHEMA_RECORDER_NOPSI=1`, `SCHEMA_RECORDER_FAKETIME=<epoch>` (offset applied to realtime).

Daemon behaviour (decisions the code must follow):
- Startup: create dir, prune `events-*.log` older than 7 days, load last 36 h into ring (skip unparsable lines), feed 7 days into culprit table; any arrow whose episode was live at load → `resolved_ts = now` (Review Focus 5). Bind socket (unlink stale first, chmod 0600, parent `/run/schema-init` 0755), `shm_open` + `ftruncate(sizeof(rec_shm_t))` + `fchmod 0644` + map, publish.
- PSI: open both triggers; on failure log one line `schema-recorder: pressure capture off (<reason>)` and continue.
- Loop: `poll` on socket, 2 PSI fds, timerfd, timeout -1.
  - Datagram → `rec_parse` → `rec_tier` → correlate if session exit → append to today's file (`O_APPEND`, one `write` per line, then `fdatasync` only for tier ≥ CRIT) → `rec_ring_push` → culprit count → if `rec_arrow_pick`, render and publish arrow.
  - PSI POLLPRI → `rec_episode_fire`; if no episode sample pending, take snapshot 1 (scan `/proc/[0-9]*/stat`, cap 4096 procs) and arm timerfd 1 s.
  - Timerfd → snapshot 2, `rec_hogs`, store top in episode with parent chain (walk `ppid` up to 4 levels, `comm ← comm ← comm`) and `/proc/top_pid/cmdline` (NULs → spaces, 96 bytes); push a `psi` `episode-start` trace event the first time; re-arm 20 s while active; when `rec_episode_check` says ended → push `episode-end` with tier from `rec_episode_arrow` (LIVE while active is reflected in the arrow `state=1`; on end `state=2, resolved_ts=now`), disarm timer.
- Arrow `state=1` (live) is set while a qualifying episode is active (≥120 s elapsed or overlapped); other events publish `state=2` with `resolved_ts = ts`.
- `SIGTERM` → unlink socket, exit 0 (shm stays for the board).

- [ ] **Step 1: Write `tests/test_recorder_daemon.sh`** (runs unprivileged in a temp dir with the env overrides and `SCHEMA_RECORDER_NOPSI=1`):
  - start daemon, `schema-recorder send src=coredump kind=crash comm=kwin_wayland pid=7 rc=11 msg=SIGSEGV`, then `send src=session kind=plasma-exit rc=0`; read shm via `./schema-ctl trace` (Task 6 — until then via a tiny `od` check of `seq` change; replace with `schema-ctl trace` assertion in Task 6) and assert the events file contains both lines.
  - send 1000 datagrams in a burst → daemon still alive, file line count ≥ 1 (no crash under flood).
  - append a truncated line to the events file, restart daemon → still loads, ring count > 0 (Review Focus 4).
  - stop the daemon, run `schema-recorder send ...` → exits 0 within 1 s (Review Focus 2).
  - idle check: daemon running 10 s with no input → `voluntary_ctxt_switches` delta in `/proc/PID/status` ≤ 1.
- [ ] **Step 2:** Run it → FAIL (no binary).
- [ ] **Step 3:** Implement `schema-recorder.c`.
- [ ] **Step 4:** `make test` → script prints `test_recorder_daemon: OK`.
- [ ] **Step 5:** Commit `feat(recorder): schema-recorder daemon`.

### Task 5: Senders — PID 1, coredump, autologin, earlyoom; dated rail lines

**Files:**
- Modify: `service.c:1622-1640` (`service_log`)
- Modify: `init.c` near `execute_survival_posture(1)` / `(0)` call sites (~3958) — send on posture change only
- Modify: `schema-coredump.c:371-378`
- Modify: `distros/fedora-kde/scripts/schema-plasma-autologin.sh` (after `printf 'plasma_exited rc=%d\n' $RC`)
- Create: `scripts/schema-recorder-earlyoom`
- Modify: `distros/fedora-kde/hosts/blakbox/services/earlyoom.svc` and its drop-in args (append `-N /usr/local/bin/schema-recorder-earlyoom` unless `-N` already present)
- Modify: `tests/test_recorder.c`

**Interfaces:**
- Consumes: `rec_send`, `rec_event_t`, `rec_tier` (Task 1/2).
- Produces: `int rec_forward_kind(const char *event)` in `recorder.h` — 1 for exactly the PID 1 kinds listed in Task 2's `rec_tier` pid1 rule, else 0.

- [ ] **Step 1: Failing tests** in `tests/test_recorder.c`: `forward_kinds` — `died`,`dormant`,`76-excised`,`watchdog-kill` → 1; `spawn`,`promote`,`timer-failed`,`ctl-restart`,`oneshot-done` → 0.
- [ ] **Step 2:** `make test` → FAIL.
- [ ] **Step 3:** Implement:
  - `rec_forward_kind`.
  - `service_log`: date format `"[%04d-%02d-%02d %02d:%02d:%02d] ..."`; then if `rec_forward_kind(event)` → `rec_send(REC_SOCK_PATH, {src=pid1, kind=event, subj=svc->name, pid=child_pid, rc=svc->exit_status, crit=!!(svc->flags & SVC_CRITICAL)})`. Before changing the format, `grep -rn 'rail.log\|\[[0-9][0-9]:' --exclude-dir=.git .` and keep every consumer working (Greg checked `tests/livetest/vmtest.sh:674` — unaffected).
  - init.c: on survival posture change send `kind=survival-enter` / `survival-leave`.
  - schema-coredump: after each `syslog` call, `rec_send` with `src=coredump kind=crash comm=c.comm pid=c.pid rc=c.sig subj=c.service msg="SIG<x>"`.
  - autologin: `schema-recorder send src=session kind=plasma-exit rc=$RC 2>/dev/null || :` guarded by `command -v schema-recorder`.
  - `scripts/schema-recorder-earlyoom`: `exec schema-recorder send src=earlyoom kind=kill pid="$EARLYOOM_PID" comm="$EARLYOOM_NAME" msg="$EARLYOOM_CMDLINE"` (one argv element per field; the recorder sanitizes — Review Focus 1).
- [ ] **Step 4:** `make test` → PASS; `make schema-init` clean with `-Wall -Wextra`; `bash -n` on both scripts; `scripts/schema-drift` clean for blakbox.
- [ ] **Step 5:** Commit `feat(recorder): PID 1, coredump, session and earlyoom senders; dated rail lines`.

### Task 6: Surfaces — board arrow and `schema-ctl trace`

**Files:**
- Modify: `schema-board.c` (map recorder shm optionally; render)
- Modify: `schema-ctl.c` (local `trace` command, like `cat`/`graph`; usage text)
- Modify: `completion/schema-ctl.bash`, `completion/_schema-ctl`
- Modify: `tests/test_recorder_daemon.sh`

**Interfaces:**
- Consumes: `rec_shm_map`, `rec_snapshot_arrow`, `rec_snapshot_ring`, `rec_arrow_shade` (Tasks 1–2).

Decisions:
- Board: open `/schema-recorder` at start and again whenever it isn't mapped (retry at most every 10 s); missing or magic mismatch → no arrow, no error message. Arrow line sits directly above the service table header; shade 1 → bold red, 2 → grey (`\033[90m`), 0 → no line. Truncate to `cols - 2` (from `TIOCGWINSZ`, default 80) counting UTF-8 code points. Mark dirty when arrow seq changes or the shade changes (the existing 5 Hz loop is enough; no new timer).
- `schema-ctl trace [--since <epoch|HH:MM>]`: no PID 1 round-trip, no root. Prints ring oldest→newest, one event per block: `YYYY-MM-DD HH:MM:SS  <src>  <kind>  <comm/subj>  pid=  rc=` then indented `msg`. Missing shm → `schema-ctl: no flight recorder running (enable schema-recorder.svc)`, exit 1.

- [ ] **Step 1: Failing test:** extend `tests/test_recorder_daemon.sh` — after the kwin crash + `plasma-exit rc=0`, `./schema-ctl trace` output contains `kwin_wayland` and `plasma-exit`; `./schema-board --once` output (with `SCHEMA_RECORDER_SHM` override) contains `desktop session ended` and `kwin_wayland`; with `SCHEMA_RECORDER_FAKETIME` +37 h the board shows no arrow line. Board honours `SCHEMA_RECORDER_SHM` too.
- [ ] **Step 2:** Run → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make test` → PASS. Manually: `schema-board --once` on blakbox (no recorder yet) renders exactly as before.
- [ ] **Step 5:** Commit `feat(recorder): board arrow and schema-ctl trace`.

### Task 7: Packaging and docs

**Files:**
- Create: `services/schema-recorder.svc.example`
- Modify: `schema-init.spec` (`core_bins`, `%files` for `/usr/bin/schema-recorder`, `/usr/local/bin/schema-recorder-earlyoom`; `%changelog`)
- Modify: `Makefile` `install:` (earlyoom hook to `/usr/local/bin`)
- Modify: `docs/operations.md`, `README.md` (short "Flight recorder" section after "Limp mode")

`schema-recorder.svc.example`: `name=schema-recorder`, `exec=/usr/bin/schema-recorder`, `needs_root=1`, `critical=0`, `no_new_privs=1`, `private_tmp=1`, `protect_system=full`, `protect_home=1` (it reads `/proc` and writes `/var/lib/schema-init`, `/run`, `/dev/shm` only). Builder confirms `/var/lib` stays writable under `protect_system=full` in this codebase; if not, drop to `protect_system=0` and say why in the commit.

- [ ] **Step 1:** `rpmbuild`-free check: `make install DESTDIR=$(mktemp -d)` lists `schema-recorder` and the example; `tests/test_doctor_hardening.py` passes (all four knobs pinned).
- [ ] **Step 2:** Commit `build(recorder): package schema-recorder (opt-in .svc.example), docs`.

### Task 8: Proof and measurement (needs Jonathan for reboots)

**Files:**
- Create: `tests/livetest/recorder-replay.sh`
- Create: `tests/livetest/recorder-bench-<host>-<date>.txt` (results)

- [ ] **Step 1: Replay the 09-22 incident** on blakbox without touching the live desktop: start a second recorder instance with test env overrides and real PSI; in a scratch cgroup (`cpu.max` capped so the box stays usable) spawn 8 busy-loop children under a 3-level parent chain named `dolphin → kioworker → cosmic-player` (`exec -a`); hold 150 s; then `schema-recorder send src=session kind=plasma-exit rc=0` into the test socket. Expected: `schema-board --once` (test shm) shows `▶ HH:MM desktop session ended — CPU pinned 2 min by cosmic-player ×8`; `schema-ctl trace` shows the chain and cmdline; a repeat run shows `(2nd time this week)`. A 60 s run with no session exit → trace only, no arrow.
- [ ] **Step 2: Baseline** (before enabling): on blakbox and Eli record 3 boots of `schema-ctl analyze` userspace time, PID 1 `VmRSS`.
- [ ] **Step 3:** Install the build, `schema-ctl add schema-recorder` (copy the example to `/etc/schema-init/services/schema-recorder.svc` first), and with Jonathan's go-ahead reboot 3× each; record the same numbers plus recorder `VmRSS` and its `voluntary_ctxt_switches` delta over 10 calm minutes. Also enable on Optiplex (no reboot needed to start it).
- [ ] **Step 4:** Write the numbers and LOC added per task (`git diff --stat master`) into the bench file and report them to Jonathan. Kill criteria: boot regression beyond noise, PID 1 RSS change beyond noise, recorder RSS ≥ 2 MB, idle wakeups > ~1/10 min.
- [ ] **Step 5:** Commit results; open the branch PR as draft, run `/code-review`, then ready/merge on Jonathan's word.
