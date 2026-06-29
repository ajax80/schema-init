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
import sys
import dbus
import dbus.lowlevel as ll
import dbus.mainloop.glib
from gi.repository import GLib

BUS_NAME = "org.freedesktop.systemd1"
PATH_PREFIX = "/org/freedesktop/systemd1"
FWD_TIMEOUT = 25000  # ms; covers an interactive polkit prompt on a write


def _append(msg, args, sig):
    if args:
        msg.append(*args, signature=sig if sig else None)


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


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
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
