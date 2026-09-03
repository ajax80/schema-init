#!/usr/bin/env python3
"""Generate the hermetic conformance fixture for test_sdbus_conformance.c.

Fully synthetic: a hand-authored dissolved policy + synthetic requests, with
verdicts computed by felt_policy.evaluate (the Python oracle). No corpus data,
so it is safe to commit and run in CI. The real proof runs over the private
14,979-msg corpus via `make verify-dbus-conformance` (local only).

Run from repo root: python3 tests/fixtures/dbus/gen_hermetic.py
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools", "dbus-learn"))
from felt_policy import parse_policy, evaluate  # noqa: E402

POLICY = """\
context = default
allow = send_destination:org.freedesktop.DBus
allow = send_destination:org.example.Public
deny = send_destination:org.example.Secret
context = group:981
allow = send_destination:org.example.Secret
context = user:0
allow = own:*
context = user:1000
allow = own_prefix:org.example.user
"""

# Synthetic requests exercising every branch the C port must reproduce.
REQS = [
    {"op": "send", "uid": 1000, "gids": [10], "destination": ":1.2",
     "destination_names": ["org.example.Public"], "interface": "org.x",
     "member": "M", "msgtype": "method_call", "reply_serial": None, "name": None},
    {"op": "send", "uid": 1000, "gids": [10], "destination": ":1.3",
     "destination_names": ["org.example.Secret"], "interface": "org.x",
     "member": "M", "msgtype": "method_call", "reply_serial": None, "name": None},
    {"op": "send", "uid": 1000, "gids": [981], "destination": ":1.3",
     "destination_names": ["org.example.Secret"], "interface": "org.x",
     "member": "M", "msgtype": "method_call", "reply_serial": None, "name": None},
    {"op": "send", "uid": 1000, "gids": [10], "destination": ":1.9",
     "destination_names": [], "interface": None, "member": None,
     "msgtype": "method_return", "reply_serial": 42, "name": None},
    {"op": "send", "uid": 1000, "gids": [10], "destination": ":1.9",
     "destination_names": ["org.example.Secret"], "interface": "org.x",
     "member": "M", "msgtype": "method_call", "reply_serial": 42, "name": None},
    {"op": "send", "uid": 81, "gids": [81], "destination": ":1.1",
     "destination_names": ["org.freedesktop.DBus"], "interface": "org.freedesktop.DBus",
     "member": "Hello", "msgtype": "method_call", "reply_serial": None, "name": None},
    {"op": "own", "uid": 0, "gids": [0], "name": "org.anything.At.All",
     "destination": None, "destination_names": [], "interface": None,
     "member": None, "msgtype": None, "reply_serial": None},
    {"op": "own", "uid": 1000, "gids": [10], "name": "org.example.user.Foo",
     "destination": None, "destination_names": [], "interface": None,
     "member": None, "msgtype": None, "reply_serial": None},
    {"op": "own", "uid": 1000, "gids": [10], "name": "org.other.Thing",
     "destination": None, "destination_names": [], "interface": None,
     "member": None, "msgtype": None, "reply_serial": None},
]


def _csv(xs):
    return ",".join(str(x) for x in (xs or []))


def _s(x):
    return "" if x is None else str(x)


def main():
    policy = parse_policy(POLICY)
    with open(os.path.join(HERE, "policy-dissolved.txt"), "w") as f:
        f.write(POLICY)
    with open(os.path.join(HERE, "policy-golden.tsv"), "w") as out:
        for req in REQS:
            verdict = evaluate(policy, req)
            has_reply = 1 if req.get("reply_serial") is not None else 0
            row = [verdict, _s(req.get("op")), _s(req.get("uid")),
                   _csv(req.get("gids")), _s(req.get("interface")),
                   _s(req.get("member")), _s(req.get("msgtype")),
                   _s(req.get("path")), _s(req.get("destination")),
                   _csv(req.get("destination_names")), _s(req.get("name")),
                   str(has_reply)]
            out.write("\t".join(row) + "\n")
    sys.stderr.write("wrote %d hermetic golden rows\n" % len(REQS))


if __name__ == "__main__":
    main()
