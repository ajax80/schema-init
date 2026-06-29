#!/usr/bin/env python3
# schema-init: session-bus org.freedesktop.systemd1 provider.
#
# schema-init has no `systemd --user`, so nothing owns org.freedesktop.systemd1
# on the user session bus; session-bus clients (Ferrix, gnome-system-monitor,
# mission-center) hit the stock /bin/false activation stub and get
# org.freedesktop.DBus.Error.Spawn.ChildExited. This process owns the name on the
# session bus and blind-forwards every call under /org/freedesktop/systemd1 to the
# real system-bus surface served by schema-systemd1.py, re-emitting its signals.
#
# Runs as the logged-in user, on that user's own session bus. Reads forward to a
# surface the user can already read unprivileged; writes forward to the system bus
# where polkit still adjudicates. Design: docs/superpowers/specs/2026-06-28-...
#
# StartTransientUnit is the exception to blind-forwarding: KDE launches every
# menu/taskbar app as a transient app-*.service and expects the *user* systemd
# manager to spawn it from the unit's ExecStart. schema-init has no user manager,
# so we ARE it for this one call -- we fork ExecStart in the live session and
# synthesise the job-done signal. (The root system bridge can't: it would run the
# app as root with no display.)
import os
import sys
import signal
import subprocess
import dbus
import dbus.lowlevel as ll
import dbus.mainloop.glib
from gi.repository import GLib

BUS_NAME = "org.freedesktop.systemd1"
PATH_PREFIX = "/org/freedesktop/systemd1"
MGR_IFACE = "org.freedesktop.systemd1.Manager"
FWD_TIMEOUT = 25000  # ms; covers an interactive polkit prompt on a write
LOG = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "schema-stu.log")

_job_seq = 0
_gui_env_cache = None
# Session vars an app needs that the relay's own (activation) env usually lacks.
_GUI_KEYS = (
    "WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR",
    "DBUS_SESSION_BUS_ADDRESS", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE",
    "XDG_DATA_DIRS", "XDG_CONFIG_DIRS", "PATH", "HOME", "USER", "LOGNAME",
    "LANG", "QT_QPA_PLATFORM", "KDE_FULL_SESSION", "KDE_SESSION_VERSION",
)


def _log(msg):
    try:
        with open(LOG, "a") as f:
            f.write(msg + "\n")
    except OSError:
        pass


def _pidof(name):
    try:
        for p in os.listdir("/proc"):
            if not p.isdigit():
                continue
            try:
                with open("/proc/%s/comm" % p) as f:
                    if f.read().strip() == name:
                        return p
            except OSError:
                continue
    except OSError:
        pass
    return None


def _session_env():
    # Harvest a real GUI environment from a live session process; the relay's own
    # env (started via dbus activation) is missing WAYLAND_DISPLAY/DISPLAY/PATH.
    global _gui_env_cache
    if _gui_env_cache is not None:
        return dict(_gui_env_cache)
    env = dict(os.environ)
    for proc in ("plasmashell", "kwin_wayland", "ksmserver", "startplasma-wayl"):
        pid = _pidof(proc)
        if not pid:
            continue
        try:
            with open("/proc/%s/environ" % pid, "rb") as f:
                raw = f.read()
        except OSError:
            continue
        for kv in raw.split(b"\0"):
            if b"=" not in kv:
                continue
            k, v = kv.split(b"=", 1)
            k = k.decode("utf-8", "replace")
            if k in _GUI_KEYS:
                env.setdefault(k, v.decode("utf-8", "replace"))
        break
    env.setdefault("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin")
    _gui_env_cache = env
    return dict(env)


def _parse_exec_and_env(props):
    exec_path, argv, kde_env = None, None, []
    try:
        for entry in props:
            key = str(entry[0])
            val = entry[1]
            if key in ("ExecStart", "ExecStartEx") and val:
                first = val[0]
                exec_path = str(first[0])
                argv = [str(a) for a in first[1]] or [exec_path]
            elif key == "Environment" and val:
                kde_env = [str(e) for e in val]
    except Exception as e:  # noqa: BLE001
        _log("parse error: %r" % e)
    return exec_path, argv, kde_env


def _child_preexec():
    # We keep SIGCHLD=SIG_IGN in the relay so our fire-and-forget launches don't
    # zombie -- but SIG_IGN survives execve and is inherited by the launched app.
    # An app that then waitpid()s its own children gets ECHILD ("no child process",
    # os error 10) and breaks (e.g. Ferrix installed-software / DMI tabs). Restore
    # default disposition in the child before exec so launched apps behave normally.
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)


def _spawn(exec_path, argv, kde_env):
    env = _session_env()
    for kv in kde_env:
        if "=" in kv:
            k, v = kv.split("=", 1)
            env[k] = v
    try:
        subprocess.Popen(
            argv, executable=exec_path, env=env,
            start_new_session=True, close_fds=True, preexec_fn=_child_preexec,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        _log("spawned %s argv=%s wayland=%s display=%s"
             % (exec_path, argv, env.get("WAYLAND_DISPLAY"), env.get("DISPLAY")))
        return True
    except Exception as e:  # noqa: BLE001
        _log("spawn FAILED %s: %r" % (exec_path, e))
        return False


def handle_start_transient_unit(session, msg):
    global _job_seq
    try:
        args = msg.get_args_list(byte_arrays=True)
        name = str(args[0])
        props = args[2] if len(args) > 2 else []
    except Exception as e:  # noqa: BLE001
        name, props = "", []
        _log("args error: %r" % e)

    exec_path, argv, kde_env = _parse_exec_and_env(props)
    if not exec_path:
        # A scope adopting already-running PIDs -- Chromium/KDE registering their
        # own child processes, NOT an app launch. Before this relay existed these
        # calls failed (no session systemd) and the app just ran un-scoped, which
        # is correct. ACKing them makes Chromium believe its network-service
        # process is under systemd scope management, which silently wedges every
        # connection it opens (tabs spin forever). So refuse: the app falls back
        # to running un-scoped exactly as it did pre-relay. Ferrix only needs
        # reads; real app launches arrive as .service units with ExecStart below.
        _log("StartTransientUnit(%s): scope self-reg -> refused (run un-scoped)" % name)
        session.send_message(ll.ErrorMessage(
            msg, "org.freedesktop.DBus.Error.NotSupported",
            "schema-init does not manage transient scopes"))
        return

    _spawn(exec_path, argv, kde_env)
    _job_seq += 1
    job_id = _job_seq
    job_path = "%s/job/%d" % (PATH_PREFIX, job_id)

    ret = ll.MethodReturnMessage(msg)
    ret.append(dbus.ObjectPath(job_path), signature="o")
    session.send_message(ret)

    # Emit JobRemoved after the reply lands so the launcher, which subscribes on
    # the returned job path, sees the unit "done".
    def _done():
        for member in ("JobNew", "JobRemoved"):
            sig = ll.SignalMessage(PATH_PREFIX, MGR_IFACE, member)
            if member == "JobNew":
                sig.append(dbus.UInt32(job_id), dbus.ObjectPath(job_path),
                           dbus.String(name), signature="uos")
            else:
                sig.append(dbus.UInt32(job_id), dbus.ObjectPath(job_path),
                           dbus.String(name), dbus.String("done"), signature="uoss")
            session.send_message(sig)
        return False

    GLib.timeout_add(15, _done)


def forward_call(session, system, msg):
    call = ll.MethodCallMessage(
        BUS_NAME, msg.get_path(), msg.get_interface(), msg.get_member()
    )
    _append(call, msg.get_args_list(byte_arrays=True), msg.get_signature())
    try:
        reply = system.send_message_with_reply_and_block(call, FWD_TIMEOUT)
        ret = ll.MethodReturnMessage(msg)
        _append(ret, reply.get_args_list(byte_arrays=True), reply.get_signature())
        session.send_message(ret)
    except dbus.DBusException as e:
        name = e.get_dbus_name() or "org.freedesktop.DBus.Error.Failed"
        session.send_message(ll.ErrorMessage(msg, name, str(e)))
    except Exception as e:  # noqa: BLE001 - never let a forward kill the relay
        session.send_message(
            ll.ErrorMessage(msg, "org.freedesktop.DBus.Error.Failed", str(e))
        )


def _append(msg, args, sig):
    if args:
        msg.append(*args, signature=sig if sig else None)


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    # We fire-and-forget launched apps; auto-reap so they don't become zombies.
    signal.signal(signal.SIGCHLD, signal.SIG_IGN)
    try:
        session = dbus.SessionBus()
    except dbus.DBusException as e:
        sys.stderr.write("schema-systemd1-session: no session bus: %s\n" % e)
        return 1
    system = dbus.SystemBus()

    flags = dbus.bus.NAME_FLAG_DO_NOT_QUEUE
    if session.request_name(BUS_NAME, flags) != dbus.bus.REQUEST_NAME_REPLY_PRIMARY_OWNER:
        sys.stderr.write("schema-systemd1-session: %s already owned; exiting\n" % BUS_NAME)
        return 0

    def session_filter(conn, msg):
        if not isinstance(msg, ll.MethodCallMessage):
            return ll.HANDLER_RESULT_NOT_YET_HANDLED
        path = msg.get_path() or ""
        if path == PATH_PREFIX and msg.get_member() == "StartTransientUnit" \
                and msg.get_interface() == MGR_IFACE:
            handle_start_transient_unit(session, msg)
            return ll.HANDLER_RESULT_HANDLED
        if path == "/" or path == PATH_PREFIX or path.startswith(PATH_PREFIX + "/"):
            forward_call(session, system, msg)
            return ll.HANDLER_RESULT_HANDLED
        return ll.HANDLER_RESULT_NOT_YET_HANDLED

    def signal_filter(conn, msg):
        if isinstance(msg, ll.SignalMessage):
            path = msg.get_path() or ""
            if path == PATH_PREFIX or path.startswith(PATH_PREFIX + "/"):
                sig = ll.SignalMessage(path, msg.get_interface(), msg.get_member())
                _append(sig, msg.get_args_list(byte_arrays=True), msg.get_signature())
                session.send_message(sig)
        return ll.HANDLER_RESULT_NOT_YET_HANDLED

    session.add_message_filter(session_filter)
    system.add_match_string("type='signal',sender='%s'" % BUS_NAME)
    system.add_message_filter(signal_filter)

    GLib.MainLoop().run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
