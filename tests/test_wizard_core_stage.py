#!/usr/bin/env python3
"""WizardCore stage->screen decision — reads the stage file via stage.py, no Qt."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(REPO, rel))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
stage = load("stage", "distros/fedora-installer/migrate/stage.py")
core = load("wizard_core", "distros/fedora-installer/wizard/core.py")

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "var/lib"))
c = core.WizardCore(root=root)

check("absent stage reads INSTALLED", c.current_stage() == stage.INSTALLED)
check("INSTALLED -> welcome", c.screen_for(stage.INSTALLED) == "welcome")
check("R1_PENDING -> waiting_reboot", c.screen_for(stage.R1_PENDING) == "waiting_reboot")
check("R1_HEAL -> summary", c.screen_for(stage.R1_HEAL) == "summary")
check("R2_PENDING -> waiting_reboot", c.screen_for(stage.R2_PENDING) == "waiting_reboot")
check("DONE -> final", c.screen_for(stage.DONE) == "final")
check("ROLLED_BACK -> rolled_back", c.screen_for(stage.ROLLED_BACK) == "rolled_back")

stage.write_stage(stage.R1_HEAL, root=root)
check("screen() reflects the written stage", c.screen() == "summary")

# waiting_reboot's only action is a real restart, and only from the boot the
# stage started waiting in: right after the reboot the stage still reads
# PENDING until --finish runs, and a click then must not reboot again (#489).
mig = core._migrate()
mig._boot_id = lambda: "boot-a"
stage.write_stage(stage.R1_PENDING, root=root, extra={"armed_boot": "boot-a"})
check("waiting_reboot offers Restart now in the armed boot",
      c.primary_action("waiting_reboot") == "Restart now")
mig._boot_id = lambda: "boot-b"
check("waiting_reboot offers no button once rebooted", c.primary_action("waiting_reboot") == "")

class FakeBackend:
    def __init__(self, rc): self.rc, self.calls = rc, []
    def reboot(self):
        self.calls.append("reboot")
        class R: pass
        R.returncode = self.rc
        return R()
mig._boot_id = lambda: "boot-a"
be = FakeBackend(0); c.backend = be
check("Restart now restarts", c.advance(True, False, {}) == "restarting" and be.calls == ["reboot"])
be = FakeBackend(1); c.backend = be
check("a refused restart says so", c.advance(True, False, {}) == "reboot_failed")
check("reboot_failed has an error message", "menu" in core.error_for("reboot_failed"))

check("welcome offers Continue", c.primary_action("welcome") == "Continue")
check("summary offers Continue", c.primary_action("summary") == "Continue")
check("final offers Finish", c.primary_action("final") == "Finish")
check("rolled_back offers Finish", c.primary_action("rolled_back") == "Finish")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
