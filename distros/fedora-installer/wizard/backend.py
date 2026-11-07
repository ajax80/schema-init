import os
import subprocess
import tempfile


class AuthFailed:
    returncode = 1
    stdout = ""
    stderr = ""

    def __init__(self, auth):
        self.auth = auth


class Backend:
    MIGRATE = "/usr/bin/schema-migrate"
    ASKPASS = "/usr/libexec/schema-init/wizard/askpass.py"

    def __init__(self, run=subprocess.run):
        self._run = run

    def _sudo(self, path, *args):
        return self._run(["sudo", path, *args], capture_output=True, text=True)

    # Authenticating apart from the action is what lets a failure say whether
    # anything was touched. LANGUAGE=C: its failures are told apart by their
    # English text, and this must not reach the action's process tree.
    def _authenticate(self):
        env = dict(os.environ, SUDO_ASKPASS=self.ASKPASS, LANGUAGE="C")
        tries = None
        try:
            fd, tries = tempfile.mkstemp(prefix="schema-askpass-")
            os.close(fd)
            env["SCHEMA_ASKPASS_TRIES"] = tries
        except OSError:
            pass
        try:
            r = self._run(["sudo", "-A", "-v"], capture_output=True, text=True, env=env)
        finally:
            if tries:
                try:
                    os.remove(tries)
                except OSError:
                    pass
        if r.returncode == 0:
            return ""
        err = getattr(r, "stderr", "") or ""
        # a cancel after a wrong try prints both
        if "no password was provided" in err:
            return "password_cancelled"
        return "wrong_password" if "incorrect password" in err else "auth_failed"

    def _keep_log(self, r):
        d = os.path.expanduser("~/.local/state/schema")
        try:
            os.makedirs(d, exist_ok=True)
            fd = os.open(os.path.join(d, "setup.log"), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write((getattr(r, "stdout", "") or "") + "\n" + (getattr(r, "stderr", "") or ""))
        except OSError:
            pass

    # Not in the sudoers grant. A GUI has no terminal for sudo to prompt on.
    def _sudo_password(self, path, *args):
        auth = self._authenticate()
        if auth:
            return AuthFailed(auth)
        # -A again: with timestamp_timeout=0 sudo asks a second time
        r = self._run(["sudo", "-A", path, *args], capture_output=True, text=True,
                      env=dict(os.environ, SUDO_ASKPASS=self.ASKPASS))
        if r.returncode != 0:
            self._keep_log(r)
        return r

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
