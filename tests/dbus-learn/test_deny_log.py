import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "dbus-learn"))
from deny_log import parse_rejection

LINE = ('Rejected send message, 2 matched rules; type="method_call", '
        'sender=":1.55" (uid=1000 pid=3210 comm="dolphin") '
        'interface="org.freedesktop.systemd1.Manager" member="StartUnit" '
        'error name="(unset)" requested_reply="0" '
        'destination="org.freedesktop.systemd1" (uid=0 pid=1 comm="schema-init")')

def test_parses_rejection_into_request_tuple():
    req = parse_rejection(LINE)
    assert req["op"] == "send"
    assert req["uid"] == 1000
    assert req["msgtype"] == "method_call"
    assert req["interface"] == "org.freedesktop.systemd1.Manager"
    assert req["member"] == "StartUnit"
    assert req["destination"] == "org.freedesktop.systemd1"

def test_non_rejection_returns_none():
    assert parse_rejection("Successfully activated service 'org.foo'") is None
