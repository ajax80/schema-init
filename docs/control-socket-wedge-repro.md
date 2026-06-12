# Control-Socket Wedge — Reproduction & Verification Plan

**Status:** open. Found on live blakbox 2026-06-11. Reproduce in the VM
harness (`schema-vmtest` / `vmtest.sh`) — **never on a live PID 1.**

## Symptom

1. Add a new `.svc` to `/etc/schema-init/services/` at runtime.
2. `schema-ctl reload`  → `ok: reload (graceful)` (works).
3. `schema-ctl start <newsvc>`  → **garbled/binary reply**.
4. Every subsequent `schema-ctl` call → `connect: Connection refused`,
   persistently. PID 1 and all running services stay alive — only the
   management socket is dead.

## What's confirmed vs. hypothesis

- **Confirmed:** the listening socket leaked into every child (missing
  `SOCK_CLOEXEC` at `init.c:976` and `accept()` at `init.c:1001`). Fixed
  (`accept4` + `SOCK_CLOEXEC`). This is the prime suspect for the wedge but
  is **not proven** to be the whole cause.
- **Hypothesis (wedge):** race between the tick loop auto-starting the new
  service and the `start` handler. After `reload`, a brand-new service has
  `inst` zeroed (`init.c:1118` memset; the state-merge loop at `1165` only
  copies state for services that already existed). `inst.state == 0 ==
  STATE_NEW_PROCESS`, so the tick loop spawns it on its own — `schema-ctl
  start` then races that. The garbled reply is likely `state_name()` reading
  an unexpected/!EXCISED/!PERFECT state in the `start` handler (`init.c:823`).

## Repro steps (in VM)

1. Boot the VM with **current /sbin/schema-init** (pre-fix binary) as PID 1.
2. In the guest:
   ```sh
   cat >/etc/schema-init/services/repro.svc <<EOF
   name=repro
   exec=/bin/true
   oneshot=1
   needs_root=1
   critical=0
   EOF
   schema-ctl reload
   schema-ctl start repro      # expect garbled reply
   schema-ctl status           # expect: connect: Connection refused
   ```
3. Capture state the moment it wedges:
   - `ss -xlp | grep schema-init.sock`  — note Recv-Q / Send-Q(backlog) and
     every process holding the fd (the leak).
   - `ls -l /proc/1/fd | grep socket`  — is PID 1 still holding `ctl_fd`?
   - Liveness probe: does PID 1 still reap? `sleep 1 & kill -9 %1` then check
     for a lingering zombie — if zombies accumulate, the **main loop is
     stuck**, not just the socket.

## Isolate the cause (bisect the hypotheses)

| Test | If wedge disappears → cause is |
|------|-------------------------------|
| `reload`, **wait 3s**, then `start repro` | the reload↔auto-start **race** |
| `start repro` on a service present **at boot** (no reload) | the **reload path** specifically |
| Rebuild with the **CLOEXEC fix**, repeat repro | the **fd-leak** |
| Add a guard in `start` handler: bound-check `inst.state` before `state_name()` | a **bad-state read** |

## Verify the fix

1. Build with the CLOEXEC change; boot VM with the new binary as PID 1.
2. Repeat the repro steps — `schema-ctl start repro` must return clean text
   and `schema-ctl status` must keep working.
3. **Leak check (the real proof for bug #2):** start a long-running service,
   then `grep schema-init.sock /proc/<that-pid>/fd/* ` (or `ls -l`) — the
   control socket must **not** appear. Pre-fix it does; post-fix it must not.
4. Run the full existing `vmtest.sh` suite to confirm no boot/timer/start
   regressions.

## If CLOEXEC alone doesn't fix the wedge

Add the two cheap hardening fixes and re-test:
- `start` handler: guard `state_name()` with a range check on `inst.state`.
- `handle_reload`: explicitly set `inst.state` for services that are new in
  the shadow set (no live match in the merge loop at `init.c:1165`), instead
  of relying on the `0`-memset meaning "NEW_PROCESS" — make intent explicit.
