import sys, pathlib, json, tempfile, os
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "dbus-learn"))
from schema_dbus_learn import record_from_message, Recorder

class FakeMsg:
    def __init__(self, **kw): self._kw = kw
    def get_type(self): return self._kw.get("type", 1)          # 1=method_call
    def get_serial(self): return self._kw.get("serial", 7)
    def get_reply_serial(self): return self._kw.get("reply_serial", 0)
    def get_sender(self): return self._kw.get("sender", ":1.42")
    def get_destination(self): return self._kw.get("destination", "org.freedesktop.login1")
    def get_path(self): return self._kw.get("path", "/org/freedesktop/login1")
    def get_interface(self): return self._kw.get("interface", "org.freedesktop.login1.Manager")
    def get_member(self): return self._kw.get("member", "Inhibit")
    def get_signature(self): return self._kw.get("signature", "ssss")

def test_record_maps_core_fields_and_resolves_names():
    creds = {":1.42": {"uid": 0, "gids": [0, 4]}}
    rec = record_from_message(FakeMsg(), {":1.42": ["org.example.App"]}, creds)
    assert rec["type"] == "method_call"
    assert rec["sender"] == ":1.42"
    assert rec["sender_names"] == ["org.example.App"]
    assert rec["destination"] == "org.freedesktop.login1"
    assert rec["member"] == "Inhibit"
    assert rec["verdict"] == "allow"           # observed delivery = allowed
    assert rec["intent"] == "method_call"      # contract layer present
    assert "body_shape" in rec                 # shape, not payload
    assert rec["uid"] == 0
    assert rec["gids"] == [0, 4]

def test_record_defaults_uid_gids_when_sender_unknown():
    rec = record_from_message(FakeMsg(sender=":1.99"), {}, {})
    assert rec["uid"] is None
    assert rec["gids"] == []

def test_recorder_appends_jsonl():
    d = tempfile.mkdtemp()
    p = os.path.join(d, "c.jsonl")
    r = Recorder(p)
    r.write({"a": 1}); r.write({"a": 2})
    lines = [json.loads(l) for l in open(p)]
    assert lines == [{"a": 1}, {"a": 2}]
