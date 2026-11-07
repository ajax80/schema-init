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
co = ctrl.WizardController(spawn=lambda f: f(), core=wc)

check("screen property reflects core (INSTALLED->welcome)", co.screen == "welcome")

co.setRecoveryAck(False)
co.continueClicked()
check("continue without ack does not deploy", be.calls == [])

co.setRecoveryAck(True)
co.continueClicked()
check("continue with ack deploys", be.calls and be.calls[-1][0] == "deploy")
check("successful deploy leaves no error", co.error == "")

# Defect B: a failed backend action surfaces an error to the user instead of
# silently looking like nothing happened.
class FailBackend:
    def deploy(self, opts=None): return type("R", (), {"returncode": 1})()
    def arm_flip(self): return type("R", (), {"returncode": 1})()
    def finish(self): return type("R", (), {"returncode": 1})()

root2 = tempfile.mkdtemp(); os.makedirs(os.path.join(root2, "var/lib"))
wc2 = core.WizardCore(backend=FailBackend(), root=root2)
co2 = ctrl.WizardController(spawn=lambda f: f(), core=wc2)
co2.setRecoveryAck(True)
co2.continueClicked()
check("failed deploy sets an error message", co2.error != "")

# Finish on the last screen closes the wizard instead of a dead button
quits = []
ctrl.QCoreApplication = type("Q", (), {"quit": staticmethod(lambda: quits.append(1))})
root3 = tempfile.mkdtemp()
stage.write_stage(stage.R3_DONE, root=root3)
co3 = ctrl.WizardController(spawn=lambda f: f(), core=core.WizardCore(backend=FakeBackend(), root=root3))
co3.continueClicked()
check("finish on the final screen closes the wizard", quits == [1])
root4 = tempfile.mkdtemp(); os.makedirs(os.path.join(root4, "var/lib"))
co4 = ctrl.WizardController(spawn=lambda f: f(), core=core.WizardCore(backend=FakeBackend(), root=root4))
co4.setRecoveryAck(True); co4.continueClicked()
check("deploy does not close the wizard", quits == [1])

# The real thing: sudo blocks for as long as a password prompt stays open, and
# the window must keep painting and refuse a second click meanwhile.
import time
from PySide6.QtCore import QEventLoop, QTimer

class SlowBackend:
    def __init__(self): self.calls = 0
    def deploy(self, opts=None):
        self.calls += 1
        time.sleep(0.6)
        return type("R", (), {"returncode": 1, "stderr": ""})()

root5 = tempfile.mkdtemp(); os.makedirs(os.path.join(root5, "var/lib"))
slow = SlowBackend()
co5 = ctrl.WizardController(core=core.WizardCore(backend=slow, root=root5))
co5.setRecoveryAck(True)
ticks = []
t = QTimer(); t.setInterval(50); t.timeout.connect(lambda: ticks.append(co5.busy)); t.start()
co5.continueClicked()
check("busy as soon as the click returns", co5.busy)
check("no error while working", co5.error == "")
co5.continueClicked()
co5.secondaryClicked()
loop = QEventLoop(); QTimer.singleShot(1000, loop.quit); loop.exec()
t.stop()
check("event loop kept running during the action", ticks.count(True) >= 5)
check("second click did not run it twice", slow.calls == 1)
check("not busy afterwards", not co5.busy)
check("failure reported afterwards", co5.error != "")

class BoomBackend:
    def deploy(self, opts=None): raise OSError("no sudo")

root6 = tempfile.mkdtemp(); os.makedirs(os.path.join(root6, "var/lib"))
co6 = ctrl.WizardController(core=core.WizardCore(backend=BoomBackend(), root=root6))
co6.setRecoveryAck(True)
co6.continueClicked()
loop = QEventLoop(); QTimer.singleShot(300, loop.quit); loop.exec()
check("an exception does not leave it busy forever", not co6.busy and co6.error != "")

class ExitBackend:
    def deploy(self, opts=None): raise SystemExit(3)

root7 = tempfile.mkdtemp(); os.makedirs(os.path.join(root7, "var/lib"))
co7 = ctrl.WizardController(core=core.WizardCore(backend=ExitBackend(), root=root7))
co7.setRecoveryAck(True)
co7.continueClicked()
loop = QEventLoop(); QTimer.singleShot(300, loop.quit); loop.exec()
check("SystemExit in the worker does not leave it busy", not co7.busy and co7.error != "")

# stdout is a closed pipe at autostart: reporting the crash can raise too
real_print = ctrl.print if hasattr(ctrl, "print") else None
def bad_print(*a, **k): raise BrokenPipeError()
ctrl.print = bad_print
root8 = tempfile.mkdtemp(); os.makedirs(os.path.join(root8, "var/lib"))
co8 = ctrl.WizardController(core=core.WizardCore(backend=BoomBackend(), root=root8))
co8.setRecoveryAck(True)
co8.continueClicked()
loop = QEventLoop(); QTimer.singleShot(300, loop.quit); loop.exec()
del ctrl.print
check("a failing report does not leave it busy", not co8.busy and co8.error != "")

def no_thread(work): raise RuntimeError("can't start new thread")
root9 = tempfile.mkdtemp(); os.makedirs(os.path.join(root9, "var/lib"))
co9 = ctrl.WizardController(core=core.WizardCore(backend=FakeBackend(), root=root9), spawn=no_thread)
co9.setRecoveryAck(True)
co9.continueClicked()
check("no thread available: not busy, error shown", not co9.busy and co9.error != "")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
