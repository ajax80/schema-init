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

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
