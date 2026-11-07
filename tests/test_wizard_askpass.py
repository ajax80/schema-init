#!/usr/bin/env python3
"""sudo askpass dialog: real Qt dialog offscreen, password on stdout, cancel fails."""
import os, subprocess, sys
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASKPASS = os.path.join(REPO, "distros/fedora-installer/wizard/askpass.py")

try:
    import PySide6  # noqa: F401
except ImportError:
    print("SKIP  PySide6 not installed"); print("PASS"); sys.exit(0)

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

# answers the dialog from a timer once it is up, the way a user would
DRIVER = """
import importlib.util, sys
from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QApplication, QLineEdit
app = QApplication(["askpass"])
action = sys.argv[2]
seen = {}
def act():
    d = QApplication.activeModalWidget()
    seen["echo"] = d.textEchoMode() == QLineEdit.Password
    seen["label"] = d.labelText()
    d.setTextValue("s3cret")
    d.accept() if action == "ok" else d.reject()
QTimer.singleShot(200, act)
s = importlib.util.spec_from_file_location("askpass", sys.argv[1])
m = importlib.util.module_from_spec(s); s.loader.exec_module(m)
rc = m.main(["askpass", "[sudo] password for u: "])
print("ECHO_HIDDEN=%s LABEL=%s" % (seen.get("echo"), seen.get("label")), file=sys.stderr)
sys.exit(rc)
"""

def drive(action, env=None):
    return subprocess.run([sys.executable, "-c", DRIVER, ASKPASS, action],
                          capture_output=True, text=True, timeout=30, env=dict(os.environ, **(env or {})))

r = drive("ok")
check("accept exits 0", r.returncode == 0)
check("accept prints only the password", r.stdout == "s3cret\n")
check("input is hidden", "ECHO_HIDDEN=True" in r.stderr)
check("shows sudo's prompt", "password for u" in r.stderr)

r = drive("cancel")
check("cancel exits nonzero", r.returncode != 0)
check("cancel prints nothing", r.stdout == "")

# sudo starts a new askpass per attempt, so the count lives in a file
import tempfile
fd, tries = tempfile.mkstemp(); os.close(fd)
r = drive("ok", {"SCHEMA_ASKPASS_TRIES": tries})
check("first attempt: no complaint", "try again" not in r.stderr.lower())
r = drive("ok", {"SCHEMA_ASKPASS_TRIES": tries})
check("second attempt says the password was wrong", "wasn't right" in r.stderr and "try again" in r.stderr.lower())
check("second attempt still shows sudo's prompt", "password for u" in r.stderr)
check("second attempt prints only the password", r.stdout == "s3cret\n")
check("the count file never holds the password", "s3cret" not in open(tries).read())
os.remove(tries)
r = drive("ok", {"SCHEMA_ASKPASS_TRIES": tries + ".gone/x"})
check("unwritable count file doesn't stop the prompt", r.returncode == 0 and r.stdout == "s3cret\n")

mk = open(os.path.join(REPO, "Makefile")).read()
check("installed executable, for sudo to run it",
      "install -m 0755 distros/fedora-installer/wizard/askpass.py $(DESTDIR)$(PREFIX)/libexec/schema-init/wizard/askpass.py" in mk)
check("source has a python shebang", open(ASKPASS).readline().startswith("#!/usr/bin/python3"))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
