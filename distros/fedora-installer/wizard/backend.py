import subprocess


class Backend:
    MIGRATE = "/usr/bin/schema-migrate"
    FLIP = "/usr/libexec/schema-init/schema-flip-apply"

    def __init__(self, run=subprocess.run):
        self._run = run

    def _sudo(self, path, *args):
        return self._run(["sudo", path, *args], capture_output=True, text=True)

    def deploy(self, opts=None):
        return self._sudo(self.MIGRATE, "--deploy", "--prebuilt", *(opts or []))

    def arm_flip(self):
        return self._sudo(self.MIGRATE, "--arm-flip")

    def finish(self):
        return self._sudo(self.MIGRATE, "--finish")

    def uninstall(self):
        return self._sudo(self.MIGRATE, "--uninstall")

    def read_stage(self):
        return (self._sudo(self.MIGRATE, "--stage").stdout or "").strip()

    def flip(self, subcmd):
        return self._sudo(self.FLIP, subcmd)
