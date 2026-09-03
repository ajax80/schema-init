#!/usr/bin/env python3
"""dbus-bus check tests — socket presence and probe both injected, no live bus."""
import os, sys, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

c = sd.DbusBus()

# socket present + bus answers → healthy
c.socket_present = lambda: True
c.probe = lambda: (0, 's "abc123"')
check("healthy bus passes", c.detect() is None)

# socket missing → flagged, names the socket path
c.socket_present = lambda: False
f = c.detect()
check("missing socket flagged", f is not None and "system_bus_socket" in f.detail)

# socket present but no answer → flagged, names GetId
c.socket_present = lambda: True
c.probe = lambda: (1, "Connection refused")
f = c.detect()
check("silent bus flagged", f is not None and "GetId" in f.detail)

# grade DEFERRED, not auto-healed
check("grade DEFERRED", c.grade == sd.DEFERRED)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
