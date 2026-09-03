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
import re
import stat
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

def _valid_hostname(name):
    if len(name) > 64:
        return False
    return all(c.isalnum() or c in '-._' for c in name)

def _update_machine_info(key, value):
    if value and any(c in value for c in '\r\n\0'):
        raise dbus.exceptions.DBusException(
            'Invalid value', name='org.freedesktop.DBus.Error.InvalidArgs')
    lines = []
    try:
        with open('/etc/machine-info', 'r') as f:
            lines = [l for l in f if not l.startswith(key + '=')]
    except FileNotFoundError:
        pass
    if value:
        lines.append('%s=%s\n' % (key, value))
    tmp = '/etc/machine-info.new'
    with open(tmp, 'w') as f:
        f.writelines(lines)
    os.replace(tmp, '/etc/machine-info')

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

def proc_starttime_ticks(pid):
    """Field 22 of /proc/<pid>/stat: process start time in clock ticks since
    boot. Unique to a process incarnation, so it distinguishes a live leader
    from a recycled pid. Returns None if the stat cannot be read. comm (field
    2) may contain spaces/parens, so split on the last ') '."""
    try:
        with open('/proc/%d/stat' % pid) as f:
            return int(f.read().rsplit(') ', 1)[1].split()[19])
    except (OSError, IndexError, ValueError):
        return None

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
SEAT_IFACE = 'org.freedesktop.login1.Seat'
MANAGER_IFACE = 'org.freedesktop.login1.Manager'
# Overridable for the same reason SCHEMA_LOGIND_ACTIVE_VT is: a test must be
# able to point the bridge at a temp tree. This one matters more than the VT
# path — the registry writes the seat/user files and sweeps stale session
# files, so a test run against the real /run/systemd would edit live state.
_RUN_SYSTEMD = os.environ.get('SCHEMA_LOGIND_RUN_DIR', '/run/systemd')
SESSIONS_DIR = _RUN_SYSTEMD + '/sessions'
SEATS_DIR = _RUN_SYSTEMD + '/seats'
USERS_DIR = _RUN_SYSTEMD + '/users'
# localed backing files, systemd-compatible paths so KDE/GNOME panels and
# desktop tooling read/write the same locations. Overridable for tests, same
# reason as SCHEMA_LOGIND_RUN_DIR.
_ETC = os.environ.get('SCHEMA_LOGIND_ETC', '/etc')
LOCALE_CONF = _ETC + '/locale.conf'
VCONSOLE_CONF = _ETC + '/vconsole.conf'
X11_KEYMAP_CONF = _ETC + '/X11/xorg.conf.d/00-keyboard.conf'
# Same override, same reason: CreateSession() creates session scope cgroups and
# ReleaseSession() rmdirs them, so a test must be able to aim that at a temp
# tree rather than the real hierarchy.
CGROUP_ROOT = os.environ.get('SCHEMA_CGROUP_ROOT', '/sys/fs/cgroup')
# The id the stub served when it could only serve one session. Still used as
# the synthesised id when /run/systemd/sessions/ is empty, which is what a
# login script that predates id allocation leaves behind.
LEGACY_SESSION_ID = '31'
SESSION_REGISTER = os.environ.get(
    'SCHEMA_SESSION_REGISTER',
    os.path.join(os.path.dirname(os.path.abspath(__file__)), 'schema-session-register'))
SESSION_UNREGISTER = os.environ.get(
    'SCHEMA_SESSION_UNREGISTER',
    os.path.join(os.path.dirname(os.path.abspath(__file__)), 'schema-session-unregister'))
# uaccess re-scan: coldplug tags and ACLs devices before any user is active, so
# a device present at boot is only granted to whoever was active then (nobody).
# Real systemd's logind (logind-acl.c devnode_acl_all) re-applies the ACL when a
# seat's active session changes; schema-udev only tags at device-add, so logind
# owns the login-time re-application. Both dirs are overridable so a test can aim
# the scan at a temp tree instead of the live /dev and udev database.
UDEV_DATA_DIR = os.environ.get('SCHEMA_LOGIND_UDEV_DATA', '/run/udev/data')
DEV_DIR = os.environ.get('SCHEMA_LOGIND_DEV_DIR', '/dev')
_UDEV_DB_RE = re.compile(r'([cb])(\d+):(\d+)')


def _rebase_devname(devname):
    """An absolute E:DEVNAME (/dev/...) resolved under DEV_DIR for test trees."""
    if devname.startswith('/dev/'):
        return os.path.join(DEV_DIR, devname[len('/dev/'):])
    return os.path.join(DEV_DIR, devname.lstrip('/'))


def _dev_rdev_map():
    """(is_block, major, minor) -> real device-node path under DEV_DIR."""
    m = {}
    for dp, _dirs, files in os.walk(DEV_DIR):
        for n in files:
            p = os.path.join(dp, n)
            try:
                st = os.lstat(p)
            except OSError:
                continue
            if stat.S_ISCHR(st.st_mode) or stat.S_ISBLK(st.st_mode):
                key = (stat.S_ISBLK(st.st_mode),
                       os.major(st.st_rdev), os.minor(st.st_rdev))
                m.setdefault(key, p)
    return m


def uaccess_seat_nodes(seat_id='seat0'):
    """/dev nodes tagged uaccess that belong to `seat_id`, per the udev db.

    A record belongs to the seat when its E:ID_SEAT equals seat_id, or it has no
    ID_SEAT and seat_id is seat0 (the udev/systemd default). Nodes are resolved
    by N:/E:DEVNAME when present, else by major:minor -> st_rdev — real records
    on this fleet carry neither name field, so the rdev map is the live path.
    """
    try:
        entries = os.listdir(UDEV_DATA_DIR)
    except OSError:
        return []
    rdev = None
    out = []
    for name in entries:
        mm = _UDEV_DB_RE.fullmatch(name)
        if not mm:
            continue
        try:
            body = open(os.path.join(UDEV_DATA_DIR, name)).read().splitlines()
        except OSError:
            continue
        if not any(ln == 'Q:uaccess' or ln == 'G:uaccess' for ln in body):
            continue
        id_seat = 'seat0'
        path = None
        for ln in body:
            if ln.startswith('E:ID_SEAT='):
                id_seat = ln[len('E:ID_SEAT='):] or 'seat0'
            elif ln.startswith('E:DEVNAME='):
                path = _rebase_devname(ln[len('E:DEVNAME='):])
            elif ln.startswith('N:'):
                path = os.path.join(DEV_DIR, ln[2:])
        if id_seat != seat_id:
            continue
        if path is None:
            if rdev is None:
                rdev = _dev_rdev_map()
            path = rdev.get((mm.group(1) == 'b',
                             int(mm.group(2)), int(mm.group(3))))
        if path and os.path.exists(path):
            out.append(path)
    return out


def apply_uaccess_acl(nodes, new_uid, old_uid=None):
    """Grant the active user rw on each node and revoke the previous user's —
    the setfacl half of devnode_acl_all. Non-fatal per node: a device that
    vanished mid-scan must not abort the rest."""
    for n in nodes:
        subprocess.run(['setfacl', '-m', 'u:%d:rw' % new_uid, n],
                       stderr=subprocess.DEVNULL)
        if old_uid is not None and old_uid != new_uid:
            subprocess.run(['setfacl', '-x', 'u:%d' % old_uid, n],
                           stderr=subprocess.DEVNULL)


def _uaccess_fingerprint(nodes):
    """A cheap identity of the live uaccess node set for change-detection:
    (path, inode, mtime_ns) per node that still exists. inode + mtime catch a
    device re-created in place (nvidia re-mknod); the path set catches nodes
    that appear or vanish. mtime, not ctime — setfacl bumps ctime, so keying on
    it would make every apply re-trigger the next cycle."""
    fp = []
    for n in nodes:
        try:
            st = os.stat(n)
        except OSError:
            continue
        fp.append((n, st.st_ino, st.st_mtime_ns))
    return tuple(sorted(fp))


def session_path_for(sid):
    return '/org/freedesktop/login1/session/_' + str(sid)


def user_path_for(uid):
    return '/org/freedesktop/login1/user/_%d' % int(uid)


def session_file_for(sid):
    return SESSIONS_DIR + '/' + str(sid)

# --- restart handoff --------------------------------------------------------
# TakeDevice() opens the DRM node and passes THAT fd to the compositor, so our
# fd and the compositor's are the same open file description -- which is the
# only reason DROP_MASTER on our side takes master away from theirs. A plain
# kill+respawn loses those fds, and a fresh open() is a different description
# that cannot drop anyone's master. The bridge then re-arms VT_PROCESS, fails
# to drop master on ctrl-alt-F<n>, and hands the VT over anyway: a black tty.
#
# So on SIGHUP we execv() OURSELVES instead of dying. exec preserves the pid
# (the kernel records the VT_PROCESS owner by pid, so mediation survives) and
# preserves every fd we mark inheritable. The device map travels through the
# environment. Upgrading the bridge = `kill -HUP`, never `restart`.
HANDOFF_ENV = 'SCHEMA_LOGIND_HANDOFF'
TTY_MAJOR = 4
SCRIPT_PATH = os.path.abspath(__file__)


def read_file_value(path, key):
    """One KEY=value out of a /run/systemd/sessions/<id> file, or ''."""
    try:
        with open(path, 'r') as f:
            for line in f:
                if line.startswith(key + '='):
                    return line.split('=', 1)[1].strip()
    except Exception:
        pass
    return ""


def read_state_file(path):
    """Whole KEY=value file as a dict. Comment lines and junk are skipped."""
    out = {}
    try:
        with open(path, 'r') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#') or '=' not in line:
                    continue
                k, v = line.split('=', 1)
                out[k.strip()] = v.strip()
    except Exception:
        pass
    return out


def _int_or(val, default):
    try:
        return int(val)
    except (TypeError, ValueError):
        return default


class SessionRecord:
    """One parsed /run/systemd/sessions/<id>.

    Any field the login script did not write falls back to runtime detection,
    because the two files deploy independently: a bridge running against an
    un-updated sddm-logged still has to produce a complete session. Missing
    keys are the normal case during a partial rollout, not an error.

    Those fallbacks are LAZY and cached. get_session_leader() and
    get_desktop_name() each walk all of /proc, and this object is rebuilt on
    every scan — resolving them eagerly would put two full /proc scans per
    session on a 4 Hz timer, which is the shape of both prior CPU-spin
    incidents. Nothing here touches /proc until D-Bus actually asks for a
    property that needs it.
    """

    def __init__(self, sid, data=None, synthesised=False):
        self.sid = str(sid)
        self.data = data or {}
        self.synthesised = synthesised
        self._cache = {}

    # -- cheap: straight off the file, safe to touch at poll cadence ---------

    @property
    def uid(self):
        if self.data.get('UID'):
            return _int_or(self.data['UID'], 0)
        return self._lazy('uid', get_active_uid)

    @property
    def seat(self):
        return self.data.get('SEAT') or 'seat0'

    @property
    def vtnr(self):
        return _int_or(self.data.get('VTNR'), 0)

    @property
    def klass(self):
        return self.data.get('CLASS') or 'user'

    @property
    def state(self):
        if self.data.get('STATE'):
            return self.data['STATE']
        # A synthesised record stands in for a session the legacy login script
        # created and never described. It is by definition the live one — the
        # single-session bridge hardcoded active=True for exactly that reason,
        # and starting it 'online' would leave on_vt_changed() with nothing to
        # transition away from, so the first PauseDevice never fires.
        return 'active' if self.synthesised else 'online'

    @property
    def active(self):
        if 'ACTIVE' in self.data:
            return self.data['ACTIVE'] == '1'
        return self.state == 'active'

    @property
    def remote(self):
        return self.data.get('REMOTE', '0') == '1'

    @property
    def is_display(self):
        return self.data.get('IS_DISPLAY', '0') == '1'

    # -- lazy: only resolved when something actually reads them --------------

    def _lazy(self, key, fn):
        if key not in self._cache:
            self._cache[key] = fn()
        return self._cache[key]

    @property
    def user(self):
        return self.data.get('USER') \
            or self._lazy('user', lambda: get_username_for_uid(self.uid))

    @property
    def type(self):
        return self.data.get('TYPE') or self._lazy('type', get_session_type)

    @property
    def desktop(self):
        return self.data.get('DESKTOP') or self._lazy('desktop', get_desktop_name)

    def _resolve_leader(self):
        pid = _int_or(self.data.get('LEADER'), 0)
        if pid:
            return pid, ''
        return get_session_leader()

    @property
    def leader(self):
        return self._lazy('leader_pair', self._resolve_leader)[0]

    @property
    def service(self):
        if self.data.get('SERVICE'):
            return self.data['SERVICE']
        comm = self._lazy('leader_pair', self._resolve_leader)[1]
        return comm.split('-', 1)[0] if comm else ''

    def _resolve_times(self):
        rt = _int_or(self.data.get('REALTIME'), 0)
        mono = _int_or(self.data.get('MONOTONIC'), 0)
        if rt:
            return rt, mono
        return proc_start_usec(self.leader) if self.leader else (0, 0)

    @property
    def realtime(self):
        return self._lazy('times', self._resolve_times)[0]

    @property
    def monotonic(self):
        return self._lazy('times', self._resolve_times)[1]

    # -- change detection ----------------------------------------------------

    # State-file key -> the D-Bus property whose value it backs. Used to emit
    # PropertiesChanged for only the keys that actually moved, rather than
    # invalidating the whole interface on every touch of the file.
    KEY_TO_PROP = {
        'UID': 'User', 'USER': 'Name', 'SEAT': 'Seat', 'VTNR': 'VTNr',
        'TYPE': 'Type', 'CLASS': 'Class', 'DESKTOP': 'Desktop',
        'STATE': 'State', 'ACTIVE': 'Active', 'REMOTE': 'Remote',
        'LEADER': 'Leader', 'SERVICE': 'Service',
    }

    def signature(self):
        """What a change looks like.

        Deliberately the raw file contents, not the resolved properties: this
        is compared on every scan, so it must not force a single lazy lookup.
        """
        return tuple(sorted(self.data.items()))

    def changed_properties(self, other):
        """D-Bus property names whose backing key differs from `other`."""
        props = []
        for key in set(self.data) | set(other.data):
            if self.data.get(key) != other.data.get(key):
                prop = self.KEY_TO_PROP.get(key)
                if prop and prop not in props:
                    props.append(prop)
        return props

    def leader_alive(self):
        pid = _int_or(self.data.get('LEADER'), 0)
        if not pid:
            return True     # nothing claimed a leader; not ours to declare dead
        if not os.path.isdir('/proc/%d' % pid):
            return False    # pid is gone outright
        stored = _int_or(self.data.get('LEADER_STARTTIME'), 0)
        if not stored:
            return True     # no baseline recorded (pre-upgrade session): prior behavior
        live = proc_starttime_ticks(pid)
        if live is None:
            return True     # can't read start-time — never false-reap
        return live == stored   # same incarnation; a mismatch means the pid was recycled


def scan_session_files():
    """All /run/systemd/sessions/<id> as {sid: SessionRecord}.

    Falls back to synthesising the legacy id when the directory is empty, so
    the bridge works unchanged against a login script that never learned to
    allocate one. Deployment order between the two files is therefore free.
    """
    records = {}
    try:
        names = os.listdir(SESSIONS_DIR)
    except OSError:
        names = []
    for name in names:
        # Real logind ids can be alphanumeric ('c1' for a greeter); ours are
        # integers. Accept both, reject dotfiles and rotation leftovers.
        if not name or not name.isalnum():
            continue
        data = read_state_file(session_file_for(name))
        if not data:
            # An id claimed but not yet described. schema-session-register
            # allocates by creating an EMPTY file (the shell's O_EXCL) and
            # only then renames the real content over it, so this is the
            # normal half-built moment of a login in progress — not a
            # session, and not something to put on the bus for 250 ms.
            continue
        records[name] = SessionRecord(name, data)

    if not records:
        records[LEGACY_SESSION_ID] = SessionRecord(LEGACY_SESSION_ID,
                                                   None, synthesised=True)
    return records




def vtnr_from_tty(tty):
    """The VT number a tty name refers to, or 0. 'tty3' -> 3, 'pts/2' -> 0."""
    if tty.startswith('tty') and tty[3:].isdigit():
        return int(tty[3:])
    return 0


def load_handoff():
    """Consume the handoff blob left by our pre-exec self, or None on cold start.

    Popped from the environment, never merely read: a stale blob inherited by a
    grandchild would name fds that mean something else entirely by then.
    """
    raw = os.environ.pop(HANDOFF_ENV, '')
    if not raw:
        return None
    try:
        return json.loads(raw)
    except Exception as e:
        print(f"login1-stub: unreadable {HANDOFF_ENV} ({e}) — cold start",
              file=sys.stderr)
        return None


def fd_matches_device(fd, major, minor):
    """True if `fd` is open on exactly the char device major:minor.

    An inherited fd number is a claim, not a fact. Everything downstream issues
    DRM and VT ioctls on these, so a handoff that lost a race and now points at
    a logfile must be rejected here rather than discovered by ioctl.
    """
    try:
        st = os.fstat(fd)
    except OSError:
        return False
    return os.major(st.st_rdev) == major and os.minor(st.st_rdev) == minor


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
    def __init__(self, bus, record, handoff=None):
        self.record = record
        self.sid = record.sid
        self.path = session_path_for(self.sid)
        dbus.service.Object.__init__(self, bus, self.path)
        self.devices = {}
        env_vt = os.environ.get('SCHEMA_LOGIND_VTNR', '')
        # The session's VT is captured at TakeControl() time — that is when the
        # compositor announces itself, and it is the only trusted moment we
        # have. Deriving it from /proc/<pid>/environ would be forgeable (see
        # get_active_uid). A VTNR in the state file is trusted, though: it is
        # root-written by the login path.
        if env_vt.isdigit():
            self.vtnr = int(env_vt)
        else:
            self.vtnr = record.vtnr or None
        self.active = record.active
        self.vt_fd = None
        self.pending_acks = set()
        self.release_timer = None
        self.locked_hint = False
        self.idle_hint = False
        self.idle_since = 0
        print(f"login1-stub: Registered Session {self.sid} at {self.path}"
              + (" (synthesised — no state file)" if record.synthesised else ""))
        if handoff:
            self._adopt_handoff(handoff)

    # -- restart handoff -----------------------------------------------------

    def _adopt_handoff(self, state):
        """Re-inherit a live session's fds and VT mediation across execv()."""
        self.vtnr = state.get('vtnr') or self.vtnr
        self.active = bool(state.get('active', True))
        self.locked_hint = bool(state.get('locked_hint', False))
        self.idle_hint = bool(state.get('idle_hint', False))
        self.idle_since = int(state.get('idle_since', 0))

        for key, fd in (state.get('devices') or {}).items():
            try:
                major, minor = (int(p) for p in key.split(':'))
            except ValueError:
                continue
            if fd_matches_device(fd, major, minor):
                os.set_inheritable(fd, False)
                self.devices[(major, minor)] = fd
            else:
                # Cleared but deliberately NOT closed: we just established that
                # this fd is not what the blob claimed, so we have no idea what
                # it actually is and closing it could take out stderr.
                try:
                    os.set_inheritable(fd, False)
                except OSError:
                    pass
                print(f"login1-stub: handoff fd={fd} is not {key} — dropped",
                      file=sys.stderr)

        vt_fd = state.get('vt_fd')
        if vt_fd is not None and self.vtnr is not None \
                and fd_matches_device(vt_fd, TTY_MAJOR, self.vtnr):
            os.set_inheritable(vt_fd, False)
            self.vt_fd = vt_fd
            # exec kept the pid, so the kernel still has us down as the VT owner
            # and mediation never lapsed. Re-issuing VT_SETMODE is belt-and-braces
            # and re-states the signal numbers; the signal handlers themselves did
            # NOT survive exec and must be re-armed or the next ctrl-alt-F<n>
            # would leave the kernel waiting for a VT_RELDISP nobody sends.
            self._arm_vt_signals()
            mode = struct.pack('bbhhh', VT_PROCESS, 0,
                               signal.SIGUSR1, signal.SIGUSR2, 0)
            try:
                fcntl.ioctl(self.vt_fd, VT_SETMODE, mode)
            except Exception as e:
                print(f"login1-stub: re-arming VT_SETMODE failed: {e}",
                      file=sys.stderr)
        elif vt_fd is not None:
            print(f"login1-stub: handoff vt_fd={vt_fd} is not tty{self.vtnr} — "
                  f"mediation NOT adopted", file=sys.stderr)

        print(f"login1-stub: adopted handoff — VT {self.vtnr}, "
              f"{len(self.devices)} device(s), mediation "
              f"{'live' if self.vt_fd is not None else 'LOST'}, active={self.active}")

    def reexec(self):
        """Replace ourselves with a fresh interpreter, keeping fds and pid."""
        if self.release_timer is not None or self.pending_acks:
            # Master is already dropped and the kernel is blocked waiting for
            # VT_RELDISP. Exec now and the new process has no pending release to
            # answer: the switch never completes and the screen stays dark.
            print("login1-stub: VT release in flight — deferring re-exec")
            GLib.timeout_add(VT_RELEASE_ACK_MS, self._reexec_retry)
            return

        state = {
            'sid': self.sid,
            'vtnr': self.vtnr,
            'active': self.active,
            'locked_hint': self.locked_hint,
            'idle_hint': self.idle_hint,
            'idle_since': self.idle_since,
            'devices': {f"{maj}:{minor}": fd
                        for (maj, minor), fd in self.devices.items()},
        }
        keep = list(self.devices.values())
        if self.vt_fd is not None:
            state['vt_fd'] = self.vt_fd
            keep.append(self.vt_fd)
        # TakeDevice() opens O_CLOEXEC and Python marks its fds non-inheritable
        # anyway (PEP 446), so without this every fd we are trying to save is
        # closed by the exec itself.
        for fd in keep:
            os.set_inheritable(fd, True)

        os.environ[HANDOFF_ENV] = json.dumps(state)
        # PID 1 execs us as bare "python3" with no PATH, so Python cannot
        # resolve its own binary and sys.executable is ''. /proc/self/exe is
        # the interpreter regardless of how argv[0] was spelled.
        interp = sys.executable or os.path.realpath('/proc/self/exe')
        print(f"login1-stub: re-exec {SCRIPT_PATH} via {interp} — handing off "
              f"{len(self.devices)} device(s) + VT {self.vtnr}")
        sys.stdout.flush()
        sys.stderr.flush()
        try:
            os.execv(interp, [interp, SCRIPT_PATH] + sys.argv[1:])
        except Exception as e:
            # execv only returns on failure, and we are still the intact old
            # process: undo the inheritable flags and carry on serving.
            print(f"login1-stub: re-exec FAILED ({e}) — continuing on old code",
                  file=sys.stderr)
            os.environ.pop(HANDOFF_ENV, None)
            for fd in keep:
                os.set_inheritable(fd, False)

    def _reexec_retry(self):
        self.reexec()
        return False

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
        self._write_back_active(active)
        self.PropertiesChanged(SESSION_IFACE, {
            'Active': dbus.Boolean(active),
            'State': dbus.String('active' if active else 'online'),
        }, [])

    def _write_back_active(self, active):
        """Keep ACTIVE=/STATE= in the state file agreeing with D-Bus.

        sd_session_is_active() parses the file, not the bus, and polkit is one
        of its callers — so leaving the file saying active while D-Bus says
        otherwise is not a cosmetic disagreement, it is two answers to the same
        question. Skipped for a synthesised session, which has no file.
        """
        if self.record.synthesised:
            return
        path = session_file_for(self.sid)
        data = read_state_file(path)
        if not data:
            return
        data['ACTIVE'] = '1' if active else '0'
        data['STATE'] = 'active' if active else 'online'
        body = ['# This is private data. Do not parse.']
        body += ['%s=%s' % (k, v) for k, v in data.items()]
        tmp = path + '.tmp'
        try:
            with open(tmp, 'w') as f:
                f.write('\n'.join(body) + '\n')
            os.rename(tmp, path)
            self.record.data = data
        except OSError as e:
            print(f"login1-stub: cannot update {path}: {e}", file=sys.stderr)
            try:
                os.unlink(tmp)
            except OSError:
                pass

    def shutdown(self):
        """Session is gone: drop mediation, close taken devices, leave the bus."""
        try:
            self._teardown_vt_mediation()
        except Exception:
            pass
        for fd in list(self.devices.values()):
            try:
                os.close(fd)
            except OSError:
                pass
        self.devices.clear()
        try:
            self.remove_from_connection()
        except Exception:
            pass

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
        self._arm_vt_signals()
        print(f"login1-stub: VT {self.vtnr} now mediated (VT_PROCESS)")

    def _arm_vt_signals(self):
        GLib.unix_signal_add(GLib.PRIORITY_HIGH, signal.SIGUSR1, self._on_vt_release)
        GLib.unix_signal_add(GLib.PRIORITY_HIGH, signal.SIGUSR2, self._on_vt_acquire)

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
        msg = dbus.lowlevel.SignalMessage(self.path, SESSION_IFACE, member)
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

    # Compat: KDE/loginctl call these on logout / session setup. schema-init
    # owns session lifecycle, so Terminate is a logged no-op (never tear down
    # the live session answering the call); SetType just records intent.
    @dbus.service.method('org.freedesktop.login1.Session', in_signature='', out_signature='')
    def Terminate(self):
        print("login1-stub: Session.Terminate() (no-op; not managed here)")

    @dbus.service.method('org.freedesktop.login1.Session', in_signature='s', out_signature='')
    def SetType(self, type_):
        print(f"login1-stub: Session.SetType({type_}) (no-op)")

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
            rec = self.record
            uid = rec.uid
            vtnr = self.vtnr if self.vtnr is not None else rec.vtnr
            seat = rec.seat
            return {
                'Id': dbus.String(self.sid),
                'User': dbus.Struct((dbus.UInt32(uid), dbus.ObjectPath(user_path_for(uid))), signature='uo'),
                'Name': dbus.String(rec.user),
                'Active': dbus.Boolean(self.active),
                'State': dbus.String('active' if self.active else 'online'),
                'VTNr': dbus.UInt32(vtnr or 0),
                'Remote': dbus.Boolean(rec.remote),
                'Type': dbus.String(rec.type),
                'Class': dbus.String(rec.klass),
                'Seat': dbus.Struct((dbus.String(seat), dbus.ObjectPath('/org/freedesktop/login1/seat/' + seat)), signature='so'),
                'CanReboot': dbus.String('yes'),
                'CanPowerOff': dbus.String('yes'),
                'Leader': dbus.UInt32(rec.leader),
                'TTY': dbus.String('tty%d' % vtnr if vtnr else ''),
                'Display': dbus.String(''),
                'Desktop': dbus.String(rec.desktop),
                'Service': dbus.String(rec.service),
                'Scope': dbus.String('session-%s.scope' % self.sid),
                'IdleHint': dbus.Boolean(self.idle_hint),
                'IdleSinceHint': dbus.UInt64(self.idle_since),
                'IdleSinceHintMonotonic': dbus.UInt64(0),
                'LockedHint': dbus.Boolean(self.locked_hint),
                'Timestamp': dbus.UInt64(rec.realtime),
                'TimestampMonotonic': dbus.UInt64(rec.monotonic),
            }
        return {}

class SessionRegistry:
    """Projects /run/systemd/sessions/ onto the login1 D-Bus surface.

    The login path owns the directory; this owns the bus objects and the
    derived seat/user files. Neither side hardcodes an id.

    Watch mechanism is the existing 250 ms VT poll, deliberately: an added
    Gio.FileMonitor or raw inotify fd would be a new long-lived descriptor in
    a GLib loop, and that is the exact shape of both prior CPU-spin incidents
    (the pidfd leak and the half-open peer wedge). A readdir of a directory
    holding single-digit entries at 4 Hz costs nothing, and SessionRecord
    keeps every /proc-walking fallback lazy so a scan stays pure file I/O.
    """

    def __init__(self, bus):
        self.bus = bus
        self.sessions = {}      # sid -> Login1Session
        self.users = {}         # uid -> Login1User
        self.seat = None        # Login1Seat, attached by main()
        self.manager = None     # Login1Manager, attached by main()
        self._sigs = {}         # sid -> record signature at last sync
        self._derived = {}      # path -> last content written
        self._last_vt = None
        self._seat_active_uid = {}   # seat_id -> uid last granted uaccess ACLs
        self._seat_uaccess_fp = {}   # seat_id -> fingerprint of nodes last granted

    # -- lookup --------------------------------------------------------------

    def get(self, sid):
        return self.sessions.get(str(sid))

    def primary(self):
        """The session an unqualified request means: the active one, else any.

        Callers like `loginctl lock-session` with no argument used to reach the
        only session there was; this keeps that working without pretending the
        choice is meaningful on a multi-session box.
        """
        for obj in self.sessions.values():
            if obj.active:
                return obj
        return next(iter(self.sessions.values()), None)

    def uids(self):
        seen = []
        for obj in self.sessions.values():
            uid = obj.record.uid
            if uid not in seen:
                seen.append(uid)
        return seen

    def seats(self):
        seen = []
        for obj in self.sessions.values():
            if obj.record.seat not in seen:
                seen.append(obj.record.seat)
        return seen or ['seat0']

    def sessions_for_seat(self, seat_id):
        return [(o.sid, o.path) for o in self.sessions.values()
                if o.record.seat == seat_id]

    def active_session_for_seat(self, seat_id):
        for obj in self.sessions.values():
            if obj.record.seat == seat_id and obj.active:
                return obj.sid, obj.path
        return '', '/'

    def display_session_path(self, uid):
        """The user's graphical session, which is what User.Display means."""
        for obj in self.sessions.values():
            if obj.record.uid == uid and obj.record.is_display:
                return obj.path
        for obj in self.sessions.values():
            if obj.record.uid == uid:
                return obj.path
        return '/'

    def user_is_active(self, uid):
        return any(o.active for o in self.sessions.values()
                   if o.record.uid == uid)

    def session_for_pid(self, pid):
        """Resolve a pid to a session the way sd_pid_get_session() does.

        The cgroup scope is authoritative — that is what real logind keys on —
        with getsid() as a fallback for a process that escaped its scope.
        """
        try:
            with open('/proc/%d/cgroup' % pid) as f:
                blob = f.read()
        except OSError:
            blob = ''
        for sid in self.sessions:
            if ('session-%s.scope' % sid) in blob:
                return self.sessions[sid]
        try:
            leader = os.getsid(pid)
        except OSError:
            return None
        for obj in self.sessions.values():
            if obj.record.leader and obj.record.leader == leader:
                return obj
        return None

    # -- sync ----------------------------------------------------------------

    def sync(self, handoff=None):
        """Diff the directory against the live objects. Returns True (GLib)."""
        try:
            records = scan_session_files()
        except Exception as e:
            print(f"login1-stub: session scan failed: {e}", file=sys.stderr)
            return True

        # Reap sessions whose leader is gone. This is the only reaping there
        # is: CreateSession() hands back a fifo_fd but nothing watches it for
        # EOF, because one long-lived fd per session in the GLib loop is the
        # shape of both prior CPU-spin incidents. Nothing reliably calls
        # ReleaseSession either — pam_systemd just drops the fifo.
        #
        # Safe to run every tick rather than only at startup: a file is only
        # considered at all once it has keys, and both writers (the register
        # script and CreateSession) rename a complete file with LEADER over
        # the empty claim. So there is no window where a live session looks
        # leaderless. A file with no LEADER key at all is the legacy
        # sddm-logged's, and leader_alive() leaves those alone.
        #
        # leader_alive() also guards against a recycled pid: it compares the
        # leader's live /proc start-ticks against the LEADER_STARTTIME baseline
        # recorded at registration, so a dead leader's pid being reused by an
        # unrelated process is still detected and reaped. A session registered
        # before that baseline existed (no LEADER_STARTTIME key) falls back to
        # bare pid-existence, same as before.
        for sid in [s for s, r in records.items() if not r.leader_alive()]:
            print(f"login1-stub: session {sid} leader is gone — reaping",
                  file=sys.stderr)
            rec = records.pop(sid)
            try:
                os.unlink(session_file_for(sid))
            except OSError:
                pass
            try:
                os.rmdir('%s/user.slice/user-%d.slice/session-%s.scope'
                         % (CGROUP_ROOT, rec.uid, sid))
            except OSError:
                pass

        for sid in [s for s in self.sessions if s not in records]:
            obj = self.sessions.pop(sid)
            self._sigs.pop(sid, None)
            path = obj.path
            obj.shutdown()
            print(f"login1-stub: session {sid} gone")
            if self.manager:
                self.manager.SessionRemoved(sid, path)

        for sid, rec in records.items():
            obj = self.sessions.get(sid)
            if obj is None:
                adopt = handoff if (handoff or {}).get(
                    'sid', LEGACY_SESSION_ID) == sid else None
                obj = Login1Session(self.bus, rec, adopt)
                self.sessions[sid] = obj
                self._sigs[sid] = rec.signature()
                if self.manager:
                    self.manager.SessionNew(sid, obj.path)
                continue

            sig = rec.signature()
            if sig != self._sigs.get(sid):
                changed = rec.changed_properties(obj.record)
                obj.record = rec
                self._sigs[sid] = sig
                if changed:
                    obj.PropertiesChanged(
                        SESSION_IFACE,
                        {k: v for k, v in obj.GetAll(SESSION_IFACE).items()
                         if k in changed}, [])

        self._sync_users()
        self._write_derived()
        return True

    def _sync_users(self):
        live = set(self.uids())
        for uid in [u for u in self.users if u not in live]:
            obj = self.users.pop(uid)
            path = obj.path
            obj.remove_from_connection()
            if self.manager:
                self.manager.UserRemoved(dbus.UInt32(uid), path)
        for uid in live:
            if uid not in self.users:
                obj = Login1User(self.bus, uid, self)
                self.users[uid] = obj
                if self.manager:
                    self.manager.UserNew(dbus.UInt32(uid), obj.path)

    # -- derived state files -------------------------------------------------

    def _write_derived(self):
        """Populate /run/systemd/seats and /run/systemd/users.

        These are derived from the whole picture, so the bridge writes them
        rather than the login script. They were created empty purely so
        sd_login_monitor_new(NULL) would not fail with -ENOENT; nothing could
        read real seat or user state off disk until now.
        """
        for seat_id in self.seats():
            members = self.sessions_for_seat(seat_id)
            active_sid, _ = self.active_session_for_seat(seat_id)
            uids = []
            for sid, _p in members:
                uid = self.sessions[sid].record.uid
                if uid not in uids:
                    uids.append(uid)
            body = ['# This is private data. Do not parse.']
            if active_sid:
                active_uid = self.sessions[active_sid].record.uid
                body.append('ACTIVE=%s' % active_sid)
                body.append('ACTIVE_UID=%d' % active_uid)
                self._reconcile_uaccess(seat_id, active_uid)
            body.append('SESSIONS=%s' % ' '.join(s for s, _ in members))
            body.append('UIDS=%s' % ' '.join(str(u) for u in uids))
            self._write_if_changed(SEATS_DIR + '/' + seat_id, '\n'.join(body) + '\n')

        for uid in self.uids():
            mine = [o for o in self.sessions.values() if o.record.uid == uid]
            display = self.display_session_path(uid).rsplit('_', 1)[-1]
            body = [
                '# This is private data. Do not parse.',
                'NAME=%s' % get_username_for_uid(uid),
                'STATE=%s' % ('active' if self.user_is_active(uid) else 'online'),
                'SESSIONS=%s' % ' '.join(o.sid for o in mine),
                'SEATS=%s' % ' '.join(sorted({o.record.seat for o in mine})),
            ]
            if display != '/':
                body.append('DISPLAY=%s' % display)
            self._write_if_changed(USERS_DIR + '/%d' % uid, '\n'.join(body) + '\n')

    def _reconcile_uaccess(self, seat_id, active_uid):
        """Re-grant the seat's uaccess devices to whoever is active now.

        Coldplug ran before login, so without this the device sits owned by no
        one until a replug; this is logind's half of the systemd uaccess
        contract that schema-udev's device-add tagging can't do. systemd re-runs
        uaccess on every device-add event, so keying only on the active uid is a
        wrong proxy for "the ACLs are present": a node that appears or is
        re-created after the first login (nvidia re-mknod, late coldplug, replug)
        keeps the same active uid yet has no ACL. So we also fire when the live
        node set moves — its (path, inode, mtime) fingerprint. setfacl'ing an
        already-correct node is idempotent, and the fingerprint short-circuits
        the steady state, so this stays cheap at the 4 Hz derive rate.
        """
        nodes = uaccess_seat_nodes(seat_id)
        fp = _uaccess_fingerprint(nodes)
        old = self._seat_active_uid.get(seat_id)
        if old == active_uid and self._seat_uaccess_fp.get(seat_id) == fp:
            return
        try:
            apply_uaccess_acl(nodes, active_uid, old)
        except Exception as e:
            print(f"login1-stub: uaccess re-scan failed on {seat_id}: {e}",
                  file=sys.stderr)
            return
        self._seat_active_uid[seat_id] = active_uid
        self._seat_uaccess_fp[seat_id] = fp

    def _write_if_changed(self, path, content):
        # Compared against what we last wrote rather than re-read: this runs at
        # 4 Hz and the point is to touch the filesystem only when something
        # actually moved.
        if self._derived.get(path) == content:
            return
        tmp = path + '.tmp'
        try:
            with open(tmp, 'w') as f:
                f.write(content)
            os.rename(tmp, path)
            self._derived[path] = content
        except OSError as e:
            print(f"login1-stub: cannot write {path}: {e}", file=sys.stderr)
            try:
                os.unlink(tmp)
            except OSError:
                pass

    # -- VT ------------------------------------------------------------------

    def poll(self):
        """The one 250 ms timer: reconcile the directory, then the active VT."""
        self.sync()

        # A session that holds VT_PROCESS mediation is authoritative and the
        # sysfs VT has not moved yet while a release is in flight. Polling
        # through that window would see "still my VT", call the session active
        # and resume devices in the middle of handing them over.
        for obj in self.sessions.values():
            if obj.vt_fd is not None:
                return True

        try:
            vt = read_active_vt()
        except Exception as e:
            print(f"login1-stub: VT poll error: {e}", file=sys.stderr)
            return True
        self.on_vt_changed(vt)
        return True

    def on_vt_changed(self, new_vt):
        if not new_vt:
            return
        for obj in self.sessions.values():
            obj.on_vt_changed(new_vt)
        if new_vt != self._last_vt:
            self._last_vt = new_vt
            self._write_derived()
            if self.seat:
                sid, path = self.active_session_for_seat(self.seat.seat_id)
                self.seat.PropertiesChanged(SEAT_IFACE, {
                    'ActiveSession': dbus.Struct(
                        (dbus.String(sid), dbus.ObjectPath(path)), signature='so'),
                }, [])


class Login1User(dbus.service.Object):
    def __init__(self, bus, uid=1000, registry=None):
        self.uid = uid
        self.registry = registry
        self.path = user_path_for(uid)
        dbus.service.Object.__init__(self, bus, self.path)
        print(f"login1-stub: Registered User at {self.path}")

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
            display = self.registry.display_session_path(uid) if self.registry \
                else session_path_for(LEGACY_SESSION_ID)
            state = 'active' if (self.registry is None
                                 or self.registry.user_is_active(uid)) else 'online'
            return {
                'UID': dbus.UInt32(uid),
                'GID': dbus.UInt32(gid),
                'Name': dbus.String(username),
                'Display': dbus.ObjectPath(display),
                'State': dbus.String(state),
            }
        return {}

class Login1Seat(dbus.service.Object):
    def __init__(self, bus, registry=None, seat_id='seat0'):
        self.seat_id = seat_id
        self.registry = registry
        self.path = '/org/freedesktop/login1/seat/' + seat_id
        dbus.service.Object.__init__(self, bus, self.path)
        print(f"login1-stub: Registered Seat at {self.path}")

    @dbus.service.signal('org.freedesktop.DBus.Properties', signature='sa{sv}as')
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

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

    # Compat: single-seat box, so Seat.ActivateSession is a true no-op (the one
    # session is always active) and Terminate is a logged no-op.
    @dbus.service.method('org.freedesktop.login1.Seat', in_signature='s', out_signature='')
    def ActivateSession(self, session_id):
        print(f"login1-stub: Seat.ActivateSession({session_id}) (no-op; single seat)")

    @dbus.service.method('org.freedesktop.login1.Seat', in_signature='', out_signature='')
    def Terminate(self):
        print("login1-stub: Seat.Terminate() (no-op; not managed here)")

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
            sid, path = (self.registry.active_session_for_seat(self.seat_id)
                         if self.registry
                         else (LEGACY_SESSION_ID, session_path_for(LEGACY_SESSION_ID)))
            sessions = (self.registry.sessions_for_seat(self.seat_id)
                        if self.registry else [])
            return {
                'Id': dbus.String(self.seat_id),
                'ActiveSession': dbus.Struct((dbus.String(sid), dbus.ObjectPath(path)), signature='so'),
                'Sessions': dbus.Array(
                    [dbus.Struct((dbus.String(s), dbus.ObjectPath(p)), signature='so')
                     for s, p in sessions], signature='(so)'),
                'CanMultiSession': dbus.Boolean(True),
                'CanTTY': dbus.Boolean(True),
                'CanGraphical': dbus.Boolean(True),
            }
        return {}

class Login1Manager(dbus.service.Object):
    def __init__(self, bus, registry=None):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/login1')
        self.registry = registry
        print("login1-stub: Registered Manager at /org/freedesktop/login1")

    @property
    def session(self):
        """The session a caller means when it does not name one."""
        return self.registry.primary() if self.registry else None

    def _lookup(self, session_id):
        obj = self.registry.get(str(session_id)) if self.registry else None
        if obj is None:
            raise dbus.exceptions.DBusException(
                "No session '%s'" % session_id,
                name='org.freedesktop.login1.NoSuchSession')
        return obj

    # -- session lifecycle signals ------------------------------------------

    @dbus.service.signal(MANAGER_IFACE, signature='so')
    def SessionNew(self, session_id, path):
        pass

    @dbus.service.signal(MANAGER_IFACE, signature='so')
    def SessionRemoved(self, session_id, path):
        pass

    @dbus.service.signal(MANAGER_IFACE, signature='uo')
    def UserNew(self, uid, path):
        pass

    @dbus.service.signal(MANAGER_IFACE, signature='uo')
    def UserRemoved(self, uid, path):
        pass

    # loginctl lock-session/unlock-session route through the Manager, not the
    # Session object. Before these existed dbus-python raised UnknownMethod as
    # an unhandled traceback, so `loginctl lock-session` failed outright.
    def _relay_lock(self, member, session_id=None):
        if session_id:
            getattr(self._lookup(session_id), member)()
            return
        targets = list(self.registry.sessions.values()) if self.registry else []
        if not targets:
            raise dbus.exceptions.DBusException(
                "No session", name='org.freedesktop.login1.NoSuchSession')
        for obj in targets:
            getattr(obj, member)()

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='')
    def LockSession(self, session_id):
        self._relay_lock('Lock', session_id)

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='')
    def UnlockSession(self, session_id):
        self._relay_lock('Unlock', session_id)

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='')
    def LockSessions(self):
        self._relay_lock('Lock')

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='')
    def UnlockSessions(self):
        self._relay_lock('Unlock')

    # Compat stubs: methods KDE/loginctl may call that this stub does not
    # faithfully back. Accepting them keeps clients from crashing on
    # UnknownMethod; they are deliberate no-ops (single-seat box, and the stub
    # does not own session/user lifecycle -- schema-init does). ActivateSession
    # is a true no-op: the one session is always the active one here.
    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='')
    def ActivateSession(self, session_id):
        print(f"login1-stub: ActivateSession({session_id}) (no-op; single seat)")

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='ss', out_signature='')
    def ActivateSessionOnSeat(self, session_id, seat_id):
        print(f"login1-stub: ActivateSessionOnSeat({session_id}, {seat_id}) (no-op)")

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='')
    def TerminateSession(self, session_id):
        print(f"login1-stub: TerminateSession({session_id}) (no-op; not managed here)")

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='u', out_signature='')
    def TerminateUser(self, uid):
        print(f"login1-stub: TerminateUser({uid}) (no-op; not managed here)")

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='')
    def TerminateSeat(self, seat_id):
        print(f"login1-stub: TerminateSeat({seat_id}) (no-op; not managed here)")

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='ubb', out_signature='')
    def SetUserLinger(self, uid, enable, interactive):
        print(f"login1-stub: SetUserLinger({uid}, {enable}) (no-op)")

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='st', out_signature='')
    def ScheduleShutdown(self, shutdown_type, usec):
        print(f"login1-stub: ScheduleShutdown({shutdown_type}, {usec}) (no-op)")

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='b')
    def CancelScheduledShutdown(self):
        print("login1-stub: CancelScheduledShutdown (nothing scheduled)")
        return dbus.Boolean(False)

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

    @dbus.service.method('org.schema.logind1.Manager', in_signature='', out_signature='s')
    def RearmVtMediation(self):
        sess = self.registry.primary() if self.registry else None
        if sess is None:
            return "no active session"
        try:
            sess._setup_vt_mediation()
            return ""
        except Exception as e:
            return f"rearm failed: {e}"

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
        out = []
        for obj in (self.registry.sessions.values() if self.registry else []):
            rec = obj.record
            out.append(dbus.Struct((dbus.String(obj.sid), dbus.UInt32(rec.uid),
                                    dbus.String(rec.user), dbus.String(rec.seat),
                                    dbus.ObjectPath(obj.path)), signature='susso'))
        return out

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(uso)')
    def ListUsers(self):
        return [dbus.Struct((dbus.UInt32(uid), dbus.String(get_username_for_uid(uid)),
                             dbus.ObjectPath(user_path_for(uid))), signature='uso')
                for uid in (self.registry.uids() if self.registry
                            else [get_active_uid()])]

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(so)')
    def ListSeats(self):
        return [dbus.Struct((dbus.String(s), dbus.ObjectPath('/org/freedesktop/login1/seat/' + s)), signature='so')
                for s in (self.registry.seats() if self.registry else ['seat0'])]

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='ssss', out_signature='h')
    def Inhibit(self, what, who, why, mode):
        print(f"login1-stub: Inhibit({what}, {who}, {why}, {mode})")
        r, w = os.pipe()
        os.close(r)
        # UnixFd dups for the wire, so our copy has to be closed or every
        # inhibitor leaks one. PowerDevil and friends take these constantly;
        # this file has already been bitten once by an fd leak reaching EMFILE.
        try:
            return dbus.types.UnixFd(w)
        finally:
            os.close(w)

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='', out_signature='a(ssssuu)')
    def ListInhibitors(self):
        return []

    @dbus.service.method('org.freedesktop.login1.Manager',
                         in_signature='uusssssussbssa(sv)',
                         out_signature='soshusub')
    def CreateSession(self, uid, pid, service, type_, class_, desktop,
                      seat_id, vtnr, tty, display, remote, remote_user,
                      remote_host, properties):
        """The call pam_systemd.so already makes on every login.

        pam_systemd is in the session stack at /etc/pam.d/system-auth, which
        /etc/pam.d/login includes, so this has been firing on every tty login
        all along and dying with UnknownMethod behind '-session optional'.
        Answering it is what gives tty logins a session; no PAM file needs
        editing, which is the whole point — a hand-added pam_exec line in
        system-auth would be reverted by the next authselect run anyway.

        Newer callers try CreateSessionWithPIDFD first and fall back here on
        UnknownMethod, so only this variant needs to exist.

        DELIBERATELY NARROW: a session is created only for a caller that names
        a real VT. pam_systemd also calls this for sudo and su with
        class=background/background-light and no tty, and inventing seat
        sessions for those would fill `loginctl list-sessions` with noise for
        every sudo on the box.
        """
        vtnr = int(vtnr) or vtnr_from_tty(str(tty))
        if vtnr <= 0:
            raise dbus.exceptions.DBusException(
                "schema-logind creates sessions only for real VTs; "
                "'%s' class=%s tty=%s named none" % (service, class_, tty),
                name='org.freedesktop.DBus.Error.NotSupported')

        uid = int(uid)
        pid = int(pid)
        seat_id = str(seat_id) or 'seat0'
        username = get_username_for_uid(uid)

        # An existing session on that VT for that user is reused rather than
        # duplicated, which is what the 'existing' return flag is for.
        for obj in self.registry.sessions.values():
            if obj.record.vtnr == vtnr and obj.record.uid == uid \
                    and not obj.record.synthesised:
                return self._session_reply(obj.sid, uid, seat_id, vtnr, True)

        cmd = [SESSION_REGISTER,
               '--uid', str(uid), '--user', str(username or uid),
               '--seat', seat_id, '--vtnr', str(vtnr),
               '--type', str(type_) if str(type_) not in ('', 'unspecified') else 'tty',
               '--class', str(class_) or 'user',
               '--desktop', str(desktop),
               '--service', str(service) or 'login',
               '--leader', str(pid)]
        if str(desktop):  # a DM login is a display session
            cmd.append('--display')
        # The daemon's own env already carries SCHEMA_LOGIND_RUN_DIR /
        # SCHEMA_CGROUP_ROOT (real or test), so inheriting it is what makes the
        # helper write to the same tree the registry reads. Never block a login:
        # a helper failure still returns a reply on the legacy id.
        fell_back = False
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
            sid = (out.stdout.strip().splitlines() or [''])[-1]
        except Exception as e:
            print("login1-stub: schema-session-register failed: %s" % e,
                  file=sys.stderr)
            sid = ''
        if not sid.isdigit():
            sid = LEGACY_SESSION_ID
            fell_back = True

        # sd_pid_get_session() is cgroup-based, so without the scope the polkit
        # auth agent for this session cannot register. The helper mkdirs the
        # scope; placing the leader in it stays caller-side (same as the GUI
        # autologin, which places its own subshell pid).
        #
        # The register helper always allocates a fresh, unused id (O_EXCL), so
        # its sid never collides. Only the exec-failure/non-digit fallback can
        # land on LEGACY_SESSION_ID while a DIFFERENT, already-live session by
        # that id exists -- writing this leader into that scope would misattribute
        # it to a stranger's session (sd_pid_get_session). Skip the placement in
        # that one case; the login still proceeds, just without VT mediation,
        # which is better than joining a stranger's scope.
        scope = '%s/user.slice/user-%d.slice/session-%s.scope' % (
            CGROUP_ROOT, uid, sid)
        try:
            if pid and not (fell_back and sid in self.registry.sessions
                            and not self.registry.sessions[sid].record.synthesised):
                with open(os.path.join(scope, 'cgroup.procs'), 'w') as f:
                    f.write('%d\n' % pid)
        except OSError as e:
            print(f"login1-stub: session {sid} scope cgroup: {e}",
                  file=sys.stderr)

        print(f"login1-stub: CreateSession -> {sid} uid={uid} vtnr={vtnr} "
              f"service={service} class={class_} leader={pid}")
        self.registry.sync()
        return self._session_reply(sid, uid, seat_id, vtnr, False)

    def _session_reply(self, sid, uid, seat_id, vtnr, existing):
        # fifo_fd: real logind hands back a pipe end and treats its EOF as
        # "session over". We deliberately do not watch it — one long-lived fd
        # per session inside the GLib loop is the shape of both prior CPU-spin
        # incidents here. Sessions are reaped by the LEADER-alive check on the
        # existing 250 ms sync instead, so this only has to be a valid fd the
        # caller can hold and close. Our copy is closed because UnixFd dups for
        # the wire; not closing it is exactly the leak just fixed in Inhibit().
        fd = os.open('/dev/null', os.O_RDONLY | os.O_CLOEXEC)
        try:
            return (dbus.String(sid),
                    dbus.ObjectPath(session_path_for(sid)),
                    dbus.String('/run/user/%d' % uid),
                    dbus.types.UnixFd(fd),
                    dbus.UInt32(uid),
                    dbus.String(seat_id),
                    dbus.UInt32(vtnr),
                    dbus.Boolean(existing))
        finally:
            os.close(fd)

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='')
    def ReleaseSession(self, session_id):
        sid = str(session_id)
        print(f"login1-stub: ReleaseSession({sid})")
        uid = self.registry.sessions[sid].record.uid if sid in self.registry.sessions else ''
        try:
            subprocess.run([SESSION_UNREGISTER, str(sid), str(uid)], timeout=5)
        except Exception as e:
            print("login1-stub: schema-session-unregister failed: %s" % e,
                  file=sys.stderr)
        self.registry.sync()

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='u', out_signature='o')
    def GetSessionByPID(self, pid):
        obj = self.registry.session_for_pid(int(pid)) if self.registry else None
        if obj is None:
            raise dbus.exceptions.DBusException(
                "PID %s is not part of any session" % pid,
                name='org.freedesktop.login1.NoSuchSession')
        return dbus.ObjectPath(obj.path)

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='u', out_signature='o')
    def GetUserByPID(self, pid):
        obj = self.registry.session_for_pid(int(pid)) if self.registry else None
        if obj is None:
            raise dbus.exceptions.DBusException(
                "PID %s is not part of any session" % pid,
                name='org.freedesktop.login1.NoSuchUser')
        return dbus.ObjectPath(user_path_for(obj.record.uid))

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='o')
    def GetSession(self, session_id):
        return dbus.ObjectPath(self._lookup(session_id).path)

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='u', out_signature='o')
    def GetUser(self, uid):
        return dbus.ObjectPath(user_path_for(uid))

    @dbus.service.method('org.freedesktop.login1.Manager', in_signature='s', out_signature='o')
    def GetSeat(self, seat_id):
        return dbus.ObjectPath('/org/freedesktop/login1/seat/' + str(seat_id or 'seat0'))

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

    # SetHostname is the transient (kernel) name; SetStaticHostname persists to
    # /etc/hostname; the rest live in /etc/machine-info. KDE's hostname panel and
    # nm-dispatcher call these in sequence, so implementing only SetHostname just
    # moves the UnknownMethod traceback to the next call.
    @dbus.service.method('org.freedesktop.hostname1', in_signature='sb', out_signature='')
    def SetHostname(self, hostname, interactive):
        if hostname and not _valid_hostname(hostname):
            raise dbus.exceptions.DBusException(
                'Invalid hostname: ' + hostname,
                name='org.freedesktop.DBus.Error.InvalidArgs')
        try:
            socket.sethostname(hostname or 'localhost')
        except OSError as e:
            raise dbus.exceptions.DBusException(
                str(e), name='org.freedesktop.hostname1.Error.Failed')

    @dbus.service.method('org.freedesktop.hostname1', in_signature='sb', out_signature='')
    def SetStaticHostname(self, hostname, interactive):
        if hostname and not _valid_hostname(hostname):
            raise dbus.exceptions.DBusException(
                'Invalid hostname: ' + hostname,
                name='org.freedesktop.DBus.Error.InvalidArgs')
        try:
            if hostname:
                tmp = '/etc/hostname.new'
                with open(tmp, 'w') as f:
                    f.write(hostname + '\n')
                os.replace(tmp, '/etc/hostname')
                socket.sethostname(hostname)
            else:
                try:
                    os.remove('/etc/hostname')
                except FileNotFoundError:
                    pass
        except OSError as e:
            raise dbus.exceptions.DBusException(
                str(e), name='org.freedesktop.hostname1.Error.Failed')

    @dbus.service.method('org.freedesktop.hostname1', in_signature='sb', out_signature='')
    def SetPrettyHostname(self, hostname, interactive):
        self._set_machine_info('PRETTY_HOSTNAME', hostname)

    @dbus.service.method('org.freedesktop.hostname1', in_signature='sb', out_signature='')
    def SetIconName(self, icon, interactive):
        self._set_machine_info('ICON_NAME', icon)

    @dbus.service.method('org.freedesktop.hostname1', in_signature='sb', out_signature='')
    def SetChassis(self, chassis, interactive):
        self._set_machine_info('CHASSIS', chassis)

    @dbus.service.method('org.freedesktop.hostname1', in_signature='sb', out_signature='')
    def SetDeployment(self, deployment, interactive):
        self._set_machine_info('DEPLOYMENT', deployment)

    @dbus.service.method('org.freedesktop.hostname1', in_signature='sb', out_signature='')
    def SetLocation(self, location, interactive):
        self._set_machine_info('LOCATION', location)

    def _set_machine_info(self, key, value):
        try:
            _update_machine_info(key, value)
        except OSError as e:
            raise dbus.exceptions.DBusException(
                str(e), name='org.freedesktop.hostname1.Error.Failed')

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


# --- localed backing store (org.freedesktop.locale1) -------------------------
# The set of locale variables systemd's localed accepts; anything else in a
# SetLocale() call is rejected rather than written blindly to /etc/locale.conf.
_LOCALE_VARS = (
    'LANG', 'LANGUAGE', 'LC_CTYPE', 'LC_NUMERIC', 'LC_TIME', 'LC_COLLATE',
    'LC_MONETARY', 'LC_MESSAGES', 'LC_PAPER', 'LC_NAME', 'LC_ADDRESS',
    'LC_TELEPHONE', 'LC_MEASUREMENT', 'LC_IDENTIFICATION', 'LC_ALL',
)


def _read_kv_conf(path):
    """Parse a KEY=VALUE shell-ish conf (locale.conf / vconsole.conf)."""
    out = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#') or '=' not in line:
                    continue
                k, v = line.split('=', 1)
                v = v.strip()
                if len(v) >= 2 and v[0] == v[-1] and v[0] in '"\'':
                    v = v[1:-1]
                out[k.strip()] = v
    except FileNotFoundError:
        pass
    return out


def _atomic_write(path, text):
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d, exist_ok=True)
    tmp = path + '.new'
    with open(tmp, 'w') as f:
        f.write(text)
    os.replace(tmp, path)


def get_locale():
    kv = _read_kv_conf(LOCALE_CONF)
    return ['%s=%s' % (k, kv[k]) for k in _LOCALE_VARS if k in kv]


def set_locale(assignments):
    parsed = []
    for a in assignments:
        if '=' not in a:
            raise ValueError('malformed locale assignment: %r' % a)
        k, v = a.split('=', 1)
        if k not in _LOCALE_VARS:
            raise ValueError('unknown locale variable: %r' % k)
        if any(c in v for c in '\r\n\0'):
            raise ValueError('invalid locale value: %r' % v)
        parsed.append('%s=%s' % (k, v))
    if parsed:
        _atomic_write(LOCALE_CONF, '\n'.join(parsed) + '\n')
    elif os.path.exists(LOCALE_CONF):
        os.remove(LOCALE_CONF)


def get_vconsole():
    kv = _read_kv_conf(VCONSOLE_CONF)
    return (kv.get('KEYMAP', ''), kv.get('KEYMAP_TOGGLE', ''))


def set_vconsole(keymap, toggle):
    # Preserve any FONT* lines already present; only rewrite the KEYMAP pair.
    kv = _read_kv_conf(VCONSOLE_CONF)
    kv.pop('KEYMAP', None)
    kv.pop('KEYMAP_TOGGLE', None)
    lines = ['%s=%s' % (k, v) for k, v in kv.items()]
    if keymap:
        lines.append('KEYMAP=%s' % keymap)
    if toggle:
        lines.append('KEYMAP_TOGGLE=%s' % toggle)
    _atomic_write(VCONSOLE_CONF, '\n'.join(lines) + '\n' if lines else '')


def get_x11_keyboard():
    layout = model = variant = options = ''
    try:
        with open(X11_KEYMAP_CONF) as f:
            for line in f:
                m = re.search(r'Option\s+"Xkb(Layout|Model|Variant|Options)"\s+"([^"]*)"',
                              line)
                if not m:
                    continue
                which, val = m.group(1), m.group(2)
                if which == 'Layout':
                    layout = val
                elif which == 'Model':
                    model = val
                elif which == 'Variant':
                    variant = val
                elif which == 'Options':
                    options = val
    except FileNotFoundError:
        pass
    return (layout, model, variant, options)


def set_x11_keyboard(layout, model, variant, options):
    for v in (layout, model, variant, options):
        if any(c in v for c in '"\r\n\0'):
            raise ValueError('invalid X11 keyboard value: %r' % v)
    opts = [('XkbLayout', layout), ('XkbModel', model),
            ('XkbVariant', variant), ('XkbOptions', options)]
    body = ['Section "InputClass"',
            '        Identifier "system-keyboard"',
            '        MatchIsKeyboard "on"']
    body += ['        Option "%s" "%s"' % (k, v) for k, v in opts if v]
    body += ['EndSection', '']
    _atomic_write(X11_KEYMAP_CONF, '\n'.join(body))


class Locale1(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/locale1')
        print("login1-stub: Registered Locale1 at /org/freedesktop/locale1")

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
        if interface_name == 'org.freedesktop.locale1' or not interface_name:
            layout, model, variant, options = get_x11_keyboard()
            keymap, toggle = get_vconsole()
            return {
                'Locale': dbus.Array([dbus.String(s) for s in get_locale()],
                                     signature='s'),
                'X11Layout': dbus.String(layout),
                'X11Model': dbus.String(model),
                'X11Variant': dbus.String(variant),
                'X11Options': dbus.String(options),
                'VConsoleKeymap': dbus.String(keymap),
                'VConsoleKeymapToggle': dbus.String(toggle),
            }
        return {}

    @dbus.service.method('org.freedesktop.locale1', in_signature='asb', out_signature='')
    def SetLocale(self, locale, interactive):
        try:
            set_locale([str(x) for x in locale])
        except (ValueError, OSError) as e:
            raise dbus.exceptions.DBusException(
                str(e), name='org.freedesktop.locale1.Error.Failed')

    # `convert` (cross-generate the X11 layout from the vconsole keymap and vice
    # versa) is not honoured yet — a v1 follow-up. The pair is written verbatim.
    @dbus.service.method('org.freedesktop.locale1', in_signature='ssbb', out_signature='')
    def SetVConsoleKeyboard(self, keymap, keymap_toggle, convert, interactive):
        try:
            set_vconsole(str(keymap), str(keymap_toggle))
        except (ValueError, OSError) as e:
            raise dbus.exceptions.DBusException(
                str(e), name='org.freedesktop.locale1.Error.Failed')

    @dbus.service.method('org.freedesktop.locale1', in_signature='ssssbb', out_signature='')
    def SetX11Keyboard(self, layout, model, variant, options, convert, interactive):
        try:
            set_x11_keyboard(str(layout), str(model), str(variant), str(options))
        except (ValueError, OSError) as e:
            raise dbus.exceptions.DBusException(
                str(e), name='org.freedesktop.locale1.Error.Failed')


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
    # Popped first thing so it cannot leak into the subprocesses we spawn.
    handoff = load_handoff()
    DBusGMainLoop(set_as_default=True)

    os.makedirs(_RUN_SYSTEMD + '/system', exist_ok=True)
    # sd_login_monitor_new(NULL) watches all four; missing dir -> ENOENT (-2) aborts WirePlumber's logind module.
    for _d in ('sessions', 'seats', 'users', 'machines'):
        os.makedirs(_RUN_SYSTEMD + '/' + _d, exist_ok=True)
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
    registry = SessionRegistry(bus)
    seat = Login1Seat(bus, registry)
    manager = Login1Manager(bus, registry)
    registry.seat = seat
    # Objects are built by the first sync, not by hand: the directory is the
    # source of truth from the very first tick, and the legacy fallback inside
    # scan_session_files() covers a login script that never allocated an id.
    # The manager is attached only afterwards so the initial population does
    # not emit SessionNew for sessions that predate us.
    registry.sync(handoff)
    registry.manager = manager

    for obj in registry.sessions.values():
        if handoff is None and obj.record.active and obj.vt_fd is None:
            # Cold start under a session that is already running: its compositor
            # took control of a bridge that no longer exists, so we hold none of
            # its fds and _setup_vt_mediation() is never called for it. Before
            # this line that failed in complete silence — ctrl-alt-F<n> simply
            # stopped working and nothing said why. Upgrade with `kill -HUP`.
            print(f"login1-stub: ORPHANED SESSION — session {obj.sid} is active "
                  f"on VT {obj.record.vtnr} but no fds were handed off. VT "
                  f"mediation is NOT armed: the recovery console is unreachable "
                  f"until that session logs out and back in.", file=sys.stderr)

    hostname = Hostname1(bus)
    timedate = Timedate1(bus)
    locale = Locale1(bus)

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
        bus.request_name('org.freedesktop.locale1', dbus.bus.NAME_FLAG_REPLACE_EXISTING)
        print("login1-stub: Successfully acquired 'org.freedesktop.locale1' name")
    except Exception as e:
        print(f"login1-stub: Failed to acquire name 'org.freedesktop.locale1': {e}", file=sys.stderr)

    try:
        bus.request_name('org.freedesktop.ConsoleKit', dbus.bus.NAME_FLAG_REPLACE_EXISTING)
        print("login1-stub: Successfully acquired 'org.freedesktop.ConsoleKit' name")
    except Exception as e:
        print(f"login1-stub: Failed to acquire name 'org.freedesktop.ConsoleKit': {e}", file=sys.stderr)

    GLib.timeout_add(VT_POLL_MS, registry.poll)
    print(f"login1-stub: watching {ACTIVE_VT_PATH} and {SESSIONS_DIR} every "
          f"{VT_POLL_MS}ms (active VT {read_active_vt()}, "
          f"{len(registry.sessions)} session(s): "
          f"{' '.join(sorted(registry.sessions)) or 'none'})")

    loop = GLib.MainLoop()

    def shutdown_handler(sig, frame):
        print(f"login1-stub: Received signal {sig}, shutting down...")
        loop.quit()

    signal.signal(signal.SIGINT, shutdown_handler)
    signal.signal(signal.SIGTERM, shutdown_handler)

    # Deliberately via GLib rather than signal.signal(): re-exec has to run on
    # the main loop, where no D-Bus reply is half-written and no VT release is
    # mid-flight, not from an async signal handler between two bytecodes.
    def hup_handler():
        print("login1-stub: SIGHUP — re-exec requested")
        # Only one session can hold VT mediation and taken device fds, and only
        # those are what the handoff exists to preserve. Re-exec through that
        # session so its release-in-flight check is the one that runs; with no
        # mediation anywhere, any session will do and there is nothing to lose.
        holder = next((o for o in registry.sessions.values()
                       if o.vt_fd is not None), None) or registry.primary()
        if holder is None:
            print("login1-stub: no session to re-exec through — exec'ing bare",
                  file=sys.stderr)
            interp = sys.executable or os.path.realpath('/proc/self/exe')
            os.execv(interp, [interp, SCRIPT_PATH] + sys.argv[1:])
        holder.reexec()
        return True

    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGHUP, hup_handler)

    loop.run()

if __name__ == '__main__':
    main()
