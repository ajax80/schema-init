import os
import importlib.util as _ilu
from importlib.machinery import SourceFileLoader

_MODDIR = os.path.dirname(os.path.abspath(__file__))


def _load_from_path(name, p):
    loader = SourceFileLoader(name, p)
    spec = _ilu.spec_from_loader(name, loader)
    m = _ilu.module_from_spec(spec); loader.exec_module(m)
    return m


def _load_stage():
    p = os.path.join(_MODDIR, "..", "migrate", "stage.py")
    if not os.path.exists(p):
        p = "/usr/libexec/schema-init/stage.py"
    if not os.path.exists(p):
        raise SystemExit("schema-wizard: stage.py not found — is schema-init-migrate installed?")
    return _load_from_path("stage", p)


def _load_migrate():
    p = os.path.join(_MODDIR, "..", "migrate", "schema-migrate.py")
    if not os.path.exists(p):
        p = "/usr/bin/schema-migrate"
    return _load_from_path("schema_migrate_ro", p)


stage = _load_stage()
_MIGRATE = None


def _migrate():
    global _MIGRATE
    if _MIGRATE is None:
        _MIGRATE = _load_migrate()
    return _MIGRATE

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
    stage.R3_PENDING: "confirming",
    stage.R3_DONE: "final",
    stage.R3_ROLLED_BACK: "rolled_back",
}

# The primary button label per screen. "" means the screen has NO clickable
# action: waiting_reboot is reboot-gated (the user restarts; advance() no-ops
# there), so it must show no button — a dead Continue is what sent a user
# rebooting into a stranded stage.
_ACTION = {
    "welcome": "Continue",
    "waiting_reboot": "",
    "summary": "Continue",
    "final": "Finish",
    "rolled_back": "Finish",
    "dbus_offer": "Switch and restart",
    "confirming": "Continue",
}

_SECONDARY = {
    "dbus_offer": "Skip — I'm done",
}


def _ok(result):
    return getattr(result, "returncode", 0) == 0


_ERROR = {
    "deploy_failed": "Setup couldn't start, so nothing was changed. "
                     "Check that you can run admin commands, then try again.",
    "arm_failed": "Couldn't arm the switch, so nothing was changed. Try again.",
    "finish_failed": "Couldn't finish tidying up. Your computer is fine.",
    "dbus_arm_failed": "Couldn't prepare the message bus switch, so nothing was changed. "
                       "You can skip it — your computer is finished as it is.",
    "confirm_failed": "Couldn't check the message bus switch yet. Try Continue again.",
    "skip_failed": "Couldn't record that choice. Your computer is fine.",
}


def error_for(outcome):
    return _ERROR.get(outcome, "")


class WizardCore:
    def __init__(self, backend=None, root="/"):
        self.backend = backend
        self.root = root

    def current_stage(self):
        return stage.read_stage(self.root)

    def screen_for(self, s):
        return _SCREEN.get(s, "welcome")

    def screen(self):
        s = self.current_stage()
        mig = _migrate()
        if s == stage.DONE and mig.dbus_offered(self.root):
            return "dbus_offer"
        if s == stage.R3_PENDING and mig.dbus_awaiting_reboot(self.root):
            return "waiting_reboot"
        return self.screen_for(s)

    def primary_action(self, screen):
        return _ACTION.get(screen, "")

    def secondary_action(self, screen):
        return _SECONDARY.get(screen, "")

    def deploy_opts(self, selections):
        defaults = {o["key"]: o["default"] for o in ADVANCED}
        flags = []
        for key, default in defaults.items():
            chosen = selections.get(key, default)
            if chosen != default and not chosen:
                flags.append(_OPT_FLAG[key])
        return flags

    def recovery_text(self):
        return _migrate().RECOVERY_TEXT

    def skip(self):
        if self.screen() != "dbus_offer":
            return "noop"
        return "skipped" if _ok(self.backend.skip_dbus()) else "skip_failed"

    def advance(self, consent, recovery_ack, selections):
        if not consent:
            return "noop"
        s = self.current_stage()
        if s == stage.INSTALLED:
            if not recovery_ack:
                return "need_recovery_ack"
            r = self.backend.deploy(self.deploy_opts(selections))
            return "deployed" if _ok(r) else "deploy_failed"
        if s == stage.R1_HEAL:
            r = self.backend.arm_flip()
            return "armed" if _ok(r) else "arm_failed"
        screen = self.screen()
        if screen == "dbus_offer":
            return "dbus_armed" if _ok(self.backend.arm_dbus()) else "dbus_arm_failed"
        if screen == "confirming":
            return "dbus_confirmed" if _ok(self.backend.confirm_dbus()) else "confirm_failed"
        if s in (stage.DONE, stage.ROLLED_BACK, stage.R3_DONE, stage.R3_ROLLED_BACK):
            r = self.backend.finish()
            return "finished" if _ok(r) else "finish_failed"
        return "noop"
