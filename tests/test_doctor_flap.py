#!/usr/bin/env python3
"""Flap backoff/escalate state machine for schema-doctor."""
import os, sys, json, tempfile, importlib.util
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

# 3 heals allowed in-window, 4th blocked + chronic
fs = sd.FlapState.load()
t = 1000
oks = []
for i in range(3):
    oks.append(fs.should_heal("x", t + i))       # True, True, True
    if oks[-1]:
        fs.record_heal("x", t + i)
blocked = fs.should_heal("x", t + 3)              # False -> chronic
check("first 3 heals allowed", oks == [True, True, True])
check("4th blocked", blocked is False)
check("marked chronic", fs.is_chronic("x") is True)

# recovery clears chronic + history
fs.recovered("x")
check("recovered clears chronic", fs.is_chronic("x") is False)
check("recovered clears heals", fs._c("x")["heals"] == [])

# window pruning: old heals age out
fs2 = sd.FlapState.load()
fs2._c("y")["heals"] = [10, 20, 30]               # far older than now
check("prune re-allows after window", fs2.should_heal("y", 10 + sd.FLAP_WINDOW + 1) is True)

# persistence + corrupt-file tolerance
fs.record_heal("z", 5000); fs.save(now=5000)
open(os.path.join(TMP, "var/lib/schema-init/doctor-state"), "w").write("{ not json")
fs3 = sd.FlapState.load()
check("corrupt state -> empty, no crash", fs3.data["checks"] == {})

# boot_id change resets heals
open(os.path.join(TMP, "proc/sys/kernel/random/boot_id"), "w").write("boot-B\n")
fs4 = sd.FlapState(json.loads(json.dumps(
    {"version": 1, "boot_id": "boot-A", "last_overall": sd.GREEN, "last_run": 0,
     "checks": {"x": {"heals": [1, 2, 3], "chronic": True, "last_state": sd.RED}}})))
open(os.path.join(TMP, "var/lib/schema-init/doctor-state"), "w").write(json.dumps(fs4.data))
fs5 = sd.FlapState.load()
check("boot change resets heals", fs5._c("x")["heals"] == [])
check("boot change clears chronic", fs5._c("x")["chronic"] is False)

class Flaky(sd.Check):
    """SAFE check that heals (verify passes) but re-breaks before the next run."""
    def __init__(self):
        self.name = self.summary = "flaky"; self.grade = sd.SAFE; self._broken = True
    def detect(self):
        return sd.Finding("broke again") if self._broken else None
    def heal(self, f): self._broken = False          # verify() -> detect() is None -> True

fs6 = sd.FlapState.load()
states = []
for i in range(4):
    c = Flaky()
    r = sd.run_checks([c], heal=True, force=set(), flap=fs6, now=9000 + i)[0]
    states.append(r.state)
check("periodic: heals 3x then chronic", states == ["healed", "healed", "healed", "chronic"], str(states))

# boot mode (flap=None) never goes chronic — heals every time
boot_states = [sd.run_checks([Flaky()], heal=True, force=set(), now=9000 + i)[0].state for i in range(4)]
check("boot mode: always heals, never chronic", boot_states == ["healed"] * 4, str(boot_states))

# wrong-shaped-but-valid JSON + boot_id mismatch -> empty state, no crash (Fix #2)
open(os.path.join(TMP, "proc/sys/kernel/random/boot_id"), "w").write("boot-C\n")
open(os.path.join(TMP, "var/lib/schema-init/doctor-state"), "w").write(
    json.dumps({"version": 1, "boot_id": "boot-B", "checks": [1, 2, 3]}))
try:
    fs7 = sd.FlapState.load()
    check("wrong-shape checks (list) -> empty, no crash", fs7.data["checks"] == {})
except Exception as e:
    check("wrong-shape checks (list) -> empty, no crash", False, f"raised {e!r}")

open(os.path.join(TMP, "proc/sys/kernel/random/boot_id"), "w").write("boot-D\n")
open(os.path.join(TMP, "var/lib/schema-init/doctor-state"), "w").write(
    json.dumps({"version": 1, "boot_id": "boot-C", "checks": {"x": 5}}))
try:
    fs8 = sd.FlapState.load()
    check("wrong-shape checks (dict of non-dicts) -> empty, no crash", fs8.data["checks"] == {})
except Exception as e:
    check("wrong-shape checks (dict of non-dicts) -> empty, no crash", False, f"raised {e!r}")

# chronic clears once the window prunes heals below threshold (Fix #5)
fs9 = sd.FlapState.load()
t2 = 2000
for i in range(3):
    fs9.should_heal("w", t2 + i)
    fs9.record_heal("w", t2 + i)
fs9.should_heal("w", t2 + 3)                      # 4th -> blocked, chronic True
check("chronic set after 4th", fs9.is_chronic("w") is True)
later = fs9.should_heal("w", t2 + sd.FLAP_WINDOW + 10)   # heals aged out of window
check("chronic clears after window prunes below threshold",
      later is True and fs9.is_chronic("w") is False)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
