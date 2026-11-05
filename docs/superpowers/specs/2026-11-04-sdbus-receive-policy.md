# schema-dbus: enforce recipient receive_* policy

## Problem (Greg audit #3, part 1)

`sdbus_route_targets` evaluated the policy only with `op="send"` (the sender's
right to send). It never evaluated `op="receive"` against each recipient, so a
`<deny receive_...>` rule was parsed but never enforced — any message allowed to
the sender reached every matching recipient. In the translated fleet policy the
one such rule is wpa_supplicant1's: non-privileged peers should not receive its
signals (the standard wpa_supplicant.conf pattern).

## Fix

- The policy carries `has_recv_deny`, set at parse time when any `deny` rule has
  a `receive_*` predicate. When it is 0 — a policy with only the default
  `receive_type` allow backbone, i.e. every receive allowed — the receive check
  is skipped entirely, so this is a no-op for such policies and adds no cost to
  the signal hot path.
- When it is 1, each signal delivery is gated by `sdbus__recv_allowed`: an
  `op="receive"` eval with the recipient's uid/gids, the message fields, and the
  sender's well-known names. Directed signals to a forbidden recipient are
  dropped; broadcast subscribers are filtered individually.
- `receive_sender` now matches set-membership over the sender's well-known names
  (with the unique name as a fallback), mirroring `send_destination`, so a
  multi-name sender is matched correctly.

## Scope

Enforcement is on the **signal** paths, where every receive rule in the fleet
policy lives. method_call/method_return/error receive-denies are not enforced:
none exist in the policy, the default `receive_type` allows cover them, and
gating a call on the recipient side would have to synthesize an AccessDenied
back to the caller (or hang it) — a larger change with no policy that needs it.
Requested replies keep their existing policy bypass. Noted as a follow-up.

## Verified

`tests/test_sdbus_policy.c`: receive eval denies the matching recipient, allows
other uids and other senders, honors the unique-name fallback, and `has_recv_deny`
is set only for a deny-receive rule. `tests/test_sdbus_route.c`: a broadcast
drops the forbidden subscriber and delivers to the allowed one. Live: with a
`has_recv_deny` policy loaded, a normal signal is still delivered to its
subscriber (no over-filtering). `make test` + Python suite pass.
