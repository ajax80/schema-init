# fdclose: refuse re-exec when a closer outlives SIGKILL

Review item 4 of #391.

`fdclose_quiesce` kills every closer before re-exec and waits up to 1 s for
them to die. A closer in uninterruptible sleep (D state, such as a hard NFS
mount) can outlive that window. It used to return 0 anyway, and the re-exec
went ahead.

After `execve`, PID 1 gets a fresh fd table, but the surviving closer keeps the
old one. That old table holds every socket and memfd PID 1 had: listen sockets,
the notify socket, fdstore entries. The new image cannot track the closer, and
a listen socket it rebinds can fail with EADDRINUSE while the old one is
pinned.

## Change

`fdclose_quiesce` returns -2 when any closer is still alive 1 s after SIGKILL.
`reexec_commit` refuses with "an fd closer outlived SIGKILL (stuck in D state);
try again later, or reboot". The survivor stays tracked and the old image keeps
running, so the state stays consistent. An RPM upgrade then takes effect at the
next re-exec or reboot.

`FDCLOSE_KILL` (default `kill`) lets the test stand in for a kill that does not
land, since a userspace test cannot put a child in D state.

## Tests

- `make test`: a closer in close(), whose SIGKILL does not land, makes
  `fdclose_quiesce` return -2 after 1 s and stay tracked; it is reaped normally
  afterwards.
- `vmtest.sh` regression: PASS.
- `REEXEC=1 tests/livetest/notifycap-vmtest.sh … old`: re-exec mid-attack still
  completes, late ready after, 0 missed pets.
