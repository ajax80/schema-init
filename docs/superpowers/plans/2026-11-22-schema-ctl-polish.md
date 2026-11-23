# schema-ctl polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `schema-top` (a top view of schema-board with CPU%/MEM from cgroups), colour in `schema-ctl status` on a terminal, and `schema-ctl log [-f] [-n N] <name>`.

**Architecture:** All client-side; PID 1 is untouched. A shared `state_colors.h` holds the state palette for the board and schema-ctl. The top view lives in `board_top.c`/`.h`, linked into schema-board and selected by argv[0] = `schema-top`. Log reading lives in `ctl_log.c`/`.h`, linked into schema-ctl.

**Tech Stack:** C99 (`-D_GNU_SOURCE`), POSIX shm, inotify, cgroup v2 files; tests in C and sh (repo patterns), Python pty for key handling.

**Spec:** `docs/superpowers/specs/2026-11-22-schema-ctl-polish-design.md`

## Global Constraints

- The board view (`schema-board`, `--tty`, `--interactive`, `--once`) keeps its layout, timing and output byte-for-byte; `tests/test_board_look.sh` passes unchanged.
- Top-view rows fit 80 visible columns: `SERVICE` (22, cut) `STATE` (11) `PID` (7) `CPU%` (6) `MEM` (6) `RST` (4).
- CPU% = Δ`usage_usec` / elapsed `CLOCK_MONOTONIC` µs × 100 (100% = one CPU); samples keyed by service name; deltas over < 100 ms keep the previous value.
- Refresh every 1 s and on a `seq` change. Default sort CPU descending.
- Colour never for `--json`, `--kv`, non-tty stdout, `NO_COLOR` set, or `TERM=dumb`.
- Log path: `/var/log/schema-init/<name>.log` and `/run/log/schema-init/<name>.log`, newer mtime wins; `--user` → `$XDG_STATE_HOME/schema-user/log/<name>.log` (`$HOME/.local/state` when unset). Default 50 lines; `-n 0` = whole file. EACCES → `needs root: sudo schema-ctl log <name>`.
- No new socket calls from schema-board in any view.

## Review Focus

- A service name with control bytes in the top view must render as `?` like the board does (no escape injection).
- A terminal narrower than 80 columns (60x20) must not wrap top-view lines: clip each row to the width.
- No shm table, or a table from another layout (magic mismatch): `schema-top` says so and exits 1, no crash.
- During `log -f`, logrotate renames the file and the new one appears seconds later: keep following, print the new lines, never exit.
- `status` output where a service name itself contains `state=` (names come from file names): only colour a known state word that follows ` state=`.

---

### Task 1: Shared state palette

**Files:**
- Create: `state_colors.h`
- Modify: `schema-board.c` (replace its `st_color()` with the header), `Makefile` (deps for schema-board, schema-ctl)
- Test: `tests/test_state_colors.c`, `Makefile` test target

**Interfaces:**
- Produces: `static inline const char *state_ansi(uint8_t state)` — the SGR string the board uses today (PERFECT `\033[35m`, FULL_TRUST/FUNDAMENTAL/SETTLED `\033[32m`, NEW_PROCESS `\033[2;32m`, RECOVERY/FRICTION `\033[33m`, DORMANT `\033[34m`, EXCISED `\033[31m`, else `\033[37m`); `static inline int state_from_name(const char *name, size_t len)` — the state enum for a name such as `PERFECT`, or -1.

- [ ] **Step 1: Write the failing test** — `tests/test_state_colors.c` asserts each mapping above, `state_from_name("FRICTION", 8) == STATE_FRICTION`, `state_from_name("PERFECTX", 8) == -1`, and `state_ansi(state_from_name("EXCISED", 7))` is `"\033[31m"`.
- [ ] **Step 2: Run** `make test` (add the build+run line next to the other unit tests). Expected: FAIL, `state_colors.h` missing.
- [ ] **Step 3: Implement** `state_colors.h` (names from schema-board's `st_name()` table); schema-board includes it and calls `state_ansi()` where it called `st_color()`.
- [ ] **Step 4: Run** `make test` and `sh tests/test_board_look.sh`. Expected: both pass; board output unchanged.
- [ ] **Step 5: Commit** `board: move the state palette to state_colors.h`.

### Task 2: Colour in `schema-ctl status`

**Files:**
- Create: `ctl_colour.h`
- Modify: `schema-ctl.c` (the `printf("%s", rbuf)` path for `status`), `Makefile`
- Test: `tests/test_ctl_colour.c`

**Interfaces:**
- Consumes: `state_ansi()`, `state_from_name()` (Task 1).
- Produces: `void status_colourize(const char *in, FILE *out)` — copies `in` to `out`, wrapping each known state word that directly follows ` state=` as `state=` + `state_ansi(s)` + word + `\033[0m`; everything else verbatim. `int colour_wanted(int fd)` — 1 iff `isatty(fd)`, `NO_COLOR` unset, and `TERM` set and not `dumb`.

- [ ] **Step 1: Write the failing test** — `tests/test_ctl_colour.c` (write to `open_memstream`):
  - `"  a  pid=1  state=PERFECT  restarts=0\n"` → `"  a  pid=1  state=\033[35mPERFECT\033[0m  restarts=0\n"`
  - `"  state=x  pid=1  state=EXCISED\n"` → only `EXCISED` wrapped (`state=x` at line start has no leading space and `x` is unknown)
  - `"  b  state=BOGUS\n"` unchanged; empty input → empty output
  - `colour_wanted` with `NO_COLOR=1` → 0; with `TERM=dumb` → 0 (use a pty via `openpty` for the tty case → 1 with `TERM=xterm`).
- [ ] **Step 2: Run** `make test`. Expected: FAIL, `ctl_colour.h` missing.
- [ ] **Step 3: Implement** both functions; in schema-ctl, for `status` with argc == 2 or `status <svc>` (not `--json`/`--kv`), call `status_colourize(rbuf, stdout)` when `colour_wanted(1)`, else the existing `printf`.
- [ ] **Step 4: Run** `make test`; then by hand `sudo ./schema-ctl status | head -3` (plain) and `sudo ./schema-ctl status` on the terminal (coloured). Expected: as described.
- [ ] **Step 5: Commit** `schema-ctl: colour state in status on a terminal`.

### Task 3: `schema-ctl log [-f] [-n N] <name>`

**Files:**
- Create: `ctl_log.c`, `ctl_log.h`
- Modify: `schema-ctl.c` (usage text + dispatch before the socket request, like `cat`/`graph`), `Makefile` (`SCHEMA_CTL_SRCS`)
- Test: `tests/test_ctl_log.sh` (run against `$(TESTDIR)/schema-ctl-testhooks`), wired into `make test` next to `test_verify_cli.sh`

**Interfaces:**
- Produces: `int log_pick(const char *name, int user, char *out, size_t n)` — 0 and the path, or -1 with errno `ENOENT`; `int log_tail(int fd, long lines, FILE *out)` — last `lines` lines (0 = all) by reading backwards in 4096-byte chunks; `int log_follow(const char *path, FILE *out)` — never returns on its own; `int ctl_log_main(int argc, char **argv, int user)` — the subcommand, exit status 0 / 1 / 64.
- Under `SCHEMA_VERIFY_TESTHOOKS` only: env `SCHEMA_CTL_LOG_VAR` and `SCHEMA_CTL_LOG_RUN` replace the two system log dirs.

- [ ] **Step 1: Write the failing test** — `tests/test_ctl_log.sh` with temp dirs `V` and `R`:
  - 120 numbered lines in `V/a.log` → `log a` prints lines 71–120 exactly; `log -n 5 a` prints 116–120; `log -n 0 a` prints all 120
  - a file with no trailing newline → last line still printed
  - only `R/b.log` exists → it is used; both `V/c.log` (older, `touch -d '1 hour ago'`) and `R/c.log` (newer) → the `R` one
  - `log nope` → exit 1, stderr contains `no log for nope`
  - `chmod 000 V/d.log` (skip if run as root) → exit 1, stderr `needs root: sudo schema-ctl log d`
  - `log` with no name → exit 64
  - follow: start `log -f e` in the background writing to `$T/out`; append `one`; `mv V/e.log V/e.log.1`; after 1 s create `V/e.log` with `two`; append `three` to the new file; within 3 s `$T/out` contains `one`, `two`, `three`; then kill it
  - `--user log f` with `XDG_STATE_HOME=$T/state` reads `$T/state/schema-user/log/f.log`
- [ ] **Step 2: Run** `sh tests/test_ctl_log.sh build/test/schema-ctl-testhooks`. Expected: FAIL (unknown command `log`).
- [ ] **Step 3: Implement** `ctl_log.c`. Follow: print from the current end, `inotify_add_watch(dir, IN_MODIFY|IN_CREATE|IN_MOVED_TO|IN_MOVED_FROM|IN_DELETE)`; on any event for the name, `fstat` the open fd vs `stat(path)`: a different inode → drain the old fd, reopen from offset 0; a size below the read offset → seek to 0 (truncate). Missing file after rotation → keep waiting.
- [ ] **Step 4: Run** the test again, then `make test`. Expected: PASS.
- [ ] **Step 5: Commit** `schema-ctl: log [-f] [-n N] <name>`.

### Task 4: Top view — sampling and one snapshot (`schema-top --once`)

**Files:**
- Create: `board_top.c`, `board_top.h`
- Modify: `schema-board.c` (argv[0] basename `schema-top` → top view), `Makefile` (schema-board sources/deps)
- Test: `tests/test_board_top.c` (pure functions), `tests/test_board_top.sh` (`--once` against a fake shm + fake cgroup dir)

**Interfaces:**
- Consumes: `state_ansi()` (Task 1); `schema_shm_t` (`schema_shm.h`).
- Produces:
  - `typedef struct { char name[64]; uint64_t usage_usec; int64_t mono_us; double cpu; long long mem; int have; } top_sample_t;`
  - `double top_cpu_pct(uint64_t prev_usec, uint64_t cur_usec, int64_t elapsed_us, double keep)` — returns `keep` when `elapsed_us < 100000` or `cur_usec < prev_usec`, else the percentage.
  - `int top_read_cgroup(const char *cgroot, const char *svc, uint64_t *usage_usec, long long *mem)` — 0 or -1.
  - `void top_sort(const schema_shm_t *s, const top_sample_t *samp, int *order, int n, int key, int reverse)` — `key` is `TOP_SORT_CPU`, `TOP_SORT_MEM` or `TOP_SORT_NAME`; ties by name.
  - `void top_render(FILE *out, const schema_shm_t *s, const top_sample_t *samp, const int *order, int n, int key, int reverse, int first, int rows, int cols)` — header + rows `first..first+rows-1`, each clipped to `cols`.
  - `int top_main(int argc, char **argv)` — the top view; `--once` samples twice 0.5 s apart.
  - Env: `SCHEMA_BOARD_CGROOT` replaces `/sys/fs/cgroup/schema-init` (the board already uses `SCHEMA_BOARD_SHM`).
- [ ] **Step 1: Write the failing tests**
  - `tests/test_board_top.c`: `top_cpu_pct(1000000, 1500000, 1000000, 9) == 50.0`; `top_cpu_pct(0, 2000000, 1000000, 0) == 200.0`; `top_cpu_pct(0, 999, 50000, 7.5) == 7.5` (too short); `top_cpu_pct(5, 4, 1000000, 3) == 3` (counter went back); `top_sort` by CPU orders a 3-service table `[a 10%, b 40%, c 40%]` as `b, c, a` (tie → name), reversed as `a, c, b`; by MEM and NAME likewise. `top_render` to a memstream with samples `cpu=0.0`, `cpu=60`, `cpu=150` → the 0.0 cell is wrapped in `\033[2m`, 60 in `\033[33m`, 150 in `\033[1;31m`; the header names the sort and the `CPU%` heading is bold (`\033[1m`); the header carries counts by state (e.g. `2 ok · 1 excised` for a table of two FUNDAMENTAL + one EXCISED) and the summed `MEM` of rows that have it.
  - `tests/test_board_top.sh`: reuse `test_board_look.sh`'s `mk` C helper (copy it in) for a 30-service table including `a-very-long-service-name-that-overflows` and `evil\033[2Jname`; fake cgroup dir with `memory.current` = `52428800` and `cpu.stat` `usage_usec 0` for service 0. Then `SCHEMA_BOARD_SHM=… SCHEMA_BOARD_CGROOT=… SCHEMA_BOARD_SIZE=80x25 ./schema-top --once`:
    - header contains `sort: CPU` and the service count
    - the row for service 0 shows `50.0M`; a service with no cgroup dir shows blank CPU%/MEM
    - widest visible line ≤ 80 (strip SGR as `test_board_look.sh` `vis_max` does); with `SCHEMA_BOARD_SIZE=60x20` ≤ 60
    - the long name is cut to 22 columns; no `\033[2J` after the first line; `evil?[2Jname` shown
    - `SCHEMA_BOARD_SHM=/nonexistent ./schema-top --once` → exit 1, stderr names the table
    - `./schema-board --once` output (same fixture) is byte-identical to before this task (save it at Step 2)
- [ ] **Step 2: Run** both tests (save the board's `--once` output first). Expected: FAIL (no `schema-top`, no `board_top.h`).
- [ ] **Step 3: Implement** `board_top.c`; `schema-board.c` `main` dispatches to `top_main` when `strcmp(basename(argv[0]), "schema-top") == 0`; the Makefile builds a `schema-top` symlink next to `schema-board` for tests.
- [ ] **Step 4: Run** both tests, `sh tests/test_board_look.sh`, `make test`. Expected: PASS.
- [ ] **Step 5: Commit** `schema-top: a top view of the board with CPU% and MEM`.

### Task 5: Top view — live loop, keys, scrolling

**Files:**
- Modify: `board_top.c`
- Test: `tests/test_board_top_keys.py` (pty)

**Interfaces:**
- Consumes: Task 4's functions.
- Produces: the live `top_main` loop — raw terminal (restored on exit and on SIGINT/SIGTERM), redraw every 1 s or on a `seq` change; keys `c` `m` `n` `r` `j`/`k` and Down/Up arrows, `q`; header `[first-last of N]` when rows < services; `--interactive` keeps the board's card prompt on the selected row (reuse schema-board's prompt code; not on by default).
- [ ] **Step 1: Write the failing test** — `tests/test_board_top_keys.py`: spawn `./schema-top` in a pty (`pty.fork`, `SCHEMA_BOARD_SIZE=80x12`, the Task 4 fixtures via env, 30 services); read a frame; assert `sort: CPU` and `[1-` in the header; send `m` → next frame has `sort: MEM`; send `j` ×5 → header first index is 6; send `r` → header shows reversed; send `q` → the process exits 0 within 2 s and the terminal attributes match those before start (`termios.tcgetattr` on the pty).
- [ ] **Step 2: Run** `python3 tests/test_board_top_keys.py`. Expected: FAIL (no live view yet).
- [ ] **Step 3: Implement** the loop with `poll()` on stdin (timeout = time left to the next 1 s tick), re-reading shm `seq` each wake.
- [ ] **Step 4: Run** the test, `make test-all`. Expected: PASS.
- [ ] **Step 5: Commit** `schema-top: live view, sort keys and scrolling`.

### Task 6: Packaging and docs

**Files:**
- Modify: `Makefile` (install: `ln -sf schema-board $(DESTDIR)$(BINDIR)/schema-top`), `schema-init.spec` (`%files`: `%{_bindir}/schema-top`), the deb packaging file that lists binaries (find with `grep -rn schema-board debian* packaging* 2>/dev/null`), `schema-ctl.c` usage (`log`), `docs/operations.md` (schema-top, `schema-ctl log`, colour), `docs/roadmap.md` (tick the schema-ctl polish line)
- Test: `make srpm` builds; `rpm -qlp` on the built RPM lists `/usr/bin/schema-top`

- [ ] **Step 1: Write the failing check** — `make srpm && rpmbuild --rebuild` (or the repo's local rpm target) then `rpm -qlp <rpm> | grep -x /usr/bin/schema-top`. Expected now: no match.
- [ ] **Step 2: Implement** the install line, `%files` entry, deb entry, usage text and docs.
- [ ] **Step 3: Run** the check again → match; `make test-all` → pass.
- [ ] **Step 4: Commit** `schema-top/log: package and document`.
