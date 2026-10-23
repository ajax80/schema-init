#!/usr/bin/env python3
import os
import sys
import signal
import subprocess
import tempfile
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
MODPATH = os.path.join(HERE, "..", "scripts", "schema-systemd1-session.py")


def load():
    spec = importlib.util.spec_from_file_location("stu_session", MODPATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    m = load()
    home = os.path.expanduser("~")

    # F4: % specifiers
    assert m._expand("%h/.local/bin/app", {}, "app.service") == home + "/.local/bin/app"
    assert m._expand("%%literal", {}, "") == "%literal"
    assert m._expand("%N", {}, "foo.service") == "foo"
    assert m._expand("%n", {}, "foo.service") == "foo.service"
    assert m._expand("%U", {}, "") == str(os.getuid())
    # F4: $VAR / ${VAR} from the provided env; unknown -> empty; specifiers first
    env = {"FOO": "/opt/x", "BAR": "9"}
    assert m._expand("$FOO/bin", env, "") == "/opt/x/bin"
    assert m._expand("${FOO}-${BAR}", env, "") == "/opt/x-9"
    assert m._expand("$MISSING/y", env, "") == "/y"
    assert m._expand("%h/$FOO", env, "") == home + "//opt/x"
    print("test_systemd1_session: expansion OK")

    # F2/F1: restart-action decision table (StartUnit on a live unit is a no-op)
    assert m._restart_action("StartUnit", False) == "spawn"
    assert m._restart_action("StartUnit", True) == "noop"
    assert m._restart_action("RestartUnit", False) == "spawn"
    assert m._restart_action("RestartUnit", True) == "restart"
    assert m._restart_action("ReloadOrRestartUnit", True) == "restart"
    assert m._restart_action("TryRestartUnit", False) == "noop"
    assert m._restart_action("TryRestartUnit", True) == "restart"
    assert m._restart_action("ReloadOrTryRestartUnit", False) == "noop"
    assert m._restart_action("ReloadOrTryRestartUnit", True) == "restart"
    print("test_systemd1_session: restart-action OK")

    # starttime probe: real pid resolves, bogus pid is None
    assert m._proc_starttime(os.getpid()) is not None
    assert m._proc_starttime(2 ** 31 - 1) is None

    # F2: live tracking + stop via os.kill (works under SIGCHLD=SIG_IGN)
    p = subprocess.Popen(["sleep", "30"])
    m._spawned["sleeper.service"] = (p.pid, m._proc_starttime(p.pid))
    assert m._unit_alive("sleeper.service") is True
    assert m._stop_unit("sleeper.service") is True
    rc = p.wait()
    assert rc in (-signal.SIGTERM, -signal.SIGKILL), rc  # actually signalled dead
    assert m._unit_alive("sleeper.service") is False      # dropped from registry
    assert m._unit_alive("nope.service") is False
    assert m._stop_unit("nope.service") is False

    # F2: pid-reuse guard — a live pid with a mismatched starttime is NOT ours,
    # so it reads as dead and is never signalled.
    q = subprocess.Popen(["sleep", "30"])
    m._spawned["stale.service"] = (q.pid, "0")   # wrong starttime
    assert m._unit_alive("stale.service") is False
    m._spawned["stale.service"] = (q.pid, "0")   # _unit_alive popped it; re-register
    assert m._stop_unit("stale.service") is False
    os.kill(q.pid, 0)                            # innocent process untouched
    q.terminate(); q.wait()
    print("test_systemd1_session: tracking OK")

    # ExecStart parse: leading modifier, line continuation, Environment=
    with tempfile.NamedTemporaryFile("w", suffix=".service", delete=False) as f:
        f.write("[Service]\n"
                "Environment=FOO=bar BAZ=qux\n"
                "ExecStart=-/usr/bin/tool --flag \\\n"
                "  --home %h\n")
        unit = f.name
    exec_path, argv, envl = m._parse_unit_execstart(unit)
    os.unlink(unit)
    assert exec_path == "/usr/bin/tool", exec_path
    assert argv == ["/usr/bin/tool", "--flag", "--home", "%h"], argv
    assert "FOO=bar" in envl and "BAZ=qux" in envl, envl
    print("test_systemd1_session: parse OK")

    # schema-user hand-off: verb table, then state lookup through a stub schema-ctl
    v = m._schema_user_verb
    assert v("StartUnit", "DORMANT") == "start"
    assert v("StartUnit", "FULL_TRUST") is None
    assert v("RestartUnit", "FULL_TRUST") == "restart"
    assert v("RestartUnit", "EXCISED") == "start"
    assert v("TryRestartUnit", "DORMANT") is None
    assert v("ReloadOrTryRestartUnit", "PERFECT") == "restart"
    assert v("StopUnit", "FULL_TRUST") == "stop"
    d = tempfile.mkdtemp()
    os.makedirs(os.path.join(d, "systemd/user"))
    os.makedirs(os.path.join(d, "schema-user"))
    with open(os.path.join(d, "systemd/user/kded.service"), "w") as f:
        f.write("[Service]\nExecStart=/bin/true\n")
    os.symlink("kded.service", os.path.join(d, "systemd/user/dbus-org.kde.kded.service"))
    with open(os.path.join(d, "systemd/user/x.service"), "w") as f:
        f.write("[Service]\nExecStart=/bin/true\n")
    # x.svc exists (a timer's) but x.service is not in the map: not schema-user's
    with open(os.path.join(d, "schema-user/units"), "w") as f:
        f.write("kded.service\tkded\nbar.service\tbar\nfoo.socket\tbar\n")
    saved_dirs, saved_rt = m.USER_UNIT_DIRS, os.environ.get("XDG_RUNTIME_DIR")
    m.USER_UNIT_DIRS = (os.path.join(d, "systemd/user"),)
    os.environ["XDG_RUNTIME_DIR"] = d
    assert m._schema_user_svc("kded.service") == "kded"
    assert m._schema_user_svc("dbus-org.kde.kded.service") == "kded"   # alias
    assert m._schema_user_svc("bar.service") == "bar"                  # socket Service=
    assert m._schema_user_svc("x.service") is None
    assert m._schema_user_svc("foo.socket") is None                    # only .service
    assert m._unit_name("org.kde.kded6") == "org.kde.kded6.service"
    os.unlink(os.path.join(d, "schema-user/units"))
    assert m._schema_user_svc("kded.service") is None                  # no map, no schema-user
    ctl = os.path.join(d, "ctl")
    with open(ctl, "w") as f:
        f.write('#!/bin/sh\necho "$*"; [ "$2" = status ]\n')
    os.chmod(ctl, 0o755)
    os.environ["SCHEMA_CTL"] = ctl
    assert m._schema_user_ctl("status", "kded") == (0, "--user status kded\n")
    assert m._schema_user_ctl("stop", "kded")[0] == 1
    os.environ["SCHEMA_CTL"] = os.path.join(d, "missing")
    assert m._schema_user_ctl("status", "kded") == (None, "")
    del os.environ["SCHEMA_CTL"]
    m.USER_UNIT_DIRS = saved_dirs
    if saved_rt is None:
        os.environ.pop("XDG_RUNTIME_DIR", None)
    else:
        os.environ["XDG_RUNTIME_DIR"] = saved_rt
    print("test_systemd1_session: schema-user hand-off OK")

    print("test_systemd1_session: OK")


if __name__ == "__main__":
    sys.exit(main())
