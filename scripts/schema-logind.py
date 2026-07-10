#!/usr/bin/env python3
import sys
import os
import signal
import time
import threading
import json
import subprocess
import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib
import socket
import pwd

def read_file_line(path):
    try:
        with open(path, 'r') as f:
            return f.read().strip()
    except Exception:
        return ""

def get_os_release_val(key):
    try:
        with open('/etc/os-release', 'r') as f:
            for line in f:
                if line.startswith(key + '='):
                    val = line.split('=', 1)[1].strip()
                    if (val.startswith('"') and val.endswith('"')) or (val.startswith("'") and val.endswith("'")):
                        val = val[1:-1]
                    return val
    except Exception:
        pass
    return ""

def get_active_uid():
    # SECURITY: resolve the active-session uid ONLY from root-created
    # /run/user/<uid> entries. Do NOT derive it from /proc/<pid>/environ
    # (e.g. XDG_VTNR mapped to the active VT) — environ is forgeable by the
    # process owner, so a low-pid unprivileged daemon could forge the active VT
    # number and make this return ITS uid, defeating caller_authorized() and
    # granting PowerOff/Reboot/TakeDevice. (/dev/ttyN is root-owned under
    # schema-init, so the device-owner signal is unavailable too.) min() gives a
    # deterministic pick from the trusted set; true foreground-session selection
    # on a multi-user box is a follow-up that must use a trusted seat source.
    # See docs/reviews/logind_authz_gate_review.md.
    try:
        uids = [int(d) for d in os.listdir('/run/user') if d.isdigit() and int(d) >= 1000]
        if uids:
            return min(uids)
    except Exception:
        pass
    return 1000


def get_username_for_uid(uid):
    try:
        return pwd.getpwuid(uid).pw_name
    except Exception:
        return "root"

def get_gid_for_uid(uid):
    try:
        return pwd.getpwuid(uid).pw_gid
    except Exception:
        return uid

def caller_uid(connection, sender):
    """uid of the D-Bus caller, or None if it can't be determined."""
    if sender is None:
        return None
    try:
        return int(connection.get_unix_user(sender))
    except Exception:
        return None

def caller_authorized(connection, sender):
    """Ownership gate real logind/ConsoleKit enforce via session/seat membership.

    Without it, ANY bus client can TakeDevice() an input-device fd (a
    keylogger) or power off the box via login1 PowerOff/Reboot or ConsoleKit
    Restart/Stop. We allow root (covers the root-run X server / greeter) and
    the active local session's uid (the user's session), and deny everyone
    else — e.g. a compromised low-privilege daemon.
    """
    uid = caller_uid(connection, sender)
    if uid is None:
        return False
    return uid == 0 or uid == get_active_uid()

def get_timezone():
    try:
        target = os.readlink('/etc/localtime')
        if 'zoneinfo/' in target:
            return target.split('zoneinfo/', 1)[1]
    except OSError:
        pass
    return read_file_line('/etc/timezone') or 'UTC'

def get_session_type():
    """Detect wayland vs x11 at runtime instead of hardcoding per distro —
    a Wayland socket in the active user's runtime dir means a Wayland session.
    This is what made the KDE and Cinnamon copies diverge."""
    try:
        rundir = '/run/user/%d' % get_active_uid()
        for name in os.listdir(rundir):
            if name.startswith('wayland-'):
                return 'wayland'
    except OSError:
        pass
    return 'x11'

def svc_name(unit):
    for suffix in ('.service', '.target', '.socket', '.timer', '.mount', '.path'):
        if unit.endswith(suffix):
            return unit[:-len(suffix)]
    return unit

def schema_ctl(action, name):
    try:
        subprocess.run(['schema-ctl', action, name], capture_output=True, timeout=5)
    except Exception as e:
        print(f"systemd1-stub: schema-ctl {action} {name} failed: {e}", file=sys.stderr)

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

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='b', out_signature='',
                         sender_keyword='sender')
    def TakeControl(self, force, sender=None):
        if not caller_authorized(self._connection, sender):
            print(f"login1-stub: DENY TakeControl from uid={caller_uid(self._connection, sender)}",
                  file=sys.stderr)
            raise dbus.exceptions.DBusException(
                "Not the active session owner",
                name='org.freedesktop.login1.AccessDenied')
        print(f"login1-stub: Session.TakeControl({force}) requested")

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='uu', out_signature='hb',
                         sender_keyword='sender')
    def TakeDevice(self, major, minor, sender=None):
        if not caller_authorized(self._connection, sender):
            print(f"login1-stub: DENY TakeDevice({major},{minor}) from "
                  f"uid={caller_uid(self._connection, sender)}", file=sys.stderr)
            raise dbus.exceptions.DBusException(
                "Not the active session owner",
                name='org.freedesktop.login1.AccessDenied')
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
        props = self.GetAll(interface_name)
        if property_name not in props:
            raise dbus.exceptions.DBusException(
                'No such property: ' + str(property_name),
                name='org.freedesktop.DBus.Error.UnknownProperty')
        return props[property_name]

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        if interface_name == 'org.freedesktop.login1.Session':
            uid = get_active_uid()
            username = get_username_for_uid(uid)
            return {
                'Id': dbus.String('31'),
                'User': dbus.Struct((dbus.UInt32(uid), dbus.ObjectPath(f'/org/freedesktop/login1/user/_{uid}')), signature='uo'),
                'Name': dbus.String(username),
                'Active': dbus.Boolean(True),
                'State': dbus.String('active'),
                'Remote': dbus.Boolean(False),
                'Type': dbus.String(get_session_type()),
                'Class': dbus.String('user'),
                'Seat': dbus.Struct((dbus.String('seat0'), dbus.ObjectPath('/org/freedesktop/login1/seat/seat0')), signature='so'),
                'CanReboot': dbus.String('yes'),
                'CanPowerOff': dbus.String('yes'),
            }
        return {}

class Login1User(dbus.service.Object):
    def __init__(self, bus, uid=1000):
        self.uid = uid
        dbus.service.Object.__init__(self, bus, f'/org/freedesktop/login1/user/_{uid}')
        print(f"login1-stub: Registered User at /org/freedesktop/login1/user/_{uid}")

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface_name, property_name):
        props = self.GetAll(interface_name)
        if property_name not in props:
            raise dbus.exceptions.DBusException(
                'No such property: ' + str(property_name),
                name='org.freedesktop.DBus.Error.UnknownProperty')
        return props[property_name]

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        if interface_name == 'org.freedesktop.login1.User':
            uid = getattr(self, 'uid', 1000)
            username = get_username_for_uid(uid)
            gid = get_gid_for_uid(uid)
            return {
                'UID': dbus.UInt32(uid),
                'GID': dbus.UInt32(gid),
                'Name': dbus.String(username),
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
        props = self.GetAll(interface_name)
        if property_name not in props:
            raise dbus.exceptions.DBusException(
                'No such property: ' + str(property_name),
                name='org.freedesktop.DBus.Error.UnknownProperty')
        return props[property_name]

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

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='b', out_signature='',
                         sender_keyword='sender')
    def PowerOff(self, interactive, sender=None):
        if not caller_authorized(self._connection, sender):
            print(f"login1-stub: DENY PowerOff from uid={caller_uid(self._connection, sender)}",
                  file=sys.stderr)
            raise dbus.exceptions.DBusException(
                "Not authorized to power off",
                name='org.freedesktop.login1.AccessDenied')
        print(f"login1-stub: PowerOff (uid={caller_uid(self._connection, sender)}) -> SIGTERM to PID 1")
        try:
            os.kill(1, signal.SIGTERM)
        except ProcessLookupError:
            print("login1-stub: PID 1 not found (not running as init system)")
            sys.exit(0)

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='b', out_signature='',
                         sender_keyword='sender')
    def Reboot(self, interactive, sender=None):
        if not caller_authorized(self._connection, sender):
            print(f"login1-stub: DENY Reboot from uid={caller_uid(self._connection, sender)}",
                  file=sys.stderr)
            raise dbus.exceptions.DBusException(
                "Not authorized to reboot",
                name='org.freedesktop.login1.AccessDenied')
        print(f"login1-stub: Reboot (uid={caller_uid(self._connection, sender)}) -> SIGINT to PID 1")
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

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(susso)')
    def ListSessions(self):
        uid = get_active_uid()
        username = get_username_for_uid(uid)
        return [dbus.Struct((dbus.String('31'), dbus.UInt32(uid), dbus.String(username), dbus.String('seat0'), dbus.ObjectPath('/org/freedesktop/login1/session/_31')), signature='susso')]

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(uso)')
    def ListUsers(self):
        uid = get_active_uid()
        username = get_username_for_uid(uid)
        return [dbus.Struct((dbus.UInt32(uid), dbus.String(username), dbus.ObjectPath(f'/org/freedesktop/login1/user/_{uid}')), signature='uso')]

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(so)')
    def ListSeats(self):
        return [dbus.Struct((dbus.String('seat0'), dbus.ObjectPath('/org/freedesktop/login1/seat/seat0')), signature='so')]

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='ssss', out_signature='h')
    def Inhibit(self, what, who, why, mode):
        print(f"login1-stub: Inhibit({what}, {who}, {why}, {mode})")
        r, w = os.pipe()
        os.close(r)
        return dbus.types.UnixFd(w)

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(ssssuu)')
    def ListInhibitors(self):
        return []

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='u', out_signature='o')
    def GetSessionByPID(self, pid):
        return dbus.ObjectPath('/org/freedesktop/login1/session/_31')

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='o')
    def GetSession(self, session_id):
        return dbus.ObjectPath('/org/freedesktop/login1/session/_31')

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='u', out_signature='o')
    def GetUser(self, uid):
        return dbus.ObjectPath(f'/org/freedesktop/login1/user/_{uid}')

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='o')
    def GetSeat(self, seat_id):
        return dbus.ObjectPath('/org/freedesktop/login1/seat/seat0')

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface_name, property_name):
        props = self.GetAll(interface_name)
        if property_name not in props:
            raise dbus.exceptions.DBusException(
                'No such property: ' + str(property_name),
                name='org.freedesktop.DBus.Error.UnknownProperty')
        return props[property_name]

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
        props = self.GetAll(interface_name)
        if property_name not in props:
            raise dbus.exceptions.DBusException(
                'No such property: ' + str(property_name),
                name='org.freedesktop.DBus.Error.UnknownProperty')
        return props[property_name]

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        if interface_name == 'org.freedesktop.hostname1':
            hn = socket.gethostname() or "localhost"
            os_pretty = get_os_release_val('PRETTY_NAME') or "Fedora Linux"
            os_cpe = get_os_release_val('CPE_NAME') or "cpe:/o:fedoraproject:fedora"
            vendor = read_file_line('/sys/class/dmi/id/sys_vendor') or "Unknown"
            model = read_file_line('/sys/class/dmi/id/product_name') or "Unknown"
            firmware = read_file_line('/sys/class/dmi/id/bios_version') or "Unknown"
            chassis = read_file_line('/sys/class/dmi/id/chassis_type') or "desktop"
            if chassis in ('3', '4', '6', '7'):
                chassis_str = 'desktop'
            elif chassis in ('8', '9', '10', '14'):
                chassis_str = 'laptop'
            else:
                chassis_str = 'desktop'
            return {
                'Hostname': dbus.String(hn),
                'StaticHostname': dbus.String(hn),
                'PrettyHostname': dbus.String(hn),
                'IconName': dbus.String('computer'),
                'Chassis': dbus.String(chassis_str),
                'KernelName': dbus.String('Linux'),
                'KernelRelease': dbus.String(os.uname().release),
                'KernelVersion': dbus.String(os.uname().version),
                'OperatingSystemPrettyName': dbus.String(f"{os_pretty} (schema-init)"),
                'OperatingSystemCPEName': dbus.String(os_cpe),
                'HardwareVendor': dbus.String(vendor),
                'HardwareModel': dbus.String(model),
                'FirmwareVersion': dbus.String(firmware),
            }
        return {}

class ConsoleKitSession(dbus.service.Object):
    def __init__(self, bus, uid=1000):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/ConsoleKit/Session1')
        self.uid = uid
        print(f"ck-stub: Registered Session at /org/freedesktop/ConsoleKit/Session1")

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='o')
    def GetId(self):
        return dbus.ObjectPath('/org/freedesktop/ConsoleKit/Session1')

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='o')
    def GetSeatId(self):
        return dbus.ObjectPath('/org/freedesktop/ConsoleKit/Seat1')

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='s')
    def GetSessionType(self):
        return "LoginSession"

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='u')
    def GetUser(self):
        return dbus.UInt32(self.uid)

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='u')
    def GetUnixUser(self):
        return dbus.UInt32(self.uid)

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='s')
    def GetXDisplay(self):
        return ":0"

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='s')
    def GetX11Display(self):
        return ":0"

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='s')
    def GetX11DisplayDevice(self):
        return "/dev/tty7"

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='s')
    def GetDisplayDevice(self):
        return "/dev/tty7"

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='s')
    def GetRemoteHostName(self):
        return ""

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='s')
    def GetLoginSessionId(self):
        return "1"

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='b')
    def IsActive(self):
        return dbus.Boolean(True)

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='b')
    def IsLocal(self):
        return dbus.Boolean(True)

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='b')
    def GetIdleHint(self):
        return dbus.Boolean(False)

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='b', out_signature='')
    def SetIdleHint(self, idle):
        pass

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='')
    def Lock(self):
        pass

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='')
    def Unlock(self):
        pass

    @dbus.service.method('org.freedesktop.ConsoleKit.Session', in_signature='', out_signature='')
    def Activate(self):
        pass

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface_name, property_name):
        props = self.GetAll(interface_name)
        if property_name not in props:
            raise dbus.exceptions.DBusException(
                'No such property: ' + str(property_name),
                name='org.freedesktop.DBus.Error.UnknownProperty')
        return props[property_name]

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        if interface_name == 'org.freedesktop.ConsoleKit.Session':
            return {
                'Id': dbus.ObjectPath('/org/freedesktop/ConsoleKit/Session1'),
                'User': dbus.UInt32(self.uid),
                'Active': dbus.Boolean(True),
                'IsLocal': dbus.Boolean(True),
                'SeatId': dbus.ObjectPath('/org/freedesktop/ConsoleKit/Seat1'),
                'SessionType': dbus.String('LoginSession'),
                'X11Display': dbus.String(':0'),
                'X11DisplayDevice': dbus.String('/dev/tty7'),
                'DisplayDevice': dbus.String('/dev/tty7'),
                'RemoteHostName': dbus.String(''),
                'IdleHint': dbus.Boolean(False),
            }
        return {}


class ConsoleKitManager(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/ConsoleKit/Manager')
        print("ck-stub: Registered Manager at /org/freedesktop/ConsoleKit/Manager")

    @dbus.service.method('org.freedesktop.ConsoleKit.Manager', in_signature='u', out_signature='o')
    def GetSessionForUnixProcess(self, pid):
        print(f"ck-stub: GetSessionForUnixProcess({pid})")
        return dbus.ObjectPath('/org/freedesktop/ConsoleKit/Session1')

    @dbus.service.method('org.freedesktop.ConsoleKit.Manager', in_signature='', out_signature='o')
    def GetCurrentSession(self):
        return dbus.ObjectPath('/org/freedesktop/ConsoleKit/Session1')

    @dbus.service.method('org.freedesktop.ConsoleKit.Manager', in_signature='', out_signature='ao')
    def GetSessions(self):
        return [dbus.ObjectPath('/org/freedesktop/ConsoleKit/Session1')]

    @dbus.service.method('org.freedesktop.ConsoleKit.Manager', in_signature='', out_signature='ao')
    def GetSeats(self):
        return [dbus.ObjectPath('/org/freedesktop/ConsoleKit/Seat1')]

    @dbus.service.method('org.freedesktop.ConsoleKit.Manager', in_signature='', out_signature='b')
    def CanRestart(self):
        print("ck-stub: CanRestart -> True")
        return dbus.Boolean(True)

    @dbus.service.method('org.freedesktop.ConsoleKit.Manager', in_signature='', out_signature='b')
    def CanStop(self):
        return dbus.Boolean(True)

    @dbus.service.method('org.freedesktop.ConsoleKit.Manager', in_signature='', out_signature='',
                         sender_keyword='sender')
    def Restart(self, sender=None):
        if not caller_authorized(self._connection, sender):
            print(f"ck-stub: DENY Restart from uid={caller_uid(self._connection, sender)}",
                  file=sys.stderr)
            raise dbus.exceptions.DBusException(
                "Not authorized to restart",
                name='org.freedesktop.ConsoleKit.Manager.NotPrivileged')
        print(f"ck-stub: Restart (uid={caller_uid(self._connection, sender)}) -> SIGINT to PID 1")
        os.kill(1, signal.SIGINT)

    @dbus.service.method('org.freedesktop.ConsoleKit.Manager', in_signature='', out_signature='',
                         sender_keyword='sender')
    def Stop(self, sender=None):
        if not caller_authorized(self._connection, sender):
            print(f"ck-stub: DENY Stop from uid={caller_uid(self._connection, sender)}",
                  file=sys.stderr)
            raise dbus.exceptions.DBusException(
                "Not authorized to stop",
                name='org.freedesktop.ConsoleKit.Manager.NotPrivileged')
        print(f"ck-stub: Stop (uid={caller_uid(self._connection, sender)}) -> SIGTERM to PID 1")
        os.kill(1, signal.SIGTERM)

    @dbus.service.method('org.freedesktop.ConsoleKit.Manager', in_signature='', out_signature='s')
    def OpenSession(self):
        return dbus.String('schema-ck-cookie')

    @dbus.service.method('org.freedesktop.ConsoleKit.Manager', in_signature='s', out_signature='b')
    def CloseSession(self, cookie):
        return dbus.Boolean(True)

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface_name, property_name):
        props = self.GetAll(interface_name)
        if property_name not in props:
            raise dbus.exceptions.DBusException(
                'No such property: ' + str(property_name),
                name='org.freedesktop.DBus.Error.UnknownProperty')
        return props[property_name]

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        return {}


class Timedate1(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/timedate1')
        print("login1-stub: Registered Timedate1 at /org/freedesktop/timedate1")

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface_name, property_name):
        return self.GetAll(interface_name).get(property_name)

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        if interface_name == 'org.freedesktop.timedate1':
            now_us = dbus.UInt64(int(time.time() * 1_000_000))
            return {
                'Timezone': dbus.String(get_timezone()),
                'LocalRTC': dbus.Boolean(False),
                'CanNTP': dbus.Boolean(True),
                'NTP': dbus.Boolean(True),
                'NTPSynchronized': dbus.Boolean(True),
                'TimeUSec': now_us,
                'RTCTimeUSec': now_us,
            }
        return {}

    @dbus.service.method('org.freedesktop.timedate1', in_signature='sb', out_signature='')
    def SetTimezone(self, timezone, interactive):
        zonefile = '/usr/share/zoneinfo/' + timezone
        if '..' in timezone or not os.path.isfile(zonefile):
            raise dbus.exceptions.DBusException(
                'Invalid time zone: ' + timezone,
                name='org.freedesktop.timedate1.Error.InvalidTimezone')
        try:
            tmp = '/etc/localtime.new'
            if os.path.lexists(tmp):
                os.remove(tmp)
            os.symlink(zonefile, tmp)
            os.replace(tmp, '/etc/localtime')
            with open('/etc/timezone', 'w') as f:
                f.write(timezone + '\n')
        except OSError as e:
            raise dbus.exceptions.DBusException(
                str(e), name='org.freedesktop.timedate1.Error.Failed')

    @dbus.service.method('org.freedesktop.timedate1', in_signature='bb', out_signature='')
    def SetLocalRTC(self, local_rtc, fix_system):
        pass

    @dbus.service.method('org.freedesktop.timedate1', in_signature='bb', out_signature='')
    def SetNTP(self, use_ntp, interactive):
        pass

    @dbus.service.method('org.freedesktop.timedate1', in_signature='xbb', out_signature='')
    def SetTime(self, usec_utc, relative, interactive):
        pass




class VarlinkServer:
    """Minimal varlink socket — systemd 256+ systemctl tries this before dbus."""

    SOCKET_PATH = '/run/systemd/private/io.systemd.Manager'

    def __init__(self):
        os.makedirs('/run/systemd/private', exist_ok=True)
        if os.path.exists(self.SOCKET_PATH):
            os.unlink(self.SOCKET_PATH)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(self.SOCKET_PATH)
        os.chmod(self.SOCKET_PATH, 0o666)
        self.sock.listen(8)
        print(f"varlink-stub: Listening at {self.SOCKET_PATH}")

    def start(self):
        t = threading.Thread(target=self._accept_loop, daemon=True)
        t.start()

    def _accept_loop(self):
        while True:
            try:
                conn, _ = self.sock.accept()
                threading.Thread(target=self._handle, args=(conn,), daemon=True).start()
            except Exception:
                break

    def _handle(self, conn):
        buf = b''
        try:
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while b'\0' in buf:
                    msg, buf = buf.split(b'\0', 1)
                    if msg:
                        self._dispatch(conn, msg)
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except Exception:
                pass

    def _dispatch(self, conn, msg):
        try:
            req = json.loads(msg)
            method = req.get('method', '')
            params = req.get('parameters', {})
            resp = self._route(method, params)
        except Exception as e:
            resp = {'error': 'io.systemd.Error.Failed', 'parameters': {'description': str(e)}}
        try:
            conn.sendall(json.dumps(resp).encode() + b'\0')
        except Exception:
            pass

    def _route(self, method, params):
        print(f"varlink-stub: {method}({params})")
        if method == 'io.systemd.Manager.StartUnit':
            schema_ctl('start', svc_name(params.get('name', '')))
            return {'parameters': {'job': '/org/freedesktop/systemd1/job/1'}}
        elif method == 'io.systemd.Manager.StopUnit':
            schema_ctl('stop', svc_name(params.get('name', '')))
            return {'parameters': {'job': '/org/freedesktop/systemd1/job/2'}}
        elif method == 'io.systemd.Manager.RestartUnit':
            schema_ctl('reset', svc_name(params.get('name', '')))
            return {'parameters': {'job': '/org/freedesktop/systemd1/job/3'}}
        elif method == 'io.systemd.Manager.EnableUnitFiles':
            return {'parameters': {'carries_install_info': False, 'install_changes': []}}
        elif method == 'io.systemd.Manager.DisableUnitFiles':
            return {'parameters': {'install_changes': []}}
        elif method == 'io.systemd.Manager.MaskUnitFiles':
            return {'parameters': {'install_changes': []}}
        elif method == 'io.systemd.Manager.GetUnitFileState':
            return {'parameters': {'state': 'disabled'}}
        elif method in ('io.systemd.Manager.Subscribe', 'io.systemd.Manager.Unsubscribe',
                        'io.systemd.Manager.Reload'):
            return {'parameters': {}}
        elif method == 'io.systemd.Manager.ListUnits':
            return {'parameters': {'units': []}}
        else:
            return {'error': 'io.systemd.Error.NoSuchMethod', 'parameters': {'method': method}}


def main():
    DBusGMainLoop(set_as_default=True)

    os.makedirs('/run/systemd/system', exist_ok=True)
    # sd_login_monitor_new(NULL) watches all four; missing dir -> ENOENT (-2) aborts WirePlumber's logind module.
    for _d in ('sessions', 'seats', 'users', 'machines'):
        os.makedirs('/run/systemd/' + _d, exist_ok=True)
    # NOTE: do NOT create /run/systemd/private — root systemctl connects there as a
    # peer sd-bus socket (to bypass polkit). We don't serve that socket, and creating
    # it as a directory makes root systemctl fail with "Connection refused" instead of
    # cleanly falling through. Non-root systemctl uses the D-Bus system bus and works.

    uid = get_active_uid()
    runtime_dir = f'/run/user/{uid}'
    os.makedirs(runtime_dir, exist_ok=True)
    os.chmod(runtime_dir, 0o700)
    try:
        os.chown(runtime_dir, uid, -1)
    except Exception:
        pass

    # systemctl on this systemd talks plain D-Bus to org.freedesktop.systemd1.Manager
    # over /run/dbus/system_bus_socket — it does NOT use varlink here. Creating a
    # varlink socket actually BREAKS systemctl (it prefers the local transport, then
    # the stub refuses the connection). So we do NOT start a varlink server.
    # NOTE: the org.freedesktop.systemd1 D-Bus surface now lives in its own service,
    # scripts/schema-systemd1.py — this logind shim no longer owns that name.

    try:
        bus = dbus.SystemBus()
    except Exception as e:
        print(f"login1-stub: Failed to connect to System Bus: {e}", file=sys.stderr)
        sys.exit(1)

    uid = get_active_uid()
    session = Login1Session(bus)
    user = Login1User(bus, uid)
    seat = Login1Seat(bus)
    manager = Login1Manager(bus)
    hostname = Hostname1(bus)
    timedate = Timedate1(bus)

    ck_session = ConsoleKitSession(bus, uid)
    ck_manager = ConsoleKitManager(bus)

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
        bus.request_name('org.freedesktop.timedate1', dbus.bus.NAME_FLAG_REPLACE_EXISTING)
        print("login1-stub: Successfully acquired 'org.freedesktop.timedate1' name")
    except Exception as e:
        print(f"login1-stub: Failed to acquire name 'org.freedesktop.timedate1': {e}", file=sys.stderr)

    try:
        bus.request_name('org.freedesktop.ConsoleKit', dbus.bus.NAME_FLAG_REPLACE_EXISTING)
        print("login1-stub: Successfully acquired 'org.freedesktop.ConsoleKit' name")
    except Exception as e:
        print(f"login1-stub: Failed to acquire name 'org.freedesktop.ConsoleKit': {e}", file=sys.stderr)

    loop = GLib.MainLoop()

    def shutdown_handler(sig, frame):
        print(f"login1-stub: Received signal {sig}, shutting down...")
        loop.quit()

    signal.signal(signal.SIGINT, shutdown_handler)
    signal.signal(signal.SIGTERM, shutdown_handler)

    loop.run()

if __name__ == '__main__':
    main()
