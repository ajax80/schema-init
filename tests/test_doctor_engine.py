#!/usr/bin/env python3
"""Engine-core tests for schema-doctor: the detect/heal/verify/back-out loop."""
import os, sys, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(name, ok, detail=''):
    results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))

class Fake(sd.Check):
    def __init__(self, name, grade, broken, heal_fixes=True, breaks_on_heal=None):
        self.name, self.summary, self.grade = name, name, grade
        self._broken, self._heal_fixes, self._breaks = broken, heal_fixes, breaks_on_heal
        self.snap_taken = self.backed_out = self.healed = False
    def detect(self):
        if self._breaks is not None and self._breaks[0]:  # collateral victim
            return sd.Finding("collateral")
        return sd.Finding("broken") if self._broken else None
    def snapshot(self): self.snap_taken = True; return "S"
    def heal(self, f):
        self.healed = True
        if self._heal_fixes: self._broken = False
        if self._breaks is not None: self._breaks[0] = True  # trip a sibling
    def back_out(self, snap): self.backed_out = True; assert snap == "S"

# clean check → state clean, no heal
r = sd.run_checks([Fake("a", sd.SAFE, broken=False)], heal=True, force=set())[0]
check("clean stays clean", r.state == "clean")

# SAFE broken, heal fixes → healed
f = Fake("b", sd.SAFE, broken=True); r = sd.run_checks([f], heal=True, force=set())[0]
check("SAFE heals", r.state == "healed" and f.snap_taken and f.healed)

# SAFE broken, heal disabled → reported, not healed
f = Fake("c", sd.SAFE, broken=True); r = sd.run_checks([f], heal=False, force=set())[0]
check("heal=False reports only", r.state == "reported" and not f.healed)

# DEFERRED broken, heal on → reported (no heal) unless forced
f = Fake("d", sd.DEFERRED, broken=True); r = sd.run_checks([f], heal=True, force=set())[0]
check("DEFERRED not auto-healed", r.state == "reported" and not f.healed)
f = Fake("d", sd.DEFERRED, broken=True); r = sd.run_checks([f], heal=True, force={"d"})[0]
check("DEFERRED heals when forced", r.state == "healed" and f.healed)

# heal does NOT fix → back_out + reported
f = Fake("e", sd.SAFE, broken=True, heal_fixes=False); r = sd.run_checks([f], heal=True, force=set())[0]
check("heal-failed backs out", r.state == "reported" and f.backed_out)

# collateral: healing 'g' breaks previously-clean 'h' → g backed out, run aborts
shared = [False]
g = Fake("g", sd.SAFE, broken=True, breaks_on_heal=shared)
h = Fake("h", sd.SAFE, broken=False, breaks_on_heal=shared)
rs = sd.run_checks([g, h], heal=True, force=set())
check("collateral backs out healer", g.backed_out)
check("collateral aborts run", any(x.name == "g" and x.state == "reported" for x in rs))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
