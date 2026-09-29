#!/usr/bin/env python3
"""The systemctl shim's unit->.svc alias table must match migrate's UNIT_TO_SVC."""
import ast, os, re, sys
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

src = open(os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")).read()
py = {}
for node in ast.parse(src).body:
    if isinstance(node, ast.Assign) and any(getattr(t, "id", "") == "UNIT_TO_SVC" for t in node.targets):
        py = ast.literal_eval(node.value)
py_aliases = {k: v for k, v in py.items() if k != v}

hdr = open(os.path.join(REPO, "systemctl_shim.h")).read()
body = hdr[hdr.index("unit_alias("):hdr.index("svc_name_for(")]
c_aliases = dict(re.findall(r'\{\s*"([^"]+)",\s*"([^"]+)"\s*\}', body))

check("migrate UNIT_TO_SVC found", py)
check("shim alias table found", c_aliases)
check("every renaming in UNIT_TO_SVC is in the shim", py_aliases.items() <= c_aliases.items())
check("the shim has no alias migrate lacks", c_aliases.items() <= py_aliases.items())
check("NetworkManager -> network-manager", c_aliases.get("NetworkManager") == "network-manager")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
