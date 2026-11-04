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
import shlex
import json
import re
import time
import subprocess
import dbus
import dbus.lowlevel as ll
import dbus.mainloop.glib
from gi.repository import GLib

BUS_NAME = "org.freedesktop.systemd1"
PATH_PREFIX = "/org/freedesktop/systemd1"
MGR_IFACE = "org.freedesktop.systemd1.Manager"
FWD_TIMEOUT = 25000  # ms; covers an interactive polkit prompt on a write

# systemd --user unit search dirs, highest precedence first (systemd.unit(5)).
# schema-init has no user manager, so we resolve StartUnit against these
# ourselves; missing any of them sends the call down the 120s-hang forward path.
def _user_unit_dirs():
    cfg = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    data = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    data_dirs = os.environ.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share"
    dirs = [os.path.join(cfg, "systemd/user"), "/etc/systemd/user"]
    rt = os.environ.get("XDG_RUNTIME_DIR")
    if rt:
        dirs.append(os.path.join(rt, "systemd/user"))
    dirs.append("/run/systemd/user")
    dirs.append(os.path.join(data, "systemd/user"))
    dirs += [os.path.join(d, "systemd/user") for d in data_dirs.split(":") if d]
    dirs += ["/usr/local/lib/systemd/user", "/usr/lib/systemd/user"]
    return tuple(dirs)


USER_UNIT_DIRS = _user_unit_dirs()
# StartUnit-family calls we spawn locally instead of blind-forwarding.
_START_MEMBERS = ("StartUnit", "RestartUnit", "TryRestartUnit",
                  "ReloadOrRestartUnit", "ReloadOrTryRestartUnit")
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
    # plasmashell frequently lacks DISPLAY (Xwayland exports :0 only after
    # plasmashell has spawned), so harvesting from it yields display=None and
    # every GDK_BACKEND=x11 menu-launch draws nothing. Fall back to :0 when
    # Xwayland is actually up so X11 app launches from the taskbar work.
    if not env.get("DISPLAY") and os.path.exists("/tmp/.X11-unix/X0"):
        env["DISPLAY"] = ":0"
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
    # The relay inherits a blocked SIGCHLD from the broker; a blocked mask also
    # survives execve and makes Qt's QProcess wait out its 30s timeouts.
    signal.pthread_sigmask(signal.SIG_SETMASK, [])


def _spawn(exec_path, argv, kde_env):
    env = _session_env()
    for kv in kde_env:
        if "=" in kv:
            k, v = kv.split("=", 1)
            env[k] = v
    try:
        proc = subprocess.Popen(
            argv, executable=exec_path, env=env,
            start_new_session=True, close_fds=True, preexec_fn=_child_preexec,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        _log("spawned %s pid=%d argv=%s wayland=%s display=%s"
             % (exec_path, proc.pid, argv, env.get("WAYLAND_DISPLAY"), env.get("DISPLAY")))
        return proc.pid
    except Exception as e:  # noqa: BLE001
        _log("spawn FAILED %s: %r" % (exec_path, e))
        return None


def handle_start_transient_unit(session, msg, system=None):
    global _job_seq
    try:
        args = msg.get_args_list(byte_arrays=True)
        name = str(args[0])
        props = args[2] if len(args) > 2 else []
    except Exception as e:  # noqa: BLE001
        name, props = "", []
        _log("args error: %r" % e)

    exec_path, argv, kde_env = _parse_exec_and_env(props)
    if not exec_path and system is not None and _SCOPE_RE.match(name):
        handle_scope(session, system, msg, name, props)
        return
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
    _reply_job_and_done(session, msg, name)


CGROUP_ROOT = os.environ.get("SCHEMA_CGROUP_ROOT", "/sys/fs/cgroup")
_SCOPE_RE = re.compile(r"^podman-[A-Za-z0-9_-]{1,64}\.scope$")
ATTACH_TIMEOUT = 2  # s; every rootless podman command waits on this


class ScopeRefused(Exception):
    pass


def _prop(props, key):
    for entry in props:
        if str(entry[0]) == key:
            return entry[1]
    return None


def _scope_check(name, props, sender_pid):
    if not _SCOPE_RE.match(name):
        return "not a podman scope"
    pids = _prop(props, "PIDs")
    # podman only ever asks for itself; the sender is blocked on our reply, so
    # its pid can't be recycled while we work.
    if pids is None or [int(p) for p in pids] != [sender_pid]:
        return "PIDs %r is not [sender %d]" % (pids, sender_pid)
    sl = _prop(props, "Slice")
    if sl is not None and str(sl) != "user.slice":
        return "Slice %s" % sl
    return None


def _user_slice(uid, cg_root):
    return "%s/user.slice/user-%d.slice/user@%d.service/user.slice" % (cg_root, uid, uid)


def _populated(path):
    try:
        with open(path + "/cgroup.events") as f:
            return "populated 1" in f.read()
    except OSError:
        return False


def _scope_prepare(name, uid, cg_root):
    sl = _user_slice(uid, cg_root)
    try:
        if os.stat(os.path.dirname(sl)).st_uid != uid:
            raise ScopeRefused("user@%d.service not delegated" % uid)
    except OSError as e:
        raise ScopeRefused("no user@%d.service: %s" % (uid, e))
    try:
        os.mkdir(sl)
    except FileExistsError:
        pass
    except OSError as e:
        raise ScopeRefused("mkdir %s: %s" % (sl, e))
    # .svc names aren't validated, so a hand-written name=user.slice would
    # share this cgroup: stay out if it looks like a schema-user service (its
    # process in the cgroup itself, or podman's --cgroups=split children).
    # Other children are expected -- an ad-hoc `podman run -d` from a scope
    # puts its container cgroup here as a sibling, named by container id.
    try:
        with open(sl + "/cgroup.procs") as f:
            if f.read().strip():
                raise ScopeRefused("%s holds processes" % sl)
    except FileNotFoundError:
        pass
    for e in os.scandir(sl):
        if e.is_dir(follow_symlinks=False) and (
                e.name == "runtime" or e.name.startswith("libpod-payload-")):
            raise ScopeRefused("%s is a service cgroup (%s)" % (sl, e.name))
    leaf = sl + "/" + name
    try:
        os.mkdir(leaf)
    except FileExistsError:
        if _populated(leaf):
            raise ScopeRefused("%s already in use" % leaf)
    except OSError as e:
        raise ScopeRefused("mkdir %s: %s" % (leaf, e))
    return leaf


def _scope_sweep(uid, cg_root=None):
    sl = _user_slice(uid, cg_root or CGROUP_ROOT)
    n = 0
    try:
        entries = list(os.scandir(sl))
    except OSError:
        return 0
    for e in entries:
        if e.is_dir(follow_symlinks=False) and _SCOPE_RE.match(e.name) and not _populated(e.path):
            try:
                os.rmdir(e.path)
                n += 1
            except OSError:
                pass
    return n


def _scope_request(name, props, sender_pid, uid, attach, cg_root=None):
    cg_root = cg_root or CGROUP_ROOT
    why = _scope_check(name, props, sender_pid)
    if why:
        return False, why
    try:
        leaf = _scope_prepare(name, uid, cg_root)
    except ScopeRefused as e:
        return False, str(e)
    try:
        attach(name, sender_pid)
    except Exception as e:  # noqa: BLE001 -- any failure: podman runs in place, as before
        try:
            os.rmdir(leaf)
        except OSError:
            pass
        if isinstance(e, dbus.DBusException):
            # dbus-python puts the far side's whole traceback in the message
            e = "%s: %s" % (e.get_dbus_name(), ((e.get_dbus_message() or "").strip().splitlines() or [""])[-1])
        return False, "attach: %s" % e
    _scope_sweep(uid, cg_root)
    return True, "attached"


def _sender_pid(session, sender):
    return int(session.call_blocking(
        "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "GetConnectionUnixProcessID", "s", (sender,)))


def _attach_via(system):
    def attach(name, pid):
        system.call_blocking(
            BUS_NAME, PATH_PREFIX, MGR_IFACE, "AttachProcessesToUnit", "ssau",
            ("user@%d.service" % os.getuid(), "/user.slice/" + name, [dbus.UInt32(pid)]),
            timeout=ATTACH_TIMEOUT)
    return attach


def handle_scope(session, system, msg, name, props):
    try:
        pid = _sender_pid(session, msg.get_sender())
    except dbus.DBusException as e:
        pid, ok, why = 0, False, "sender pid: %s" % e
    else:
        ok, why = _scope_request(name, props, pid, os.getuid(), _attach_via(system))
    _log("scope %s pid=%d -> %s" % (name, pid, why if ok else "refused (%s)" % why))
    if ok:
        _reply_job_and_done(session, msg, name)
    else:
        session.send_message(ll.ErrorMessage(
            msg, "org.freedesktop.DBus.Error.NotSupported",
            "schema-init does not manage transient scopes"))


def _reply_job_and_done(session, msg, name):
    # Reply with a synthetic job object path, then emit JobNew/JobRemoved(done)
    # so a launcher subscribed on the returned job sees the unit finish.
    global _job_seq
    _job_seq += 1
    job_id = _job_seq
    job_path = "%s/job/%d" % (PATH_PREFIX, job_id)

    ret = ll.MethodReturnMessage(msg)
    ret.append(dbus.ObjectPath(job_path), signature="o")
    session.send_message(ret)

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


def _find_user_unit(name):
    if not name.endswith((".service", ".target", ".socket", ".scope")):
        name = name + ".service"
    # An instance-specific file wins anywhere on the path, then the template --
    # systemd's order for foo@bar.service.
    cands = [name]
    stem, dot, suf = name.rpartition(".")
    if stem.endswith("@"):
        return None  # systemd refuses to start a bare template
    if "@" in stem:
        cands.append(stem.split("@", 1)[0] + "@." + suf)
    for c in cands:
        for d in USER_UNIT_DIRS:
            path = os.path.join(d, c)
            if os.path.isfile(path):
                return path
    return None


def _unit_dropins(name, unit_file=None):
    # systemd's drop-in .conf files for a unit, applied in filename order. Dirs:
    # <unit>.d, its template's, each dash prefix's (foo-.service.d) and the type's
    # (service.d), most specific first; a filename seen first (higher-priority unit
    # dir, then more specific) shadows later ones, so a /dev/null link masks one.
    # An alias also takes the drop-ins of the unit it links to, as systemd does.
    name = _unit_name(name)
    stem, _, suf = name.rpartition(".")
    names = [name]
    if "@" in stem:
        names.append(stem.split("@", 1)[0] + "@." + suf)
    real = os.path.basename(os.path.realpath(unit_file)) if unit_file else name
    if real not in names and not real.startswith(stem.split("@", 1)[0] + "@."):
        names.append(real)
    base = stem.split("@", 1)[0]
    parts = base.split("-")
    names += ["-".join(parts[:i]) + "-." + suf for i in range(len(parts) - 1, 0, -1)]
    names.append(suf)
    found = {}
    for d in USER_UNIT_DIRS:
        for n in names:
            dd = os.path.join(d, n + ".d")
            try:
                ents = os.listdir(dd)
            except OSError:
                continue
            for e in ents:
                if e.endswith(".conf") and not e.startswith(".") and e not in found:
                    found[e] = os.path.join(dd, e)
    return [found[k] for k in sorted(found)]


def _unescape(s):
    return re.sub(r"\\x([0-9a-fA-F]{2})", lambda m: chr(int(m.group(1), 16)), s.replace("-", "/"))


def _path_unescape(s):
    return "/" if s == "-" else "/" + _unescape(s)


def _parse_unit_execstart(path, dropins=()):
    # Return (cmds, environment_list, type) from a unit's [Service] section and
    # then its drop-ins; cmds = [(ignore_failure, no_vars, exec_path, argv)]. An
    # empty ExecStart=/Environment= resets the list, as in systemd. '@' means the
    # next word is argv[0].
    cmds, env, stype = [], [], "simple"
    lines = []
    for i, p in enumerate((path,) + tuple(dropins)):
        try:
            with open(p) as f:
                raw = f.read()
        except OSError as e:
            _log("unit read error %s: %r" % (p, e))
            if i == 0:
                return [], [], stype
            continue
        lines.append("[]")   # each file opens its own sections
        lines += raw.replace("\\\n", " ").splitlines()
    section = None
    for line in lines:
        line = line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            continue
        if section != "Service" or "=" not in line:
            continue
        key, val = line.split("=", 1)
        key, val = key.strip(), val.strip()
        if key == "ExecStart":
            if not val:
                cmds = []
                continue
            pre = ""
            while val and val[0] in "-@+!:":
                pre += val[0]
                val = val[1:]
            try:
                argv = shlex.split(val)
            except ValueError:
                argv = val.split()
            if not argv:
                continue
            exec_path = argv[0]
            if "@" in pre:
                argv = argv[1:] or [exec_path]
            cmds.append(("-" in pre, ":" in pre, exec_path, argv))
        elif key == "Environment":
            if not val:
                env = []
                continue
            try:
                env.extend(shlex.split(val))
            except ValueError:
                env.extend(val.split())
        elif key == "Type":
            stype = val or "simple"
    return cmds, env, stype


# Runs a oneshot's ExecStart= list in order as one tracked pid (the last step
# is exec'd into it unless '-' makes its failure moot); a non-'-' failure stops
# the rest with sh's exit code.
# execvp, not sh, so an '@' argv[0] reaches each step.
_CHAIN = """import os, sys, json, signal
# python ignores these at startup and the steps would inherit it
signal.signal(signal.SIGPIPE, signal.SIG_DFL)
signal.signal(signal.SIGXFSZ, signal.SIG_DFL)
steps = json.loads(sys.argv[1])
for i, (ign, exe, argv) in enumerate(steps):
    if i == len(steps) - 1 and not ign:
        try:
            os.execvp(exe, argv)
        except Exception:
            sys.exit(127)
    pid = os.fork()
    if pid == 0:
        try:
            os.execvp(exe, argv)
        finally:
            os._exit(127)
    rc = os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1])
    if rc and not ign:
        sys.exit(rc if rc > 0 else 128 - rc)
"""


def _plan(cmds, stype="simple"):
    # One spawnable (exec_path, argv) for the unit: None if there is no command,
    # "multi" if several ExecStart= on a non-oneshot (systemd refuses to load it).
    if not cmds:
        return None
    if len(cmds) == 1:
        return cmds[0][-2], cmds[0][-1]
    if stype != "oneshot":
        return "multi"
    steps = [(ignore, exe, argv) for ignore, _, exe, argv in cmds]
    return sys.executable, [sys.executable, "-I", "-c", _CHAIN, json.dumps(steps)]


# --- unit-string expansion (systemd % specifiers + $VAR) -------------------
# Portal .service ExecStart lines routinely use %h/%t/$XDG_* -- with no real
# user manager to expand them, an unexpanded "%h/.local/bin/foo" is spawned
# verbatim and ENOENTs. Expand the common specifiers and env-var references
# ourselves, matching systemd's order (specifiers first, then variables).
# systemd only expands valid variable names: $0, $1 reach the command untouched
_VAR_RE = re.compile(r"\$\{([A-Za-z_]\w*)\}|\$([A-Za-z_]\w*)")


def _username():
    try:
        import pwd
        return pwd.getpwuid(os.getuid()).pw_name
    except Exception:  # noqa: BLE001
        return os.environ.get("USER") or os.environ.get("LOGNAME") or str(os.getuid())


def _runtime_dir():
    return os.environ.get("XDG_RUNTIME_DIR") or ("/run/user/%d" % os.getuid())


def _expand(s, env, unit_name="", variables=True):
    base = unit_name
    for suf in (".service", ".target", ".socket", ".scope"):
        if base.endswith(suf):
            base = base[:-len(suf)]
            break
    prefix, _, inst = base.partition("@")
    specifiers = {
        "i": lambda: inst,
        "I": lambda: _unescape(inst),
        "p": lambda: prefix,
        "P": lambda: _unescape(prefix),
        "f": lambda: _path_unescape(inst or prefix),
        "h": lambda: os.path.expanduser("~"),
        "t": _runtime_dir,
        "U": lambda: str(os.getuid()),
        "u": _username,
        "n": lambda: unit_name,
        "N": lambda: base,
        "%": lambda: "%",
    }
    out, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if c == "%" and i + 1 < n and s[i + 1] in specifiers:
            out.append(specifiers[s[i + 1]]())
            i += 2
            continue
        out.append(c)
        i += 1
    expanded = "".join(out)
    if not variables:  # ExecStart=: prefix -- specifiers only, $VAR left literal
        return expanded
    return _VAR_RE.sub(lambda m: env.get(m.group(1) or m.group(2), ""), expanded)


# --- spawned-unit tracking (for RestartUnit et al.) -------------------------
# StartUnit-family calls all resolve to a fire-and-forget spawn; without
# tracking, RestartUnit(foo) would start a SECOND foo beside the first. Track
# (pid, starttime) per unit so restarts stop the old one first. Children are
# auto-reaped (SIGCHLD=SIG_IGN), so liveness is os.kill(pid,0) -- but a bare pid
# is reuse-unsafe (a recycled pid would read as "alive" and get SIGTERM'd), so
# we pin it with /proc/pid/stat starttime, which is unique per pid incarnation.
_spawned = {}  # unit name -> (pid, starttime)


def _proc_starttime(pid):
    # Field 22 of /proc/pid/stat. comm (field 2) can hold spaces/parens, so key
    # off the last ')': the remaining fields start at field 3 (state) = index 0.
    try:
        with open("/proc/%d/stat" % pid) as f:
            data = f.read()
    except OSError:
        return None
    rp = data.rfind(")")
    if rp < 0:
        return None
    fields = data[rp + 2:].split()
    return fields[19] if len(fields) > 19 else None


def _unit_alive(name):
    rec = _spawned.get(name)
    if not rec:
        return False
    pid, st = rec
    try:
        os.kill(pid, 0)
    except OSError:
        _spawned.pop(name, None)
        return False
    if _proc_starttime(pid) != st:   # pid recycled to an unrelated process
        _spawned.pop(name, None)
        return False
    return True


def _signal_group(pid, sig):
    # Spawns lead their own session (pgid == pid), so signal the group: a oneshot
    # chain's sh would otherwise die and leave its running step behind. Fall back
    # to the pid when it is not a group leader (it setsid'd away, or a test child).
    try:
        if os.getpgid(pid) == pid:
            os.killpg(pid, sig)
        else:
            os.kill(pid, sig)
        return True
    except OSError:
        return False


def _stop_unit(name):
    rec = _spawned.pop(name, None)
    if not rec:
        return False
    pid, st = rec
    if _proc_starttime(pid) != st:   # gone or recycled -> never signal a stranger
        return False
    if not _signal_group(pid, signal.SIGTERM):
        return False
    # Wait (bounded) for it to exit before the respawn races it for the unit's
    # well-known bus name; escalate to SIGKILL if it will not go.
    for _ in range(10):              # ~0.5s
        if _proc_starttime(pid) != st:
            break
        try:
            os.kill(pid, 0)
        except OSError:
            break
        time.sleep(0.05)
    else:
        _signal_group(pid, signal.SIGKILL)
    _log("stopped %s pid=%d" % (name, pid))
    return True


def _restart_action(member, alive):
    # -> "spawn" | "restart" | "noop", matching systemd: StartUnit on a running
    # unit is a no-op; Try* variants only act on a live unit.
    restart = member in ("RestartUnit", "TryRestartUnit",
                         "ReloadOrRestartUnit", "ReloadOrTryRestartUnit")
    if alive:
        return "restart" if restart else "noop"
    try_only = member in ("TryRestartUnit", "ReloadOrTryRestartUnit")
    return "noop" if try_only else "spawn"


# --- units schema-user runs: hand them to it -----------------------------------
# One supervisor, not two: a unit schema-user generated is started/stopped by
# schema-user, never spawned here beside it. Anything it does not run (or a
# schema-user that is not up) keeps the paths below.
_UP_STATES = ("FUNDAMENTAL", "FRICTION", "SETTLED", "NEW_PROCESS",
              "RECOVERY", "FULL_TRUST", "PERFECT")


def _schema_user_ctl(*args):
    # (exit status, stdout) of `schema-ctl --user ...`; status None = no run
    ctl = os.environ.get("SCHEMA_CTL") or "schema-ctl"
    try:
        r = subprocess.run([ctl, "--user"] + list(args), stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=10, text=True)
    except (OSError, subprocess.SubprocessError):
        return None, ""
    return r.returncode, r.stdout


def _unit_name(name):
    return (name if name.endswith((".service", ".target", ".socket", ".scope"))
            else name + ".service")


def _schema_user_svc(unit_name):
    # the .svc schema-user runs this unit as, from schema-import --user's map;
    # an Alias= (dbus-org.*) resolves through the file it links to
    rt = os.environ.get("XDG_RUNTIME_DIR")
    if not rt or not unit_name.endswith(".service"):
        return None
    try:
        with open(os.path.join(rt, "schema-user/units")) as f:
            m = dict(ln.rstrip("\n").split("\t", 1) for ln in f if "\t" in ln)
    except OSError:
        return None
    if unit_name in m:
        return m[unit_name]
    # instances resolve to their template file, which must not alias-match
    path = None if "@" in unit_name else _find_user_unit(unit_name)
    return m.get(os.path.basename(os.path.realpath(path))) if path else None


def _schema_user_verb(member, state):
    # schema-ctl verb for a manager call on a unit in `state`; None = nothing to do
    if member == "StopUnit":
        return "stop"
    return {"spawn": "start", "restart": "restart"}.get(
        _restart_action(member, state in _UP_STATES))


def handle_schema_user_unit(session, msg, member):
    try:
        name = str(msg.get_args_list(byte_arrays=True)[0])
    except Exception:  # noqa: BLE001
        return False
    svc = _schema_user_svc(_unit_name(name))
    if not svc:
        return False
    rc, out = _schema_user_ctl("status", svc)
    m = rc == 0 and re.search(r"\bstate=(\w+)", out)
    if not m:
        return False            # schema-user not up, or not running it (yet)
    verb = _schema_user_verb(member, m.group(1))
    _log("%s(%s): schema-user %s %s (was %s)" % (member, name, verb or "no-op", svc, m.group(1)))
    if verb and _schema_user_ctl(verb, svc)[0] != 0:
        session.send_message(ll.ErrorMessage(
            msg, "org.freedesktop.DBus.Error.Failed", "schema-user: %s %s failed" % (verb, svc)))
        return True
    _reply_job_and_done(session, msg, name)
    return True


def handle_start_unit(session, msg, member="StartUnit"):
    # KDE portal .service files carry SystemdService=, so dbus delegates their
    # activation to org.freedesktop.systemd1.Manager.StartUnit on the session
    # bus. With no systemd --user, blind-forwarding that to the system bus never
    # spawns the *user* binary -> the well-known name never appears -> dbus
    # StartServiceByName waits the full 120s. So we ARE the user manager here:
    # resolve the unit file, spawn its ExecStart in the live session, ack the job.
    try:
        name = str(msg.get_args_list(byte_arrays=True)[0])
    except Exception as e:  # noqa: BLE001
        _log("%s args error: %r" % (member, e))
        return False
    unit_name = _unit_name(name)

    action = _restart_action(member, _unit_alive(unit_name))
    if action == "noop":
        # StartUnit on an already-running unit, or Try*Restart on one that is not
        # running: systemd does nothing but succeeds. Ack the job (so the caller
        # does not block) and spawn nothing.
        _log("%s(%s): no-op ack (already running / nothing to restart)" % (member, name))
        _reply_job_and_done(session, msg, name)
        return True

    unit_file = _find_user_unit(name)
    if not unit_file:
        _log("%s(%s): no user unit file -> forward" % (member, name))
        return False
    cmds, unit_env, stype = _parse_unit_execstart(unit_file, _unit_dropins(unit_name, unit_file))
    if not cmds:
        _log("%s(%s): no ExecStart in %s -> forward" % (member, name, unit_file))
        return False

    # Expand % specifiers and $VAR against the unit's Environment= layered over
    # the live session env, so ExecStart tokens like %h / $XDG_RUNTIME_DIR resolve.
    scratch = _session_env()
    expanded_env = []
    for kv in unit_env:
        if "=" not in kv:
            continue
        k, v = kv.split("=", 1)
        v = _expand(v, scratch, unit_name)
        scratch[k] = v
        expanded_env.append("%s=%s" % (k, v))
    plan = _plan([(ign, nv, _expand(exe, scratch, unit_name, not nv),
                   [_expand(a, scratch, unit_name, not nv) for a in av])
                  for ign, nv, exe, av in cmds], stype)
    if plan == "multi":
        session.send_message(ll.ErrorMessage(
            msg, "org.freedesktop.DBus.Error.Failed",
            "schema-init: %s has more than one ExecStart= but is not Type=oneshot" % unit_name))
        return True
    exec_path, argv = plan

    if action == "restart":
        _stop_unit(unit_name)
    _log("%s(%s): spawning %s from %s" % (member, name, exec_path, unit_file))
    pid = _spawn(exec_path, argv, expanded_env)
    if not pid:
        # Do not fake a done job for a launch that never happened (would mask the
        # failure); report it so the caller sees a failed activation, as systemd would.
        session.send_message(ll.ErrorMessage(
            msg, "org.freedesktop.DBus.Error.Failed",
            "schema-init: failed to spawn %s" % exec_path))
        return True
    _spawned[unit_name] = (pid, _proc_starttime(pid))
    _reply_job_and_done(session, msg, name)
    return True


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
        member = msg.get_member()
        if path == PATH_PREFIX and msg.get_interface() == MGR_IFACE:
            if member == "StartTransientUnit":
                handle_start_transient_unit(session, msg, system)
                return ll.HANDLER_RESULT_HANDLED
            if (member in _START_MEMBERS or member == "StopUnit") and \
                    handle_schema_user_unit(session, msg, member):
                return ll.HANDLER_RESULT_HANDLED
            if member in _START_MEMBERS and handle_start_unit(session, msg, member):
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

    GLib.timeout_add_seconds(60, lambda: _scope_sweep(os.getuid()) or True)
    GLib.MainLoop().run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
