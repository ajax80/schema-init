import os
import importlib.util as _ilu
from PySide6.QtCore import QObject, Property, Signal, Slot

_MODDIR = os.path.dirname(os.path.abspath(__file__))


def _load(name, fname):
    spec = _ilu.spec_from_file_location(name, os.path.join(_MODDIR, fname))
    m = _ilu.module_from_spec(spec); spec.loader.exec_module(m); return m


_core = _load("wizard_core", "core.py")
_status = _load("wizard_status", "status.py")
_backend = _load("wizard_backend", "backend.py")


_INSTANCES = []  # keep-alive: QML context properties don't hold a Python ref


class WizardController(QObject):
    changed = Signal()

    def __init__(self, core=None, parent=None):
        super().__init__(parent)
        _INSTANCES.append(self)
        self._core = core or _core.WizardCore(backend=_backend.Backend())
        self._recovery_ack = False
        self._error = ""
        self._selections = {o["key"]: o["default"] for o in _core.ADVANCED}

    def _get_screen(self):
        return self._core.screen()

    def _get_primary_action(self):
        return self._core.primary_action(self._core.screen())

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

    screen = Property(str, _get_screen, notify=changed)
    primaryAction = Property(str, _get_primary_action, notify=changed)
    error = Property(str, _get_error, notify=changed)
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

    @Slot()
    def continueClicked(self):
        outcome = self._core.advance(consent=True, recovery_ack=self._recovery_ack,
                                     selections=self._selections)
        self._error = _core.error_for(outcome)
        self.changed.emit()
