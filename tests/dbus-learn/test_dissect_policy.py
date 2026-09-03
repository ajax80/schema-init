import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "dbus-learn"))
from dissect_policy import dissolve_text
from felt_policy import parse_policy, evaluate

XML = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <policy context="default">
    <deny own="*"/>
    <deny send_type="method_call"/>
    <allow send_type="signal"/>
  </policy>
  <policy user="root">
    <allow own="org.freedesktop.login1"/>
    <allow send_destination="org.freedesktop.login1"/>
  </policy>
</busconfig>
"""

def test_dissolve_then_evaluate_matches_intent():
    text = dissolve_text(XML)
    pol = parse_policy(text)
    assert [c.kind for c in pol] == ["default", "user"]
    # non-root cannot own login1; root can
    assert evaluate(pol, {"op": "own", "uid": 1000, "gids": [1000], "name": "org.freedesktop.login1"}) == "deny"
    assert evaluate(pol, {"op": "own", "uid": 0, "gids": [0], "name": "org.freedesktop.login1"}) == "allow"

def test_send_destination_and_interface_are_anded():
    xml = ('<busconfig><policy context="default">'
           '<allow send_destination="a.b" send_interface="a.b.I"/>'
           '</policy></busconfig>')
    pol = parse_policy(dissolve_text(xml))
    r = pol[0].rules[0]
    assert r.decision == "allow"
    assert r.predicates == {"send_destination": "a.b", "send_interface": "a.b.I"}
