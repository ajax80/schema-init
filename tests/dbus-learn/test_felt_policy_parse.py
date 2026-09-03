import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "dbus-learn"))
from felt_policy import parse_policy, Rule, Context

def test_parses_contexts_and_rules():
    text = """\
context=default

allow=user:*
deny=own:*
deny=send_type:method_call

context=user:root

allow=own:org.freedesktop.login1
allow=send_destination:org.freedesktop.login1,send_interface:org.freedesktop.login1.Manager
"""
    ctxs = parse_policy(text)
    assert [c.kind for c in ctxs] == ["default", "user"]
    assert ctxs[1].selector == "root"
    assert ctxs[0].rules[0] == Rule("allow", {"user": "*"})
    assert ctxs[0].rules[2] == Rule("deny", {"send_type": "method_call"})
    # chained predicates on one line are ANDed
    assert ctxs[1].rules[1] == Rule(
        "allow",
        {"send_destination": "org.freedesktop.login1",
         "send_interface": "org.freedesktop.login1.Manager"},
    )
