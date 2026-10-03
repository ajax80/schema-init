#!/usr/bin/env python3
"""migrate --deploy lays down the Plasma session support the ISO kickstart does.

Without it a migrated box had no XDG autostart (the wizard never reopened
after a reboot), no ssh-agent and no plasmashell watchdog — found on a fresh
Fedora 44 KDE VM.

  ./tests/test_migrate_session_support.py     exit 0 all pass, 1 any fail
"""
import os, sys, stat, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

root = tempfile.mkdtemp()
for d in ("etc/xdg/autostart", "usr/bin", "boot/loader/entries", "var/lib", "home/jandoe", "usr/lib/systemd"):
    os.makedirs(os.path.join(root, d))
open(os.path.join(root, "etc/os-release"), "w").write("ID=fedora\n")
open(os.path.join(root, "usr/bin/plasmashell"), "w").close()
for b in ("schema-init", "schema-ctl", "schema-subreaper"):
    open(os.path.join(root, "usr/bin", b), "w").close()
open(os.path.join(root, "etc/fstab"), "w").write("UUID=aaa / ext4 defaults 0 1\n")
open(os.path.join(root, "etc/passwd"), "w").write("jandoe:x:1000:1000::/home/jandoe:/bin/bash\n")
open(os.path.join(root, "boot/loader/entries/f-6.10.0.conf"), "w").write("title Fedora\nversion 6.10.0\noptions root=UUID=aaa ro\n")
open(os.path.join(root, "etc/xdg/autostart/schema-wizard.desktop"), "w").write("[Desktop Entry]\nExec=/usr/bin/schema-wizard\n")
os.environ["MIGRATE_ROOT"] = root; os.environ["MIGRATE_KERNEL"] = "6.10.0"

spec = importlib.util.spec_from_file_location("sm_sess", os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
m.main(["--deploy", "--prebuilt"], run=lambda *a, **k: type("R", (), {"returncode": 0, "stdout": ""})())

def mode(rel):
    return stat.S_IMODE(os.stat(os.path.join(root, rel)).st_mode)

lib = "usr/local/lib/schema/"
check("autostart runner installed, executable", mode(lib + "schema-autostart-runner.sh") == 0o755)
check("plasmashell watchdog installed, executable", mode(lib + "schema-plasma-watchdog.sh") == 0o755)
check("environment.d replay installed", os.path.exists(os.path.join(root, lib + "zzz-environment-d.sh")))
env = sorted(os.listdir(os.path.join(root, lib + "plasma-env")))
check("plasma-env hooks incl. the autostart trigger", env == ["05-kdedefaults.sh", "no-app-scope.sh", "ssh-agent-sock.sh", "zz-schema-autostart.sh"])

ua = os.path.join(root, "home/jandoe/.config/autostart/schema-wizard.desktop")
check("wizard autostart in the user's ~/.config/autostart", os.path.exists(ua))
mf = open(os.path.join(root, m.Manifest.PATH)).read()
check("session files recorded for uninstall", "/usr/local/lib/schema/schema-autostart-runner.sh" in mf
      and "/home/jandoe/.config/autostart/schema-wizard.desktop" in mf)

m.teardown(root)
check("teardown removes the user autostart", not os.path.exists(ua))
check("teardown removes the system autostart", not os.path.exists(os.path.join(root, "etc/xdg/autostart/schema-wizard.desktop")))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
