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
from PySide6.QtCore import QUrl

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

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
