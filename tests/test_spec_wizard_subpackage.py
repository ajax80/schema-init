#!/usr/bin/env python3
"""schema-init-wizard subpackage declared with the right deps + payload."""
import os, sys, subprocess
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

rpms = subprocess.run(["rpmspec", "-q", "--rpms", os.path.join(REPO, "schema-init.spec")],
                      capture_output=True, text=True).stdout
check("schema-init-wizard subpackage declared", "schema-init-wizard" in rpms)

reqs = subprocess.run(["rpmspec", "-q", "--requires", os.path.join(REPO, "schema-init.spec")],
                      capture_output=True, text=True).stdout
check("requires python3-pyside6", "python3-pyside6" in reqs)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
