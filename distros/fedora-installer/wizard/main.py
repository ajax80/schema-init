import os
import sys
import importlib.util as _ilu
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtCore import QUrl

_MODDIR = os.path.dirname(os.path.abspath(__file__))


def _controller():
    spec = _ilu.spec_from_file_location("wizard_controller", os.path.join(_MODDIR, "controller.py"))
    m = _ilu.module_from_spec(spec); spec.loader.exec_module(m)
    return m.WizardController()


def main(argv):
    app = QGuiApplication.instance() or QGuiApplication(argv)
    engine = QQmlApplicationEngine()
    engine.rootContext().setContextProperty("wizard", _controller())
    engine.load(QUrl.fromLocalFile(os.path.join(_MODDIR, "qml", "Main.qml")))
    if not engine.rootObjects():
        return 1
    return app.exec()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
