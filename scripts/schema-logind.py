#!/usr/bin/env python3
import sys
import os
import fcntl
import struct
import termios
import signal
import time
import threading
import json
import subprocess
import dbus
import dbus.service
import dbus.lowlevel
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

_COMPOSITORS = ('kwin_wayland', 'cinnamon', 'gnome-shell', 'Xorg', 'plasmashell')

def get_session_leader():
    """(pid, comm) of the session leader, or (0, '').

    Found as getsid() of a live compositor owned by the active uid — on a real
    login that resolves to the display-manager session process (sddm-logged
    here), which is exactly what logind calls Leader. Deliberately NOT read from
    /proc/<pid>/environ: see the note in get_active_uid(). Worst case a caller
    gets Leader=0, which is what it already got before this existed."""
    uid = get_active_uid()
    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue
        pid = int(entry)
        try:
            if os.stat('/proc/%d' % pid).st_uid != uid:
                continue
            if read_file_line('/proc/%d/comm' % pid) not in _COMPOSITORS:
                continue
            sid = os.getsid(pid)
            return sid, (read_file_line('/proc/%d/comm' % sid) or '')
        except (OSError, ProcessLookupError):
            continue
    return 0, ''

def get_desktop_name():
    # Runtime-detected for the same reason get_session_type() is: hardcoding it
    # is what made the KDE and Cinnamon copies of this file diverge.
    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue
        comm = read_file_line('/proc/%s/comm' % entry)
        if comm == 'kwin_wayland' or comm == 'plasmashell':
            return 'KDE'
        if comm == 'cinnamon':
            return 'X-Cinnamon'
        if comm == 'gnome-shell':
            return 'GNOME'
    return ''

def proc_start_usec(pid):
    """(realtime, monotonic) session start in usec, from /proc/<pid>/stat field
    22. That field is already ticks-since-boot, so it IS the monotonic stamp;
    adding /proc/stat btime converts it to realtime. loginctl needs both — with
    TimestampMonotonic=0 it prints the session duration as '(null)'."""
    try:
        with open('/proc/%d/stat' % pid) as f:
            fields = f.read().rsplit(') ', 1)[1].split()
        ticks = int(fields[19])
        hz = os.sysconf('SC_CLK_TCK')
        monotonic = int(ticks / hz * 1000000)
        with open('/proc/stat') as f:
            for line in f:
                if line.startswith('btime '):
                    btime = int(line.split()[1])
                    return int(btime * 1000000) + monotonic, monotonic
    except (OSError, IndexError, ValueError):
        pass
    return 0, 0

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

# --- VT / session-device handoff -------------------------------------------
# A compositor that TakeDevice()s the DRM node becomes DRM master and owns the
# scanout. If nothing tells it the console went away, it keeps master forever
# and ctrl-alt-F<n> switches the VT in the kernel with no visible effect. That
# is exactly what schema-init shipped with. We watch the active VT, emit
# PauseDevice/ResumeDevice, and drop/reacquire DRM master ourselves the way
# systemd-logind's session_device_stop()/start() do.
ACTIVE_VT_PATH = os.environ.get('SCHEMA_LOGIND_ACTIVE_VT', '/sys/class/tty/tty0/active')
VT_POLL_MS = 250
DRM_MAJOR = 226
VT_ACTIVATE = 0x5606
VT_SETMODE = 0x5602
VT_RELDISP = 0x5605
VT_AUTO = 0
VT_PROCESS = 1
VT_ACKACQ = 2
VT_RELEASE_ACK_MS = 500
DRM_IOCTL_SET_MASTER = 0x641E
DRM_IOCTL_DROP_MASTER = 0x641F
SESSION_IFACE = 'org.freedesktop.login1.Session'
# The single-session stub's identity. Hardcoded in seven places before this;
# collecting it here is the first step toward a real multi-session table.
SESSION_ID = '31'
SESSION_PATH = '/org/freedesktop/login1/session/_' + SESSION_ID


def read_active_vt():
    """Current active VT number ('tty3' -> 3), or 0 when it can't be read."""
    val = read_file_line(ACTIVE_VT_PATH)
    if val.startswith('tty'):
        val = val[3:]
    try:
        return int(val)
    except ValueError:
        return 0


def vt_activate(vtnr):
    """Ask the kernel to switch to VT `vtnr`. Returns True on success."""
    try:
        fd = os.open('/dev/tty0', os.O_RDWR | os.O_NOCTTY)
    except Exception as e:
        print(f"login1-stub: VT_ACTIVATE open failed: {e}", file=sys.stderr)
        return False
    try:
        fcntl.ioctl(fd, VT_ACTIVATE, vtnr)
        return True
    except Exception as e:
        print(f"login1-stub: VT_ACTIVATE({vtnr}) failed: {e}", file=sys.stderr)
        return False
    finally:
        os.close(fd)


class Login1Session(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, SESSION_PATH)
        self.devices = {}
        env_vt = os.environ.get('SCHEMA_LOGIND_VTNR', '')
        # The session's VT is captured at TakeControl() time — that is when the
        # compositor announces itself, and it is the only trusted moment we
        # have. Deriving it from /proc/<pid>/environ would be forgeable (see
        # get_active_uid).
        self.vtnr = int(env_vt) if env_vt.isdigit() else None
        self.active = True
        self.vt_fd = None
        self.pending_acks = set()
        self.release_timer = None
        self.locked_hint = False
        self.idle_hint = False
        self.idle_since = 0
        print("login1-stub: Registered Session at " + SESSION_PATH)

    # -- session-device handoff ---------------------------------------------

    @dbus.service.signal(SESSION_IFACE, signature='uus')
    def PauseDevice(self, major, minor, kind):
        pass

    @dbus.service.signal(SESSION_IFACE, signature='uuh')
    def ResumeDevice(self, major, minor, fd):
        pass

    @dbus.service.signal('org.freedesktop.DBus.Properties', signature='sa{sv}as')
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def _drm_master(self, major, fd, acquire):
        """Drop or reacquire DRM master on a taken device. No-op for non-DRM."""
        if major != DRM_MAJOR:
            return
        req = DRM_IOCTL_SET_MASTER if acquire else DRM_IOCTL_DROP_MASTER
        try:
            fcntl.ioctl(fd, req, 0)
            print(f"login1-stub: {'SET' if acquire else 'DROP'}_MASTER ok on fd={fd}")
        except Exception as e:
            print(f"login1-stub: {'SET' if acquire else 'DROP'}_MASTER failed on "
                  f"fd={fd}: {e}", file=sys.stderr)

    def _set_active(self, active):
        if self.active == active:
            return
        self.active = active
        self.PropertiesChanged(SESSION_IFACE, {
            'Active': dbus.Boolean(active),
            'State': dbus.String('active' if active else 'online'),
        }, [])

    def on_vt_changed(self, new_vt):
        """Active VT moved. Pause or resume every device this session took."""
        if self.vtnr is None or new_vt == 0:
            return
        should_be_active = (new_vt == self.vtnr)
        if should_be_active == self.active:
            return

        if not should_be_active:
            print(f"login1-stub: VT {new_vt} != session VT {self.vtnr} — pausing "
                  f"{len(self.devices)} device(s)")
            self._set_active(False)
            for (major, minor), fd in self.devices.items():
                self._drm_master(major, fd, acquire=False)
                self.PauseDevice(dbus.UInt32(major), dbus.UInt32(minor), 'gone')
        else:
            print(f"login1-stub: VT {new_vt} == session VT {self.vtnr} — resuming "
                  f"{len(self.devices)} device(s)")
            for (major, minor), fd in self.devices.items():
                self._drm_master(major, fd, acquire=True)
                self.ResumeDevice(dbus.UInt32(major), dbus.UInt32(minor),
                                  dbus.types.UnixFd(fd))
            self._set_active(True)

    def poll_active_vt(self):
        # Only a fallback for when VT_PROCESS could not be set up. While a
        # release is pending the sysfs VT has not moved yet, so polling would
        # see "still my VT", decide the session is active and resume devices in
        # the middle of handing them over.
        if self.vt_fd is not None:
            return True
        try:
            self.on_vt_changed(read_active_vt())
        except Exception as e:
            print(f"login1-stub: VT poll error: {e}", file=sys.stderr)
        return True

    # -- VT_PROCESS mediation -----------------------------------------------
    #
    # Watching the active VT and cleaning up afterwards is too late: the kernel
    # completes the switch synchronously, and fbcon's mode restore runs during
    # that switch while the compositor still holds DRM master. It fails silently
    # and is never retried, so the console never repaints. VT_PROCESS makes the
    # kernel ask first -- it signals us and waits for VT_RELDISP -- which is the
    # only way to drop master BEFORE the switch instead of after it.

    def _setup_vt_mediation(self):
        if self.vtnr is None or self.vt_fd is not None:
            return
        try:
            fd = os.open(f'/dev/tty{self.vtnr}', os.O_RDWR | os.O_NOCTTY)
        except Exception as e:
            print(f"login1-stub: VT mediation open failed: {e}", file=sys.stderr)
            return
        mode = struct.pack('bbhhh', VT_PROCESS, 0, signal.SIGUSR1, signal.SIGUSR2, 0)
        try:
            fcntl.ioctl(fd, VT_SETMODE, mode)
        except Exception as e:
            os.close(fd)
            print(f"login1-stub: VT_SETMODE(VT_PROCESS) failed: {e}", file=sys.stderr)
            return
        self.vt_fd = fd
        self._disarm_vt_flow_control(fd)
        GLib.unix_signal_add(GLib.PRIORITY_HIGH, signal.SIGUSR1, self._on_vt_release)
        GLib.unix_signal_add(GLib.PRIORITY_HIGH, signal.SIGUSR2, self._on_vt_acquire)
        print(f"login1-stub: VT {self.vtnr} now mediated (VT_PROCESS)")

    def _disarm_vt_flow_control(self, fd):
        # The session VT stays in KD_TEXT with a live line discipline
        # (KDGKBMODE=K_UNICODE), so every keystroke the compositor consumes is
        # ALSO fed to the console's line discipline. With IXON on, one stray ^S
        # sets tty->flow.stopped, do_con_write() then accepts 0 bytes forever,
        # and every later write to /dev/console sleeps in n_tty_write() with
        # MAX_SCHEDULE_TIMEOUT -- no timeout, no signal. On 2026-07-26 that
        # caught PID 1's own shutdown log and hung the box twice.
        #
        # Deliberately NOT KDSKBMODE=K_OFF, which is what systemd-logind does
        # here. K_OFF would also stop the kernel from handling ctrl-alt-F<n>,
        # and that kernel-level switch is the limp-mode escape hatch for a
        # wedged compositor -- the whole point of the recovery console.
        try:
            termios.tcflow(fd, termios.TCOON)          # clear any existing stop
            attrs = termios.tcgetattr(fd)
            attrs[0] &= ~(termios.IXON | termios.IXANY)
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            print(f"login1-stub: VT {self.vtnr} flow control disarmed (IXON off)")
        except Exception as e:
            print(f"login1-stub: disarming VT {self.vtnr} flow control failed: {e}",
                  file=sys.stderr)

    def _teardown_vt_mediation(self):
        if self.vt_fd is None:
            return
        try:
            fcntl.ioctl(self.vt_fd, VT_SETMODE,
                        struct.pack('bbhhh', VT_AUTO, 0, 0, 0, 0))
        except Exception as e:
            print(f"login1-stub: restoring VT_AUTO failed: {e}", file=sys.stderr)
        try:
            os.close(self.vt_fd)
        except Exception:
            pass
        self.vt_fd = None
        print("login1-stub: VT mediation released (VT_AUTO restored)")

    def _reldisp(self, arg):
        if self.vt_fd is None:
            return
        if self.release_timer is not None:
            GLib.source_remove(self.release_timer)
            self.release_timer = None
        self.pending_acks.clear()
        try:
            fcntl.ioctl(self.vt_fd, VT_RELDISP, arg)
            print(f"login1-stub: VT_RELDISP({arg}) — switch allowed to proceed")
        except Exception as e:
            print(f"login1-stub: VT_RELDISP({arg}) failed: {e}", file=sys.stderr)

    def _release_ack_timeout(self):
        self.release_timer = None
        print(f"login1-stub: {len(self.pending_acks)} device ack(s) missing — "
              f"releasing anyway", file=sys.stderr)
        self._reldisp(1)
        return False

    def _on_vt_release(self, *_):
        # The chord can arrive twice: the kernel's own VT handler signals us,
        # and the compositor independently calls Seat.SwitchTo for the same
        # keypress. Re-running the pause would overwrite the pending ack timer
        # (leaking it) and re-drop an already-dropped master.
        if self.release_timer is not None or not self.active:
            print("login1-stub: VT release already in flight — ignoring duplicate")
            return True
        print(f"login1-stub: VT release requested — pausing {len(self.devices)} device(s)")
        # Anything that escapes here would leave the kernel waiting forever for a
        # VT_RELDISP that never comes, with DRM master already dropped: a black
        # screen with no way back. Releasing the VT always wins over reporting.
        try:
            self._set_active(False)
            self.pending_acks = set(self.devices.keys())
            for (major, minor), fd in self.devices.items():
                self._drm_master(major, fd, acquire=False)
                self.PauseDevice(dbus.UInt32(major), dbus.UInt32(minor), 'pause')
        except Exception as e:
            print(f"login1-stub: pause failed ({e}) — releasing VT anyway",
                  file=sys.stderr)
            self._reldisp(1)
            return True
        if self.pending_acks:
            self.release_timer = GLib.timeout_add(VT_RELEASE_ACK_MS,
                                                  self._release_ack_timeout)
        else:
            self._reldisp(1)
        return True

    def _on_vt_acquire(self, *_):
        print(f"login1-stub: VT acquired — resuming {len(self.devices)} device(s)")
        self._reldisp(VT_ACKACQ)
        for (major, minor), fd in self.devices.items():
            self._drm_master(major, fd, acquire=True)
            self.ResumeDevice(dbus.UInt32(major), dbus.UInt32(minor),
                              dbus.types.UnixFd(fd))
        self._set_active(True)
        return True

    @dbus.service.method(SESSION_IFACE, in_signature='', out_signature='')
    def Activate(self):
        print(f"login1-stub: Session.Activate() -> VT {self.vtnr}")
        if self.vtnr:
            vt_activate(self.vtnr)

    def _emit_bare_signal(self, member):
        """Emit Lock/Unlock on login1.Session.

        These exist as BOTH a method and a signal on the real interface. The
        method is the request ("please lock"); the signal is what the session's
        own screen locker subscribes to and acts on. dbus-python derives the
        D-Bus member name from the Python function __name__, so a class cannot
        carry both under one name — send the signal message directly instead.
        Without this, Lock() printed a line and nothing ever locked."""
        msg = dbus.lowlevel.SignalMessage(SESSION_PATH, SESSION_IFACE, member)
        self._connection.send_message(msg)

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='', out_signature='')
    def Lock(self):
        print("login1-stub: Session.Lock() requested")
        self._emit_bare_signal('Lock')

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='', out_signature='')
    def Unlock(self):
        print("login1-stub: Session.Unlock() requested")
        self._emit_bare_signal('Unlock')

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='b', out_signature='')
    def SetLockedHint(self, locked):
        # The screen locker reports its state here; everything that asks "is the
        # screen locked" reads the resulting LockedHint property.
        locked = bool(locked)
        if locked == self.locked_hint:
            return
        self.locked_hint = locked
        print(f"login1-stub: Session.SetLockedHint({locked})")
        self.PropertiesChanged(SESSION_IFACE,
                               {'LockedHint': dbus.Boolean(locked)}, [])

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='b', out_signature='')
    def SetIdleHint(self, idle):
        idle = bool(idle)
        if idle == self.idle_hint:
            return
        self.idle_hint = idle
        self.idle_since = int(time.time() * 1000000) if idle else 0
        print(f"login1-stub: Session.SetIdleHint({idle})")
        self.PropertiesChanged(SESSION_IFACE, {
            'IdleHint': dbus.Boolean(idle),
            'IdleSinceHint': dbus.UInt64(self.idle_since),
        }, [])

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
        if self.vtnr is None:
            self.vtnr = read_active_vt() or None
            print(f"login1-stub: session VT captured as {self.vtnr}")
        self._setup_vt_mediation()

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='', out_signature='')
    def ReleaseControl(self):
        print("login1-stub: Session.ReleaseControl() requested")
        self._teardown_vt_mediation()

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
        self.pending_acks.discard((major, minor))
        if not self.pending_acks and self.release_timer is not None:
            self._reldisp(1)

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
        if interface_name == 'org.freedesktop.login1.Session' or not interface_name:
            uid = get_active_uid()
            username = get_username_for_uid(uid)
            vtnr = self.vtnr if self.vtnr is not None else read_active_vt()
            leader_pid, leader_comm = get_session_leader()
            started, started_mono = proc_start_usec(leader_pid) if leader_pid else (0, 0)
            return {
                'Id': dbus.String('31'),
                'User': dbus.Struct((dbus.UInt32(uid), dbus.ObjectPath(f'/org/freedesktop/login1/user/_{uid}')), signature='uo'),
                'Name': dbus.String(username),
                'Active': dbus.Boolean(self.active),
                'State': dbus.String('active' if self.active else 'online'),
                'VTNr': dbus.UInt32(vtnr),
                'Remote': dbus.Boolean(False),
                'Type': dbus.String(get_session_type()),
                'Class': dbus.String('user'),
                'Seat': dbus.Struct((dbus.String('seat0'), dbus.ObjectPath('/org/freedesktop/login1/seat/seat0')), signature='so'),
                'CanReboot': dbus.String('yes'),
                'CanPowerOff': dbus.String('yes'),
                'Leader': dbus.UInt32(leader_pid),
                'TTY': dbus.String('tty%d' % vtnr if vtnr else ''),
                'Display': dbus.String(''),
                'Desktop': dbus.String(get_desktop_name()),
                'Service': dbus.String(leader_comm.split('-', 1)[0] if leader_comm else ''),
                'Scope': dbus.String('session-31.scope'),
                'IdleHint': dbus.Boolean(self.idle_hint),
                'IdleSinceHint': dbus.UInt64(self.idle_since),
                'IdleSinceHintMonotonic': dbus.UInt64(0),
                'LockedHint': dbus.Boolean(self.locked_hint),
                'Timestamp': dbus.UInt64(started),
                'TimestampMonotonic': dbus.UInt64(started_mono),
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
        if interface_name == 'org.freedesktop.login1.User' or not interface_name:
            uid = getattr(self, 'uid', 1000)
            username = get_username_for_uid(uid)
            gid = get_gid_for_uid(uid)
            return {
                'UID': dbus.UInt32(uid),
                'GID': dbus.UInt32(gid),
                'Name': dbus.String(username),
                'Display': dbus.ObjectPath(SESSION_PATH),
                'State': dbus.String('active'),
            }
        return {}

class Login1Seat(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/login1/seat/seat0')
        print("login1-stub: Registered Seat at /org/freedesktop/login1/seat/seat0")

    @dbus.service.method('org.freedesktop.login1.Seat', in_signature='u', out_signature='',
                         sender_keyword='sender')
    def SwitchTo(self, vtnr, sender=None):
        if not caller_authorized(self._connection, sender):
            print(f"login1-stub: DENY SwitchTo({vtnr}) from "
                  f"uid={caller_uid(self._connection, sender)}", file=sys.stderr)
            raise dbus.exceptions.DBusException(
                "Not the active session owner",
                name='org.freedesktop.login1.AccessDenied')
        print(f"login1-stub: Seat.SwitchTo({vtnr}) requested")
        if not vt_activate(int(vtnr)):
            raise dbus.exceptions.DBusException(
                f"VT_ACTIVATE({vtnr}) failed",
                name='org.freedesktop.login1.FailedToSwitch')

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
        if interface_name == 'org.freedesktop.login1.Seat' or not interface_name:
            return {
                'Id': dbus.String('seat0'),
                'ActiveSession': dbus.Struct((dbus.String('31'), dbus.ObjectPath(SESSION_PATH)), signature='so'),
                'CanMultiSession': dbus.Boolean(True),
                'CanTTY': dbus.Boolean(True),
                'CanGraphical': dbus.Boolean(True),
            }
        return {}

class Login1Manager(dbus.service.Object):
    def __init__(self, bus, session=None):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/login1')
        self.session = session
        print("login1-stub: Registered Manager at /org/freedesktop/login1")

    # loginctl lock-session/unlock-session route through the Manager, not the
    # Session object. Before these existed dbus-python raised UnknownMethod as
    # an unhandled traceback, so `loginctl lock-session` failed outright.
    def _relay_lock(self, member):
        if self.session is None:
            raise dbus.exceptions.DBusException(
                "No session", name='org.freedesktop.login1.NoSuchSession')
        getattr(self.session, member)()

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='')
    def LockSession(self, session_id):
        self._relay_lock('Lock')

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='')
    def UnlockSession(self, session_id):
        self._relay_lock('Unlock')

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='')
    def LockSessions(self):
        self._relay_lock('Lock')

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='')
    def UnlockSessions(self):
        self._relay_lock('Unlock')

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
        return [dbus.Struct((dbus.String('31'), dbus.UInt32(uid), dbus.String(username), dbus.String('seat0'), dbus.ObjectPath(SESSION_PATH)), signature='susso')]

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
        return dbus.ObjectPath(SESSION_PATH)

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='o')
    def GetSession(self, session_id):
        return dbus.ObjectPath(SESSION_PATH)

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
        if interface_name == 'org.freedesktop.login1.Manager' or not interface_name:
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
        if interface_name == 'org.freedesktop.hostname1' or not interface_name:
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
        if interface_name == 'org.freedesktop.ConsoleKit.Session' or not interface_name:
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
        if interface_name == 'org.freedesktop.timedate1' or not interface_name:
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
    # PID 1 redirects our stdout to a logfile, which makes it block-buffered:
    # diagnostics sit in an 8K buffer and are invisible until the process exits.
    sys.stdout.reconfigure(line_buffering=True)
    sys.stderr.reconfigure(line_buffering=True)
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
    manager = Login1Manager(bus, session)
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

    GLib.timeout_add(VT_POLL_MS, session.poll_active_vt)
    print(f"login1-stub: watching {ACTIVE_VT_PATH} every {VT_POLL_MS}ms "
          f"(active VT {read_active_vt()})")

    loop = GLib.MainLoop()

    def shutdown_handler(sig, frame):
        print(f"login1-stub: Received signal {sig}, shutting down...")
        loop.quit()

    signal.signal(signal.SIGINT, shutdown_handler)
    signal.signal(signal.SIGTERM, shutdown_handler)

    loop.run()

if __name__ == '__main__':
    main()
