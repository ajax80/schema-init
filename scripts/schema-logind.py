#!/usr/bin/env python3
import sys
import os
import signal
import dbus
import dbus.service
import dbus.exceptions
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib
import socket
import pwd
import time

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
    try:
        uids = [int(d) for d in os.listdir('/run/user') if d.isdigit() and int(d) >= 1000]
        if uids:
            return uids[0]
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
    """Ownership gate real logind enforces via session/seat membership.

    Without it, ANY bus client can TakeDevice() an input-device fd (a
    keylogger) or power off the box. We can't track real session ownership
    in a stub, so we allow root (covers root-run display-manager greeters)
    and the active local session's uid (the user's compositor), and deny
    everyone else — e.g. a compromised low-privilege daemon.
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
        return self.GetAll(interface_name).get(property_name)

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
                'Type': dbus.String('wayland'),
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
        return self.GetAll(interface_name).get(property_name)

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
            hn = socket.gethostname() or "localhost"
            os_pretty = get_os_release_val('PRETTY_NAME') or "Fedora Linux"
            os_cpe = get_os_release_val('CPE_NAME') or "cpe:/o:fedoraproject:fedora"
            
            # Read DMI hardware attributes dynamically
            vendor = read_file_line('/sys/class/dmi/id/sys_vendor') or "Unknown"
            model = read_file_line('/sys/class/dmi/id/product_name') or "Unknown"
            firmware = read_file_line('/sys/class/dmi/id/bios_version') or "Unknown"
            chassis = read_file_line('/sys/class/dmi/id/chassis_type') or "desktop"
            
            # Map common chassis type numbers to strings
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
    uid = get_active_uid()
    session = Login1Session(bus)
    user = Login1User(bus, uid)
    seat = Login1Seat(bus)
    manager = Login1Manager(bus)
    hostname = Hostname1(bus)
    systemd = Systemd1Manager(bus)
    timedate = Timedate1(bus)

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

    try:
        bus.request_name('org.freedesktop.timedate1', dbus.bus.NAME_FLAG_REPLACE_EXISTING)
        print("login1-stub: Successfully acquired 'org.freedesktop.timedate1' name")
    except Exception as e:
        print(f"login1-stub: Failed to acquire name 'org.freedesktop.timedate1': {e}", file=sys.stderr)

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
