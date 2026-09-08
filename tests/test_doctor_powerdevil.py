#!/usr/bin/env python3
"""powerdevil-running tests — fake /proc + session under DOCTOR_ROOT, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
os.makedirs(os.path.join(root, "run/systemd/sessions"))
with open(os.path.join(root, "run/systemd/sessions", "1"), "w") as fh:
    fh.write("UID=1000\nVTNR=1\n")


def mkproc(pid, cmd, env=None):
    d = os.path.join(root, "proc", str(pid))
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "cmdline"), "wb") as fh:
        fh.write(cmd.encode() + b"\0")
    with open(os.path.join(d, "environ"), "wb") as fh:
        fh.write(b"".join((k + "=" + v).encode() + b"\0" for k, v in (env or {}).items()))


spec = importlib.util.spec_from_file_location(
    "schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

c = sd.PowerDevilRunning()

# plasmashell carries a live session bus → powerdevil is launchable → healable
mkproc(1000, "plasmashell", {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
                             "XDG_RUNTIME_DIR": "/run/user/1000",
                             "WAYLAND_DISPLAY": "wayland-0"})
f = c.detect()
check("detect flags powerdevil down", f is not None)
check("detail names X-systemd-skip", f is not None and "X-systemd-skip" in f.detail)
check("finding is now healable (session bus present)", f is not None and f.healable is True)
check("check grade is SAFE (self-heals)", c.grade == sd.SAFE)

# heal launches powerdevil in the session via the injected spawner
launched = {}
def fake_spawn(uid, env, argv):
    launched["uid"] = uid; launched["argv"] = argv; launched["env"] = env
    mkproc(1200, "/usr/libexec/org_kde_powerdevil")   # simulate it coming up
    return 4242
sd.spawn_in_session = fake_spawn

snap = c.snapshot()
c.heal(f)
check("heal spawned as the session uid", launched.get("uid") == 1000)
check("heal launched org_kde_powerdevil", any("org_kde_powerdevil" in a for a in launched.get("argv", [])))
check("heal passed the session bus through", "DBUS_SESSION_BUS_ADDRESS" in (launched.get("env") or {}))
check("verify passes once powerdevil runs", c.verify() is True)

# back_out kills exactly the pid we started
killed = []
real_kill = os.kill
os.kill = lambda pid, sig: killed.append((pid, sig))
c.back_out(snap)
os.kill = real_kill
check("back_out kills the pid we started", (4242,) == tuple(k[0] for k in killed))

# not healable when there is no session bus to launch into
real_ase, real_running = sd.active_session_env, sd._running
sd.active_session_env = lambda: (1000, None)      # session has no DBUS bus
sd._running = lambda needle, tbl=None: False       # and powerdevil is down
f3 = sd.PowerDevilRunning().detect()
sd.active_session_env, sd._running = real_ase, real_running
check("not healable without a session bus", f3 is not None and f3.healable is False)

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
