#!/usr/bin/env python3
# tools/compat-corpus.py classifies each directive: mapped, warned, silent, inert,
# skipped-unit. Two fixture units pin each class.
import json, os, subprocess, sys, tempfile
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with tempfile.TemporaryDirectory() as d:
    open(os.path.join(d, "a.service"), "w").write(
        "[Unit]\nDescription=a\n[Service]\nExecStart=/bin/true\nKeyringMode=private\n"
        "CapabilityBoundingSet=CAP_NET_ADMIN\n[Install]\nWantedBy=multi-user.target\n")
    open(os.path.join(d, "t@.service"), "w").write("[Service]\nExecStart=/bin/true\nUser=nobody\n")
    out = os.path.join(d, "r.json")
    r = subprocess.run([sys.executable, os.path.join(REPO, "tools/compat-corpus.py"), "--json", out, d],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    rec = {u["unit"]: u for u in json.load(open(out))["units"]}
    st = {(x["section"], x["key"]): x["status"] for x in rec["a.service"]["directives"]}
    assert rec["a.service"]["status"] == "imported" and rec["a.service"]["loader"] is None, rec["a.service"]
    assert st[("Service", "ExecStart")] == "mapped", st
    assert st[("Service", "CapabilityBoundingSet")] == "warned", st
    assert st[("Service", "KeyringMode")] == "silent", st
    assert st[("Unit", "Description")] == "inert" and st[("Install", "WantedBy")] == "inert", st
    assert rec["t@.service"]["status"] == "skipped", rec["t@.service"]
    assert {x["status"] for x in rec["t@.service"]["directives"]} == {"skipped-unit"}
import importlib.util
_s = importlib.util.spec_from_file_location("cc", os.path.join(REPO, "tools/compat-corpus.py"))
cc = importlib.util.module_from_spec(_s)
_s.loader.exec_module(cc)
assert {"ReadWritePaths", "ProtectSystem"} <= cc.warned_of(
    ["ReadWritePaths=/etc/x is read-only under protect_system=full, lowered to protect_system=1"])
assert "ProtectHome" in cc.warned_of(["ReadWritePaths=/home/x is read-only under protect_home=1, lowered to protect_home=0"])
_dyn = cc.si.unit_to_svc("dyn", cc.si.parse_unit("[Service]\nExecStart=/bin/d\nDynamicUser=yes\n"))
_w = cc.warned_of([ln[len(cc.si.WARN_PREFIX):] for ln in _dyn.splitlines() if ln.startswith(cc.si.WARN_PREFIX)])
assert "DynamicUser" not in _w and {"RemoveIPC", "RestrictSUIDSGID"} <= _w, _w
print("ok compat_corpus")
