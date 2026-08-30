#!/usr/bin/env python3
"""service classification tests — no systemctl, inject the unit list."""
import os, sys, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

# fake systemctl output through an injected runner
class R:
    def __init__(self, out): self.stdout = out; self.returncode = 0
def fake_run(argv, **kw):
    return R("NetworkManager.service loaded active running Network Manager\n"
             "sshd.service loaded active running OpenSSH\n"
             "systemd-udevd.service loaded active running udev\n"
             "tailscaled.service loaded active running Tailscale\n")

units = sm.running_services(run=fake_run)
check("parses unit basenames", "NetworkManager" in units and "tailscaled" in units)

c = sm.classify_services(units, sm.load_prevent_set())
check("NetworkManager is covered", "network-manager" in c["covered"])
check("sshd is covered", "sshd" in c["covered"])
check("systemd-udevd is schema-owned", "systemd-udevd" in c["schema_owned"])
check("tailscaled is leftover", "tailscaled" in c["leftover"])
check("no unit lands in two buckets",
      not (set(c["covered"]) & set(c["leftover"])) and not (set(c["schema_owned"]) & set(c["leftover"])))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
