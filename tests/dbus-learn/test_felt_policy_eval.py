# tests/dbus-learn/test_felt_policy_eval.py
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "dbus-learn"))
from felt_policy import parse_policy, evaluate

POLICY = parse_policy("""\
context=default
allow=user:*
deny=own:*
deny=send_type:method_call
allow=send_type:signal

context=user:root
allow=own:org.freedesktop.login1
allow=send_destination:org.freedesktop.login1
""")

def test_default_denies_method_call_to_unlisted_destination():
    req = {"op": "send", "uid": 1000, "gids": [1000], "destination": "com.example.Foo",
           "interface": "com.example.Foo", "member": "Do", "msgtype": "method_call", "path": "/"}
    assert evaluate(POLICY, req) == "deny"

def test_signal_allowed_by_default():
    req = {"op": "send", "uid": 1000, "gids": [1000], "destination": None,
           "interface": "com.example.Foo", "member": "Ping", "msgtype": "signal", "path": "/"}
    assert evaluate(POLICY, req) == "allow"

def test_root_may_own_and_send_to_login1():
    own = {"op": "own", "uid": 0, "gids": [0], "name": "org.freedesktop.login1"}
    assert evaluate(POLICY, own) == "allow"
    send = {"op": "send", "uid": 0, "gids": [0], "destination": "org.freedesktop.login1",
            "interface": "org.freedesktop.login1.Manager", "member": "Inhibit",
            "msgtype": "method_call", "path": "/org/freedesktop/login1"}
    assert evaluate(POLICY, send) == "allow"

def test_nonroot_cannot_own_login1():
    own = {"op": "own", "uid": 1000, "gids": [1000], "name": "org.freedesktop.login1"}
    assert evaluate(POLICY, own) == "deny"

def test_group_context_applies_only_to_members():
    policy = parse_policy("""\
context=default
deny=send_type:method_call
context=group:wheel
allow=send_destination:com.example.Service
""")
    # Get the gid for 'wheel' group
    import grp
    wheel_gid = grp.getgrnam("wheel").gr_gid

    # Request from a user in wheel group should be allowed
    req_in_group = {"op": "send", "uid": 1001, "gids": [1001, wheel_gid],
                    "destination": "com.example.Service", "interface": "com.x",
                    "member": "Do", "msgtype": "method_call", "path": "/"}
    assert evaluate(policy, req_in_group) == "allow"

    # Request from a user NOT in wheel group should be denied
    req_not_in_group = {"op": "send", "uid": 1000, "gids": [1000],
                        "destination": "com.example.Service", "interface": "com.x",
                        "member": "Do", "msgtype": "method_call", "path": "/"}
    assert evaluate(policy, req_not_in_group) == "deny"

def test_send_destination_resolves_through_destination_names():
    # policy is written in well-known names; the wire destination is often the
    # unique name. destination_names carries the owned well-known name(s), and a
    # send_destination rule must match against that set.
    policy = parse_policy("""\
context=default
deny=send_type:method_call
allow=send_destination:org.freedesktop.login1
""")
    by_unique = {"op": "send", "uid": 1000, "gids": [1000], "destination": ":1.8",
                 "destination_names": ["org.freedesktop.login1"], "interface": "i",
                 "member": "m", "msgtype": "method_call", "path": "/"}
    assert evaluate(policy, by_unique) == "allow"
    # a unique name owning NO matching well-known name stays denied
    unowned = dict(by_unique, destination=":1.99", destination_names=[])
    assert evaluate(policy, unowned) == "deny"

def test_last_match_wins_over_the_owned_name_set():
    # a connection owning both an allowed and a later-denied name must resolve to
    # the LAST matching rule (deny), not allow-if-any.
    policy = parse_policy("""\
context=default
allow=send_destination:a.Allowed
deny=send_destination:b.Denied
""")
    req = {"op": "send", "uid": 1000, "gids": [1000], "destination": ":1.5",
           "destination_names": ["a.Allowed", "b.Denied"], "interface": "i",
           "member": "m", "msgtype": "method_call", "path": "/"}
    assert evaluate(policy, req) == "deny"

def test_requested_reply_bypasses_send_destination_deny():
    # a method_return/error carrying a reply_serial is a requested reply and is
    # exempt from send policy (dbus send_requested_reply defaults true).
    policy = parse_policy("""\
context=default
allow=send_type:method_return
deny=send_destination:org.freedesktop.NetworkManager
""")
    base = {"op": "send", "uid": 81, "gids": [81], "destination": ":1.5",
            "destination_names": ["org.freedesktop.NetworkManager"], "interface": "i",
            "member": "m", "msgtype": "method_return", "path": "/"}
    assert evaluate(policy, dict(base, reply_serial=42)) == "allow"   # tracked reply
    assert evaluate(policy, dict(base, reply_serial=None)) == "deny"  # not a reply
    call = dict(base, msgtype="method_call", reply_serial=None)
    assert evaluate(policy, call) == "deny"                           # exemption is replies only

def test_receive_sender_matches_sender_name_not_destination():
    policy = parse_policy("""\
context=default
allow=receive_type:signal
deny=receive_sender:org.foo
""")
    denied = {"op": "receive", "uid": 1000, "gids": [1000],
              "sender_name": "org.foo", "msgtype": "signal",
              "destination": None, "interface": None, "member": None, "path": None}
    assert evaluate(policy, denied) == "deny"

    allowed = {"op": "receive", "uid": 1000, "gids": [1000],
               "sender_name": "org.bar", "msgtype": "signal",
               "destination": None, "interface": None, "member": None, "path": None}
    assert evaluate(policy, allowed) == "allow"

def test_user_context_overrides_group_context():
    # Real on-box identities: uid 0 = root, gid 10 = wheel.
    # user:root is declared FIRST in the file, group:wheel SECOND — this only
    # discriminates real tier ordering (group < user) from mere file order.
    policy = parse_policy("""\
context=default
deny=send_type:method_call
context=user:root
deny=send_destination:com.example.Service
context=group:wheel
allow=send_destination:com.example.Service
""")
    req = {"op": "send", "uid": 0, "gids": [0, 10],
           "destination": "com.example.Service", "interface": "com.x",
           "member": "Do", "msgtype": "method_call", "path": "/"}
    # root is in wheel too; user tier must be evaluated AFTER group tier and win,
    # even though it appears earlier in the file.
    assert evaluate(policy, req) == "deny"
