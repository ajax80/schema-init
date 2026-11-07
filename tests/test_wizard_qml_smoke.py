#!/usr/bin/env python3
"""QML load-smoke — the engine loads Main.qml offscreen with no errors."""
import os, sys, importlib.util
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

try:
    import PySide6  # noqa: F401
except ImportError:
    print("SKIP  PySide6 not installed"); print("PASS"); sys.exit(0)

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

WIZ = os.path.join(REPO, "distros/fedora-installer/wizard")
spec = importlib.util.spec_from_file_location("wizard_main", os.path.join(WIZ, "main.py"))
wm = importlib.util.module_from_spec(spec); spec.loader.exec_module(wm)

from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtCore import QUrl, QEventLoop, QTimer

app = QGuiApplication.instance() or QGuiApplication(sys.argv)
engine = QQmlApplicationEngine()
errors = []
engine.warnings.connect(lambda ws: errors.extend(str(w.toString()) for w in ws))
ctrl_spec = importlib.util.spec_from_file_location("wizard_controller", os.path.join(WIZ, "controller.py"))
cm = importlib.util.module_from_spec(ctrl_spec); ctrl_spec.loader.exec_module(cm)
engine.rootContext().setContextProperty("wizard", cm.WizardController())
engine.load(QUrl.fromLocalFile(os.path.join(WIZ, "qml", "Main.qml")))

check("Main.qml produced a root object", len(engine.rootObjects()) == 1)
check("no QML warnings/errors", errors == [])

# the wizard can open before schema-migrate --finish promotes the stage at
# boot; while it says "restart", it must keep re-reading the stage
import tempfile
stage_spec = importlib.util.spec_from_file_location("stage", os.path.join(REPO, "distros/fedora-installer/migrate/stage.py"))
st = importlib.util.module_from_spec(stage_spec); stage_spec.loader.exec_module(st)
core_spec = importlib.util.spec_from_file_location("wizard_core", os.path.join(WIZ, "core.py"))
co = importlib.util.module_from_spec(core_spec); core_spec.loader.exec_module(co)
root = tempfile.mkdtemp()
st.write_stage(st.R1_PENDING, root=root)
w = cm.WizardController(core=co.WizardCore(backend=None, root=root))
e2 = QQmlApplicationEngine()
e2.rootContext().setContextProperty("wizard", w)
e2.load(QUrl.fromLocalFile(os.path.join(WIZ, "qml", "Main.qml")))
seen = []
w.changed.connect(lambda: seen.append(w.screen))
st.transition(st.R1_HEAL, root=root)
loop = QEventLoop(); QTimer.singleShot(2600, loop.quit); loop.exec()
check("waiting_reboot re-polls and picks up the promoted stage", "summary" in seen)

# Continue must stay on screen with the longest screen and an error showing
from PySide6.QtCore import QObject, QPointF
from PySide6.QtQuick import QQuickItem

class Fail:
    def deploy(self, opts=None): return type("R", (), {"returncode": 1, "stderr": ""})()

def settle():
    loop = QEventLoop(); QTimer.singleShot(150, loop.quit); loop.exec()

root3 = tempfile.mkdtemp(); os.makedirs(os.path.join(root3, "var/lib"))
w3 = cm.WizardController(core=co.WizardCore(backend=Fail(), root=root3), spawn=lambda f: f())
e3 = QQmlApplicationEngine()
e3.rootContext().setContextProperty("wizard", w3)
e3.load(QUrl.fromLocalFile(os.path.join(WIZ, "qml", "Main.qml")))
win = e3.rootObjects()[0]
btn = win.findChild(QQuickItem, "primaryButton")
check("found Continue", btn is not None)
w3.setRecoveryAck(True)
w3.continueClicked()
settle()
check("error is showing", w3.error != "")
# offscreen fonts are smaller than a real desktop's: a shorter window makes the
# same overflow that clipped Continue on a real box
win.setHeight(360)
settle()
bottom = btn.mapToScene(QPointF(0, btn.height())).y()
check("Continue inside the window with an error showing (%.0f <= %d)" % (bottom, win.height()), bottom <= win.height())
ack = win.findChild(QQuickItem, "ackBox")
check("recovery checkbox inside the window at that height",
      ack.mapToScene(QPointF(0, ack.height())).y() <= win.height())
scroll = win.findChild(QQuickItem, "pageScroll")
check("recovery checkbox can't scroll out of view", not scroll.isAncestorOf(ack))

# while working: buttons off, something moving, window can't be closed away
class Hold:
    def deploy(self, opts=None):
        time.sleep(0.5); return type("R", (), {"returncode": 0, "stderr": ""})()
import time
root4 = tempfile.mkdtemp(); os.makedirs(os.path.join(root4, "var/lib"))
w4 = cm.WizardController(core=co.WizardCore(backend=Hold(), root=root4))
e4 = QQmlApplicationEngine()
e4.rootContext().setContextProperty("wizard", w4)
e4.load(QUrl.fromLocalFile(os.path.join(WIZ, "qml", "Main.qml")))
win4 = e4.rootObjects()[0]
w4.setRecoveryAck(True)
w4.continueClicked()
settle()
check("Continue disabled while working", win4.findChild(QQuickItem, "primaryButton").property("enabled") is False)
check("busy indicator running", win4.findChild(QQuickItem, "busyIndicator").property("running") is True)
win4.close()
settle()
check("first close while working: window stays", win4.isVisible())
hint = win4.findChild(QQuickItem, "closeHint")
check("first close says what closing now means", hint.property("visible") is True and "half-done" in hint.property("text"))
loop = QEventLoop(); QTimer.singleShot(600, loop.quit); loop.exec()
check("Continue back after", w4.busy is False)
check("close hint gone when the work ends", hint.property("visible") is False)

# a hung backend must not make the window impossible to close
class Hang:
    def deploy(self, opts=None):
        time.sleep(30)
root5 = tempfile.mkdtemp(); os.makedirs(os.path.join(root5, "var/lib"))
w5 = cm.WizardController(core=co.WizardCore(backend=Hang(), root=root5))
e5 = QQmlApplicationEngine()
e5.rootContext().setContextProperty("wizard", w5)
e5.load(QUrl.fromLocalFile(os.path.join(WIZ, "qml", "Main.qml")))
win5 = e5.rootObjects()[0]
w5.setRecoveryAck(True)
w5.continueClicked()
settle()
win5.close(); settle()
check("still open after one close", win5.isVisible())
win5.close(); settle()
check("second close quits anyway", not win5.isVisible())

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
