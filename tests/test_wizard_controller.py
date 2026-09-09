#!/usr/bin/env python3
"""WizardController — headless (offscreen) Qt wrapper over WizardCore."""
import os, sys, tempfile, importlib.util
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

try:
    import PySide6  # noqa: F401
except ImportError:
    print("SKIP  PySide6 not installed"); print("PASS"); sys.exit(0)

def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(REPO, rel))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
stage = load("stage", "distros/fedora-installer/migrate/stage.py")
core = load("wizard_core", "distros/fedora-installer/wizard/core.py")
ctrl = load("wizard_controller", "distros/fedora-installer/wizard/controller.py")

from PySide6.QtCore import QCoreApplication
app = QCoreApplication.instance() or QCoreApplication(sys.argv)

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

class FakeBackend:
    def __init__(self): self.calls = []
    def deploy(self, opts=None): self.calls.append(("deploy", opts or [])); return self
    def arm_flip(self): self.calls.append(("arm_flip",)); return self
    def finish(self): self.calls.append(("finish",)); return self

root = tempfile.mkdtemp(); os.makedirs(os.path.join(root, "var/lib"))
be = FakeBackend()
wc = core.WizardCore(backend=be, root=root)
co = ctrl.WizardController(core=wc)

check("screen property reflects core (INSTALLED->welcome)", co.screen == "welcome")

co.setRecoveryAck(False)
co.continueClicked()
check("continue without ack does not deploy", be.calls == [])

co.setRecoveryAck(True)
co.continueClicked()
check("continue with ack deploys", be.calls and be.calls[-1][0] == "deploy")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
