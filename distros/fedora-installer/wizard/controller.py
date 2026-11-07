import os
import threading
import importlib.util as _ilu
from PySide6.QtCore import QCoreApplication, QObject, Property, Signal, Slot

_MODDIR = os.path.dirname(os.path.abspath(__file__))


def _load(name, fname):
    spec = _ilu.spec_from_file_location(name, os.path.join(_MODDIR, fname))
    m = _ilu.module_from_spec(spec); spec.loader.exec_module(m); return m


_core = _load("wizard_core", "core.py")
_status = _load("wizard_status", "status.py")
_backend = _load("wizard_backend", "backend.py")


_INSTANCES = []  # keep-alive: QML context properties don't hold a Python ref


def _in_thread(work):
    threading.Thread(target=work, daemon=True).start()


class WizardController(QObject):
    changed = Signal()
    _done = Signal(str)

    def __init__(self, core=None, parent=None, spawn=_in_thread):
        super().__init__(parent)
        _INSTANCES.append(self)
        self._spawn = spawn
        self._busy = False
        self._done.connect(self._finished)
        self._core = core or _core.WizardCore(backend=_backend.Backend())
        self._recovery_ack = False
        self._error = ""
        self._selections = {o["key"]: o["default"] for o in _core.ADVANCED}

    def _get_screen(self):
        return self._core.screen()

    def _get_primary_action(self):
        return self._core.primary_action(self._core.screen())

    def _get_secondary_action(self):
        return self._core.secondary_action(self._core.screen())

    def _get_overall(self):
        try:
            with open(os.path.join(self._core.root, "run/schema-init/doctor-status.json")) as fh:
                return _status.parse_status(fh.read())["overall"]
        except OSError:
            return "UNKNOWN"

    def _get_status_items(self):
        try:
            with open(os.path.join(self._core.root, "run/schema-init/doctor-status.json")) as fh:
                return _status.parse_status(fh.read())["items"]
        except OSError:
            return []

    def _get_recovery_ack(self):
        return self._recovery_ack

    def _get_error(self):
        return self._error

    def _get_detail(self):
        return self._core.detail()

    def _get_busy(self):
        return self._busy

    screen = Property(str, _get_screen, notify=changed)
    primaryAction = Property(str, _get_primary_action, notify=changed)
    secondaryAction = Property(str, _get_secondary_action, notify=changed)
    error = Property(str, _get_error, notify=changed)
    detail = Property(str, _get_detail, notify=changed)
    busy = Property(bool, _get_busy, notify=changed)
    overall = Property(str, _get_overall, notify=changed)
    statusItems = Property('QVariantList', _get_status_items, notify=changed)
    recoveryAck = Property(bool, _get_recovery_ack, notify=changed)

    @Slot()
    def refresh(self):
        self.changed.emit()

    @Slot(bool)
    def setRecoveryAck(self, v):
        self._recovery_ack = bool(v)
        self.changed.emit()

    @Slot(str, bool)
    def setAdvanced(self, key, v):
        self._selections[key] = bool(v)

    @Slot(result=str)
    def recoveryText(self):
        return self._core.recovery_text()

    # sudo blocks for as long as a password prompt stays open, and a deploy for
    # minutes: on the GUI thread that shows as "(Not Responding)".
    def _start(self, action):
        if self._busy:
            return
        self._busy = True
        self._error = ""
        self.changed.emit()

        def work():
            outcome = "crashed"
            try:
                outcome = action()
            except Exception as e:
                print("schema-wizard: %r" % e, flush=True)
            finally:
                self._done.emit(outcome)   # or busy stays set and the window never recovers
        try:
            self._spawn(work)
        except RuntimeError:
            self._finished("crashed")

    @Slot(str)
    def _finished(self, outcome):
        self._busy = False
        self._error = _core.error_for(outcome)
        self.changed.emit()
        if outcome == "finished":
            QCoreApplication.quit()

    @Slot()
    def secondaryClicked(self):
        self._start(self._core.secondary)

    @Slot()
    def continueClicked(self):
        ack, selections = self._recovery_ack, dict(self._selections)
        self._start(lambda: self._core.advance(consent=True, recovery_ack=ack, selections=selections))
