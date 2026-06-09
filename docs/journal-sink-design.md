# schema-journal-sink — design notes (Track B)

**Track B (compatibility surface).** Makes unmodified userland believe `journald`
exists, without running journald or implementing the journal database. We provide
journald's *ingestion sockets*, drain them, and write plain text into schema-init's
existing log location. Wear the face; skip the machinery. See
`../README.md` and the doctrine: a quiet "key" to systemd's "lock."

## Why schema-init can do this cheaply

systemd-the-init *requires* journald because PID 1 wires every service's
stdout/stderr to journald's stream socket. **schema-init does not** — it `dup2`s
each service's own log file at fork time (`service.c`, `service_spawn`). So
journald is never a *boot* dependency for us. journal-sink exists only to satisfy
*foreign* software (libsystemd consumers, `syslog(3)` users) that assumes a
journald-shaped logging endpoint. It is **opt-in**, not required to boot.

## What "journald exists" decomposes into

Three ingestion sockets plus a directory. We implement ingestion only — no binary
journal format, no querying, no metadata indexing.

| Endpoint | Socket type | Who writes to it | Our handling |
|----------|-------------|------------------|--------------|
| `/dev/log` | `AF_UNIX` `SOCK_DGRAM`, perms 0666 | classic `syslog(3)` — the bulk of software | `recv` datagrams; best-effort parse leading `<PRI>`; append to logfile |
| `/run/systemd/journal/socket` | `AF_UNIX` `SOCK_DGRAM`, 0666 | libsystemd `sd_journal_send()` / `systemd-cat` | `recvmsg`; extract `MESSAGE=` field; **handle SCM_RIGHTS fd-passing** for oversized entries (read the passed memfd/tmpfile, then drain) |
| `/run/systemd/journal/stdout` | `AF_UNIX` `SOCK_STREAM`, listening, 0666 | libsystemd `sd_journal_stream_fd()` | `accept`; consume the multi-line stream header; everything after = log lines, write through |
| `/run/systemd/journal/` (dir) | — | code that `stat()`s for "journald is home" | `mkdir -p` at startup |

### Native protocol notes (`/run/systemd/journal/socket`)
- Entry = newline-separated `FIELD=value` lines. A binary/multiline field is
  `FIELD\n` + `<u64 LE length>` + `<raw bytes>` + `\n`.
- If the entry is too large for one datagram, the client sends it as a sealed
  memfd/`O_TMPFILE` fd via `SCM_RIGHTS` ancillary data. The sink MUST `recvmsg`
  with a control buffer, detect the passed fd, `read()` it to EOF, then `close()`
  it. Ignoring this leaks fds and drops big log lines.
- For the sink we only care about `MESSAGE=`; all other fields (`PRIORITY=`,
  `SYSLOG_IDENTIFIER=`, `_PID=`, `CODE_FILE=`, …) are read and discarded (we may
  keep `PRIORITY=` and `SYSLOG_IDENTIFIER=` for the output line).

### Stream header (`/run/systemd/journal/stdout`)
On connect, the client writes a fixed header of newline-terminated lines, in order:
1. identifier (tag), 2. unit id, 3. priority (int), 4. level-prefix (0/1),
5. forward-to-syslog (0/1), 6. forward-to-kmsg (0/1), 7. forward-to-console (0/1).
After those 7 lines, the rest of the stream is log payload (newline-delimited if
level-prefix=0; otherwise each line may be prefixed with `<N>`). The sink consumes
the 7 header lines, records the identifier, then writes subsequent lines through.

## Output format (deliberately dumb)

Plain text, one line per message, into `/var/log/schema-init/journal.log`
(fallback `/run/log/schema-init/journal.log`, matching the rest of schema-init).
```
<ISO-8601 ts> <src> <pri> <tag>: <message>
```
e.g. `2026-06-09T10:30:01Z devlog info sshd: Accepted publickey for ...`

Optional config (env or compile flag), parity knobs with journald:
- `JOURNAL_SINK_FILE` — override output path.
- `JOURNAL_SINK_KMSG=1` — also forward to `/dev/kmsg` (like `ForwardToKmsg=`),
  useful on embedded where flash writes are undesirable.
- `JOURNAL_SINK_MAXBYTES` — simple size cap with single-file truncate-and-rotate
  (no journald-grade vacuuming).

## Architecture

Single process, single-threaded `poll()` loop (matches schema-init's house style):
- watch: `/dev/log` (dgram), native socket (dgram), stdout listen socket (stream).
- on stdout-listen readable → `accept`, add conn fd to the poll set (cap
  `MAX_STREAM_CONNS`, e.g. 64; refuse/close beyond cap).
- per stream conn: read, split lines, write through; on EOF remove from set.
- on any dgram readable → `recvmsg` (with control buffer for SCM_RIGHTS), process.
- All sockets created `SOCK_CLOEXEC | SOCK_NONBLOCK`; `unlink()` stale paths before
  `bind`; chmod 0666 so unprivileged clients can write.

Resource posture: O(1) memory, bounded conns, no dynamic allocation in the hot
path (fixed line buffer, drop lines longer than buffer with a truncation marker).

## Packaging

- New source `schema-journal-sink.c` (~150–200 lines C), its own Makefile target
  alongside `schema-ctl` / `schema-subreaper`; static build; cross-compiles via the
  existing `aarch64` target.
- Reference service `services/journal-sink.svc.example`:
  ```ini
  name=journal-sink
  exec=/usr/local/bin/schema-journal-sink
  needs_root=1
  ready_path=/run/systemd/journal/socket
  ```
  `ready_path` lets any service that wants journald do `dep=journal-sink` and
  block until the socket is live. Start it early (before foreign loggers). Not
  `critical` — if it dies the recovery arc restarts it; its absence must never
  hang boot (that is the whole Track B ethos).

## Scope boundaries (be honest in the README)

**In:** the three ingestion sockets + dir, drain to plain logfile, optional kmsg
forward and size cap.

**Out (documented gaps, add only if something demands them):**
- No journal **binary format** / no `/var/log/journal/*.journal`.
- No **`journalctl` querying**. If needed later: a separate thin `journalctl`
  shim that `tail`s/greps the flat log — most embedded/container software writes
  logs but never reads them back.
- No structured indexing, cursors, `MESSAGE_ID`, boot IDs, FSS sealing, rate-limit
  quotas, or D-Bus journal methods.
- Does **not** create `/run/systemd/system` (the `sd_booted()` signal) — that is a
  separate Track B shim; cross-reference, don't bundle.

## Testing

- `logger "hello"` → line appears in the sink log (covers `/dev/log`).
- `systemd-cat echo hi` (if libsystemd present) or a tiny test client that
  `connect`s the native socket and sends `MESSAGE=hi\n` → appears (covers native +
  SCM_RIGHTS path with an oversized message).
- A test client that connects the stream socket, writes the 7-line header, then
  log lines → appears (covers stdout stream).
- Negative: an app linking libsystemd and calling `sd_journal_print()` must not
  error or block when the sink is running.

## Status

Design only. Queue: build **after** PR #8 (start_timeout) merges. Becomes its own
PR. Track B per the schema-init doctrine.
