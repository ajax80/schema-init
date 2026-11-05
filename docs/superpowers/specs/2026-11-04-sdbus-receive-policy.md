# schema-dbus: enforce recipient receive_* policy

## Problem (Greg audit #3, part 1)

`sdbus_route_targets` evaluated the policy only with `op="send"` (the sender's
right to send). It never evaluated `op="receive"` against each recipient, so a
`<deny receive_...>` rule was parsed but never enforced — any message allowed to
the sender reached every matching recipient. In the translated fleet policy the
one such rule is wpa_supplicant1's: non-privileged peers should not receive its
signals (the standard wpa_supplicant.conf pattern).

## Fix

- The policy carries `has_recv_rule`, set at parse time when ANY rule -- allow or
  deny -- has a `receive_*` predicate. A policy with no receive rule leaves
  receive unconfigured (all allowed) and the check is skipped. A whitelist of
  allow-receive rules counts too: the engine default-denies whatever the
  whitelist doesn't cover, so enforcement must run (an earlier version keyed the
  flag on deny-receive only, which let such whitelists silently bypass policy --
  fixed after code review).
- When it is 1, each signal delivery is gated by `sdbus__recv_allowed`: an
  `op="receive"` eval with the recipient's uid/gids, the message fields, and the
  sender's well-known names. Directed signals to a forbidden recipient are
  dropped (and dropped, fail-closed, if the resolved owner conn can't be found);
  broadcast subscribers are filtered individually.
- `receive_sender` is matched as a scalar well-known name, exactly as the
  reference model `tools/dbus-learn/felt_policy.py` (`_RECV`) does; the route
  path passes the sender's first owned well-known name (its unique as fallback).
  A multi-name sender named by a non-first `receive_sender` rule is a limitation
  of the reference model, not reached by the fleet policy. (An earlier version
  did set-membership, diverging from the model and its corpus gate -- reverted.)

## Scope

Enforcement is on the **signal** paths, where every receive rule in the fleet
policy lives. method_call/method_return/error receive-denies are not enforced:
none exist in the policy, the default `receive_type` allows cover them, and
gating a call on the recipient side would have to synthesize an AccessDenied
back to the caller (or hang it) — a larger change with no policy that needs it.
Requested replies keep their existing policy bypass. method_call/return receive
enforcement, and a per-sender prefilter to skip the eval on the hot path when no
receive rule could match the sender, are noted as follow-ups.

## Verified

`tests/test_sdbus_policy.c`: receive eval denies the matching recipient, allows
other uids and other senders, an allow-whitelist default-denies the rest, and
`has_recv_rule` is set for any receive rule but not for a receive-free policy. `tests/test_sdbus_route.c`: a broadcast
drops the forbidden subscriber and delivers to the allowed one. Live: with a
`has_recv_deny` policy loaded, a normal signal is still delivered to its
subscriber (no over-filtering). `make test` + Python suite pass.
