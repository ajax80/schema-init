#!/usr/bin/env python3
import sys
import os
import signal
import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

class Login1Session(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/login1/session/_31')
        self.devices = {}
        print("login1-stub: Registered Session at /org/freedesktop/login1/session/_31")

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='', out_signature='')
    def Lock(self):
        print("login1-stub: Session.Lock() requested")

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='', out_signature='')
    def Unlock(self):
        print("login1-stub: Session.Unlock() requested")

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='b', out_signature='')
    def TakeControl(self, force):
        print(f"login1-stub: Session.TakeControl({force}) requested")

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='uu', out_signature='hb')
    def TakeDevice(self, major, minor):
        print(f"login1-stub: Session.TakeDevice({major}, {minor}) requested")
        key = (major, minor)
        if key in self.devices:
            print(f"login1-stub: Device {major}:{minor} already taken, closing existing")
            try:
                os.close(self.devices.pop(key))
            except Exception:
                pass

        device_path = f"/dev/char/{major}:{minor}"
        try:
            fd = os.open(device_path, os.O_RDWR | os.O_CLOEXEC | os.O_NONBLOCK)
            self.devices[key] = fd
            print(f"login1-stub: Opened {device_path} as fd={fd}")
            return dbus.types.UnixFd(fd), dbus.Boolean(False)
        except Exception as e:
            print(f"login1-stub: Failed to open {device_path}: {e}", file=sys.stderr)
            raise dbus.exceptions.DBusException(
                f"Failed to open device {device_path}: {e}",
                name='org.freedesktop.login1.FailedToOpenDevice'
            )

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='uu', out_signature='')
    def ReleaseDevice(self, major, minor):
        print(f"login1-stub: Session.ReleaseDevice({major}, {minor}) requested")
        key = (major, minor)
        fd = self.devices.pop(key, None)
        if fd is not None:
            try:
                os.close(fd)
                print(f"login1-stub: Closed fd={fd} for device {major}:{minor}")
            except Exception as e:
                print(f"login1-stub: Error closing fd={fd}: {e}", file=sys.stderr)

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='uu', out_signature='')
    def PauseDeviceComplete(self, major, minor):
        print(f"login1-stub: Session.PauseDeviceComplete({major}, {minor}) requested")

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface_name, property_name):
        return self.GetAll(interface_name).get(property_name)

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        if interface_name == 'org.freedesktop.login1.Session':
            return {
                'Id': dbus.String('31'),
                'User': dbus.Struct((dbus.UInt32(1000), dbus.ObjectPath('/org/freedesktop/login1/user/_1000')), signature='uo'),
                'Name': dbus.String('root'),
                'Active': dbus.Boolean(True),
                'State': dbus.String('active'),
                'Remote': dbus.Boolean(False),
                'Type': dbus.String('wayland'),
                'Class': dbus.String('user'),
                'Seat': dbus.Struct((dbus.String('seat0'), dbus.ObjectPath('/org/freedesktop/login1/seat/seat0')), signature='so'),
            }
        return {}

class Login1User(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/login1/user/_1000')
        print("login1-stub: Registered User at /org/freedesktop/login1/user/_1000")

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface_name, property_name):
        return self.GetAll(interface_name).get(property_name)

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        if interface_name == 'org.freedesktop.login1.User':
            return {
                'UID': dbus.UInt32(1000),
                'GID': dbus.UInt32(1000),
                'Name': dbus.String('root'),
                'Display': dbus.ObjectPath('/org/freedesktop/login1/session/_31'),
                'State': dbus.String('active'),
            }
        return {}

class Login1Seat(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/login1/seat/seat0')
        print("login1-stub: Registered Seat at /org/freedesktop/login1/seat/seat0")

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface_name, property_name):
        return self.GetAll(interface_name).get(property_name)

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        if interface_name == 'org.freedesktop.login1.Seat':
            return {
                'Id': dbus.String('seat0'),
                'ActiveSession': dbus.Struct((dbus.String('31'), dbus.ObjectPath('/org/freedesktop/login1/session/_31')), signature='so'),
                'CanMultiSession': dbus.Boolean(True),
                'CanTTY': dbus.Boolean(True),
                'CanGraphical': dbus.Boolean(True),
            }
        return {}

class Login1Manager(dbus.service.Object):
    def __init__(self, bus):
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

    # Async delay logic to prevent the LogindSessionBackend race condition
    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='s',
                         async_callbacks=('reply_cb', 'error_cb'))
    def CanSuspend(self, reply_cb, error_cb):
        GLib.timeout_add(50, lambda: self._delayed_reply(reply_cb, "na"))

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='s',
                         async_callbacks=('reply_cb', 'error_cb'))
    def CanHibernate(self, reply_cb, error_cb):
        GLib.timeout_add(50, lambda: self._delayed_reply(reply_cb, "na"))

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='s',
                         async_callbacks=('reply_cb', 'error_cb'))
    def CanHybridSleep(self, reply_cb, error_cb):
        GLib.timeout_add(50, lambda: self._delayed_reply(reply_cb, "na"))

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='s',
                         async_callbacks=('reply_cb', 'error_cb'))
    def CanSuspendThenHibernate(self, reply_cb, error_cb):
        GLib.timeout_add(50, lambda: self._delayed_reply(reply_cb, "na"))

    def _delayed_reply(self, reply_cb, val):
        reply_cb(val)
        return False

    # ── Session, User, Seat methods returning dummy paths ─────────────────

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(susso)')
    def ListSessions(self):
        return [dbus.Struct((dbus.String('31'), dbus.UInt32(1000), dbus.String('root'), dbus.String('seat0'), dbus.ObjectPath('/org/freedesktop/login1/session/_31')), signature='susso')]

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(uso)')
    def ListUsers(self):
        return [dbus.Struct((dbus.UInt32(1000), dbus.String('root'), dbus.ObjectPath('/org/freedesktop/login1/user/_1000')), signature='uso')]

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(ss)')
    def ListSeats(self):
        return [dbus.Struct((dbus.String('seat0'), dbus.ObjectPath('/org/freedesktop/login1/seat/seat0')), signature='ss')]

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='u', out_signature='o')
    def GetSessionByPID(self, pid):
        return dbus.ObjectPath('/org/freedesktop/login1/session/_31')

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='o')
    def GetSession(self, session_id):
        return dbus.ObjectPath('/org/freedesktop/login1/session/_31')

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='u', out_signature='o')
    def GetUser(self, uid):
        return dbus.ObjectPath('/org/freedesktop/login1/user/_1000')

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='o')
    def GetSeat(self, seat_id):
        return dbus.ObjectPath('/org/freedesktop/login1/seat/seat0')

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
                'CanPowerOff': dbus.String('yes'),
                'CanReboot': dbus.String('yes'),
                'CanSuspend': dbus.String('na'),
                'CanHibernate': dbus.String('na'),
                'CanHybridSleep': dbus.String('na'),
                'CanSuspendThenHibernate': dbus.String('na'),
            }
        return {}

class Hostname1(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/hostname1')
        print("login1-stub: Registered Hostname1 at /org/freedesktop/hostname1")

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface_name, property_name):
        return self.GetAll(interface_name).get(property_name)

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        if interface_name == 'org.freedesktop.hostname1':
            return {
                'Hostname': dbus.String('GreyBox'),
                'StaticHostname': dbus.String('GreyBox'),
                'PrettyHostname': dbus.String('GreyBox'),
                'IconName': dbus.String('computer'),
                'Chassis': dbus.String('desktop'),
                'KernelName': dbus.String('Linux'),
                'KernelRelease': dbus.String('6.1.0-49-amd64'),
                'KernelVersion': dbus.String('#1 SMP PREEMPT_DYNAMIC Debian 6.1.0-49'),
                'OperatingSystemPrettyName': dbus.String('Fedora Linux 44 (schema-init)'),
                'OperatingSystemCPEName': dbus.String('cpe:/o:fedoraproject:fedora:44'),
                'HardwareVendor': dbus.String('Dell Inc.'),
                'HardwareModel': dbus.String('Inspiron 3542'),
                'FirmwareVersion': dbus.String('A12'),
            }
        return {}

class Systemd1Manager(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/systemd1')
        print("login1-stub: Registered Systemd1 Manager at /org/freedesktop/systemd1")

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='s', out_signature='s')
    def GetUnitFileState(self, name):
        print(f"systemd1-stub: GetUnitFileState({name}) requested")
        if name in ('sddm.service', 'sddm', 'display-manager.service'):
            return "enabled"
        return "disabled"

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='s', out_signature='o')
    def GetUnit(self, name):
        print(f"systemd1-stub: GetUnit({name}) requested")
        escaped = name.replace('.', '_2e').replace('-', '_2d')
        return dbus.ObjectPath(f'/org/freedesktop/systemd1/unit/{escaped}')

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='', out_signature='a(ssssssouso)')
    def ListUnits(self):
        print("systemd1-stub: ListUnits() requested")
        return [
            dbus.Struct((
                dbus.String('sddm.service'),
                dbus.String('Simple Desktop Display Manager'),
                dbus.String('loaded'),
                dbus.String('active'),
                dbus.String('running'),
                dbus.String(''),
                dbus.ObjectPath('/org/freedesktop/systemd1/unit/sddm_2eservice'),
                dbus.UInt32(0),
                dbus.String(''),
                dbus.ObjectPath('/')
            ), signature='ssssssouso')
        ]

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface_name, property_name):
        return self.GetAll(interface_name).get(property_name)

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        if interface_name == 'org.freedesktop.systemd1.Manager':
            return {
                'Version': dbus.String('256'),
                'Features': dbus.String('+PAM +AUDIT +SELINUX -APPARMOR +GLIB'),
                'Architecture': dbus.String('x86-64'),
            }
        return {}

def main():
    DBusGMainLoop(set_as_default=True)

    try:
        bus = dbus.SystemBus()
    except Exception as e:
        print(f"login1-stub: Failed to connect to System Bus: {e}", file=sys.stderr)
        sys.exit(1)

    # Instantiate the dummy objects
    session = Login1Session(bus)
    user = Login1User(bus)
    seat = Login1Seat(bus)
    manager = Login1Manager(bus)
    hostname = Hostname1(bus)
    systemd = Systemd1Manager(bus)

    # Request the well-known names
    try:
        bus.request_name('org.freedesktop.login1', dbus.bus.NAME_FLAG_REPLACE_EXISTING)
        print("login1-stub: Successfully acquired 'org.freedesktop.login1' name")
    except Exception as e:
        print(f"login1-stub: Failed to acquire name 'org.freedesktop.login1': {e}", file=sys.stderr)
        sys.exit(1)

    try:
        bus.request_name('org.freedesktop.hostname1', dbus.bus.NAME_FLAG_REPLACE_EXISTING)
        print("login1-stub: Successfully acquired 'org.freedesktop.hostname1' name")
    except Exception as e:
        print(f"login1-stub: Failed to acquire name 'org.freedesktop.hostname1': {e}", file=sys.stderr)

    try:
        bus.request_name('org.freedesktop.systemd1', dbus.bus.NAME_FLAG_REPLACE_EXISTING)
        print("login1-stub: Successfully acquired 'org.freedesktop.systemd1' name")
    except Exception as e:
        print(f"login1-stub: Failed to acquire name 'org.freedesktop.systemd1': {e}", file=sys.stderr)

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
