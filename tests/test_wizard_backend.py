#!/usr/bin/env python3
"""wizard Backend — builds sudo helper argv, injectable run, no privilege needed."""
import os, sys, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/wizard/backend.py")
spec = importlib.util.spec_from_file_location("wizard_backend", MOD)
be = importlib.util.module_from_spec(spec); spec.loader.exec_module(be)

results = []
def check(n, ok): results.append(bool(ok)); print(("  ok  " if ok else "  FAIL ") + n)

calls = []
envs = []
def rec(argv, *a, **k):
    calls.append(argv)
    envs.append(k.get("env"))
    class R: returncode = 0; stdout = "INSTALLED\n"
    return R()

b = be.Backend(run=rec)

b.deploy(["--advanced-no-snapshot"])
check("deploy asks for a password through askpass",
      calls[-1] == ["sudo", "-A", be.Backend.MIGRATE, "--deploy", "--prebuilt", "--advanced-no-snapshot"])
check("deploy points sudo at the askpass helper",
      envs[-1] and envs[-1].get("SUDO_ASKPASS") == be.Backend.ASKPASS)

b.arm_flip()
check("arm_flip shells sudo schema-migrate --arm-flip",
      calls[-1] == ["sudo", be.Backend.MIGRATE, "--arm-flip"])

for meth, flag in (("arm_dbus", "--arm-dbus"), ("confirm_dbus", "--confirm-dbus"), ("skip_dbus", "--skip-dbus")):
    getattr(b, meth)()
    check(meth + " shells sudo schema-migrate " + flag, calls[-1] == ["sudo", be.Backend.MIGRATE, flag])

b.finish()
check("finish shells sudo schema-migrate --finish",
      calls[-1] == ["sudo", be.Backend.MIGRATE, "--finish"])

b.uninstall()
check("uninstall asks for a password through askpass",
      calls[-1] == ["sudo", "-A", be.Backend.MIGRATE, "--uninstall"])
check("uninstall points sudo at the askpass helper",
      envs[-1] and envs[-1].get("SUDO_ASKPASS") == be.Backend.ASKPASS)

r = b.read_stage()
check("read_stage runs --stage and returns trimmed stdout", r == "INSTALLED")

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
