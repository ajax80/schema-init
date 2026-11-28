#!/usr/bin/env python3
# schema-doctor's verify check: verify-status from the last real verify run -> a colour
import os, sys, tempfile, importlib.util
TMP = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = TMP
os.makedirs(os.path.join(TMP, "proc/sys/kernel/random"), exist_ok=True)
open(os.path.join(TMP, "proc/sys/kernel/random/boot_id"), "w").write("boot-A\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(name, ok, detail=''):
    results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))

ST = os.path.join(TMP, "run/schema-init/verify-status")
def status(text):
    os.makedirs(os.path.dirname(ST), exist_ok=True)
    if text is None:
        if os.path.exists(ST): os.unlink(ST)
    else:
        open(ST, "w").write(text)
def run():
    return sd.run_checks([sd.VerifyStatus()], heal=False, force=set())[0]

status(None); r = run()
check("no status file: GREEN", sd.result_color(r) == sd.GREEN, r.state)
status("exit=0\nts=1\nresult=0 fail, 0 change, 0 warn\n"); r = run()
check("exit=0: GREEN", sd.result_color(r) == sd.GREEN)
status("exit=1\nts=1\nresult=1 fail, 0 change, 0 warn\nfail=next boot: init= missing\n"); r = run()
check("exit=1: RED", sd.result_color(r) == sd.RED, sd.result_color(r))
check("RED names the FAIL and what to run", "init= missing" in r.detail and "schema-ctl verify" in r.detail, r.detail)
status("exit=2\nts=1\nresult=0 fail, 1 change (not accepted), 0 warn\n"); r = run()
check("exit=2: AMBER, no popup grade", sd.result_color(r) == sd.AMBER, sd.result_color(r))
check("AMBER shows the result line", "1 change (not accepted)" in r.detail, r.detail)
status("exit=3\nts=1\nresult=0 fail, 0 change, 0 warn, 1 unresolved\n"); r = run()
check("exit=3: AMBER", sd.result_color(r) == sd.AMBER)
status("garbage\n"); r = run()
check("garbage: GREEN, no exception", sd.result_color(r) == sd.GREEN)
check("a DEFERRED result without a colour is still AMBER",
      sd.result_color(sd.CheckResult("x", "", "reported", grade=sd.DEFERRED)) == sd.AMBER)
check("a SAFE reported result without a colour is still RED",
      sd.result_color(sd.CheckResult("x", "", "reported")) == sd.RED)
check("verify is registered", any(c.name == "verify" for c in sd.REGISTRY))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
