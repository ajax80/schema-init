#!/usr/bin/env python3
"""schema-systemctl shim packaging spec tests — script-style."""
import os, sys
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = open(os.path.join(REPO, "schema-init.spec")).read()

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

check("shim in migrate_bins", "schema-systemctl" in SPEC and "migrate_bins" in SPEC)
check("shim shipped in -migrate %files", "%{_bindir}/schema-systemctl" in SPEC)
check("post diverts systemctl", "systemctl.real" in SPEC and "%post migrate" in SPEC)
check("post is idempotent", "! -L /usr/bin/systemctl" in SPEC)
check("postun restore gated on removal", "-eq 0" in SPEC and "%postun migrate" in SPEC)
check("transfiletrigger re-diverts", "%transfiletriggerin migrate -- /usr/bin/systemctl" in SPEC)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
