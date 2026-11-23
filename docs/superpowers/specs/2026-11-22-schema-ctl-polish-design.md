# schema-ctl polish — design (2026-11-22)

Roadmap item: `schema-top`, colour in `schema-ctl status` on a terminal, and
`schema-ctl log [-f] <svc>`. (`schema-ctl graph` already exists.)

## Why

Day-to-day operation of a schema-init box: see at a glance what is running and
what it costs, read a service's log without remembering paths, and spot a bad
state in `status` without reading every line. Jonathan asked for it as the
cheapest high-use item on the roadmap.

## Goals

- `schema-top`: a live, colour view over ssh with per-service memory and CPU.
- `schema-ctl status` coloured by state when stdout is a terminal.
- `schema-ctl log [-f] [-n N] <svc>` that keeps following across logrotate.

## Non-goals

- Per-process or per-thread views, history graphs, remote/fleet aggregation.
- A top view for `schema-user` (user services) — later, if wanted.
- journald semantics (priorities, boot ids, structured fields).

## Anti-goals

- schema-board must keep its limp-mode guarantees: read-only shm, no control
  socket, works while the socket is wedged. The top view adds only cgroup file
  reads — no socket calls, no `/proc` walk.
- Never colour output a script reads: `--json`, `--kv`, pipes, `TERM=dumb`,
  `NO_COLOR`.

## Approaches (schema-top)

- **A — chosen (Jonathan + Claire agreed):** extend `schema-board` with a top
  view and install it also as `schema-top`. One shm reader, one palette, one
  refresh loop. (The board view itself keeps its layout; see below.)
- B — a separate `schema-top` binary: cleaner name, but duplicates the shm
  reader, colours and refresh loop.

## Design

### schema-top (schema-board)

- **The board view is unchanged.** Invoked as `schema-board` (limp-mode board,
  `--tty`, `--interactive`, `--once`) the layout, timing and output stay exactly
  as today: its rows already use the 80-column budget (78-column cells, checked
  at 80 and 60 columns by `tests/test_board_look.sh`), and its `--once` must stay
  instant for the tests that call it repeatedly. *(Greg's review: width and
  `--once` traps.)*
- **The top view** (invoked as `schema-top`) has its own row layout that fits 80
  columns: `SERVICE` (22, cut) `STATE` (11) `PID` (7) `CPU%` (6) `MEM` (6)
  `RST` (4). WT and CARDS are dropped from this view; the board still shows them.
- `MEM` = `memory.current`; `CPU%` = the `usage_usec` delta in `cpu.stat` divided
  by the elapsed `CLOCK_MONOTONIC` time since that service's last sample (100% =
  one CPU, as `top`). Samples are kept by service name, not row index, since
  sorting reorders rows. A service with no cgroup or an unreadable file shows
  blank. A delta over less than 100 ms is not shown (keeps the previous value),
  so an early wake on a `seq` change can't spike or zero it.
- Refresh: every 1 s, and on a `seq` change.
- Header: system state, service count, counts by state (e.g. `62 ok · 1
  friction · 4 excised`), total supervised memory, and the active sort.
- Colour: state as on the board; CPU% dim at 0.0, yellow at 50%+, bold red at
  100%+ (a pinned core). The active sort column's header is bold.
- Keys: `c` CPU, `m` memory, `n` name, `r` reverse, `j`/`k` or arrows scroll
  when there are more services than rows (header shows `[1-18 of 47]`), `q`
  quit. Starts sorted by CPU, descending.
- `schema-top --once`: one snapshot with CPU% sampled over 0.5 s.
- `--interactive` (the board's card prompt on the selected row) works in the top
  view too, but only when asked for: a viewer never restarts anything by default.
- Packaging: `schema-top` installed as a symlink to `schema-board`
  (Makefile install, spec `%files`, deb).

### Colour in `schema-ctl status`

- Client-side: PID 1 sends plain text over the socket; schema-ctl colours the
  `state=` token on its way to stdout. PID 1 is untouched.
- On only when stdout is a tty, `NO_COLOR` is unset and `TERM` isn't `dumb`;
  never for `--json` / `--kv`.
- The state's value is coloured (`state=` + coloured `PERFECT`), with the same
  palette as schema-board, moved into one small header (`state_colors.h`) that
  both include so they can't drift. (The boot rail's console palette is a
  different thing — RGB values for the VT — and stays where it is.)

### `schema-ctl log [-f] [-n N] <svc>`

- Path: `/var/log/schema-init/<name>.log` and `/run/log/schema-init/<name>.log`
  (where PID 1 writes when /var isn't writable); if both exist, the newer by
  mtime. With `--user`, `$XDG_STATE_HOME/schema-user/log/<name>.log`. Because it
  goes by file name, `schema-ctl log rail` shows the boot rail's `rail.log` with
  no special case.
- Prints the last N lines (default 50; `-n 0` = whole file), found by reading
  backwards from the end in 4 KB chunks, so a huge log costs nothing extra.
- `-f` follows: inotify on the directory; on rename/create of the name (logrotate)
  or a truncation, reopen and continue from the start of the new file.
- System logs are `0640 root`: on EACCES say "needs root: sudo schema-ctl log
  <svc>", matching verify's wording. Unknown service with no log file: say so,
  exit 1.

## Review notes

Greg reviewed this design (2026-11-22). Taken: column budget, instant
board `--once`, monotonic CPU deltas keyed by name, scrolling, newer-mtime log
pick, `rail` via file name, backwards tail, header summary, CPU heat colours,
sort indicator, `r` reverse, shared palette header. Not taken: the card prompt
by default in schema-top (opt-in via `--interactive` instead — a viewer
shouldn't restart things on a stray Enter); sharing the palette with the boot
rail (different palette).

## Left to the builder

Exact header wording and colours within the rules above, test fixtures (fake cgroup dir via an env override, as other tests do).
