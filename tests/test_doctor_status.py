#!/usr/bin/env python3
"""Status color mapping + overall rollup for schema-doctor."""
import os, sys, importlib.util
import tempfile
os.environ["DOCTOR_ROOT"] = tempfile.mkdtemp()
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(name, ok, detail=''):
    results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))

CR = sd.CheckResult
check("clean -> GREEN",   sd.result_color(CR("a", "", "clean")) == sd.GREEN)
check("healed -> AMBER",  sd.result_color(CR("a", "", "healed")) == sd.AMBER)
check("chronic -> RED",   sd.result_color(CR("a", "", "chronic")) == sd.RED)
check("reported DEFERRED -> AMBER", sd.result_color(CR("a", "", "reported", grade=sd.DEFERRED)) == sd.AMBER)
check("reported SAFE -> RED",       sd.result_color(CR("a", "", "reported", grade=sd.SAFE)) == sd.RED)
check("overall worst-wins RED", sd.overall_color(
    [CR("a", "", "clean"), CR("b", "", "healed"), CR("c", "", "chronic")]) == sd.RED)
check("overall AMBER when best is healed", sd.overall_color(
    [CR("a", "", "clean"), CR("b", "", "healed")]) == sd.AMBER)
check("overall GREEN all clean", sd.overall_color([CR("a", "", "clean")]) == sd.GREEN)

import json
rs = [CR("card-input-acl", "gpu", "clean"),
      CR("powerdevil-running", "power", "chronic", "died", "", "chronic — 3+"),
      CR("session-single", "sess", "reported", "orphan #31", "", "deferred", grade=sd.DEFERRED)]
txt = sd.render_status(rs, "periodic")
check("status header has overall RED", txt.splitlines()[0].startswith("schema-doctor: RED"))
check("status lists each check", "card-input-acl" in txt and "powerdevil-running" in txt)
p = sd.write_status(txt)
check("status file written", os.path.exists(p))
check("read_status round-trips", sd.read_status() == txt)
j = json.loads(sd.status_json(rs, "periodic"))
check("json overall RED", j["overall"] == sd.RED)
check("json mode periodic", j["mode"] == "periodic")
check("json 3 checks", len(j["checks"]) == 3)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
