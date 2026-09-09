#!/usr/bin/env python3
"""wizard status parsing + humanize_reason — pure, no Qt."""
import os, sys, json, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/wizard/status.py")
spec = importlib.util.spec_from_file_location("wizard_status", MOD)
st = importlib.util.module_from_spec(spec); spec.loader.exec_module(st)

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

good = json.dumps({"overall": "AMBER", "mode": "heal", "ts": 1,
                   "checks": [{"name": "card-input-acl", "color": "GREEN", "state": "clean",
                               "detail": "", "action": ""},
                              {"name": "powerdevil-running", "color": "AMBER", "state": "reported",
                               "detail": "PowerDevil is not running", "action": "detect-only"}]})
p = st.parse_status(good)
check("overall parsed", p["overall"] == "AMBER")
check("items parsed", len(p["items"]) == 2 and p["items"][1]["name"] == "powerdevil-running")

check("bad json -> UNKNOWN, empty items", st.parse_status("{not json")["overall"] == "UNKNOWN")
check("empty -> UNKNOWN", st.parse_status("")["items"] == [])

check("humanize: schema-udev not running",
      "didn't come up" in st.humanize_reason("schema-udev not running").lower())
check("humanize: desktop never confirmed",
      "desktop" in st.humanize_reason("desktop never confirmed across 2 armed boots").lower())
check("humanize: unknown reason falls back to something non-empty",
      len(st.humanize_reason("some novel reason")) > 0)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
