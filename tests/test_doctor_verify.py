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

# --- notifications, --verify-only, lock, boot reset ---
import json, subprocess, time
sent = []
sd.notify_send = lambda uid, env, summ, body: sent.append((summ, body))
sd.active_session_env = lambda: (1000, {"DBUS_SESSION_BUS_ADDRESS": "x"})
def fresh():
    p = os.path.join(TMP, sd.FlapState.PATH)
    if os.path.exists(p): os.unlink(p)
    sent.clear()
def verify_only_pass():
    flap = sd.FlapState.load()
    sd.notify_transitions([run()], flap, True, partial=True)
    flap.save()
    return flap

fresh()
status("exit=1\nts=1\nresult=1 fail\nfail=next boot: init= missing\n")
verify_only_pass()
check("RED: one popup, its body is the FAIL", sent == [("schema-doctor: verify", "next boot may fail: next boot: init= missing — run schema-ctl verify")], str(sent))
verify_only_pass()
check("same RED again: no second popup", len(sent) == 1, str(sent))
status("exit=0\nts=2\nresult=0 fail\n")
verify_only_pass()
check("GREEN with nothing else red: all clear", sent[-1][0] == "schema-doctor: all clear", str(sent))

fresh()
flap = sd.FlapState.load()
flap._c("power")["last_state"] = sd.RED
flap.data["last_overall"] = sd.RED
flap.save()
status("exit=1\nts=1\nresult=1 fail\nfail=x\n"); verify_only_pass()
status("exit=0\nts=2\nresult=0 fail\n"); flap = verify_only_pass()
check("verify recovers while power is RED: no all clear", not any(s == "schema-doctor: all clear" for s, _ in sent), str(sent))
check("...but says verify itself cleared", sent[-1][0] == "schema-doctor: verify cleared", str(sent))
check("last_overall stays RED", flap.data.get("last_overall") == sd.RED, flap.data.get("last_overall"))

# the CLI: only the verify entry of the status files, no report rewrite
fresh()
os.makedirs(os.path.join(TMP, "run/schema-init"), exist_ok=True)
SJ = os.path.join(TMP, "run/schema-init/doctor-status.json")
json.dump({"overall": "RED", "mode": "periodic", "ts": 1,
           "checks": [{"name": "power", "color": "RED", "state": "reported", "detail": "d", "action": ""}]},
          open(SJ, "w"))
status("exit=1\nts=1\nresult=1 fail\nfail=x\n")
DOC = os.path.join(REPO, "scripts", "schema-doctor.py")
env = dict(os.environ, DOCTOR_ROOT=TMP)
r = subprocess.run([sys.executable, DOC, "--verify-only"], env=env, capture_output=True, text=True, timeout=30)
check("--verify-only exits 0", r.returncode == 0, r.stderr[-300:])
doc = json.load(open(SJ))
names = {c["name"]: c["color"] for c in doc["checks"]}
check("--verify-only keeps the other checks", names.get("power") == "RED" and names.get("verify") == "RED", str(names))
txt = open(os.path.join(TMP, "run/schema-init/doctor-status")).read()
check("status text carries both", "power" in txt and "verify" in txt, txt)
check("--verify-only writes no report", not os.path.exists(os.path.join(TMP, "var/log/schema-init/doctor-report.txt")))

# boot reset: a RED verify from the last boot doesn't come back as "all clear"
fresh()
status("exit=1\nts=1\nresult=1 fail\nfail=x\n"); verify_only_pass()
open(os.path.join(TMP, "proc/sys/kernel/random/boot_id"), "w").write("boot-B\n")
status(None)
flap = sd.FlapState.load()
check("new boot: verify last_state GREEN", flap._c("verify")["last_state"] == sd.GREEN, str(flap._c("verify")))
sent.clear(); verify_only_pass()
check("new boot, verify GREEN: no popup", sent == [], str(sent))

# the lock: a --verify-only run waits for a holder
with sd.FlapState.locked():
    p = subprocess.Popen([sys.executable, DOC, "--verify-only"], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    check("--verify-only waits on the flap lock", p.poll() is None)
check("released: it finishes", p.wait(timeout=30) == 0)

# review: a non-UTF-8 byte in a FAIL line (an fstab path) must not crash the doctor
open(ST, "wb").write(b"exit=1\nts=1\nresult=1 fail\nfail=/mnt/caf\xe9 missing\n")
try:
    r = run(); ok = sd.result_color(r) == sd.RED
except Exception as e:
    ok = False; r = e
check("non-UTF-8 FAIL line: RED, no crash", ok, str(r))

# review: a stale stored RED (a check no longer run) must not make every verify run
# re-arm a spurious "all clear" from the next full run
fresh()
flap = sd.FlapState.load()
flap._c("gone-check")["last_state"] = sd.RED
flap.save()
status("exit=0\nts=1\nresult=0 fail\n"); verify_only_pass()
for _ in range(2):
    flap = sd.FlapState.load()
    sd.notify_transitions([sd.CheckResult("power", "", "clean"), run()], flap, True)
    flap.save()
    verify_only_pass()
check("stale entry: at most the one cleanup all clear, no cycle", sum(s == "schema-doctor: all clear" for s, _ in sent) <= 1, str(sent))
sent.clear()
for _ in range(2):
    flap = sd.FlapState.load()
    sd.notify_transitions([sd.CheckResult("power", "", "clean"), run()], flap, True)
    flap.save()
    verify_only_pass()
check("stale entry: later cycles stay quiet", sent == [], str(sent))
check("full run zeroes entries it didn't run", sd.FlapState.load().data["checks"]["gone-check"]["last_state"] == sd.GREEN)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
