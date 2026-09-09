#!/usr/bin/env python3
"""wizard flow orchestration + recovery-ack gate — injected Backend, temp stage."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(REPO, rel))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
stage = load("stage", "distros/fedora-installer/migrate/stage.py")
core = load("wizard_core", "distros/fedora-installer/wizard/core.py")

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

class FakeBackend:
    def __init__(self): self.calls = []
    def deploy(self, opts=None): self.calls.append(("deploy", opts or [])); return self
    def arm_flip(self): self.calls.append(("arm_flip",)); return self
    def finish(self): self.calls.append(("finish",)); return self

def fresh(stg):
    root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "var/lib"))
    if stg is not None:
        stage.write_stage(stg, root=root)
    be = FakeBackend()
    return core.WizardCore(backend=be, root=root), be

# recovery card gate
c, be = fresh(None)  # INSTALLED
check("INSTALLED without ack refuses", c.advance(consent=True, recovery_ack=False, selections={}) == "need_recovery_ack")
check("refused -> nothing deployed", be.calls == [])

c, be = fresh(None)
check("INSTALLED with ack deploys", c.advance(consent=True, recovery_ack=True, selections={"snapshot": False}) == "deployed")
check("deploy got the advanced flags", be.calls[-1] == ("deploy", ["--advanced-no-snapshot"]))

c, be = fresh(stage.R1_HEAL)
check("R1_HEAL arms the flip", c.advance(consent=True, recovery_ack=True, selections={}) == "armed")
check("arm_flip called", be.calls[-1] == ("arm_flip",))

c, be = fresh(stage.DONE)
check("DONE finishes/tears down", c.advance(consent=True, recovery_ack=True, selections={}) == "finished")

c, be = fresh(stage.ROLLED_BACK)
check("ROLLED_BACK finishes/tears down", c.advance(consent=True, recovery_ack=True, selections={}) == "finished")

c, be = fresh(None)
check("no consent -> noop", c.advance(consent=False, recovery_ack=True, selections={}) == "noop")
check("no consent -> nothing called", be.calls == [])

check("recovery_text is the plain-language card", "black" in c.recovery_text().lower())

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
