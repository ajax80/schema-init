import os
import subprocess
import tempfile


class Backend:
    MIGRATE = "/usr/bin/schema-migrate"
    ASKPASS = "/usr/libexec/schema-init/wizard/askpass.py"

    def __init__(self, run=subprocess.run):
        self._run = run

    def _sudo(self, path, *args):
        return self._run(["sudo", path, *args], capture_output=True, text=True)

    # Not in the sudoers grant. A GUI has no terminal for sudo to prompt on.
    def _sudo_password(self, path, *args):
        # LANGUAGE=C: the core tells sudo's failures apart by their English text
        fd, tries = tempfile.mkstemp(prefix="schema-askpass-")
        os.close(fd)
        try:
            return self._run(["sudo", "-A", path, *args], capture_output=True, text=True,
                             env=dict(os.environ, SUDO_ASKPASS=self.ASKPASS, LANGUAGE="C",
                                      SCHEMA_ASKPASS_TRIES=tries))
        finally:
            os.remove(tries)

    def deploy(self, opts=None):
        return self._sudo_password(self.MIGRATE, "--deploy", "--prebuilt", *(opts or []))

    def arm_flip(self):
        return self._sudo(self.MIGRATE, "--arm-flip")

    def arm_dbus(self):
        return self._sudo(self.MIGRATE, "--arm-dbus")

    def confirm_dbus(self):
        return self._sudo(self.MIGRATE, "--confirm-dbus")

    def skip_dbus(self):
        return self._sudo(self.MIGRATE, "--skip-dbus")

    def retry(self):
        return self._sudo(self.MIGRATE, "--retry")

    def dismiss(self):
        return self._sudo(self.MIGRATE, "--dismiss")

    def why(self):
        return (self._sudo(self.MIGRATE, "--why").stdout or "").strip()

    def finish(self):
        return self._sudo(self.MIGRATE, "--finish")

    def uninstall(self):
        return self._sudo_password(self.MIGRATE, "--uninstall")

    def read_stage(self):
        return (self._sudo(self.MIGRATE, "--stage").stdout or "").strip()
