#!/usr/bin/env python3
import sys
import os
import signal
import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

class Login1Manager(dbus.service.Object):
    def __init__(self, bus):
        # Register the manager object
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/login1')
        print("login1-stub: Registered Manager at /org/freedesktop/login1")

    # ── Manager Methods ───────────────────────────────────────────────────

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='b', out_signature='')
    def PowerOff(self, interactive):
        print("login1-stub: PowerOff requested -> sending SIGTERM to PID 1")
        try:
            os.kill(1, signal.SIGTERM)
        except ProcessLookupError:
            print("login1-stub: PID 1 not found (not running as init system)")
            sys.exit(0)

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='b', out_signature='')
    def Reboot(self, interactive):
        print("login1-stub: Reboot requested -> sending SIGINT to PID 1")
        try:
            os.kill(1, signal.SIGINT)
        except ProcessLookupError:
            print("login1-stub: PID 1 not found (not running as init system)")
            sys.exit(0)

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='s')
    def CanPowerOff(self):
        return "yes"

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='s')
    def CanReboot(self):
        return "yes"

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='s')
    def CanSuspend(self):
        return "na"

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='s')
    def CanHibernate(self):
        return "na"

    # ── Stubbed Session, User, Seat methods ────────────────────────────────

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(sus)')
    def ListSessions(self):
        return []

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(su)')
    def ListUsers(self):
        return []

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(ss)')
    def ListSeats(self):
        return []

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='u', out_signature='o')
    def GetSessionByPID(self, pid):
        raise dbus.exceptions.DBusException(
            "org.freedesktop.login1.NoSessionForPID",
            f"No session found for PID {pid}"
        )

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='o')
    def GetSession(self, session_id):
        raise dbus.exceptions.DBusException(
            "org.freedesktop.login1.NoSuchSession",
            f"No session found with ID {session_id}"
        )

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='u', out_signature='o')
    def GetUser(self, uid):
        raise dbus.exceptions.DBusException(
            "org.freedesktop.login1.NoSuchUser",
            f"No user found with UID {uid}"
        )

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='o')
    def GetSeat(self, seat_id):
        raise dbus.exceptions.DBusException(
            "org.freedesktop.login1.NoSuchSeat",
            f"No seat found with ID {seat_id}"
        )

    # ── Properties Interface ──────────────────────────────────────────────

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface_name, property_name):
        return self.GetAll(interface_name).get(property_name)

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        if interface_name == 'org.freedesktop.login1.Manager':
            return {
                'NAutoVTs': dbus.UInt32(6),
                'KillUserProcesses': dbus.Boolean(False),
                'PreparingForShutdown': dbus.Boolean(False),
                'PreparingForSleep': dbus.Boolean(False),
                'PreparingForReboot': dbus.Boolean(False),
            }
        return {}

def main():
    DBusGMainLoop(set_as_default=True)

    try:
        bus = dbus.SystemBus()
    except Exception as e:
        print(f"login1-stub: Failed to connect to System Bus: {e}", file=sys.stderr)
        sys.exit(1)

    manager = Login1Manager(bus)

    # Request the well-known name
    try:
        bus.request_name('org.freedesktop.login1', dbus.bus.NAME_FLAG_REPLACE_EXISTING)
        print("login1-stub: Successfully acquired 'org.freedesktop.login1' name")
    except Exception as e:
        print(f"login1-stub: Failed to acquire name 'org.freedesktop.login1': {e}", file=sys.stderr)
        sys.exit(1)

    loop = GLib.MainLoop()
    
    # Handle clean termination
    def shutdown_handler(sig, frame):
        print(f"login1-stub: Received signal {sig}, shutting down...")
        loop.quit()

    signal.signal(signal.SIGINT, shutdown_handler)
    signal.signal(signal.SIGTERM, shutdown_handler)

    loop.run()

if __name__ == '__main__':
    main()
