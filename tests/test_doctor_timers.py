#!/usr/bin/env python3
"""unported-timers tests — fake systemd + schema trees under DOCTOR_ROOT, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root

spec = importlib.util.spec_from_file_location(
    "schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

def put(rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "w").write(text)

lib, wants = "usr/lib/systemd/system", "etc/systemd/system/timers.target.wants"
svcd = "etc/schema-init/services"
os.makedirs(os.path.join(root, wants))
os.makedirs(os.path.join(root, svcd))

def enable(t, target=None):
    os.symlink(target or "/" + lib + "/" + t, os.path.join(root, wants, t))

def timer(stem, exe, unit=None):
    put(f"{lib}/{stem}.timer", "[Timer]\nOnCalendar=daily\n" + (f"Unit={unit}\n" if unit else ""))
    put(f"{lib}/{unit or stem + '.service'}", f"[Service]\nType=oneshot\nExecStart={exe} --run\n")

c = sd.UnportedTimers()
check("clean with no enabled timers", c.detect() is None)

timer("logrotate", "/usr/sbin/logrotate")
put("usr/share/schema-init/services/logrotate.svc", "name=logrotate\non_calendar=00:10\n")
enable("logrotate.timer")
f = c.detect()
check("RPM-only box missing logrotate.svc is flagged", f is not None and "logrotate.timer" in f.detail)
check("remedy copies the shipped .svc", f is not None and "cp /usr/share/schema-init/services/logrotate.svc" in f.detail)

put(f"{svcd}/logrotate.svc", "name=logrotate\nexec=/usr/sbin/logrotate\non_calendar=00:10\n")
check("same-name .svc covers it", c.detect() is None)

timer("restic-backup", "/usr/bin/restic")
enable("restic-backup.timer")
put(f"{svcd}/nightly-backup.svc", "name=nightly-backup\nexec=/usr/bin/restic\non_calendar=03:30\n")
check("timer .svc running the same binary covers it", c.detect() is None)

timer("plocate-updatedb", "/usr/sbin/updatedb")
enable("plocate-updatedb.timer")
put(f"{svcd}/updatedb-daemon.svc", "name=updatedb-daemon\nexec=/usr/sbin/updatedb\n")
f = c.detect()
check("a non-timer .svc on the same binary does not cover it",
      f is not None and "schema-import plocate-updatedb.timer" in f.detail)
os.unlink(os.path.join(root, wants, "plocate-updatedb.timer"))

timer("mlocate", "/usr/sbin/mlocate-run", unit="mlocate-run.service")
enable("mlocate.timer")
put(f"{svcd}/mlocate-run.svc", "name=mlocate-run\nexec=/usr/sbin/mlocate-run\n")
check("Unit= target's .svc covers it", c.detect() is None)

timer("systemd-tmpfiles-clean", "/usr/bin/systemd-tmpfiles")
enable("systemd-tmpfiles-clean.timer")
timer("fstrim", "/usr/sbin/fstrim")
enable("fstrim.timer", target="/dev/null")
put(f"{lib}/orphan.timer", "[Timer]\nOnCalendar=daily\n")
enable("orphan.timer")
check("reclaimed, masked and service-less timers are ignored", c.detect() is None)

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
