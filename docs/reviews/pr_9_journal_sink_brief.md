# PR #9 Review Brief — schema-journal-sink (Track B journald shim)

**For:** Greg (review + pressure-test)
**PR:** https://github.com/ajax80/schema-init/pull/9 → `master`
**Branch:** `feat/journal-sink` (rebased onto merged PR #8, `5b077f4`)
**Code:** `schema-journal-sink.c` (~340 lines), `Makefile`, `services/journal-sink.svc.example`
**Design:** `docs/journal-sink-design.md`

## What it is

Opt-in, non-critical Track B compatibility shim. Provides journald's three
ingestion sockets and drains them to a plain logfile so foreign libsystemd /
`syslog(3)` software finds a journald-shaped endpoint. **No** journal DB, **no**
`journalctl`, **no** binary format. schema-init itself never needs it to boot
(PID 1 dup2s each service's own log fd) — this exists purely for foreign userland.

- `/dev/log` — `SOCK_DGRAM`, classic `syslog(3)`.
- `/run/systemd/journal/socket` — `SOCK_DGRAM`, libsystemd native (`MESSAGE=`,
  binary fields, SCM_RIGHTS fd-passing for oversized entries).
- `/run/systemd/journal/stdout` — `SOCK_STREAM`, 7-line header then payload.
- Output: `/var/log/schema-init/journal.log` (fallback `/run/log/...`), one line
  per message: `<ISO-8601 ts> <src> <pri> <tag>: <message>`.
- Env knobs: `JOURNAL_SINK_{FILE,KMSG,MAXBYTES}`.

## Already self-tested (live, on the PID-1 box, then restored pristine)

All three sockets verified. Three bugs caught and fixed in-loop:
1. `SYSLOG_IDENTIFIER` compared as 16 chars — it's 17 → tags were dropped. Fixed.
2. Embedded `\n` in a binary `MESSAGE` split one entry across two physical log
   lines → now folded to spaces (one message = one line). Fixed.
3. `/dev/log` left the RFC3164 `Mmm DD HH:MM:SS` timestamp in the body and lost
   the tag → now stripped. Fixed.

## Scope note (deliberate, not an oversight)

Binds `/dev/log` **directly** rather than journald's `dev-log`+symlink dance.
Simpler, works for every `syslog(3)` client. Flag if you think the symlink form
matters for a specific consumer; otherwise it stands.

## Pressure-test scenarios (please break these)

### 1. SCM_RIGHTS fd leak / exhaustion
Flood the native socket with oversized (memfd / `O_TMPFILE`) entries passed via
`SCM_RIGHTS`. Verify: every passed fd is `close()`d in `handle_native` /
`drain_passed_fd` (watch `ls /proc/<pid>/fd` for growth); the `MAX_FD_BYTES`
(1 MiB) cap truncates rather than over-reading; multiple fds in one `cmsg` all
drain. **Risk:** a leaked fd here eventually wedges the daemon.

### 2. Stream connection cap
Open >`MAX_STREAM_CONNS` (64) concurrent connections to
`/run/systemd/journal/stdout`. Verify: connections beyond the cap are cleanly
refused (`accept4`'d then `close()`d — no crash, no fd leak); a closed conn frees
its slot so a later client can reconnect; the `poll`-set / `conns[]` index mapping
(`idx` walk at the bottom of the loop) stays correct as slots churn. **Risk:** the
parallel `pfd[idx]` ↔ `conns[i]` walk is the trickiest bookkeeping in the file.

### 3. Malformed native entries
Send to `/run/systemd/journal/socket`: a truncated `<u64 LE length>` (fewer than 8
bytes after `FIELD\n`); a length that runs past the datagram end; a binary field
with no trailing `\n`; a field name with no `=` and no valid binary framing.
Verify: `parse_native` never reads past `end` (all the `if (... > end) break;`
guards hold), no infinite loop, no crash.

### 4. /dev/log parser robustness
Send via `/dev/log`: a message with no `<PRI>`; no tag; a `:` inside the message
body (must not be mistaken for a tag separator); a line longer than `LINE_MAX_`;
`<PRI>` followed by something that only *looks* like an RFC3164 timestamp.
Verify: priority/tag/timestamp parsing degrades gracefully (tag `-`, not garbage),
the one-line invariant holds, no over-read.

### 5. Restart / recovery + stale sockets
Kill the sink mid-traffic (`SIGKILL`). Verify: schema-init's recovery arc restarts
it (it's non-critical, `ready_path=/run/systemd/journal/socket`); on rebind the
stale socket paths are `unlink()`ed first (they are — confirm no `EADDRINUSE`); a
client that had a live stream conn reconnects cleanly. Confirm absence of the sink
**never** blocks boot (the whole Track B ethos).

## Output format check

Confirm the line format is grep-friendly and stable, and that `JOURNAL_SINK_MAXBYTES`
truncate-and-restart doesn't corrupt a line mid-write (it truncates between
messages, not within one — verify under a small cap with steady traffic).

## If you check in fixes

Push to `feat/journal-sink` and drop your findings in
`docs/reviews/pr_9_review_journal_sink.md` (matching #7/#8). Then it goes through
the VM harness gate (`~/schema-livetest/vmtest.sh`) before merge.
