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

# waiting_reboot is reboot-gated: it must expose NO clickable action (a dead
# Continue button there is what sent a user rebooting into a stuck stage).
check("welcome offers Continue", c.primary_action("welcome") == "Continue")
check("waiting_reboot offers no button", c.primary_action("waiting_reboot") == "")
check("summary offers Continue", c.primary_action("summary") == "Continue")
check("final offers Finish", c.primary_action("final") == "Finish")
check("rolled_back offers Finish", c.primary_action("rolled_back") == "Finish")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
