#!/usr/bin/env python3
import sys
import os
import signal
import time
import subprocess
import json
import re
import dbus
import dbus.service
import dbus.server
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

# Mimic a modern systemd version so tools don't feature-gate us out of basic
# service management. One-line change if a client tries unmapped 256-era APIs.
SYSTEMD_COMPAT_VERSION = "256"

# Absolute path: under schema-init's service environment PATH does not include
# /usr/local/bin, so a bare 'schema-ctl' is not found (and an absolute path also
# avoids PATH-hijack for this root daemon).
SCHEMA_CTL = "/usr/local/bin/schema-ctl"

# Unit names reach schema_ctl() from D-Bus callers (any local client on the
# system bus) and we run as root. schema-ctl's wire protocol is newline-
# delimited text, so an unvalidated name with a newline/space could smuggle a
# second root command into the control socket. Allow only safe chars and reject
# a leading '-'; this still permits real names like getty-tty2 / mount-efi.
_UNIT_RE = re.compile(r'^[A-Za-z0-9:_.@-]+$')

def schema_ctl(action, name):
    if not name or name.startswith('-') or not _UNIT_RE.match(name):
        print(f"systemd1: refusing unsafe unit name {name!r}", file=sys.stderr)
        return
    try:
        subprocess.run([SCHEMA_CTL, action, name], capture_output=True, timeout=5)
    except Exception as e:
        print(f"systemd1: schema-ctl {action} {name} failed: {e}", file=sys.stderr)

def escape_unit_name(name):
    # Systemd-like escaping: replace '.' with '_2e', '-' with '_2d', etc.
    return name.replace('.', '_2e').replace('-', '_2d')

def unescape_unit_name(escaped):
    return escaped.replace('_2e', '.').replace('_2d', '-')

def svc_name(unit):
    for suffix in ('.service', '.target', '.socket', '.timer', '.mount', '.path'):
        if unit.endswith(suffix):
            return unit[:-len(suffix)]
    return unit

class Systemd1Unit(dbus.service.Object):
    def __init__(self, bus, name, status_dict, manager):
        self.name = name  # e.g., 'frigate.service'
        self.short_name = svc_name(name)
        self.escaped = escape_unit_name(name)
        self.path = f'/org/freedesktop/systemd1/unit/{self.escaped}'
        self.manager = manager
        
        dbus.service.Object.__init__(self, bus, self.path)
        
        self.update_state(status_dict)
        print(f"systemd1: Registered Unit {self.name} at {self.path}")

    def update_state(self, status_dict):
        prev_active = getattr(self, 'active_state', None)
        self.pid = status_dict.get('pid', 0)
        self.raw_state = status_dict.get('state', 'UNKNOWN')
        self.restarts = status_dict.get('restarts', 0)

        # State Mapping logic
        self.service_type = 'simple'
        if self.pid == 0 and self.raw_state in ('PERFECT', 'FUNDAMENTAL', 'SETTLED', 'FULL_TRUST'):
            self.active_state = 'active'
            self.sub_state = 'exited'
            self.service_type = 'oneshot'
        elif self.raw_state in ('PERFECT', 'FUNDAMENTAL', 'SETTLED', 'FULL_TRUST'):
            self.active_state = 'active'
            self.sub_state = 'running'
        elif self.raw_state in ('NEW_PROCESS', 'FRICTION', 'RECOVERY'):
            self.active_state = 'activating'
            self.sub_state = 'start'
        elif self.raw_state == 'DORMANT':
            self.active_state = 'activating'
            self.sub_state = 'auto-restart'
        elif self.raw_state == 'EXCISED':
            if self.name in self.manager.recently_stopped:
                self.active_state = 'inactive'
                self.sub_state = 'dead'
            else:
                self.active_state = 'failed'
                self.sub_state = 'failed'
        else:
            self.active_state = 'inactive'
            self.sub_state = 'dead'

        if self.active_state != 'inactive' and self.name in self.manager.recently_stopped:
            self.manager.recently_stopped.remove(self.name)

        # Basic metadata
        self.description = f"Schema service {self.short_name}"
        self.unit_file_state = 'enabled'

        # ActiveEnterTimestamp must reflect when the unit *entered* active, not
        # "now on every poll" — otherwise `systemctl status` always shows "1s ago".
        if self.active_state == 'active' and prev_active != 'active':
            self.active_enter_timestamp = int(time.time() * 1_000_000)
        elif not hasattr(self, 'active_enter_timestamp'):
            self.active_enter_timestamp = 0

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface_name, property_name):
        props = self.GetAll(interface_name)
        if property_name not in props:
            raise dbus.exceptions.DBusException(
                'No such property: ' + str(property_name),
                name='org.freedesktop.DBus.Error.UnknownProperty')
        return props[property_name]

    def _unit_props(self):
        return {
            'Id': dbus.String(self.name),
            'Names': dbus.Array([dbus.String(self.name)], signature='s'),
            'Description': dbus.String(self.description),
            'LoadState': dbus.String('loaded'),
            'ActiveState': dbus.String(self.active_state),
            'SubState': dbus.String(self.sub_state),
            'UnitFileState': dbus.String(self.unit_file_state),
            'UnitFilePreset': dbus.String('enabled'),
            'FragmentPath': dbus.String(f'/etc/schema-init/services/{self.short_name}.svc'),
            'ActiveEnterTimestamp': dbus.UInt64(self.active_enter_timestamp),
            'Job': dbus.Struct((dbus.UInt32(0), dbus.ObjectPath('/')), signature='uo'),
            'CanStart': dbus.Boolean(True),
            'CanStop': dbus.Boolean(True),
            'CanReload': dbus.Boolean(False),
            'CanRestart': dbus.Boolean(True),
        }

    def _service_props(self):
        return {
            'Type': dbus.String(self.service_type),
            'MainPID': dbus.UInt32(self.pid),
            'ExecMainPID': dbus.UInt32(self.pid),
            'NRestarts': dbus.UInt32(self.restarts),
            'Result': dbus.String('success'),
        }

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface_name):
        # systemctl/sd-bus calls GetAll("") to fetch every property across all
        # interfaces at once; an empty interface must return the union, or
        # status/show get nothing and systemctl aborts on the null Id.
        if interface_name == 'org.freedesktop.systemd1.Unit':
            return self._unit_props()
        if interface_name == 'org.freedesktop.systemd1.Service':
            return self._service_props()
        if not interface_name:
            props = self._unit_props()
            props.update(self._service_props())
            return props
        return {}

    @dbus.service.signal('org.freedesktop.DBus.Properties', signature='sa{sv}as')
    def PropertiesChanged(self, interface_name, changed_properties, invalidated_properties):
        pass

def _read_btime_usec():
    """CLOCK_REALTIME of monotonic-zero (boot) in usec, from /proc/stat btime."""
    try:
        with open('/proc/stat') as f:
            for line in f:
                if line.startswith('btime '):
                    return int(line.split()[1]) * 1_000_000
    except Exception:
        pass
    return 0


def _pid1_start_usec():
    """Monotonic usec at which PID 1 (schema-init) started — ~= userspace start."""
    try:
        with open('/proc/1/stat') as f:
            data = f.read()
        after = data[data.rindex(')') + 2:].split()  # skip pid + (comm)
        starttime_ticks = int(after[19])              # field 22 = starttime
        return int(starttime_ticks * 1_000_000 / os.sysconf('SC_CLK_TCK'))
    except Exception:
        return 0


def manager_boot_timestamps():
    """Best-effort systemd Manager boot-timing properties. schema-init has no
    firmware/loader/initrd handoff timing, so those are 0; kernel monotonic base
    is 0; userspace ~= PID1 start. Enough for clients (Ferrix, systemd-analyze)
    to render 'Startup finished in ...' instead of erroring on UnknownProperty."""
    btime = _read_btime_usec()
    usp = _pid1_start_usec()
    z = dbus.UInt64(0)
    return {
        'FirmwareTimestamp': z, 'FirmwareTimestampMonotonic': z,
        'LoaderTimestamp': z, 'LoaderTimestampMonotonic': z,
        'KernelTimestamp': dbus.UInt64(btime), 'KernelTimestampMonotonic': z,
        'InitRDTimestamp': z, 'InitRDTimestampMonotonic': z,
        'UserspaceTimestamp': dbus.UInt64(btime + usp),
        'UserspaceTimestampMonotonic': dbus.UInt64(usp),
        'FinishTimestamp': dbus.UInt64(btime + usp),
        'FinishTimestampMonotonic': dbus.UInt64(usp),
    }


def manager_environment():
    """PID 1's environment block as KEY=val strings — the schema-init analog of
    systemd's Manager.Environment (the env handed to spawned services)."""
    try:
        with open('/proc/1/environ', 'rb') as f:
            raw = f.read()
        items = [p.decode('utf-8', 'replace') for p in raw.split(b'\x00') if p]
        return dbus.Array([dbus.String(i) for i in items], signature='s')
    except Exception:
        return dbus.Array([], signature='s')


class Systemd1Manager(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, '/org/freedesktop/systemd1')
        self.bus = bus
        self.units = {}
        self.recently_stopped = set()
        self.next_job_id = 1
        self.poll_and_update()
        print("systemd1: Registered Manager at /org/freedesktop/systemd1")

    def poll_and_update(self):
        try:
            # Query schema-ctl status --json
            res = subprocess.run([SCHEMA_CTL, 'status', '--json'], capture_output=True, text=True, timeout=5)
            if res.returncode == 0:
                data = json.loads(res.stdout)
                services_list = data.get('services', [])
            else:
                services_list = []
        except Exception as e:
            print(f"systemd1: Failed to poll status: {e}", file=sys.stderr)
            services_list = []

        current_names = set()
        for s in services_list:
            short_name = s.get('name')
            if not short_name:
                continue
            name = f"{short_name}.service"
            current_names.add(name)

            if name not in self.units:
                # New unit discovered
                unit_obj = Systemd1Unit(self.bus, name, s, self)
                self.units[name] = unit_obj
                self.UnitNew(name, dbus.ObjectPath(unit_obj.path))
            else:
                # Existing unit: update and detect state transitions
                unit = self.units[name]
                old_active = unit.active_state
                old_sub = unit.sub_state
                old_pid = unit.pid
                unit.update_state(s)
                
                # If properties changed, emit PropertiesChanged signal
                changed = {}
                if unit.active_state != old_active:
                    changed['ActiveState'] = dbus.String(unit.active_state)
                if unit.sub_state != old_sub:
                    changed['SubState'] = dbus.String(unit.sub_state)
                if unit.pid != old_pid:
                    changed['MainPID'] = dbus.UInt32(unit.pid)
                    changed['ExecMainPID'] = dbus.UInt32(unit.pid)
                
                if changed:
                    unit.PropertiesChanged('org.freedesktop.systemd1.Unit', changed, [])

        # Clean up removed units
        for name in list(self.units.keys()):
            if name not in current_names:
                unit = self.units.pop(name)
                self.UnitRemoved(name, dbus.ObjectPath(unit.path))
                unit.remove_from_connection()

        return True

    def _trigger_job_signals(self, unit_name):
        job_id = self.next_job_id
        self.next_job_id += 1
        job_path = dbus.ObjectPath(f'/org/freedesktop/systemd1/job/{job_id}')
        
        self.JobNew(dbus.UInt32(job_id), job_path, unit_name)
        # Force a poll update immediately to propagate states
        self.poll_and_update()
        self.JobRemoved(dbus.UInt32(job_id), job_path, unit_name, "done")
        return job_path

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='s', out_signature='s')
    def GetUnitFileState(self, name):
        self.poll_and_update()
        if name in self.units:
            return self.units[name].unit_file_state
        return "disabled"

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='s', out_signature='o')
    def GetUnit(self, name):
        self.poll_and_update()
        if name in self.units:
            return dbus.ObjectPath(self.units[name].path)
        escaped = escape_unit_name(name)
        return dbus.ObjectPath(f'/org/freedesktop/systemd1/unit/{escaped}')

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='s', out_signature='o')
    def LoadUnit(self, name):
        return self.GetUnit(name)

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='', out_signature='a(ssssssouso)')
    def ListUnits(self):
        self.poll_and_update()
        result = []
        for name, unit in self.units.items():
            result.append(dbus.Struct((
                dbus.String(name),
                dbus.String(unit.description),
                dbus.String('loaded'),
                dbus.String(unit.active_state),
                dbus.String(unit.sub_state),
                dbus.String(''),
                dbus.ObjectPath(unit.path),
                dbus.UInt32(0),
                dbus.String(''),
                dbus.ObjectPath('/')
            ), signature='ssssssouso'))
        return result

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='as', out_signature='a(ssssssouso)')
    def ListUnitsFiltered(self, states):
        units = self.ListUnits()
        if not states:
            return units
        return [u for u in units if u[3] in states]

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='ss', out_signature='o')
    def StartUnit(self, name, mode):
        print(f"systemd1: StartUnit({name}, {mode})")
        schema_ctl('start', svc_name(str(name)))
        return self._trigger_job_signals(str(name))

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='ss', out_signature='o')
    def StopUnit(self, name, mode):
        print(f"systemd1: StopUnit({name}, {mode})")
        self.recently_stopped.add(str(name))
        schema_ctl('stop', svc_name(str(name)))
        return self._trigger_job_signals(str(name))

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='ss', out_signature='o')
    def RestartUnit(self, name, mode):
        print(f"systemd1: RestartUnit({name}, {mode})")
        schema_ctl('restart', svc_name(str(name)))
        return self._trigger_job_signals(str(name))

    def _is_active(self, name):
        u = self.units.get(name)
        return bool(u and u.active_state in ('active', 'activating'))

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='ss', out_signature='o')
    def TryRestartUnit(self, name, mode):
        print(f"systemd1: TryRestartUnit({name}, {mode})")
        self.poll_and_update()
        if self._is_active(str(name)):
            schema_ctl('restart', svc_name(str(name)))
        return self._trigger_job_signals(str(name))

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='ss', out_signature='o')
    def ReloadOrRestartUnit(self, name, mode):
        print(f"systemd1: ReloadOrRestartUnit({name}, {mode})")
        schema_ctl('restart', svc_name(str(name)))
        return self._trigger_job_signals(str(name))

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='ss', out_signature='o')
    def ReloadOrTryRestartUnit(self, name, mode):
        print(f"systemd1: ReloadOrTryRestartUnit({name}, {mode})")
        self.poll_and_update()
        if self._is_active(str(name)):
            schema_ctl('restart', svc_name(str(name)))
        return self._trigger_job_signals(str(name))

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='', out_signature='ao')
    def EnqueueMarkedJobs(self):
        print("systemd1: EnqueueMarkedJobs (no marker tracking; returning empty)")
        return dbus.Array([], signature='o')

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='', out_signature='')
    def Reexecute(self):
        print("systemd1: Reexecute (no-op; schema-init is PID 1, not systemd)")

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='sba(sv)', out_signature='')
    def SetUnitProperties(self, name, runtime, properties):
        print(f"systemd1: SetUnitProperties({name}) (no-op)")

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='asbb', out_signature='ba(sss)')
    def EnableUnitFiles(self, names, runtime, force):
        print(f"systemd1: EnableUnitFiles({list(names)})")
        return dbus.Boolean(True), []

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='asb', out_signature='a(sss)')
    def DisableUnitFiles(self, names, runtime):
        print(f"systemd1: DisableUnitFiles({list(names)})")
        return []

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='asbb', out_signature='a(sss)')
    def MaskUnitFiles(self, names, runtime, force):
        print(f"systemd1: MaskUnitFiles({list(names)})")
        return []

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='', out_signature='')
    def Subscribe(self):
        pass

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='', out_signature='')
    def Unsubscribe(self):
        pass

    @dbus.service.method('org.freedesktop.systemd1.Manager', in_signature='', out_signature='')
    def Reload(self):
        print("systemd1: Reload requested")
        try:
            subprocess.run([SCHEMA_CTL, 'reload'], capture_output=True, timeout=5)
        except Exception as e:
            print(f"systemd1: schema-ctl reload failed: {e}", file=sys.stderr)

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
        # As on units, GetAll("") must return the Manager's properties too.
        if interface_name == 'org.freedesktop.systemd1.Manager' or not interface_name:
            props = {
                'Version': dbus.String(SYSTEMD_COMPAT_VERSION),
                'SystemState': dbus.String('running'),
                # Keep Features empty: don't advertise PAM/SELINUX/etc. we don't
                # back, so clients don't try APIs we deliberately don't implement.
                'Features': dbus.String(''),
                'Architecture': dbus.String('x86-64'),
                'Environment': manager_environment(),
            }
            props.update(manager_boot_timestamps())
            return props
        return {}

    # Signals
    @dbus.service.signal('org.freedesktop.systemd1.Manager', signature='so')
    def UnitNew(self, unit_id, unit_path):
        pass

    @dbus.service.signal('org.freedesktop.systemd1.Manager', signature='so')
    def UnitRemoved(self, unit_id, unit_path):
        pass

    @dbus.service.signal('org.freedesktop.systemd1.Manager', signature='uos')
    def JobNew(self, job_id, job_path, unit_id):
        pass

    @dbus.service.signal('org.freedesktop.systemd1.Manager', signature='uoss')
    def JobRemoved(self, job_id, job_path, unit_id, result):
        pass

PRIVATE_SOCKET_PATH = '/run/systemd/private'

class PrivateSocketServer(dbus.server.Server):
    conn_mgrs = None

    def connection_added(self, conn):
        if self.conn_mgrs is None:
            self.conn_mgrs = {}
        self.conn_mgrs[id(conn)] = Systemd1Manager(conn)

    def connection_removed(self, conn):
        m = (self.conn_mgrs or {}).pop(id(conn), None)
        if m is not None:
            try:
                m.remove_from_connection()
            except Exception:
                pass

def main():
    DBusGMainLoop(set_as_default=True)

    try:
        bus = dbus.SystemBus()
    except Exception as e:
        print(f"systemd1: Failed to connect to System Bus: {e}", file=sys.stderr)
        sys.exit(1)

    manager = Systemd1Manager(bus)

    try:
        bus.request_name('org.freedesktop.systemd1', dbus.bus.NAME_FLAG_REPLACE_EXISTING)
        print("systemd1: Successfully acquired 'org.freedesktop.systemd1' name")
    except Exception as e:
        print(f"systemd1: Failed to acquire name 'org.freedesktop.systemd1': {e}", file=sys.stderr)
        sys.exit(1)

    try:
        if os.path.exists(PRIVATE_SOCKET_PATH):
            os.unlink(PRIVATE_SOCKET_PATH)
        priv_server = PrivateSocketServer('unix:path=' + PRIVATE_SOCKET_PATH)
        os.chmod(PRIVATE_SOCKET_PATH, 0o600)
        print(f"systemd1: serving root private socket at {PRIVATE_SOCKET_PATH}")
    except Exception as e:
        print(f"systemd1: failed to start private socket: {e}", file=sys.stderr)

    loop = GLib.MainLoop()

    # GLib Timeout to poll status every 1 second
    GLib.timeout_add(1000, manager.poll_and_update)

    def shutdown_handler(sig, frame):
        print(f"systemd1: Received signal {sig}, shutting down...")
        try:
            os.unlink(PRIVATE_SOCKET_PATH)
        except OSError:
            pass
        loop.quit()

    signal.signal(signal.SIGINT, shutdown_handler)
    signal.signal(signal.SIGTERM, shutdown_handler)

    loop.run()

if __name__ == '__main__':
    main()
