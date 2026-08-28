#!/usr/bin/env python3
"""End-to-end mode wiring: --periodic writes state+status, --status reads, --heal is boot."""
import os, sys, tempfile, importlib.util
TMP = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = TMP
os.makedirs(os.path.join(TMP, "proc/sys/kernel/random"), exist_ok=True)
open(os.path.join(TMP, "proc/sys/kernel/random/boot_id"), "w").write("boot-A\n")
os.makedirs(os.path.join(TMP, "etc/schema-init"), exist_ok=True)
open(os.path.join(TMP, "etc/schema-init/doctor.conf"), "w").write("notify=no\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(name, ok, detail=''):
    results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))

# empty REGISTRY -> all-clean run, deterministic
sd.REGISTRY[:] = []

check("read_config parses notify", sd.read_config() == (True, set(), False))

rc = sd.main(["--heal", "--periodic"])
check("periodic exits 0", rc == 0)
check("periodic writes state file", os.path.exists(os.path.join(TMP, "var/lib/schema-init/doctor-state")))
check("periodic writes status file", os.path.exists(os.path.join(TMP, "run/schema-init/doctor-status")))

import io, contextlib
buf = io.StringIO()
with contextlib.redirect_stdout(buf):
    sd.main(["--status"])
check("--status prints GREEN (empty registry)", "GREEN" in buf.getvalue())

# boot mode writes status but not a fresh state file: remove state, run --heal, assert none created
os.remove(os.path.join(TMP, "var/lib/schema-init/doctor-state"))
sd.main(["--heal"])
check("boot mode writes no state file", not os.path.exists(os.path.join(TMP, "var/lib/schema-init/doctor-state")))
check("boot mode still writes status", os.path.exists(os.path.join(TMP, "run/schema-init/doctor-status")))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
