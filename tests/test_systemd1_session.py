#!/usr/bin/env python3
import json
import os
import sys
import signal
import time
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
    # F5: template instance specifiers
    assert m._expand("%i|%I|%p|%P|%f", {}, "pk@home-a\\x2db.service") == \
        "home-a\\x2db|home/a-b|pk|pk|/home/a-b"
    assert m._expand("%i|%I|%p|%f", {}, "plain.service") == "||plain|/plain"
    assert m._expand("%n|%N", {}, "w@x.service") == "w@x.service|w@x"
    assert m._expand("%f", {}, "fs@-.service") == "/"
    print("test_systemd1_session: expansion OK")

    # F5: foo@bar resolves to foo@bar.service if present, else foo@.service
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "tpl@.service"), "w").close()
        open(os.path.join(d, "tpl@own.service"), "w").close()
        saved = m.USER_UNIT_DIRS
        m.USER_UNIT_DIRS = [d]
        try:
            assert m._find_user_unit("tpl@x") == os.path.join(d, "tpl@.service")
            assert m._find_user_unit("tpl@x.service") == os.path.join(d, "tpl@.service")
            assert m._find_user_unit("tpl@own.service") == os.path.join(d, "tpl@own.service")
            assert m._find_user_unit("tpl@.service") is None
            assert m._find_user_unit("nope@x") is None
        finally:
            m.USER_UNIT_DIRS = saved
    print("test_systemd1_session: template lookup OK")

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
    # restart of a oneshot chain must not orphan the step it is running
    g = subprocess.Popen(["/bin/sh", "-c", "sleep 30 || exit $?; exec sleep 31"], start_new_session=True)
    kid = None
    for _ in range(50):  # /proc scan, not pgrep: CI container has no procps
        for d in filter(str.isdigit, os.listdir("/proc")):
            try:
                with open("/proc/%s/stat" % d) as f:
                    if f.read().rsplit(")", 1)[1].split()[1] == str(g.pid):
                        kid = int(d)
            except OSError:
                pass
        if kid:
            break
        time.sleep(0.05)
    assert kid, "chain step never started"
    m._spawned["chain.service"] = (g.pid, m._proc_starttime(g.pid))
    assert m._stop_unit("chain.service") is True
    g.wait()
    time.sleep(0.1)
    try:  # killed but unreaped (container pid 1 may not reap) still counts as dead
        with open("/proc/%d/stat" % kid) as f:
            state = f.read().rsplit(")", 1)[1].split()[0]
    except OSError:
        state = "gone"
    assert state in ("gone", "Z", "X"), "chain step orphaned (state %s)" % state
    print("test_systemd1_session: tracking OK")

    # ExecStart parse: leading modifier, line continuation, Environment=
    with tempfile.NamedTemporaryFile("w", suffix=".service", delete=False) as f:
        f.write("[Service]\n"
                "Environment=FOO=bar BAZ=qux\n"
                "ExecStart=-/usr/bin/tool --flag \\\n"
                "  --home %h\n")
        unit = f.name
    cmds, envl, stype = m._parse_unit_execstart(unit)
    os.unlink(unit)
    assert cmds == [(True, False, "/usr/bin/tool", ["/usr/bin/tool", "--flag", "--home", "%h"])], cmds
    assert "FOO=bar" in envl and "BAZ=qux" in envl, envl
    assert stype == "simple", stype

    # F7: multiple ExecStart=, empty-value reset (ExecStart= and Environment=), @argv0
    def parse(body):
        with tempfile.NamedTemporaryFile("w", suffix=".service", delete=False) as f:
            f.write(body)
        try:
            return m._parse_unit_execstart(f.name)
        finally:
            os.unlink(f.name)
    cmds, envl, stype = parse("[Service]\nType=oneshot\nEnvironment=A=1\nExecStart=/bin/old\n"
                              "ExecStart=\nEnvironment=\nEnvironment=B=2\n"
                              "ExecStart=/bin/a x\nExecStart=-@/bin/b bee y\nExecStart=:/bin/c $X\n")
    assert stype == "oneshot"
    assert cmds == [(False, False, "/bin/a", ["/bin/a", "x"]), (True, False, "/bin/b", ["bee", "y"]),
                    (False, True, "/bin/c", ["/bin/c", "$X"])], cmds
    assert parse("[Service]\nType=oneshot\nType=\nExecStart=/bin/a\n")[2] == "simple"
    assert m._expand("$HOME %N", {"HOME": "/h"}, "u.service", False) == "$HOME u"
    assert m._expand("$0 ${1} $HOME", {"HOME": "/h"}, "u.service") == "$0 ${1} /h"
    assert envl == ["B=2"], envl

    assert m._plan([]) is None
    assert m._plan([(False, False, "/bin/a", ["a0", "x"])]) == ("/bin/a", ["a0", "x"])
    assert m._plan([(False, False, "/bin/a", ["/bin/a"]), (False, False, "/bin/b", ["/bin/b"])], "simple") == "multi"
    ex, av = m._plan([(False, False, "/bin/a", ["/bin/a", "x y"]), (True, False, "/bin/b", ["bee"]),
                      (False, False, "/bin/c", ["/bin/c", "$z"])], "oneshot")
    assert ex == sys.executable and av[:3] == [sys.executable, "-I", "-c"], av
    assert json.loads(av[4]) == [[False, "/bin/a", ["/bin/a", "x y"]], [True, "/bin/b", ["bee"]],
                                 [False, "/bin/c", ["/bin/c", "$z"]]], av[4]
    # and it really runs in order, stopping at the first unignored failure
    with tempfile.TemporaryDirectory() as d:
        log = os.path.join(d, "log")
        ex, av = m._plan([(False, False, "/bin/sh", ["/bin/sh", "-c", "echo 1 >>" + log]),
                          (True, False, "/bin/false", ["/bin/false"]),
                          (False, False, "/bin/sh", ["/bin/sh", "-c", "echo 2 >>%s; exit 3" % log]),
                          (False, False, "/bin/sh", ["/bin/sh", "-c", "echo 3 >>" + log])], "oneshot")
        assert subprocess.run(av, executable=ex).returncode == 3
        assert open(log).read() == "1\n2\n", open(log).read()
        # '@' argv[0] reaches every step, the exec'd last one too
        a0 = os.path.join(d, "a0")
        step = ["/bin/sh", "-c", 'echo "$0" >> ' + a0]
        ex, av = m._plan([(False, False, "/bin/sh", ["first"] + step[1:]),
                          (False, False, "/bin/sh", ["last"] + step[1:])], "oneshot")
        assert subprocess.run(av, executable=ex).returncode == 0
        assert open(a0).read() == "first\nlast\n", open(a0).read()
        # a step killed by a signal ends the chain as sh would: 128+sig
        ex, av = m._plan([(False, False, "/bin/sh", ["/bin/sh", "-c", "kill -TERM $$"]),
                          (False, False, "/bin/true", ["/bin/true"])], "oneshot")
        assert subprocess.run(av, executable=ex).returncode == 128 + signal.SIGTERM
        ex, av = m._plan([(True, False, "/nonexistent", ["/nonexistent"]),
                          (False, False, "/nonexistent2", ["x"])], "oneshot")
        assert subprocess.run(av, executable=ex).returncode == 127
        # '-' on the last step: its failure is ignored, as systemd does
        ex, av = m._plan([(False, False, "/bin/true", ["/bin/true"]),
                          (True, False, "/nonexistent", ["/nonexistent"])], "oneshot")
        assert subprocess.run(av, executable=ex).returncode == 0
        # steps get SIGPIPE back at its default: a writer to a closed pipe dies of it
        ex, av = m._plan([(False, False, "/bin/true", ["/bin/true"]),
                          (False, False, "/bin/sh", ["/bin/sh", "-c", "kill -PIPE $$"])], "oneshot")
        r = subprocess.run(av, executable=ex, capture_output=True, text=True)
        assert r.returncode == -signal.SIGPIPE, r.returncode
        # an empty '@' argv[0] is an exec failure, not a traceback
        ex, av = m._plan([(False, False, "/bin/true", ["/bin/true"]),
                          (False, False, "/bin/true", [""])], "oneshot")
        r = subprocess.run(av, executable=ex, capture_output=True, text=True)
        assert r.returncode == 127 and "Traceback" not in r.stderr, r

    # drop-ins: <unit>.d, template, dash prefix and type-level dirs, in filename
    # order; a higher-priority unit dir shadows a same-named file; /dev/null masks
    with tempfile.TemporaryDirectory() as d:
        hi, lo = os.path.join(d, "hi"), os.path.join(d, "lo")
        def put(root, rel, body):
            p = os.path.join(root, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w") as f:
                f.write(body)
        put(lo, "foo-bar@.service", "[Service]\nExecStart=/bin/base\nEnvironment=A=0\n")
        put(lo, "service.d/10-all.conf", "[Service]\nEnvironment=ALL=1\n")
        put(lo, "foo-.service.d/20-prefix.conf", "[Service]\nEnvironment=P=1\n")
        put(lo, "foo-bar@.service.d/30-tpl.conf", "[Service]\nExecStart=\nExecStart=@/bin/real nick %i\n")
        put(lo, "foo-bar@x.service.d/40-inst.conf", "[Service]\nEnvironment=A=1\n")
        put(lo, "foo-bar@x.service.d/50-shadowed.conf", "[Service]\nEnvironment=S=lo\n")
        put(hi, "foo-bar@x.service.d/50-shadowed.conf", "[Service]\nEnvironment=S=hi\n")
        put(lo, "foo-bar@x.service.d/60-masked.conf", "[Service]\nEnvironment=M=1\n")
        os.makedirs(os.path.join(hi, "foo-bar@.service.d"))
        os.symlink("/dev/null", os.path.join(hi, "foo-bar@.service.d/60-masked.conf"))
        put(lo, "foo-bar@x.service.d/not-a-conf", "[Service]\nEnvironment=N=1\n")
        put(lo, "foo-bar@x.service.d/.hidden.conf", "[Service]\nEnvironment=H=1\n")
        # an alias picks up the drop-ins of the unit it links to
        put(lo, "real.service", "[Service]\nExecStart=/bin/orig\n")
        put(lo, "real.service.d/o.conf", "[Service]\nExecStart=\nExecStart=/bin/over\n")
        os.symlink(os.path.join(lo, "real.service"), os.path.join(lo, "dbus-org.x.service"))
        put(lo, "foo-bar@x.service.d/70-other.conf", "[Unit]\nExecStart=/bin/ignored\n")
        saved, m.USER_UNIT_DIRS = m.USER_UNIT_DIRS, (hi, lo)
        try:
            dropins = m._unit_dropins("foo-bar@x.service")
            assert [os.path.basename(p) for p in dropins] == [
                "10-all.conf", "20-prefix.conf", "30-tpl.conf", "40-inst.conf",
                "50-shadowed.conf", "60-masked.conf", "70-other.conf"], dropins
            assert dropins[4].startswith(hi) and dropins[5].startswith(hi), dropins
            cmds, envl, _ = m._parse_unit_execstart(m._find_user_unit("foo-bar@x.service"), dropins)
            assert cmds == [(False, False, "/bin/real", ["nick", "%i"])], cmds
            assert envl == ["A=0", "ALL=1", "P=1", "A=1", "S=hi"], envl
            assert m._unit_dropins("plain.service") == [os.path.join(lo, "service.d/10-all.conf")]
            f = m._find_user_unit("dbus-org.x.service")
            cmds, _, _ = m._parse_unit_execstart(f, m._unit_dropins("dbus-org.x.service", f))
            assert cmds == [(False, False, "/bin/over", ["/bin/over"])], cmds
        finally:
            m.USER_UNIT_DIRS = saved
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

    # podman's own scope request (podman exec from a login shell)
    podman = [("Slice", "user.slice"), ("PIDs", [42]), ("Delegate", True),
              ("DefaultDependencies", False)]
    assert m._scope_check("podman-42.scope", podman, 42) is None
    assert m._scope_check("podman-pause-7.scope", podman, 42) is None
    assert m._scope_check("app-x.scope", podman, 42) is not None
    assert m._scope_check("chrome-1.scope", podman, 42) is not None
    assert m._scope_check("podman-42.scope", podman, 43) is not None
    assert m._scope_check("podman-42.scope", [("PIDs", [42, 43])], 42) is not None
    assert m._scope_check("podman-42.scope", [("Slice", "user.slice")], 42) is not None
    assert m._scope_check("podman-42.scope", [("Slice", "foo.slice"), ("PIDs", [42])], 42) is not None
    assert m._scope_check("podman-42.scope", [("PIDs", [42])], 42) is None

    me = os.getuid()
    with tempfile.TemporaryDirectory() as root:
        svc = "%s/user.slice/user-%d.slice/user@%d.service" % (root, me, me)
        os.makedirs(svc)
        sl = svc + "/user.slice"
        pop = set()
        saved_pop = m._populated
        m._populated = lambda p: p in pop
        try:
            leaf = m._scope_prepare("podman-42.scope", me, root)
            assert leaf == sl + "/podman-42.scope" and os.path.isdir(leaf)
            assert m._scope_prepare("podman-42.scope", me, root) == leaf    # empty leftover reused
            pop.add(leaf)
            try:
                m._scope_prepare("podman-42.scope", me, root)
                assert False, "populated leftover reused"
            except m.ScopeRefused:
                pass
            cid = "cd954c4e" * 8                                            # ad-hoc podman run -d
            os.mkdir(sl + "/" + cid)
            assert m._scope_prepare("podman-44.scope", me, root) == sl + "/podman-44.scope"
            assert m._scope_sweep(me, root) == 1 and os.path.isdir(sl + "/" + cid)
            os.rmdir(sl + "/" + cid)
            for child in ("runtime", "libpod-payload-ab12"):                # a Quadlet .svc named user.slice
                os.mkdir(sl + "/" + child)
                try:
                    m._scope_prepare("podman-43.scope", me, root)
                    assert False, "service cgroup %s accepted" % child
                except m.ScopeRefused:
                    pass
                assert not os.path.exists(sl + "/podman-43.scope")
                os.rmdir(sl + "/" + child)
            with open(sl + "/cgroup.procs", "w") as f:
                f.write("123\n")
            try:
                m._scope_prepare("podman-43.scope", me, root)
                assert False, "user.slice holding processes accepted"
            except m.ScopeRefused:
                pass
            os.unlink(sl + "/cgroup.procs")
            try:
                m._scope_prepare("podman-43.scope", me + 1, root)
                assert False, "someone else's tree accepted"
            except m.ScopeRefused:
                pass

            os.mkdir(sl + "/podman-1.scope")
            os.mkdir(sl + "/other.scope")
            assert m._scope_sweep(me, root) == 1                             # podman-1; 42 is populated
            assert not os.path.exists(sl + "/podman-1.scope")
            assert os.path.isdir(leaf) and os.path.isdir(sl + "/other.scope")
            pop.clear()
            os.rmdir(sl + "/other.scope")

            calls = []
            ok, why = m._scope_request("podman-50.scope", podman[:1] + [("PIDs", [50])], 50, me,
                                       lambda name, pid: calls.append((name, pid)), root)
            assert ok and calls == [("podman-50.scope", 50)], (ok, why, calls)

            def broken(name, pid):
                raise RuntimeError("no bridge")
            ok, why = m._scope_request("podman-51.scope", [("PIDs", [51])], 51, me, broken, root)
            assert not ok and "no bridge" in why
            assert not os.path.exists(sl + "/podman-51.scope")
            ok, why = m._scope_request("podman-52.scope", [("PIDs", [9])], 52, me, broken, root)
            assert not ok and not os.path.exists(sl + "/podman-52.scope")
            ok, why = m._scope_request("podman-53.scope", [("PIDs", 7)], 53, me, broken, root)
            assert not ok and "error" in why                                   # malformed PIDs: answered, not raised
        finally:
            m._populated = saved_pop
    print("test_systemd1_session: podman scopes OK")

    # StopUnit acts on what this shim spawned and never reaches a same-named system unit
    class _Msg:
        def __init__(self, name):
            self.name = name

        def get_args_list(self, byte_arrays=False):
            return [self.name, "replace"]

    class _Session:
        def __init__(self):
            self.sent = []

        def send_message(self, reply):
            self.sent.append(reply)

    acked, timers = [], []
    saved = (m._reply_job_and_done, m.GLib.timeout_add_seconds, m.USER_UNIT_DIRS, m._signal_group)
    m._reply_job_and_done = lambda session, msg, name: acked.append(name)
    m.GLib.timeout_add_seconds = lambda sec, fn, *a: timers.append((sec, fn, a))
    try:
        with tempfile.TemporaryDirectory() as units:
            m.USER_UNIT_DIRS = [units]
            with open(units + "/real.service", "w") as f:
                f.write("[Service]\nExecStart=/usr/bin/sleep 60\n")
            os.symlink("real.service", units + "/dbus-org.x.service")

            # a running unit gets SIGTERM and the job is acked
            p = subprocess.Popen(["sleep", "60"], start_new_session=True)
            m._spawned["probe.service"] = (p.pid, m._proc_starttime(p.pid))
            assert m.handle_stop_unit(None, _Msg("probe.service")) is True
            assert p.wait(timeout=5) == -signal.SIGTERM
            assert acked == ["probe.service"] and "probe.service" not in m._spawned

            # a known user unit that is not running: ack, no forward to the system bus
            acked.clear()
            assert m.handle_stop_unit(None, _Msg("real.service")) is True
            assert acked == ["real.service"]

            # no user unit and nothing spawned: not ours, forward
            acked.clear()
            assert m.handle_stop_unit(None, _Msg("never-spawned.service")) is False
            assert acked == []

            # started through an alias, stopped by its real name
            assert m.handle_start_unit(None, _Msg("dbus-org.x.service")) is True
            pid = m._spawned["real.service"][0]
            assert m.handle_stop_unit(None, _Msg("real.service")) is True
            assert os.waitpid(pid, 0)[1] == signal.SIGTERM and "real.service" not in m._spawned
            timers.clear()

            # a slow unit keeps its stop grace: no SIGKILL at the restart wait
            acked.clear()
            p = subprocess.Popen(["sh", "-c", "trap '' TERM; exec sleep 60"], start_new_session=True)
            time.sleep(0.2)
            m._spawned["slow.service"] = (p.pid, m._proc_starttime(p.pid))
            assert m.handle_stop_unit(None, _Msg("slow.service")) is True
            assert p.poll() is None and acked == ["slow.service"]
            assert [t[0] for t in timers] == [90]
            sec, fn, args = timers[0]
            assert fn(*args) is False                                  # one-shot GLib source
            assert p.wait(timeout=5) == -signal.SIGKILL

            # a signal that fails is an error, and the unit stays tracked
            acked.clear()
            p = subprocess.Popen(["sleep", "60"], start_new_session=True)
            m._spawned["stuck.service"] = rec = (p.pid, m._proc_starttime(p.pid))
            m._signal_group = lambda pid, sig: False
            session = _Session()
            saved_ll = m.ll
            m.ll = type("L", (), {"ErrorMessage": staticmethod(lambda msg, err, text: text)})
            try:
                assert m.handle_stop_unit(session, _Msg("stuck.service")) is True
            finally:
                m.ll = saved_ll
            assert acked == [] and len(session.sent) == 1 and m._spawned["stuck.service"] == rec
            p.kill()
            p.wait()
    finally:
        m._reply_job_and_done, m.GLib.timeout_add_seconds, m.USER_UNIT_DIRS, m._signal_group = saved
    print("test_systemd1_session: StopUnit OK")

    # A session caller means its user manager, so these stay here: forwarded,
    # they would reach the system manager, which now refuses the desktop user.
    import dbus
    import dbus.lowlevel as dll
    def call(member, sig=None, *args):
        msg = dll.MethodCallMessage("x.y", m.PATH_PREFIX, m.MGR_IFACE, member)
        if args:
            msg.append(*args, signature=sig)
        return msg
    saved_reply, saved_ll = m._reply_job_and_done, m.ll
    acked = []
    m._reply_job_and_done = lambda session, msg, name: acked.append(name)
    # an unsent message has serial 0, and libdbus aborts on replying to one
    m.ll = type("L", (), {"MethodCallMessage": dll.MethodCallMessage,
                          "MethodReturnMessage": staticmethod(lambda msg: "return"),
                          "ErrorMessage": staticmethod(lambda msg, err, text: "error")})
    try:
        for member, sig, args in (("Reload", None, ()), ("SetEnvironment", "as", (["A=1"],)),
                                  ("UnsetEnvironment", "as", (["A"],)),
                                  ("UnsetAndSetEnvironment", "asas", (["A"], ["B=1"]))):
            session = _Session()
            assert m.handle_user_manager_write(session, call(member, sig, *args), member) is True
            assert session.sent == ["return"], member
        for member in m._START_MEMBERS + ("StopUnit",):
            acked.clear()
            assert m.handle_user_manager_write(_Session(), call(member, "ss", "plasma-x.target", "fail"), member) is True
            assert acked == ["plasma-x.target"], member
            # a real service is the system's, and polkit decides
            assert m.handle_user_manager_write(_Session(), call(member, "ss", "sshd.service", "fail"), member) is False
        assert m.handle_user_manager_write(_Session(), call("ListUnits"), "ListUnits") is False

        # The relay's call blocks its only thread: a prompt would freeze every app
        # launch in the session, then time out with the dialog still up.
        class _System:
            def send_message_with_reply_and_block(self, fwd, timeout):
                self.fwd = fwd
                raise dbus.DBusException("no", name="org.freedesktop.DBus.Error.AccessDenied")
        for flag in (True, False):
            system, msg = _System(), call("RestartUnit", "ss", "sshd.service", "replace")
            msg.set_allow_interactive_authorization(flag)
            m.forward_call(_Session(), system, msg)
            assert system.fwd.get_allow_interactive_authorization() is False
    finally:
        m._reply_job_and_done, m.ll = saved_reply, saved_ll
    print("test_systemd1_session: user manager writes OK")

    print("test_systemd1_session: OK")


if __name__ == "__main__":
    sys.exit(main())
