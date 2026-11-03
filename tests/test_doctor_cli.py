#!/usr/bin/env python3
"""CLI/config/report tests for schema-doctor."""
import os, sys, json, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def load(root):
    os.environ["DOCTOR_ROOT"] = root
    spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(name, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}")

root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "etc/schema-init"))
sd = load(root)

# config: missing file → heal on, nothing disabled
heal, dis, notify = sd.read_config(); check("default config heals", heal is True and dis == set())

# config: heal=no + disable list
with open(os.path.join(root, "etc/schema-init/doctor.conf"), "w") as fh:
    fh.write("heal=no\ndisable=foo, bar\n")
heal, dis, notify = sd.read_config(); check("config parsed", heal is False and dis == {"foo", "bar"})

# report writer creates a 0644 file
res = [sd.CheckResult("x", "x summary", "healed", "was broken", "systemd: rw", "healed")]
p = sd.write_report(sd.render_report(res))
check("report written 0644", os.path.exists(p) and (os.stat(p).st_mode & 0o777) == 0o644)
check("report names the check", "x summary" in open(p).read())

# json render is valid and carries state
j = json.loads(sd.render_json(res)); check("json ok", j[0]["state"] == "healed")

# session gate: a real session file (id != 31) satisfies it fast
os.makedirs(os.path.join(root, "run/systemd/sessions"))
open(os.path.join(root, "run/systemd/sessions", "1"), "w").close()
check("session gate sees session 1", sd.wait_for_session(1.0) is True)

# main always returns 0 even with an exploding check in the registry
class Boom(sd.Check):
    name = "boom"; summary = "boom"; grade = sd.SAFE
    def detect(self): raise RuntimeError("kaboom")
sd.REGISTRY[:] = [Boom()]
check("main survives an exploding check", sd.main(["--check"]) == 0)

# a non-root --check: the report dir isn't writable. Still prints, still exits 0
import io, contextlib
logd = os.path.join(root, "var/log/schema-init")
if os.path.exists(os.path.join(logd, "doctor-report.txt")):
    os.unlink(os.path.join(logd, "doctor-report.txt"))   # an existing file stays writable in a 0555 dir
os.chmod(logd, 0o555)
buf, err = io.StringIO(), io.StringIO()
try:
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
        rc = sd.main(["--check"])
except Exception as e:
    rc = "raised %r" % e
finally:
    os.chmod(logd, 0o755)
if os.geteuid() != 0:   # root writes through a 0555 dir
    check("unwritable report dir: exit 0, results printed, says why",
          rc == 0 and "boom" in buf.getvalue() and "report not written" in err.getvalue())

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
