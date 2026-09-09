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
def rec(argv, *a, **k):
    calls.append(argv)
    class R: returncode = 0; stdout = "INSTALLED\n"
    return R()

b = be.Backend(run=rec)

b.deploy(["--advanced-no-snapshot"])
check("deploy shells sudo schema-migrate --deploy --prebuilt + opts",
      calls[-1] == ["sudo", be.Backend.MIGRATE, "--deploy", "--prebuilt", "--advanced-no-snapshot"])

b.arm_flip()
check("arm_flip shells sudo schema-migrate --arm-flip",
      calls[-1] == ["sudo", be.Backend.MIGRATE, "--arm-flip"])

b.finish()
check("finish shells sudo schema-migrate --finish",
      calls[-1] == ["sudo", be.Backend.MIGRATE, "--finish"])

b.uninstall()
check("uninstall shells sudo schema-migrate --uninstall",
      calls[-1] == ["sudo", be.Backend.MIGRATE, "--uninstall"])

r = b.read_stage()
check("read_stage runs --stage and returns trimmed stdout", r == "INSTALLED")

b.flip("reboot")
check("flip shells sudo schema-flip-apply <subcmd>",
      calls[-1] == ["sudo", be.Backend.FLIP, "reboot"])

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
