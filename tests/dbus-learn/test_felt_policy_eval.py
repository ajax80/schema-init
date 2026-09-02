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
