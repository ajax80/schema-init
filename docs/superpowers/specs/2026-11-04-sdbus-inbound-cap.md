# schema-dbus: cap per-connection inbound buffering

## Problem (Greg audit #3, part 2)

`sdbus_conn_in_append` grows `c->in` toward a message's declared length as bytes
arrive, and a message is only parsed/consumed once complete. A client that sends
a header declaring a large body (up to `SDBUS_MAX_MSG`, 128 MB) and then dribbles
bytes without ever completing it holds that much in the root broker. With
`SDBUS_MAX_CONN_PER_UID` = 256, one local uid could pin ~32 GB and OOM the broker.
Garbage can't do this (a bad header fails `sdbus_wire_parse` and the buffer is
dropped) — it takes a valid-but-huge declared length, dribbled.

## Fix

A per-connection inbound ceiling, mirroring the existing outbound cap
(`SDBUS_MAX_OUTGOING_BYTES`): `SDBUS_MAX_CONN_PENDING` = 16 MB.
`sdbus_conn_in_append` refuses an append that would exceed it, sets `in_over`,
and `on_readable` drops the connection — the same disconnect the outbound
`oq_over` path already does. 16 MB is far above any real system-bus message, and
the broker already refuses to *relay* anything larger (the outbound cap is the
same size), so this accepts nothing that could have been delivered anyway.

## Verified

`tests/test_sdbus_conn.c`: a stream past the cap flags `in_over`, `c->in` never
grows beyond the cap, the realloc doubling stays bounded, and an append that
would exceed the cap is refused whole. Live: a raw client authenticated to a
private-socket broker, declaring a 20 MB body and dribbling, is dropped at ~16 MB
— before the body completes. `make test` + Python suite pass.

## Scope notes (from code review)

- **Driver-addressed messages 16 MB-128 MB** are now disconnected mid-receive
  rather than processed. Every driver call (Hello, RequestName, AddMatch, ...)
  is tiny; nothing legitimate approaches 16 MB, and an oversized driver message
  is itself anomalous. The ceiling is deliberate.
- **Per-uid aggregate** is still 256 conns x 16 MB (plus the symmetric outbound
  backlog). That matches the existing outbound cap's exposure and is 8x tighter
  than stock dbus-daemon (256 x `max_incoming_bytes` ~= 127 MB). A per-uid
  inbound+outbound memory budget would bound it further -- a follow-up that
  should also cover the pre-existing outbound cap, not just this change.

## Follow-up (separate PR)

The router evaluates only `op="send"`, never recipient `receive_*` rules; a
`<deny receive_...>` is not enforced. Tracked separately.
