#!/usr/bin/env python3
"""wizard advanced-option model — safe defaults, dangerous items carry a warning, flag mapping."""
import os, sys, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(REPO, rel))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
core = load("wizard_core", "distros/fedora-installer/wizard/core.py")

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

keys = {o["key"] for o in core.ADVANCED}
check("every layer present", keys == {"keep_fallback_entry", "udev_flip", "dbus_broker",
                                      "snapshot", "doctor_timers"})
check("all default to the safe path", all(o["default"] for o in core.ADVANCED))
check("dangerous toggles carry a warning line",
      all(o["warning"] for o in core.ADVANCED if o["dangerous"]))

c = core.WizardCore()
check("no deviation -> no flags", c.deploy_opts({o["key"]: o["default"] for o in core.ADVANCED}) == [])
check("snapshot off -> --advanced-no-snapshot",
      "--advanced-no-snapshot" in c.deploy_opts({"snapshot": False}))
check("keep_fallback_entry off -> --advanced-no-fallback-entry",
      "--advanced-no-fallback-entry" in c.deploy_opts({"keep_fallback_entry": False}))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
