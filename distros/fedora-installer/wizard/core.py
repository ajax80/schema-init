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
    "rolled_back": "Try again",
}

_WHY = (
    (("launcher fell back", "is not schema-dbus"),
     "The new message bus didn't start, so your computer went back to the standard one."),
    (("GetId",),
     "The new message bus didn't answer at all."),
    (("not serving",),
     "The new message bus started, but some system services (networking, sign-in or permissions) "
     "didn't appear on it in time."),
    (("not schema-dbus when the desktop checked",),
     "When the desktop started, the new message bus wasn't the one running."),
    (("not in charge when the desktop checked",),
     "When the desktop started, the new device manager wasn't the one running."),
    (("never confirmed",),
     "The desktop didn't confirm the switch after two restarts."),
    (("schema-udev not running",),
     "The new device manager didn't start."),
    (("/dev/input",),
     "No keyboard or mouse was found with the new device manager."),
    (("/dev/dri",),
     "The display couldn't be opened with the new device manager."),
    (("by-uuid",),
     "Your disks couldn't be found by their IDs with the new device manager."),
    (("missing core node",),
     "Basic system devices were missing with the new device manager."),
)

RETRY_NOTE = ("Trying again is safe: if it fails the same way, your computer puts itself back again. "
              "It helps most after an update.")


def humanize_why(raw):
    raw = (raw or "").strip()
    for keys, text in _WHY:
        if any(k in raw for k in keys):
            return text
    return "The switch didn't pass its health check." if raw else ""


def _ok(result):
    return getattr(result, "returncode", 0) == 0


_ERROR = {
    "reboot_pending": "An update is waiting for a restart, so nothing was changed. "
                      "Restart your computer, then open setup again.",
    "deploy_failed": "Setup couldn't start, so nothing was changed. "
                     "Check that you can run admin commands, then try again.",
    "arm_failed": "Couldn't arm the switch, so nothing was changed. Try again.",
    "finish_failed": "Couldn't finish tidying up. Your computer is fine.",
    "dbus_arm_failed": "Couldn't prepare the message bus switch, so nothing was changed. "
                       "You can skip it — your computer is finished as it is.",
    "confirm_failed": "Couldn't check the message bus switch yet. Try Continue again.",
    "skip_failed": "Couldn't record that choice. Your computer is fine.",
    "retry_failed": "Couldn't set up another try, so nothing was changed. Your computer is fine.",
}


def error_for(outcome):
    return _ERROR.get(outcome, "")


class WizardCore:
    def __init__(self, backend=None, root="/"):
        self.backend = backend
        self.root = root
        self._why = None

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

    def secondary(self):
        screen = self.screen()
        if screen == "rolled_back":
            return self.retry()
        if screen == "dbus_offer":
            return self.skip()
        return "noop"

    def skip(self):
        if self.screen() != "dbus_offer":
            return "noop"
        return "skipped" if _ok(self.backend.skip_dbus()) else "skip_failed"

    def retry(self):
        if self.screen() != "rolled_back":
            return "noop"
        return "retried" if _ok(self.backend.retry()) else "retry_failed"

    def detail(self):
        if self.screen() != "rolled_back":
            return ""
        cur = self.current_stage()
        if self._why is None or self._why[0] != cur:
            self._why = (cur, self.backend.why())
        raw = self._why[1]
        why = humanize_why(raw)
        if not why:
            return RETRY_NOTE
        return "%s\n\n%s\n\n(%s)" % (why, RETRY_NOTE, raw)

    def advance(self, consent, recovery_ack, selections):
        if not consent:
            return "noop"
        s = self.current_stage()
        if s == stage.INSTALLED:
            if not recovery_ack:
                return "need_recovery_ack"
            r = self.backend.deploy(self.deploy_opts(selections))
            if getattr(r, "returncode", 0) == _migrate().REBOOT_PENDING_RC:
                return "reboot_pending"
            return "deployed" if _ok(r) else "deploy_failed"
        if s == stage.R1_HEAL:
            r = self.backend.arm_flip()
            return "armed" if _ok(r) else "arm_failed"
        screen = self.screen()
        if screen == "dbus_offer":
            return "dbus_armed" if _ok(self.backend.arm_dbus()) else "dbus_arm_failed"
        if screen == "confirming":
            return "dbus_confirmed" if _ok(self.backend.confirm_dbus()) else "confirm_failed"
        if s in (stage.ROLLED_BACK, stage.R3_ROLLED_BACK):
            return "finished" if _ok(self.backend.dismiss()) else "finish_failed"
        if s in (stage.DONE, stage.R3_DONE):
            r = self.backend.finish()
            return "finished" if _ok(r) else "finish_failed"
        return "noop"
