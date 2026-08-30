#!/usr/bin/env python3
"""prevent-set.list parser tests."""
import os, sys, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

ps = sm.load_prevent_set()
check("returns the four categories",
      set(ps) == {"script", "config", "service", "exclude"})
check("scripts include plasma-session-start.sh", "plasma-session-start.sh" in ps["script"])
check("services include schema-logind", "schema-logind" in ps["service"])
check("config includes plasma-workspace", any("plasma-workspace" in c for c in ps["config"]))
check("frigate is excluded, not a service",
      "frigate" in ps["exclude"] and "frigate" not in ps["service"])
check("comments and blanks ignored", "" not in ps["service"] and "#" not in "".join(ps["service"]))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
