#!/usr/bin/env python3
"""schema-init-migrate subpackage + narrow sudoers spec tests — script-style."""
import importlib.util, os, re, shutil, subprocess, sys
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
    m = re.search(r"^%files " + pkg + r"\b.*?$(.*?)(?=^%(?:package|files|description|pre|post|changelog)\b|\Z)",
                  spec, re.M | re.S)
    return m.group(1) if m else ""
daemons, migrate = files_section("daemons"), files_section("migrate")
for f in ("schema-dbus-run.sh", "dissect_policy.py", "schema-dbus-flip.sh",
          "schema-dbus-flip-healthcheck.sh", "schema-dbus/masked"):
    check(f + " owned by -daemons, not -migrate", f in daemons and f not in migrate)

# sudoers grant is narrow (no blanket ALL)
s = open(os.path.join(REPO, "distros/fedora-installer/migrate/schema-wizard.sudoers")).read()
rule = "".join(l for l in s.splitlines() if not l.startswith("#")).replace("\\", " ")
grants = [" ".join(g.split()) for g in rule.split("NOPASSWD:", 1)[1].split(",")]
check("sudoers is narrow", grants and "ALL" not in grants)
check("sudoers: every grant pins its arguments", all(len(g.split()) >= 2 for g in grants))
check("sudoers: no argument wildcards", "*" not in rule)
for flag in ("--deploy", "--uninstall", "--discover", "--dry-run", "--advanced"):
    check("sudoers: " + flag + " needs a password", flag not in rule)
check("sudoers: flip helper not granted", "schema-flip-apply" not in rule)

# every call the wizard makes without a password is granted, exactly
spec_be = importlib.util.spec_from_file_location(
    "wizard_backend", os.path.join(REPO, "distros/fedora-installer/wizard/backend.py"))
be = importlib.util.module_from_spec(spec_be); spec_be.loader.exec_module(be)
seen = []
def rec(argv, *a, **k):
    seen.append(argv)
    class R: returncode = 0; stdout = ""
    return R()
b = be.Backend(run=rec)
for meth in ("arm_flip", "arm_dbus", "confirm_dbus", "skip_dbus", "retry", "dismiss", "why", "finish", "read_stage"):
    getattr(b, meth)()
    check("sudoers grants wizard " + meth, " ".join(seen[-1][1:]) in grants)
check("sudoers grants nothing the wizard doesn't call",
      set(grants) == {" ".join(c[1:]) for c in seen})
if shutil.which("visudo"):
    r = subprocess.run(["visudo", "-cf", os.path.join(REPO, "distros/fedora-installer/migrate/schema-wizard.sudoers")],
                       capture_output=True, text=True)
    check("sudoers parses under visudo", r.returncode == 0 or "Permission denied" in r.stderr)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
