#!/usr/bin/env python3
"""schema-dbus live config reload: SIGHUP, ReloadConfig and (Task 3) inotify
rebuild the activation table and, on the system bus, re-dissolve the policy
without restarting the broker. The broker runs on a scratch socket with a fake
dissolver; it never touches the live bus.

  ./tests/test_dbus_live_reload.py     exit 0 all pass, 1 any fail
"""
import importlib.util
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("fdstore_wire", os.path.join(HERE, "test_dbus_fdstore_restart.py"))
W = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(W)
Client, CALL, RETURN, ERROR = W.Client, W.CALL, W.RETURN, W.ERROR
BROKER = W.BROKER

BASE = ("context = default\n"
        "allow = send_destination:*\n"
        "allow = send_type:signal\n"
        "allow = own:*\n")
# the fake dissolver prints next.policy; ctl holds "sleep <s>" / "exit <n>" lines
DISSECT = r'''import os, sys, time
d = os.path.dirname(os.path.abspath(__file__))
for line in open(os.path.join(d, "ctl")).read().split("\n"):
    k, _, v = line.partition(" ")
    if k == "sleep": time.sleep(float(v))
    if k == "exit": sys.exit(int(v))
sys.stdout.write(open(os.path.join(d, "next.policy")).read())
'''


class Env:
    def __init__(self, dissect=True, path=None, extra_env=None, system=True):
        self.dir = tempfile.mkdtemp(prefix="sdbus-reload-")
        self.bus = os.path.join(self.dir, "bus")
        self.policy = os.path.join(self.dir, "dbus.policy")
        self.svcdir = os.path.join(self.dir, "svc")
        self.ovrdir = os.path.join(self.dir, "ovr")
        os.makedirs(self.svcdir)
        os.makedirs(self.ovrdir)
        self.mask = os.path.join(self.dir, "masked")
        open(self.mask, "w").close()
        self.write(self.policy, BASE)
        self.set_next(BASE + "allow = send_type:method_return\n")
        self.ctl("")
        self.write(os.path.join(self.dir, "dissect.py"), DISSECT)
        env = {"PATH": path if path is not None else os.environ.get("PATH", "/usr/bin:/bin"),
               "HOME": self.dir,
               "SCHEMA_DBUS_SOCKET": self.bus,
               "SCHEMA_DBUS_POLICY": self.policy,
               "SCHEMA_DBUS_SVCDIRS": self.ovrdir + ":" + self.svcdir,
               "SCHEMA_DBUS_OVERRIDE_DIR": self.ovrdir,
               "SCHEMA_DBUS_MASKFILE": self.mask,
               "SCHEMA_DBUS_SYSTEM_CONF": os.path.join(self.dir, "system.conf")}
        if dissect:
            env["SCHEMA_DBUS_DISSECT"] = os.path.join(self.dir, "dissect.py")
        env.update(extra_env or {})
        self.log = os.path.join(self.dir, "broker.log")
        self.proc = subprocess.Popen([BROKER] + (["--system"] if system else []), env=env,
                                     stdin=subprocess.DEVNULL, stdout=open(self.log, "w"),
                                     stderr=subprocess.STDOUT)
        for _ in range(100):
            if os.path.exists(self.bus):
                break
            time.sleep(0.05)

    def write(self, path, text):
        with open(path, "w") as f:
            f.write(text)

    def set_next(self, text):
        self.write(os.path.join(self.dir, "next.policy"), text)

    def ctl(self, text):
        self.write(os.path.join(self.dir, "ctl"), text)

    def logtext(self):
        return open(self.log).read()

    def reloads(self, trigger=None):
        return [l for l in self.logtext().splitlines()
                if "schema-dbus: reload (" in l and (trigger is None or "(%s)" % trigger in l)]

    def wait_reloads(self, n, timeout=5, trigger=None):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if len(self.reloads(trigger)) >= n:
                return True
            time.sleep(0.05)
        return False

    def activatable(self, name, marker, systemd=False, d=None):
        script = os.path.join(self.dir, name + ".sh")
        self.write(script, "#!/bin/sh\ntouch %s\n" % marker)
        os.chmod(script, 0o755)
        body = "[D-BUS Service]\nName=%s\nExec=%s\n" % (name, script)
        if systemd:
            body += "SystemdService=%s.service\n" % name
        self.write(os.path.join(d or self.svcdir, name + ".service"), body)

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        shutil.rmtree(self.dir, ignore_errors=True)


def reload_config(c, timeout=10):
    s = c.send(CALL, [(1, "o", W.DRIVER[1]), (2, "s", W.DRIVER[2]), (3, "s", "ReloadConfig"),
                      (6, "s", W.DRIVER[0])])
    return c.wait(lambda m: m.mtype in (RETURN, ERROR) and m.reply_serial == s, timeout)


def poke(c, dest, timeout=3):
    """Call dest with auto-start; return the reply/error message or None."""
    s = c.send(CALL, [(1, "o", "/org/test"), (2, "s", "org.test.Iface"), (3, "s", "Poke"), (6, "s", dest)])
    try:
        return c.wait(lambda m: m.mtype in (RETURN, ERROR) and m.reply_serial == s, timeout)
    except TimeoutError:
        return None


def running(arg):
    """pids whose cmdline has arg (no pgrep: CI has no procps)"""
    pids = []
    for d in os.listdir("/proc"):
        try:
            if d.isdigit() and arg.encode() in open("/proc/%s/cmdline" % d, "rb").read().split(b"\0"):
                pids.append(int(d))
        except OSError:
            pass
    return pids


def wait_file(path, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if os.path.exists(path):
            return True
        time.sleep(0.05)
    return False


def denied(a, b, name):
    """a sends to name (owned by b): True if the broker answered AccessDenied."""
    s = a.send(CALL, [(1, "o", "/org/test"), (2, "s", "org.test.Iface"), (3, "s", "Ping"), (6, "s", name)])
    try:
        m = a.wait(lambda m: m.mtype == ERROR and m.reply_serial == s, 1)
        return m.fields.get(4) == "org.freedesktop.DBus.Error.AccessDenied"
    except TimeoutError:
        b.wait(lambda m: m.mtype == CALL and m.member == "Ping", 2)
        return False


class Base(unittest.TestCase):
    def setUp(self):
        self.envs = []

    def tearDown(self):
        for e in self.envs:
            e.close()

    def env(self, **kw):
        e = Env(**kw)
        self.envs.append(e)
        return e


class Reload(Base):
    def test_sighup_does_not_kill(self):
        e = self.env()
        os.kill(e.proc.pid, signal.SIGHUP)
        self.assertTrue(e.wait_reloads(1, trigger="sighup"), e.logtext())
        self.assertIsNone(e.proc.poll())
        Client(e.bus).close()

    def test_reload_config_replies_after_swap(self):
        e = self.env()
        a, b = Client(e.bus), Client(e.bus)
        b.driver("RequestName", "su", ("org.test.Deny", 0))
        self.assertFalse(denied(a, b, "org.test.Deny"))
        e.ctl("sleep 0.5")
        e.set_next(BASE + "deny = send_destination:org.test.Deny\n")
        m = reload_config(a)
        self.assertEqual(m.mtype, RETURN, m.args)
        self.assertTrue(denied(a, b, "org.test.Deny"))
        self.assertIn("deny = send_destination:org.test.Deny", open(e.policy).read())

    def test_cache_stamped_with_dissolve_start(self):
        # like the launcher: a conf changed during the dissolve must look newer
        # than the cache, or the next boot trusts a cache that lacks it
        cache = os.path.join(tempfile.gettempdir(), "sdbus-reload-cache-%d" % os.getpid())
        try:
            e = self.env(extra_env={"SCHEMA_DBUS_CACHE": cache})
            a = Client(e.bus)
            e.ctl("sleep 0.6")
            t0 = time.time()
            self.assertEqual(reload_config(a).mtype, RETURN)
            self.assertEqual(open(cache).read(), open(e.policy).read())
            self.assertLess(os.path.getmtime(cache), t0 + 0.3)
        finally:
            if os.path.exists(cache):
                os.unlink(cache)

    def test_existing_connections_survive(self):
        e = self.env()
        a = Client(e.bus)
        a.driver("RequestName", "su", ("org.test.Keep", 0))
        self.assertEqual(reload_config(a).mtype, RETURN)
        self.assertEqual(a.driver("GetNameOwner", "s", ("org.test.Keep",)).args[0], a.unique)

    def test_bad_dissolve_keeps_old(self):
        e = self.env()
        a, b = Client(e.bus), Client(e.bus)
        b.driver("RequestName", "su", ("org.test.Deny", 0))
        e.ctl("exit 1")
        e.set_next(BASE + "deny = send_destination:org.test.Deny\n")
        m = reload_config(a)
        self.assertEqual((m.mtype, m.fields.get(4)), (ERROR, "org.freedesktop.DBus.Error.Failed"))
        self.assertFalse(denied(a, b, "org.test.Deny"))
        self.assertIn("keeping the old policy", e.logtext())

    def test_missing_python3_keeps_old(self):
        empty = tempfile.mkdtemp()
        try:
            e = self.env(path=empty)
            a = Client(e.bus)
            m = reload_config(a)
            self.assertEqual(m.mtype, ERROR)
            self.assertIn("keeping the old policy", e.logtext())
            a.driver("ListNames")
        finally:
            os.rmdir(empty)

    def test_dissolver_timeout_keeps_old(self):
        e = self.env(extra_env={"SCHEMA_DBUS_RELOAD_TIMEOUT_MS": "500"})
        a = Client(e.bus)
        e.ctl("sleep 60")
        t0 = time.monotonic()
        m = reload_config(a)
        self.assertEqual(m.mtype, ERROR)
        self.assertLess(time.monotonic() - t0, 5)
        self.assertIn("timed out", " ".join(m.args))
        a.driver("ListNames")
        self.assertEqual(running(os.path.join(e.dir, "dissect.py")), [])

    def test_sanity_refuses_lost_own(self):
        e = self.env()
        a = Client(e.bus)
        a.driver("RequestName", "su", ("org.test.Keep", 0))
        e.set_next("context = default\nallow = send_destination:*\nallow = send_type:signal\n"
                   "allow = own:org.test.Other\ndeny = own:org.test.Keep\n")
        m = reload_config(a)
        self.assertEqual(m.mtype, ERROR)
        self.assertIn("org.test.Keep", " ".join(m.args))
        self.assertEqual(a.driver("GetNameOwner", "s", ("org.test.Keep",)).args[0], a.unique)

    def test_owner_conf_removed_applies(self):
        e = self.env()
        a = Client(e.bus)
        a.driver("RequestName", "su", ("org.test.Gone", 0))
        e.set_next("context = default\nallow = send_destination:*\nallow = send_type:signal\n"
                   "allow = own:org.test.Other\n")
        self.assertEqual(reload_config(a).mtype, RETURN, e.logtext())
        self.assertIn("own org.test.Gone: no rules left", e.logtext())
        self.assertEqual(a.driver("GetNameOwner", "s", ("org.test.Gone",)).args[0], a.unique)

    def test_new_activation_file_after_reload(self):
        e = self.env()
        a = Client(e.bus)
        marker = os.path.join(e.dir, "new.ran")
        e.activatable("org.test.New", marker)
        self.assertEqual(reload_config(a).mtype, RETURN)
        poke(a, "org.test.New")
        self.assertTrue(wait_file(marker))

    def test_raw_systemd_service_held(self):
        e = self.env()
        a = Client(e.bus)
        raw = os.path.join(e.dir, "raw.ran")
        e.activatable("org.test.Raw", raw, systemd=True)
        self.assertEqual(reload_config(a).mtype, RETURN)
        m = poke(a, "org.test.Raw")
        self.assertEqual(m.fields.get(4), "org.freedesktop.DBus.Error.ServiceUnknown")
        self.assertFalse(os.path.exists(raw))
        ovr = os.path.join(e.dir, "ovr.ran")
        e.activatable("org.test.Raw", ovr, systemd=True, d=e.ovrdir)
        self.assertEqual(reload_config(a).mtype, RETURN)
        poke(a, "org.test.Raw")
        self.assertTrue(wait_file(ovr))

    def test_reload_without_dissect_env(self):
        e = self.env(dissect=False)
        a = Client(e.bus)
        marker = os.path.join(e.dir, "nd.ran")
        e.activatable("org.test.NoDissect", marker)
        self.assertEqual(reload_config(a).mtype, RETURN)
        poke(a, "org.test.NoDissect")
        self.assertTrue(wait_file(marker))

    def test_concurrent_reload_requests(self):
        e = self.env()
        a, b = Client(e.bus), Client(e.bus)
        e.ctl("sleep 0.5")
        sa = a.send(CALL, [(1, "o", W.DRIVER[1]), (2, "s", W.DRIVER[2]), (3, "s", "ReloadConfig"), (6, "s", W.DRIVER[0])])
        time.sleep(0.1)
        sb = b.send(CALL, [(1, "o", W.DRIVER[1]), (2, "s", W.DRIVER[2]), (3, "s", "ReloadConfig"), (6, "s", W.DRIVER[0])])
        ma = a.wait(lambda m: m.mtype in (RETURN, ERROR) and m.reply_serial == sa, 10)
        mb = b.wait(lambda m: m.mtype in (RETURN, ERROR) and m.reply_serial == sb, 10)
        self.assertEqual((ma.mtype, mb.mtype), (RETURN, RETURN))
        time.sleep(0.3)
        self.assertEqual(len(e.reloads("ReloadConfig")), 2, e.logtext())

    def test_sigterm_during_reload(self):
        e = self.env()
        a = Client(e.bus)
        e.ctl("sleep 5")
        s = a.send(CALL, [(1, "o", W.DRIVER[1]), (2, "s", W.DRIVER[2]), (3, "s", "ReloadConfig"), (6, "s", W.DRIVER[0])])
        time.sleep(0.3)
        os.kill(e.proc.pid, signal.SIGTERM)
        m = a.wait(lambda m: m.mtype in (RETURN, ERROR) and m.reply_serial == s, 5)
        self.assertEqual(m.mtype, ERROR)
        e.proc.wait(5)
        self.assertEqual(running(os.path.join(e.dir, "dissect.py")), [])


class Inotify(Base):
    FAST = {"SCHEMA_DBUS_DEBOUNCE_MS": "300"}

    def test_inotify_new_activation_file(self):
        e = self.env(extra_env=self.FAST)
        a = Client(e.bus)
        marker = os.path.join(e.dir, "auto.ran")
        e.activatable("org.test.Auto", marker)
        self.assertTrue(e.wait_reloads(1, trigger="inotify"), e.logtext())
        poke(a, "org.test.Auto")
        self.assertTrue(wait_file(marker))

    def test_inotify_policy_change(self):
        sysd = tempfile.mkdtemp()
        try:
            e = self.env(extra_env=dict(self.FAST, SCHEMA_DBUS_WATCH_EXTRA=sysd))
            a, b = Client(e.bus), Client(e.bus)
            b.driver("RequestName", "su", ("org.test.Deny", 0))
            self.assertFalse(denied(a, b, "org.test.Deny"))
            e.set_next(BASE + "deny = send_destination:org.test.Deny\n")
            e.write(os.path.join(sysd, "zz-test.conf"), "<busconfig/>\n")
            self.assertTrue(e.wait_reloads(1, trigger="inotify"), e.logtext())
            self.assertTrue(denied(a, b, "org.test.Deny"))
        finally:
            shutil.rmtree(sysd)

    def test_inotify_burst_one_reload(self):
        e = self.env(extra_env=self.FAST)
        for i in range(20):
            e.write(os.path.join(e.svcdir, "org.test.B%d.service" % i),
                    "[D-BUS Service]\nName=org.test.B%d\nExec=/bin/true\n" % i)
            time.sleep(0.05)
        self.assertTrue(e.wait_reloads(1, trigger="inotify"), e.logtext())
        time.sleep(1)
        self.assertEqual(len(e.reloads("inotify")), 1, e.logtext())
        self.assertIn("20 activatable services", e.reloads("inotify")[0])

    def test_watched_dir_removed(self):
        e = self.env(extra_env=self.FAST)
        marker = os.path.join(e.dir, "gone.ran")
        e.activatable("org.test.Gone", marker)
        self.assertTrue(e.wait_reloads(1, trigger="inotify"), e.logtext())
        shutil.rmtree(e.svcdir)
        self.assertTrue(e.wait_reloads(2, trigger="inotify"), e.logtext())
        self.assertIsNone(e.proc.poll())
        a = Client(e.bus)
        m = poke(a, "org.test.Gone")
        self.assertEqual(m.fields.get(4), "org.freedesktop.DBus.Error.ServiceUnknown")
        self.assertFalse(os.path.exists(marker))

    def test_override_dir_created_late_releases_hold(self):
        e = self.env(extra_env=self.FAST)
        a = Client(e.bus)
        shutil.rmtree(e.ovrdir)
        self.assertTrue(e.wait_reloads(1, trigger="inotify"), e.logtext())
        e.activatable("org.test.Late", os.path.join(e.dir, "raw.ran"), systemd=True)
        self.assertTrue(e.wait_reloads(2, trigger="inotify"), e.logtext())
        os.makedirs(e.ovrdir)
        self.assertTrue(e.wait_reloads(3, trigger="inotify"), e.logtext())
        ovr = os.path.join(e.dir, "ovr.ran")
        e.activatable("org.test.Late", ovr, systemd=True, d=e.ovrdir)
        self.assertTrue(e.wait_reloads(4, trigger="inotify"), e.logtext())
        poke(a, "org.test.Late")
        self.assertTrue(wait_file(ovr), e.logtext())

    def test_session_bus_inotify(self):
        e = self.env(extra_env=self.FAST, system=False, dissect=False)
        a = Client(e.bus)
        marker = os.path.join(e.dir, "sess.ran")
        e.activatable("org.test.Sess", marker)
        self.assertTrue(e.wait_reloads(1, trigger="inotify"), e.logtext())
        poke(a, "org.test.Sess")
        self.assertTrue(wait_file(marker))


if __name__ == "__main__":
    unittest.main()
