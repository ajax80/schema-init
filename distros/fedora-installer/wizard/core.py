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


stage = _load_stage()

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
