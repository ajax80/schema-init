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

    print("test_systemd1_session: OK")


if __name__ == "__main__":
    sys.exit(main())
