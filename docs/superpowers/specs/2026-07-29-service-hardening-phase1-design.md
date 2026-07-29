# Service Hardening — Phase 1: Capability & Privilege Restriction

**Date:** 2026-07-29
**Status:** Approved design, ready for implementation plan
**Scope:** One PR. Opt-in `no_new_privs` and capability-allowlist hardening applied in the service child between fork and exec.

## Motivation

systemd is not short on sandboxing knobs — `NoNewPrivileges`, capability bounding sets,
seccomp, namespaces all exist. Its weakness is that every one ships **off**: units opt in,
and in practice almost none do. `systemd-analyze security` scores most stock units UNSAFE
for exactly this reason.

schema-init's differentiator (Doctrine A — capability) is to invert that default: services
run sandboxed unless they declare otherwise. This spec is **Phase 1** of that arc and delivers
the two cheapest, highest-value, lowest-breakage primitives — `no_new_privs` and capability
restriction — as **opt-in** fields. A later, separate PR flips the global default to on once
these are proven live. Phases 2 (mount-namespace isolation) and 3 (seccomp) are out of scope
here and get their own specs.

Rollout was decided as **opt-in first, flip the default once proven** — do not bet the daily
driver on hardening 49 services blind.

## Non-goals (this spec)

- Mount namespaces / PrivateTmp / ProtectSystem / ProtectHome (Phase 2).
- seccomp syscall filtering (Phase 3).
- Flipping the global default from opt-in to opt-out (follow-up PR, own spec).
- Any `libcap` / `libseccomp` dependency. Capability ops are hand-rolled via raw syscalls to
  keep the trusted computing base minimal.

## New `.svc` fields

Both default to "no change," so every existing service file is untouched in behavior.

| Field | Value | Effect |
|-------|-------|--------|
| `no_new_privs` | `1` | Sets `PR_SET_NO_NEW_PRIVS` in the child. The process and all descendants can never gain privileges through setuid/setgid binaries or file capabilities. Irreversible for the process's lifetime. |
| `keep_caps` | comma list of `CAP_*` names, e.g. `CAP_NET_BIND_SERVICE,CAP_SYS_TIME` | Allowlist. Every capability **not** listed is dropped from the bounding set and cleared from the permitted/effective/inheritable sets. Unset = capabilities untouched (current behavior). `keep_caps=` (empty value) = drop **all** capabilities. |

Parsing lives in `service_load_one` (`service.c`), alongside the existing `strcmp(line, ...)`
field handlers. `keep_caps` is parsed once at load into a fixed-width `uint64_t cap_keep_mask`
on `service_t` (bit N = `CAP_*` value N); a new `int flags` bit `SVC_NO_NEW_PRIVS` records the
`no_new_privs` request. A separate `uint8_t cap_restrict` field records whether `keep_caps` was
present at all (to distinguish "unset / untouched" from "present but empty / drop all"), since a
zero mask is a valid explicit request.

### `service_t` additions (`service.h`)

```c
uint64_t cap_keep_mask;   /* bit N set = keep CAP_N; only meaningful if cap_restrict */
uint8_t  cap_restrict;    /* 1 if keep_caps= was present in the .svc (even if empty) */
```

`SVC_NO_NEW_PRIVS` is added to the existing `SVC_*` flag word as `(1 << 7)` — bits `(1 << 0)`
through `(1 << 6)` are already allocated (`SVC_ONESHOT` … `SVC_TIMER_PERSIST`).

Capability count guard: bits 0..63 cover all currently-defined Linux capabilities
(`CAP_LAST_CAP` is 40 on current kernels). An unknown `CAP_*` name in `keep_caps` is a **load
error** for that file (service skipped, logged) — a typo must not silently widen privilege.

## Mechanism

A single bounded function:

```c
/* Apply opt-in hardening in the child, before setuid and before execv.
 * Returns 0 on success. On any failure of a REQUESTED step, returns -1;
 * the caller fail-closes (_exit(126)). No-op for services that request
 * neither no_new_privs nor keep_caps. */
int service_apply_hardening(const service_t *svc);
```

Declared in `service.h`, defined in `service.c`. It performs, in order:

1. **Capability restriction** (only if `svc->cap_restrict`), while still root:
   - For every capability value `c` in `0..CAP_LAST_CAP` **not** in `cap_keep_mask`:
     `prctl(PR_CAPBSET_DROP, c, 0, 0, 0)`. Removes it from the bounding set so it can never be
     reacquired. **`EINVAL` from this call is not a failure** — it means `c` exceeds the running
     kernel's maximum capability (schema-init may be compiled against newer kernel headers whose
     `CAP_LAST_CAP` is higher than the live kernel supports). Treat `EINVAL` as the upper boundary:
     skip it and continue (or break); only a non-`EINVAL` error (e.g. `EPERM`) is a real failure
     that triggers fail-closed.
   - `capset()` via `syscall(SYS_capset)` with a `_LINUX_CAPABILITY_VERSION_3` header and a
     two-element `__user_cap_data_struct[2]` payload, setting `permitted`, `effective`, and
     `inheritable` to exactly `cap_keep_mask` (split across the two 32-bit data words). This is
     what actually strips privilege from a service that stays root; a uid-dropping service loses
     caps on `setuid` anyway, but doing both is correct and cheap.
2. **`no_new_privs`** (only if `SVC_NO_NEW_PRIVS`): `prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)`.
   Done last, right before returning to the caller (which then does setuid → execv). Must be set
   after any capset (setting it first does not block capset, but ordering it last keeps the
   "nothing between this and exec" invariant clear).

No raw capability header struct is available without `<sys/capability.h>` (libcap) or
`<linux/capability.h>`. Use `<linux/capability.h>` (kernel UAPI, present in kernel-headers, no
library link) for `_LINUX_CAPABILITY_VERSION_3`, `__user_cap_header_struct`,
`__user_cap_data_struct`, `CAP_LAST_CAP`, and the `CAP_*` value macros used by the name→value
parse table.

### Call site (`service.c`, child block ~line 411–421)

Current child order: priority → (if run_uid) initgroups/setgid/setuid → execv.

New order:

```
... existing: setsid, log dup2, INSTANCE env, setpriority ...

if (service_apply_hardening(svc) != 0)      /* caps dropped here, while root */
    _exit(126);

if (svc->run_uid) {                          /* existing uid-drop block */
    ... XDG_RUNTIME_DIR, initgroups, setgid, setuid ...
}

execv(svc->exec, svc->argv);
_exit(127);
```

`service_apply_hardening` must run **before** the uid-drop block: `PR_CAPBSET_DROP` and `capset`
require `CAP_SETPCAP`, held only while root. `no_new_privs` set before `setuid` is fine and does
not block the intended `setgid`/`setuid` (those are not "gaining" privilege).

Note: with `no_new_privs` set, a uid-dropping service can no longer use setuid helpers. This is
intended and is the caller's responsibility to declare correctly — documented in the field table.

**Non-root capability retention is out of scope for Phase 1.** `capset` sets the permitted/
effective sets while root, but the kernel clears them on `setuid()` to a non-root uid. Retaining a
specific cap across the uid drop (e.g. a `nobody` daemon that must keep `CAP_NET_BIND_SERVICE`)
requires ambient capabilities — `prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0)` for each
kept cap, after `capset` and before `setuid`. Phase 1 targets root-staying daemons (`chrony`,
`avahi`), where this does not arise; ambient-cap raising is deferred to a later phase and, if
requested for a uid-dropping service in Phase 1, `keep_caps` simply has no post-`setuid` effect
(the `no_new_privs` + bounding-drop benefit still applies).

## Failure policy: fail-closed

If a requested hardening step fails (`prctl`/`capset` returns a non-`EINVAL` error), the child
writes one line to its already-redirected stderr (service log) —
`[schema-init] HARDENING FAILED for <name>: <step>: <errno>` — and `_exit(126)`. The write uses
async-signal-safe raw I/O (`dprintf(2, ...)` / `write(2)`), **not** `fprintf(stderr, ...)`: this is
a post-`fork`, pre-`exec` child, and stdio would risk flushing buffers inherited from PID 1. It does **not**
exec unhardened. On a modern kernel, run as root (PID 1's children start as root before the
optional uid drop), these calls do not fail, so there is no boot-wedge risk for correctly-written
services; the exit code surfaces genuine misconfiguration (e.g. an impossible cap request) through
the normal restart/cooldown/excise machinery rather than silently running a service that believed
it was sandboxed. `126` is chosen to be distinct from `127` (execv-failed) and `0`/`88` (clean).

A service that requests neither field: `service_apply_hardening` returns 0 immediately, zero
syscalls, byte-for-byte the current spawn path.

## Testing

### vmtest (mechanism proof under real PID-1 boot)

Add a hardening assertion to the vmtest harness (`~/schema-livetest/`):

- A new test service `test-hardened.svc` with `no_new_privs=1` and
  `keep_caps=CAP_NET_BIND_SERVICE`, running a tiny payload that reads its own
  `/proc/self/status` and prints `NoNewPrivs` and `CapBnd` to the console, then stays up.
- PASS additionally requires the serial log to show `NoNewPrivs: 1` and a `CapBnd` value equal to
  the single-bit mask for `CAP_NET_BIND_SERVICE` (`0x0000000000000400`) for that service.
- The existing five PASS markers are unchanged; this is an added marker, so a green run proves the
  hardening applies correctly under a genuine `rdinit=/sbin/schema-init` boot.

The vmtest runs as root with no uid-drop, exercising the capset-on-a-root-service path — the one
that most needs proving.

### Live pilot (blakbox)

After vmtest green: add `no_new_privs=1` and a correct `keep_caps=` to **one** low-stakes service
(chrony: `keep_caps=CAP_SYS_TIME,CAP_NET_BIND_SERVICE`; or avahi: `keep_caps=CAP_NET_BIND_SERVICE`
— verify against the daemon's actual needs first), reload, and confirm:

- `grep -E 'NoNewPrivs|CapBnd' /proc/<pid>/status` shows `NoNewPrivs: 1` and the reduced `CapBnd`.
- The daemon still functions (chrony still steps the clock / avahi still resolves `.local`).

Only after both pass does the default-flip PR proceed.

## Files touched

- `service.h` — two struct fields, one `SVC_*` flag, `service_apply_hardening` prototype.
- `service.c` — `#include <linux/capability.h>` + `<sys/prctl.h>` + `<sys/syscall.h>`; the
  `CAP_*` name→value parse table; two field handlers in `service_load_one`; the
  `service_apply_hardening` definition; the call site in the child.
- `~/schema-livetest/` harness — `test-hardened.svc` + the two added assertion greps (harness
  lives outside the repo; note in the PR that the vmtest change is applied there).
- One pilot `.svc` (chrony or avahi) is edited **only for the live test**, not committed as part
  of this PR unless the pilot is kept hardened after verification.

## Rollout (this PR ends here)

1. Merge Phase 1 (opt-in only) after vmtest green + live pilot verified.
2. Separate follow-up PR + spec flips the default: unset `keep_caps` ⇒ drop to a minimal safe
   baseline, unset `no_new_privs` ⇒ on, with a new `unsafe=1` per-service opt-out for the handful
   that genuinely need full privilege. That PR carries its own vmtest sweep across all 49 services.
