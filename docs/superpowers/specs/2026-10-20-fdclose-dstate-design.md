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

## Review of #394 (folded in)

Quiesce kills every closer before it can know a survivor will force a
refusal. The old image then ran on with a killed overflow closer's unreached
fds orphaned, fd passing still shut, and a stale batch record. The same gap
already existed when `fexecve` itself failed. `fdclose_resume()` now runs on
both refusal paths. The unreached fds are still in the table, because close()
removes an fd before it blocks, so they park again for a new closer, and fd
passing reopens. On the exec-failure path it runs after the blob and oldexe
are closed, since either could hold a number the killed closer freed.

Not changed: a refusal can stall the loop for up to 2 s when a survivor exists
(the first 1 s predates this). A retry can report "not reached close()" for a
killed task stuck in its exit path.

## Tests

- `make test`: a closer in close(), whose SIGKILL does not land, makes
  `fdclose_quiesce` return -2 after 1 s and stay tracked; it is reaped normally
  afterwards.
- `make test`: an overflow closer killed by a refused quiesce has its unreached
  fds closed by a new overflow closer, and fd passing reopens.
- `vmtest.sh` regression: PASS.
- `REEXEC=1 tests/livetest/notifycap-vmtest.sh … old`: re-exec mid-attack still
  completes, late ready after, 0 missed pets.
