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
    def arm_dbus(self): self.calls.append(("arm_dbus",)); return self
    def confirm_dbus(self): self.calls.append(("confirm_dbus",)); return self
    def skip_dbus(self): self.calls.append(("skip_dbus",)); return self

def fresh(stg, extra=None):
    root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "var/lib"))
    if stg is not None:
        stage.write_stage(stg, root=root, extra=extra)
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

c, be = fresh(stage.DONE, extra={"adv": ["no-dbus-broker"]})
check("DONE (dbus opted out) is final", c.screen() == "final")
check("DONE (dbus opted out) finishes/tears down", c.advance(consent=True, recovery_ack=True, selections={}) == "finished")

# R3: after udev DONE the wizard offers the dbus flip
c, be = fresh(stage.DONE)
check("DONE offers the dbus flip", c.screen() == "dbus_offer")
check("dbus_offer has a Skip", c.secondary_action("dbus_offer") != "")
check("dbus_offer Continue arms dbus", c.advance(consent=True, recovery_ack=True, selections={}) == "dbus_armed")
check("arm_dbus called", be.calls[-1] == ("arm_dbus",))

c, be = fresh(stage.DONE)
check("Skip declines the dbus flip", c.skip() == "skipped" and be.calls[-1] == ("skip_dbus",))

c, be = fresh(stage.DONE, extra={"adv": ["no-dbus-broker"]})
check("Skip does nothing off the offer screen", c.skip() == "noop" and be.calls == [])

bid = open("/proc/sys/kernel/random/boot_id").read().strip()
c, be = fresh(stage.R3_PENDING, extra={"dbus_armed_boot": bid})
check("R3 same boot -> waiting_reboot", c.screen() == "waiting_reboot")
check("R3 same boot never confirms", c.advance(consent=True, recovery_ack=True, selections={}) == "noop" and be.calls == [])

c, be = fresh(stage.R3_PENDING, extra={"dbus_armed_boot": "an-earlier-boot"})
check("R3 after reboot -> confirming", c.screen() == "confirming")
check("confirming confirms", c.advance(consent=True, recovery_ack=True, selections={}) == "dbus_confirmed")
check("confirm_dbus called", be.calls[-1] == ("confirm_dbus",))

check("R3_DONE -> final", fresh(stage.R3_DONE)[0].screen() == "final")
check("R3_ROLLED_BACK -> rolled_back", fresh(stage.R3_ROLLED_BACK)[0].screen() == "rolled_back")
c, be = fresh(stage.R3_DONE)
check("R3_DONE finishes", c.advance(consent=True, recovery_ack=True, selections={}) == "finished")

c, be = fresh(stage.ROLLED_BACK)
check("ROLLED_BACK finishes/tears down", c.advance(consent=True, recovery_ack=True, selections={}) == "finished")

c, be = fresh(None)
check("no consent -> noop", c.advance(consent=False, recovery_ack=True, selections={}) == "noop")
check("no consent -> nothing called", be.calls == [])

# a backend action that fails must NOT be reported as success (Defect B): a
# non-zero return means the deploy/arm/finish did not happen.
class FailBackend(FakeBackend):
    def deploy(self, opts=None): FakeBackend.deploy(self, opts); return type("R", (), {"returncode": 1})()
    def arm_flip(self): FakeBackend.arm_flip(self); return type("R", (), {"returncode": 2})()

def failing(stg):
    root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "var/lib"))
    if stg is not None: stage.write_stage(stg, root=root)
    return core.WizardCore(backend=FailBackend(), root=root)

check("failed deploy -> deploy_failed", failing(None).advance(consent=True, recovery_ack=True, selections={}) == "deploy_failed")
check("failed arm -> arm_failed", failing(stage.R1_HEAL).advance(consent=True, recovery_ack=True, selections={}) == "arm_failed")

check("recovery_text is the plain-language card", "black" in c.recovery_text().lower())

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
