#!/usr/bin/env python3
"""Notify targeting + edge-triggered transitions for schema-doctor."""
import os, sys, tempfile, importlib.util
TMP = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = TMP
# fabricate an active session (uid 1000) and a GUI proc carrying the /tmp/dbus bus
os.makedirs(os.path.join(TMP, "run/systemd/sessions"), exist_ok=True)
open(os.path.join(TMP, "run/systemd/sessions/1"), "w").write("UID=1000\nACTIVE=1\n")
pdir = os.path.join(TMP, "proc/999"); os.makedirs(pdir, exist_ok=True)
open(os.path.join(pdir, "cmdline"), "wb").write(b"plasmashell\0")
open(os.path.join(pdir, "environ"), "wb").write(
    b"DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-abc\0WAYLAND_DISPLAY=wayland-0\0")
os.makedirs(os.path.join(TMP, "proc/sys/kernel/random"), exist_ok=True)
open(os.path.join(TMP, "proc/sys/kernel/random/boot_id"), "w").write("boot-A\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec); spec.loader.exec_module(sd)

results = []
def check(name, ok, detail=''):
    results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))

CR = sd.CheckResult

uid, env = sd.active_session_env()
check("finds active uid", uid == 1000, str(uid))
check("harvests /tmp/dbus bus", env and env.get("DBUS_SESSION_BUS_ADDRESS") == "unix:path=/tmp/dbus-abc")
check("notify argv targets uid + app", sd._notify_argv(1000, "S", "B")[:6] ==
      ["setpriv", "--reuid", "1000", "--regid", "1000", "--clear-groups"])
check("notify argv guards dash-leading summary with --", sd._notify_argv(1000, "S", "B")[-3:] ==
      ["--", "S", "B"])
check("notify argv survives dash-leading summary/body", sd._notify_argv(1000, "-x", "-y")[-3:] ==
      ["--", "-x", "-y"])

# capture notify_send calls without running anything
sent = []
sd.notify_send = lambda uid, env, summary, body: sent.append((summary, body))

fs = sd.FlapState.load()
# RED transition (chronic check), prev GREEN -> one notify
red = [CR("powerdevil-running", "power", "chronic", "died", "", "chronic")]
sd.notify_transitions(red, fs, enabled=True)
check("RED transition notifies once", len(sent) == 1, str(sent))
# same RED again -> no new notify (edge-triggered)
sd.notify_transitions(red, fs, enabled=True)
check("no repeat notify while still RED", len(sent) == 1)
# recovery to GREEN -> 'all clear'
sent.clear()
green = [CR("powerdevil-running", "power", "clean")]
sd.notify_transitions(green, fs, enabled=True)
check("recovery to GREEN notifies", any("clear" in s.lower() for s, _ in sent), str(sent))
# disabled -> no sends but bookkeeping still updates
sent.clear()
fs2 = sd.FlapState.load()
sd.notify_transitions(red, fs2, enabled=False)
check("disabled suppresses send", sent == [])
check("disabled still records last_state", fs2._c("powerdevil-running")["last_state"] == sd.RED)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
