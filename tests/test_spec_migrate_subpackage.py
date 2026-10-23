#!/usr/bin/env python3
"""schema-init-migrate subpackage + narrow sudoers spec tests — script-style."""
import os, sys, subprocess
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

# the -migrate subpackage is declared
out = subprocess.run(["rpmspec", "-q", "--rpms", os.path.join(REPO, "schema-init.spec")],
                     capture_output=True, text=True).stdout
check("schema-init-migrate subpackage declared", "schema-init-migrate" in out)

# schema-udev is not double-listed into the base package
out = subprocess.run(["rpmspec", "-q", "--qf", "[%{FILENAMES}\\n]", os.path.join(REPO, "schema-init.spec")],
                     capture_output=True, text=True).stdout
check("schema-udev not in base package", out.count("/bin/schema-udev") <= 1)

# the broker's boot-time runtime ships in -daemons, so a box flipped without
# -migrate (by hand, or a future rail) still gets launcher/dissolver fixes
spec = open(os.path.join(REPO, "schema-init.spec")).read()
def files_section(pkg):
    body = spec.split("\n%files " + pkg + "\n", 1)[1]
    return body.split("\n\n", 1)[0]
daemons, migrate = files_section("daemons"), files_section("migrate")
for f in ("schema-dbus-run.sh", "dissect_policy.py", "schema-dbus-flip.sh",
          "schema-dbus-flip-healthcheck.sh", "schema-dbus/masked"):
    check(f + " owned by -daemons, not -migrate", f in daemons and f not in migrate)

# sudoers grant is narrow (no blanket ALL)
s = open(os.path.join(REPO, "distros/fedora-installer/migrate/schema-wizard.sudoers")).read()
check("sudoers is narrow", "NOPASSWD" in s and "ALL=(root) NOPASSWD: ALL" not in s)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
