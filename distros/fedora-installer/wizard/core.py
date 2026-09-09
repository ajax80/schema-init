import os
import importlib.util as _ilu

_MODDIR = os.path.dirname(os.path.abspath(__file__))


def _load_stage():
    p = os.path.join(_MODDIR, "..", "migrate", "stage.py")
    if not os.path.exists(p):
        p = "/usr/libexec/schema-init/stage.py"
    if not os.path.exists(p):
        raise SystemExit("schema-wizard: stage.py not found — is schema-init-migrate installed?")
    spec = _ilu.spec_from_file_location("stage", p)
    m = _ilu.module_from_spec(spec); spec.loader.exec_module(m)
    return m


def _load_migrate():
    p = os.path.join(_MODDIR, "..", "migrate", "schema-migrate.py")
    if not os.path.exists(p):
        p = "/usr/bin/schema-migrate"
    spec = _ilu.spec_from_file_location("schema_migrate_ro", p)
    m = _ilu.module_from_spec(spec); spec.loader.exec_module(m)
    return m


stage = _load_stage()

ADVANCED = [
    {"key": "keep_fallback_entry", "label": "Keep the current system as a backup boot option",
     "default": True, "dangerous": True,
     "warning": "Don't turn this off unless you're certain what it does — it is your way back."},
    {"key": "snapshot", "label": "Take a filesystem snapshot before changing anything",
     "default": True, "dangerous": True,
     "warning": "Don't turn this off unless you're certain what it does."},
    {"key": "udev_flip", "label": "Switch to the schema device manager (second reboot)",
     "default": True, "dangerous": True,
     "warning": "Don't turn this off unless you're certain what it does."},
    {"key": "dbus_broker", "label": "Switch to the schema message bus (second reboot)",
     "default": True, "dangerous": True,
     "warning": "Don't turn this off unless you're certain what it does."},
    {"key": "doctor_timers", "label": "Let the doctor keep watch and self-heal",
     "default": True, "dangerous": False, "warning": ""},
]

_OPT_FLAG = {
    "keep_fallback_entry": "--advanced-no-fallback-entry",
    "snapshot": "--advanced-no-snapshot",
    "udev_flip": "--advanced-no-udev-flip",
    "dbus_broker": "--advanced-no-dbus-broker",
    "doctor_timers": "--advanced-no-doctor-timers",
}

_SCREEN = {
    stage.INSTALLED: "welcome",
    stage.R1_PENDING: "waiting_reboot",
    stage.R1_HEAL: "summary",
    stage.R2_PENDING: "waiting_reboot",
    stage.DONE: "final",
    stage.ROLLED_BACK: "rolled_back",
}


class WizardCore:
    def __init__(self, backend=None, root="/"):
        self.backend = backend
        self.root = root

    def current_stage(self):
        return stage.read_stage(self.root)

    def screen_for(self, s):
        return _SCREEN.get(s, "welcome")

    def screen(self):
        return self.screen_for(self.current_stage())

    def deploy_opts(self, selections):
        defaults = {o["key"]: o["default"] for o in ADVANCED}
        flags = []
        for key, default in defaults.items():
            chosen = selections.get(key, default)
            if chosen != default and not chosen:
                flags.append(_OPT_FLAG[key])
        return flags

    def recovery_text(self):
        return _load_migrate().RECOVERY_TEXT

    def advance(self, consent, recovery_ack, selections):
        if not consent:
            return "noop"
        s = self.current_stage()
        if s == stage.INSTALLED:
            if not recovery_ack:
                return "need_recovery_ack"
            self.backend.deploy(self.deploy_opts(selections))
            return "deployed"
        if s == stage.R1_HEAL:
            self.backend.arm_flip()
            return "armed"
        if s in (stage.DONE, stage.ROLLED_BACK):
            self.backend.finish()
            return "finished"
        return "noop"
