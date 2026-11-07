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
tries_seen = []
def rec(argv, *a, **k):
    calls.append(argv)
    envs.append(k.get("env"))
    p = (k.get("env") or {}).get("SCHEMA_ASKPASS_TRIES")
    tries_seen.append(p and (p, os.stat(p).st_mode & 0o777))
    class R: returncode = 0; stdout = "INSTALLED\n"
    return R()

b = be.Backend(run=rec)

b.deploy(["--advanced-no-snapshot"])
check("deploy authenticates first", calls[-2] == ["sudo", "-A", "-v"])
check("authenticating gets English from sudo, to tell its failures apart", envs[-2].get("LANGUAGE") == "C")
check("authenticating gives askpass a private count file", tries_seen[-2] and tries_seen[-2][1] == 0o600)
check("count file removed afterwards", not os.path.exists(tries_seen[-2][0]))
check("deploy runs through askpass",
      calls[-1] == ["sudo", "-A", be.Backend.MIGRATE, "--deploy", "--prebuilt", "--advanced-no-snapshot"])
check("deploy points sudo at the askpass helper",
      envs[-1] and envs[-1].get("SUDO_ASKPASS") == be.Backend.ASKPASS)
check("the deploy's tree doesn't get LANGUAGE=C", envs[-1].get("LANGUAGE") == os.environ.get("LANGUAGE"))
check("a second prompt for the deploy isn't counted as a retry", "SCHEMA_ASKPASS_TRIES" not in envs[-1])

# auth failures: told apart, and the action never runs
def scripted(auth_rc, auth_err, run_rc=0, run_err=""):
    seen = []
    def run(argv, *a, **k):
        seen.append(argv)
        rc, err = (auth_rc, auth_err) if argv[:3] == ["sudo", "-A", "-v"] else (run_rc, run_err)
        return type("R", (), {"returncode": rc, "stderr": err, "stdout": "out-text"})()
    return be.Backend(run=run), seen

for err, want in (("Sorry, try again.\nsudo: 3 incorrect password attempts\n", "wrong_password"),
                  ("sudo: no password was provided\nsudo: a password is required\n", "password_cancelled"),
                  ("Sorry, try again.\nsudo: no password was provided\nsudo: 1 incorrect password attempt\n", "password_cancelled"),
                  ("alice is not in the sudoers file.\n", "auth_failed")):
    bb, seen = scripted(1, err)
    r = bb.deploy()
    check("auth failure -> " + want, r.returncode != 0 and r.auth == want)
    check("  action not run", len(seen) == 1)

# the action's own failure is not an auth failure, whatever it prints, and is kept
import tempfile
home = tempfile.mkdtemp()
old_home = os.environ.get("HOME"); os.environ["HOME"] = home
bb, seen = scripted(0, "", run_rc=1, run_err="Traceback: 3 incorrect password attempts\n")
r = bb.deploy()
check("action failure is not auth", getattr(r, "auth", "") == "" and r.returncode == 1)
logp = os.path.join(home, ".local/state/schema/setup.log")
check("action failure is logged", os.path.exists(logp) and "Traceback" in open(logp).read() and "out-text" in open(logp).read())
check("log is private", os.stat(logp).st_mode & 0o077 == 0)
os.environ["HOME"] = "/proc/nonexistent"
bb, _ = scripted(0, "", run_rc=1, run_err="boom")
check("unwritable log doesn't hide the failure", bb.deploy().returncode == 1)
if old_home is None: del os.environ["HOME"]
else: os.environ["HOME"] = old_home

# the count file is optional
real_mk = be.tempfile.mkstemp
def nomk(*a, **k): raise OSError("disk full")
be.tempfile.mkstemp = nomk
bb, seen = scripted(0, "")
check("no temp space: still runs", bb.deploy().returncode == 0 and len(seen) == 2)
be.tempfile.mkstemp = real_mk
def rm_during(argv, *a, **k):
    p = (k.get("env") or {}).get("SCHEMA_ASKPASS_TRIES")
    if p: os.remove(p)
    return type("R", (), {"returncode": 0, "stderr": "", "stdout": ""})()
check("count file cleaned away meanwhile: result kept", be.Backend(run=rm_during).deploy().returncode == 0)

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
