# Service Hardening — Phase 2 (chrony): Right-Sized Bounding Set for a Self-Privdropping Daemon

**Date:** 2026-07-30
**Status:** Approved design, ready for implementation plan
**Scope:** One PR. `.svc`-only hardening of `chronyd` plus a livetest-harness fix that closes the false-green gap Phase 1's pilot exposed. **No changes to `service.c`, `caps.c`, or `init.c`.**

## Motivation

Phase 1 shipped opt-in `no_new_privs` + `keep_caps`, then piloted them on `chronyd` live. The
pilot **failed**: `keep_caps=CAP_SYS_TIME,CAP_NET_BIND_SERVICE` crash-looped chronyd to DORMANT,
never syncing. Root cause: **chronyd self-privdrops** — it starts as root, creates and `chown`s
`/run/chrony`, then `setgid`/`setuid`s to the `chrony` user and retains `CAP_SYS_TIME` across the
transition via libcap. Trimming the bounding set to `{SYS_TIME, NET_BIND}` stripped
`CAP_CHOWN`/`CAP_SETUID`/`CAP_SETGID`, so chronyd's own startup died before it ever synced.

Two secondary findings from the pilot, both material to this phase:

1. **Harness false-green.** The vmtest `test-chrony.svc` ran `/bin/sleep` with
   `keep_caps=CAP_SYS_TIME` and asserted only that `CapBnd` collapsed to the single bit. `/bin/sleep`
   never `chown`s, `setuid`s, or retains caps — so the test went green while the real daemon would
   crash. **VM tests did not cover self-privdropping daemons.**
2. **No hot-revert.** `schema-ctl reload` refuses a modified `.svc` (integrity check → "takes effect
   at next boot"). A bad hardening `.svc` cannot be hot-reverted; it needs a reboot.

This phase adopts the **right-sized bounding set** approach (chosen over the alternative of having
init fully drop chronyd with ambient capabilities). chronyd keeps doing its own, well-tested
privdrop; we simply stop trimming the bounding set below what that privdrop needs. This resolves the
documented failure with **zero init code**, at the cost of a ~50 ms root startup window that ends
with chronyd running as uid `chrony` with an effectively minimal cap set — the same runtime posture
the ambient-caps approach would reach, for a fraction of the PID-1 risk.

## Non-goals (this spec)

- Ambient capabilities (`PR_CAP_AMBIENT_RAISE`) and any `service.c` spawn reorder. Not needed:
  chronyd retains `CAP_SYS_TIME` itself via `PR_SET_KEEPCAPS`.
- init creating/owning `/run/chrony`. chronyd creates and `chown`s it itself (it has `CAP_CHOWN`
  in the kept set and runs as root at that point).
- Any change to `service.c`, `caps.c`, `init.c`, or the `.svc` parser. The Phase-1 machinery is
  sufficient as-is.
- Hardening any service other than `chronyd`.
- Flipping the global default (still gated on a later PR + its own vmtest sweep).

## The `.svc` change

`services/chronyd.svc`, from:

```
name=chronyd
exec=/usr/sbin/chronyd
args=-d
dep=network-manager
needs_root=1
critical=0
```

to:

```
name=chronyd
exec=/usr/sbin/chronyd
args=-d
dep=network-manager
needs_root=1
critical=0
no_new_privs=1
keep_caps=CAP_SYS_TIME,CAP_NET_BIND_SERVICE,CAP_CHOWN,CAP_SETUID,CAP_SETGID
```

### Why these five, and only these

chronyd's startup + privdrop chain, mapped to the capability each step requires:

| Step (chronyd, as root) | Capability |
|---|---|
| `bind()` UDP `:123` for NTP | `CAP_NET_BIND_SERVICE` (bit 10) |
| create + `chown /run/chrony` → `chrony` | `CAP_CHOWN` (bit 0) |
| `setgid(996)` → group `chrony` | `CAP_SETGID` (bit 6) |
| `setuid(996)` → user `chrony` | `CAP_SETUID` (bit 7) |
| `adjtimex()` / step the clock, retained across the uid drop via `PR_SET_KEEPCAPS` | `CAP_SYS_TIME` (bit 25) |

Bounding-set mask = `0x00000000020004C1` (CapBnd prints as `00000000020004c1`). This trims the
inherited bounding set from the full
~40-capability default to exactly 5 — a real reduction — while remaining sufficient for chronyd's
own privdrop.

**Confirmed against the live config.** `/etc/chrony.conf` contains only `driftfile
/var/lib/chrony/drift`: no `lock_all` (would need `CAP_IPC_LOCK`) and no `sched_priority` (would
need `CAP_SYS_NICE`/`CAP_SYS_RESOURCE`). The five-cap set is complete for this deployment. If
`chrony.conf` later gains `lock_all` or scheduling directives, `keep_caps` must be widened
accordingly — noted here so the coupling is explicit.

`no_new_privs=1` is safe: `PR_SET_NO_NEW_PRIVS` only blocks privilege *gain via `execve`* (setuid
binaries, file capabilities). chronyd's `setuid(2)`/`setgid(2)` syscalls are unaffected, and it
re-execs nothing privileged.

## Closing the harness false-green (the substantive work)

The pilot's lesson is that a capability set for a self-privdropping daemon **cannot** be validated by
a payload that never privdrops. Simply updating `test-chrony.svc` to the new five-cap `keep_caps`
while it still runs `/bin/sleep` would false-green again — `/bin/sleep` exercises none of `bind(:123)`,
`chown`, `setgid`, `setuid`, or cap retention.

### Approach: a static `test_privdrop` helper (not real chronyd)

The livetest initramfs is deliberately busybox-only and libc-free (`tests/livetest/vmtest.sh`
builds it from busybox applets plus statically-linked test binaries). Dragging the real `chronyd`
plus its shared-library closure (`libcap`, `libgnutls`, `libnettle`, libc, `ld.so`) and
`/etc/passwd` into the cpio fights the harness design and is brittle across chrony package updates.

Instead, add a small **statically-linked** helper, `tests/livetest/test_privdrop.c`, that replays
chronyd's exact privileged sequence under the trimmed bounding set and reports success only if every
cap-gated step succeeds. It uses raw `syscall(SYS_capset)` (as `caps.c` already does) — **no libcap
dependency** — and numeric uid with `setgroups(0, NULL)` — **no `/etc/passwd` dependency**.

Sequence (chronyd's real order):

1. Assert running as root (uid 0). Bail nonzero otherwise.
2. `bind()` a UDP socket to `0.0.0.0:123` → exercises `CAP_NET_BIND_SERVICE`.
3. `mkdir("/run/chrony-test", 0750)` then `chown(..., 996, 996)` → exercises `CAP_CHOWN`.
4. `prctl(PR_SET_KEEPCAPS, 1)`, `setgroups(0, NULL)`, `setgid(996)`, `setuid(996)` → exercises
   `CAP_SETGID` / `CAP_SETUID`.
5. `syscall(SYS_capset, ...)` raising `CAP_SYS_TIME` into the effective set from the permitted set
   retained across the uid drop by `PR_SET_KEEPCAPS`.
6. `adjtimex()` with a zero `ADJ_OFFSET` (benign, harmless in the VM) → proves `CAP_SYS_TIME` is
   effective **after** the drop, the exact property chrony needs.
7. `printf("PRIVDROP_OK uid=%d\n", getuid())` and idle (so the harness can also read
   `/proc/self/status`).

Any cap-gated step returning `EPERM` under a too-tight bounding set → the helper `exit`s nonzero
**before** printing the marker → the assertion below fails. That is the anti-false-green: the test
can only go green if the five-cap set actually permits the full privdrop.

### Harness edits (`tests/livetest/vmtest.sh`)

- Build and stage `test_privdrop` (compile `-static`; link `/bin/test_privdrop` in `$ROOT`, matching
  the existing static-binary precedent in the harness).
- **Replace** the false-greening `test-chrony.svc` (`exec=/bin/sleep`, `keep_caps=CAP_SYS_TIME`)
  with `test-privdrop.svc`:
  ```
  name=test-privdrop
  exec=/bin/test_privdrop
  needs_root=1
  no_new_privs=1
  keep_caps=CAP_SYS_TIME,CAP_NET_BIND_SERVICE,CAP_CHOWN,CAP_SETUID,CAP_SETGID
  ```
- Capture the helper's `/proc/<pid>/status` to the serial log alongside the existing
  `chrony-capbnd`-style probes.
- **Assertions** (add; keep the existing structural PASS markers):
  1. Serial log contains `PRIVDROP_OK` (privdrop completed under the trimmed set).
  2. The helper's `/proc/<pid>/status` shows `Uid:` line with real uid `996` (it actually dropped).
  3. Its `CapBnd` equals `00000000020004c1` (the five-cap mask) and `CapEff` includes
     `CAP_SYS_TIME` (bit 25) after the drop.

The old single-bit `CapBnd` assertion is superseded by (3); remove it with the `/bin/sleep`
`test-chrony.svc` it belonged to.

## Testing & rollout

`schema-ctl reload` rejects modified `.svc` files at runtime, so the change takes effect only on a
fresh boot. The gate order:

1. **vmtest green** (`tests/livetest/vmtest.sh`): the `PRIVDROP_OK` + uid-996 + CapBnd/CapEff
   assertions pass under a genuine `rdinit=/sbin/schema-init` boot. This proves the five-cap set
   permits a self-privdrop — the property the pilot's `/bin/sleep` test could not check.
2. **Hardware reboot** (blakbox) with the existing rollback net (GRUB `init=` → the preharden
   binary; getty-tty2 autologin recovery). PID-1 binary is unchanged (`.svc`-only), so this is a
   config reboot, not a binary swap.
3. **Reality check after boot:** `pgrep -x chronyd` alive; `chronyc tracking` → `Leap status:
   Normal`; `grep -E 'NoNewPrivs|CapBnd' /proc/$(pgrep -x chronyd)/status` shows `NoNewPrivs: 1`
   and the reduced `CapBnd`; chronyd running as uid `chrony`.

The VM gate proves the caps permit the privileged syscall sequence; the hardware boot proves the
real daemon syncs. The helper is a faithful **model** of chronyd's privdrop, not chronyd itself, so
the hardware `chronyc tracking` check is the backstop against any divergence between model and
daemon.

### Optional tightening (one in-VM pass)

After the five-cap set is green, optionally drop `CAP_NET_BIND_SERVICE` and re-run vmtest to check
whether chronyd (client-only, ephemeral source ports) still needs to bind `:123`. Keep whatever the
VM proves; do not hand-tighten below a validated set.

## Files touched

- `services/chronyd.svc` — add `no_new_privs=1` + the five-cap `keep_caps` (committed; this is the
  deliverable, not a throwaway pilot edit).
- `tests/livetest/test_privdrop.c` — new static helper (raw `SYS_capset`, numeric uid).
- `tests/livetest/vmtest.sh` — build/stage `test_privdrop`; replace `test-chrony.svc` with
  `test-privdrop.svc`; add the `PRIVDROP_OK` / uid-996 / CapBnd+CapEff assertions; remove the
  superseded `/bin/sleep` single-bit assertion.

No `service.c`, `caps.c`, `init.c`, `service.h`, or parser changes.

## Rollout (this PR ends here)

1. Merge after vmtest green + hardware reboot verified (chronyd syncs under the hardened `.svc`).
2. The global default-flip remains gated on its own later PR + full-fleet vmtest sweep, unchanged by
   this phase. This PR's contribution to that arc is a proven pattern for self-privdropping daemons
   and a livetest harness that can no longer false-green on one.
```