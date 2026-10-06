#!/usr/bin/env python3
"""schema-import — drain the systemctl shim's enable-intent queue into schema
service files. Phase 2 of the compat translator: the runtime importer.

The shim (schema-systemctl) records `enable`/`preset` intent to
pending.list; this tool reads each queued unit, parses its [Service]/[Install]
sections, and emits a schema `.svc` on 80/20 field coverage. Ratholes
(Type=forking without PIDFile=, templates, missing ExecStart) are logged and skipped,
never half-translated. Type=notify maps to notify=1 (PID 1 speaks sd_notify);
Type=dbus maps to ready_bus_name=<BusName> (the schema-dbus broker reports it).

Stdlib only. MIGRATE_ROOT prefixes filesystem paths (tests inject a temp tree);
SCHEMA_STATE_DIR / SCHEMA_SVC_DIR override the queue and output locations, and
match the shim's own env knobs.
"""
import argparse
import os
import re
import shlex
import sys

_VAR_RE = re.compile(r"\$\{?(\w+)\}?")
_BRACED_RE = re.compile(r"\$\{(\w+)\}")

# ---------------------------------------------------------------------------
# Pure translation core (no filesystem) — unit-tested in test_import_units.py.
# ---------------------------------------------------------------------------

_EXEC_PREFIX = "@-:+!"
WARN_PREFIX = "# schema-import WARN: "   # systemd ExecStart special-char prefixes, stripped


def parse_unit(text):
    """Parse a systemd unit file into {section: [(key, value), ...]}.

    Handles backslash line-continuation and duplicate keys (kept in order).
    Comments (# or ;) and blank lines are dropped. Values are returned raw.
    """
    sections = {}
    cur = None
    pending = ""
    for raw in text.splitlines():
        line = raw.strip()
        if pending:
            line = pending + " " + line
            pending = ""
        if line.endswith("\\"):
            pending = line[:-1].rstrip()
            continue
        if not line or line[0] in "#;":
            continue
        if line.startswith("[") and line.endswith("]"):
            cur = line[1:-1]
            sections.setdefault(cur, [])
            continue
        if cur is None or "=" not in line:
            continue
        key, val = line.split("=", 1)
        sections[cur].append((key.strip(), val.strip()))
    return sections


def _get_last(section, key):
    """Last value for a key in a section (systemd 'last assignment wins'), or ''."""
    out = ""
    for k, v in section:
        if k == key:
            out = v
    return out


def _get_all(section, key):
    return [v for k, v in section if k == key]


def _exec_tokens(execstart):
    """argv list from an ExecStart value: strip leading @-:+! prefix chars off
    the executable, then shell-split. Returns [] if empty."""
    s = execstart.strip()
    while s and s[0] in _EXEC_PREFIX:
        s = s[1:].lstrip()
    if not s:
        return []
    try:
        return shlex.split(s)
    except ValueError:
        return s.split()


def _expand_argv(argv, env):
    """Resolve $VAR / ${VAR} tokens against captured Environment= values, since
    schema-init exec()s with no shell. A token that stays unresolved is dropped — matching systemd,
    which expands an unset variable to nothing. Returns (argv, dropped_count)."""
    out, dropped = [], 0
    for tok in argv:
        if "$" not in tok:
            out.append(tok)
            continue
        # systemd expands $FOO only as a whole word; inside a word only ${FOO}
        # (a bare $FOO there is left for the shell of an sh -c script).
        pat = _VAR_RE if re.fullmatch(r"\$\{?\w+\}?", tok) else _BRACED_RE
        rep = pat.sub(lambda m: env.get(m.group(1), "\0"), tok)
        if "\0" in rep:
            dropped += 1
            continue
        if rep:
            out.append(rep)
    return out, dropped


def _env_pairs(values):
    """Flatten Environment= lines into KEY=VALUE strings, honoring quoting and
    multiple pairs per line. Tokens without '=' are dropped."""
    out = []
    for v in values:
        try:
            toks = shlex.split(v)
        except ValueError:
            toks = v.split()
        for t in toks:
            if "=" in t:
                out.append(t)
    return out


_SAFE_RE = re.compile(r"[\w@%+=:,./${}-]+")
_PRE_SEARCH = ("usr/local/sbin", "usr/local/bin", "usr/sbin", "usr/bin", "sbin", "bin")


def _cmd_quote(tok):
    if _SAFE_RE.fullmatch(tok):
        return tok
    return '"%s"' % tok.replace("\\", "\\\\").replace('"', '\\"')


def _which(cmd):
    root = os.environ.get("MIGRATE_ROOT") or "/"
    for d in _PRE_SEARCH:
        if os.access(os.path.join(root, d, cmd), os.X_OK):
            return "/" + d + "/" + cmd
    return None


def _exec_pre(svc):
    out = []
    for v in _get_all(svc, "ExecStartPre"):
        if not v.strip():
            out = []
            continue
        s = v.strip()
        pre = ""
        while s and s[0] in _EXEC_PREFIX:
            pre += s[0]
            s = s[1:]
        if set(pre) - set("+-"):
            raise Skip("ExecStartPre=%s uses a prefix schema-init can't honour" % v)
        try:
            argv = shlex.split(s)
        except ValueError:
            raise Skip("ExecStartPre=%s has unbalanced quotes" % v)
        if not argv:
            continue
        if not argv[0].startswith("/"):
            exe = _which(argv[0])
            if not exe:
                raise Skip("ExecStartPre command %s not found" % argv[0])
            argv[0] = exe
        out.append(("".join(sorted(set(pre))), argv))
    if len(out) > 8:
        raise Skip("%d ExecStartPre= lines (limit 8)" % len(out))
    return out


class Skip(Exception):
    """Raised when a unit falls in a known rathole and must not be translated."""


_TRUE = ("1", "yes", "true", "on")
_FALSE = ("", "0", "no", "false", "off")


def _hardening(svc):
    """The four hardening knobs, always explicit so the unit means the same
    thing whatever the host's hardening-default is. Returns (lines, warnings);
    a warning names each directive mapped weaker than the unit asked."""
    lines, warns = [], []

    def boolean(directive, key):
        v = _get_last(svc, directive).lower()
        if v in _TRUE:
            lines.append("%s=1" % key)
            return
        if v not in _FALSE:
            warns.append("%s=%s unrecognised, mapped to %s=0" % (directive, v, key))
        lines.append("%s=0" % key)

    boolean("NoNewPrivileges", "no_new_privs")
    boolean("PrivateTmp", "private_tmp")

    v = _get_last(svc, "ProtectSystem").lower()
    if v in _TRUE:
        lines.append("protect_system=1")
    elif v == "full":
        lines.append("protect_system=full")
    elif v == "strict":
        lines.append("protect_system=full")
        warns.append("ProtectSystem=strict mapped to protect_system=full (/ not read-only)")
    else:
        if v not in _FALSE:
            warns.append("ProtectSystem=%s unrecognised, mapped to protect_system=0" % v)
        lines.append("protect_system=0")

    v = _get_last(svc, "ProtectHome").lower()
    if v in _TRUE:
        lines.append("protect_home=1")
    else:
        if v not in _FALSE:
            warns.append("ProtectHome=%s has no equivalent, mapped to protect_home=0" % v)
        lines.append("protect_home=0")
    return lines, warns


_SPAN_UNITS = {"us": 1e-6, "usec": 1e-6, "ms": 1e-3, "msec": 1e-3, "": 1, "s": 1,
               "sec": 1, "second": 1, "seconds": 1, "m": 60, "min": 60,
               "minute": 60, "minutes": 60, "h": 3600, "hr": 3600,
               "hour": 3600, "hours": 3600, "d": 86400, "day": 86400,
               "days": 86400, "w": 604800, "week": 604800, "weeks": 604800}


def _timespan_sec(v):
    """systemd timespan ('30', '1min 30s', '500ms') to whole seconds, rounded up.
    None when unparseable or infinity."""
    v = v.strip().lower()
    if not v or v == "infinity":
        return None
    total = 0.0
    for num, unit in re.findall(r"(\d+(?:\.\d+)?)\s*([a-z]*)", v):
        if unit not in _SPAN_UNITS:
            return None
        total += float(num) * _SPAN_UNITS[unit]
    if not re.fullmatch(r"(\s*\d+(?:\.\d+)?\s*[a-z]*)+", v):
        return None
    sec = int(total)
    return sec + (1 if total > sec else 0)


_DOW = ("mon", "tue", "wed", "thu", "fri", "sat", "sun")


def _calendar(v):
    """systemd OnCalendar= to a schema on_calendar= value ("HH:MM", "Mon HH:MM",
    "15 HH:MM"); "hourly" to ("interval", 3600). None if not expressible."""
    v = " ".join(v.split()).lower()
    short = {"daily": "00:00", "midnight": "00:00", "weekly": "Mon 00:00",
             "monthly": "1 00:00"}
    if v in short:
        return short[v]
    if v in ("hourly", "*:00", "*:00:00", "*-*-* *:00", "*-*-* *:00:00"):
        return ("interval", 3600)
    m = re.fullmatch(r"(?:([a-z]{3})[a-z]*\s+)?(?:\*-\*-(\*|\d{1,2})\s+)?"
                     r"(\d{1,2}):(\d{2})(?::00)?", v)
    if not m:
        return None
    dow, dom, h, mi = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
    if h > 23 or mi > 59 or (dow and dow not in _DOW):
        return None
    hm = "%02d:%02d" % (h, mi)
    if dow and dom not in (None, "*"):
        return None
    if dow:
        return "%s %s" % (dow.capitalize(), hm)
    if dom not in (None, "*"):
        if not 1 <= int(dom) <= 31:
            return None
        return "%d %s" % (int(dom), hm)
    return hm


def timer_lines(sections):
    """[Timer] keys to schema timer lines plus notes. Raises Skip when the
    schedule has no schema equivalent."""
    t = sections.get("Timer", [])
    cal = _get_last(t, "OnCalendar")
    boot = _get_last(t, "OnBootSec") or _get_last(t, "OnStartupSec")
    active = _get_last(t, "OnUnitActiveSec") or _get_last(t, "OnUnitInactiveSec")
    lines, notes = [], []
    if len(_get_all(t, "OnCalendar")) > 1:
        raise Skip("several OnCalendar= — schema has one schedule per service")
    if cal:
        c = _calendar(cal)
        if c is None:
            raise Skip("OnCalendar=%s has no schema on_calendar equivalent" % cal)
        if isinstance(c, tuple):
            lines += ["on_boot_sec=%d" % c[1], "on_active_sec=%d" % c[1]]
        else:
            lines.append("on_calendar=%s" % c)
        if boot or active:
            notes.append("OnBootSec/OnUnit*Sec ignored next to OnCalendar")
    else:
        if not boot and not active:
            raise Skip("timer has no OnCalendar=, OnBootSec= or OnUnitActiveSec=")
        for key, val in (("on_boot_sec", boot), ("on_active_sec", active)):
            if val:
                sec = _timespan_sec(val)
                if sec is None:
                    raise Skip("%s timespan '%s' not understood" % (key, val))
                lines.append("%s=%d" % (key, sec))
        if active and not boot:
            lines.insert(0, "on_boot_sec=%d" % _timespan_sec(active))
    if _get_last(t, "Persistent").lower() in ("yes", "true", "1", "on"):
        if any(ln.startswith("on_calendar=") for ln in lines):
            lines.append("persistent=1")
        else:
            notes.append("Persistent= dropped (catch-up applies to calendar timers only)")
    for k in ("RandomizedDelaySec", "AccuracySec"):
        if _get_last(t, k):
            notes.append("dropped %s" % k)
    return lines, notes


def timer_to_svc(name, timer_sections, svc_sections, known=None):
    """A .timer and the .service it starts as one schema timer .svc, named after
    the timer."""
    tlines, tnotes = timer_lines(timer_sections)
    tconds = condition_lines(timer_sections)
    sconds = condition_lines(svc_sections)
    if any(":|" in c for c in tconds) and any(":|" in c for c in sconds):
        raise Skip("timer and service both have |-conditions — schema has one group per service")
    if len(tconds) + len(sconds) > 8:
        raise Skip("%d conditions across timer and service (limit 8)" % (len(tconds) + len(sconds)))
    tlines += tconds
    body = unit_to_svc(name, svc_sections, known=known)
    out = []
    for ln in body.splitlines():
        if ln.startswith("# schema-import: "):
            kept = [n for n in ln[len("# schema-import: "):].split("; ")
                    if n != "no [Install] section"]
            kept += tnotes
            tnotes = []
            if kept:
                out.append("# schema-import: " + "; ".join(kept))
        elif ln in ("no_restart=1", "oneshot=1"):
            continue
        else:
            out.append(ln)
    if tnotes:
        out.insert(0, "# schema-import: " + "; ".join(tnotes))
    return "\n".join(out + tlines) + "\n"


_LISTEN_KINDS = {"ListenStream": "stream", "ListenDatagram": "dgram",
                 "ListenSequentialPacket": "seqpacket", "ListenFIFO": "fifo"}
_LISTEN_ADDR = re.compile(r"/\S+|@\S+|\d+|[\d.]+:\d+|\[[0-9a-fA-F:.]+\]:\d+")


def socket_lines(sections):
    """[Socket] keys to schema listen=/socket_* lines plus notes. Raises Skip
    when a listener has no schema equivalent."""
    sock = sections.get("Socket", [])
    if _get_last(sock, "Accept").lower() in _TRUE:
        raise Skip("Accept=yes starts one service instance per connection — instances unsupported")
    listens, lines, notes = [], [], []
    for k, v in sock:
        if k in _LISTEN_KINDS:
            if v == "":
                listens = []
                continue
            v = v.replace("%t", "/run")
            if "%" in v or not _LISTEN_ADDR.fullmatch(v) or (k == "ListenFIFO" and v[0] != "/"):
                raise Skip("%s=%s not understood" % (k, v))
            listens.append("listen=%s:%s" % (_LISTEN_KINDS[k], v))
        elif k.startswith("Listen"):
            raise Skip("%s= has no schema equivalent" % k)
        elif k == "SocketMode":
            lines.append("socket_mode=%s" % v)
        elif k in ("SocketUser", "SocketGroup"):
            lines.append("socket_%s=%s" % (k[6:].lower(), v))
        elif k not in ("Service", "Accept") and "dropped %s" % k not in notes:
            notes.append("dropped %s" % k)
    if not listens:
        raise Skip("socket has no Listen*= line")
    if len(listens) > 4:
        raise Skip("%d Listen*= lines (limit 4)" % len(listens))
    return listens + lines, notes


_COND_RUNTIME = {
    "PathExists": "path_exists", "PathExistsGlob": "path_exists_glob",
    "PathIsDirectory": "path_is_dir", "PathIsSymbolicLink": "path_is_symlink",
    "PathIsMountPoint": "path_is_mount", "PathIsReadWrite": "path_is_rw",
    "DirectoryNotEmpty": "dir_not_empty", "FileNotEmpty": "file_not_empty",
    "FileIsExecutable": "file_is_exec", "ACPower": "ac_power",
    "KernelCommandLine": "kernel_cmdline",
}
_COND_STATIC = ("Virtualization", "Security", "ControlGroupController", "Capability",
                "CPUs", "FirstBoot", "Architecture")
_ARCH = {"x86_64": "x86-64", "aarch64": "arm64", "i686": "x86", "armv7l": "arm",
         "riscv64": "riscv64", "ppc64le": "ppc64-le", "s390x": "s390x"}
_FACTS = None   # tests inject host facts here


def _read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def host_facts():
    """What the machine-fixed conditions are judged against: this host."""
    global _FACTS
    if _FACTS is not None:
        return _FACTS
    import glob
    import platform
    import subprocess

    def detect(*a):
        try:
            r = subprocess.run(["systemd-detect-virt"] + list(a), capture_output=True, text=True)
            return r.returncode, r.stdout.strip()
        except OSError:
            return None, ""
    rc, virt = detect()
    if rc is None:
        flags = _read("/proc/cpuinfo")
        virt, kind = ("vm-other", "vm") if " hypervisor" in flags else ("none", None)
    else:
        virt = virt or "none"
        kind = "vm" if detect("--vm")[0] == 0 else "container" if detect("--container")[0] == 0 else None
    sb = glob.glob("/sys/firmware/efi/efivars/SecureBoot-*")
    sec = set()
    if os.path.exists("/sys/fs/selinux/enforce"):
        sec.add("selinux")
    if _read("/sys/module/apparmor/parameters/enabled") == "Y":
        sec.add("apparmor")
    if os.path.exists("/proc/self/loginuid"):
        sec.add("audit")
    if os.path.isdir("/sys/fs/smackfs"):
        sec.add("smack")
    if glob.glob("/sys/class/tpmrm/*"):
        sec.add("tpm2")
    try:
        if sb and open(sb[0], "rb").read()[-1:] == b"\x01":
            sec.add("uefi-secureboot")
    except OSError:
        pass
    ctl = _read("/sys/fs/cgroup/cgroup.controllers", None)
    _FACTS = {"virt": virt, "virt_kind": kind, "security": sec,
              "cgroup_v2": ctl is not None, "controllers": set((ctl or "").split()),
              "cpus": len(os.sched_getaffinity(0)),
              "arch": _ARCH.get(platform.machine(), platform.machine())}
    return _FACTS


def _static_condition(name, arg, facts):
    """True/False for a machine-fixed condition on this host; None if the value
    is not one we can judge."""
    a = arg.strip().lower()
    if name == "Virtualization":
        if a in _TRUE:
            return facts["virt"] != "none"
        if a in _FALSE:
            return facts["virt"] == "none"
        if a in ("vm", "container"):
            return facts["virt_kind"] == a
        if a == "private-users":
            return None
        return facts["virt"] == a
    if name == "Security":
        known = ("selinux", "apparmor", "audit", "smack", "tpm2", "uefi-secureboot")
        return a in facts["security"] if a in known else None
    if name == "ControlGroupController":
        if a == "v2":
            return facts["cgroup_v2"]
        if a == "v1":
            return not facts["cgroup_v2"]
        return all(c in facts["controllers"] for c in a.split())
    if name == "Capability":
        return True                       # PID 1 starts services as root, all caps
    if name == "CPUs":
        m = re.fullmatch(r"(<=|>=|!=|<|>|=)?\s*(\d+)", a)
        if not m:
            return None
        op, n, c = m.group(1) or "=", int(m.group(2)), facts["cpus"]
        return {"<=": c <= n, ">=": c >= n, "!=": c != n, "<": c < n, ">": c > n, "=": c == n}[op]
    if name == "FirstBoot":
        return a not in _TRUE             # a migrated system is past its first boot
    if name == "Architecture":
        return a == "native" or a == facts["arch"]
    return None


def _device_path(unit):
    """dev-virtio\\x2dports-org.qemu.guest_agent.0.device -> /dev/virtio-ports/org.qemu.guest_agent.0"""
    body = unit[:-len(".device")]
    out = re.sub(r"\\x([0-9a-fA-F]{2})", lambda m: "\0" + m.group(1), body)
    out = out.replace("-", "/")
    out = re.sub(r"\0([0-9a-fA-F]{2})", lambda m: chr(int(m.group(1), 16)), out)
    return "/" + out


def condition_lines(sections, facts=None):
    """[Unit] Condition*/Assert* (and BindsTo/Requires on a .device) to schema
    condition= lines. Machine-fixed conditions are judged here, against this
    host. Raises Skip when one fails or has no equivalent."""
    unit = sections.get("Unit", [])
    conds = []
    for k, v in unit:
        if k.startswith("Condition") or k.startswith("Assert"):
            if v == "":
                conds = []
            else:
                conds.append((k[len("Condition"):] if k.startswith("Condition") else k[len("Assert"):], k, v))
    for k in ("BindsTo", "Requires"):
        for v in _get_all(unit, k):
            for dev in v.split():
                if dev.endswith(".device"):
                    if not dev.startswith("dev-"):
                        raise Skip("%s=%s: only /dev device units map to a path" % (k, dev))
                    conds.append(("PathExists", k, _device_path(dev)))
    lines, static_trig, static_any = [], 0, False
    facts = facts or None
    for name, key, v in conds:
        trig = v.startswith("|")
        rest = v[1:] if trig else v
        neg = rest.startswith("!")
        arg = rest[1:] if neg else rest
        if name in _COND_RUNTIME:
            if "%" in arg or not arg:
                raise Skip("%s=%s uses a specifier" % (key, v))
            lines.append("condition=%s:%s%s%s" % (_COND_RUNTIME[name], "|" if trig else "",
                                                  "!" if neg else "", arg))
        elif name in _COND_STATIC:
            facts = facts or host_facts()
            r = _static_condition(name, arg, facts)
            if r is None:
                raise Skip("%s=%s not understood" % (key, v))
            r = r != neg
            if trig:
                static_trig += 1
                static_any |= r
            elif not r:
                raise Skip("%s=%s does not hold on this host" % (key, v))
        else:
            raise Skip("%s= has no schema equivalent" % key)
    if static_trig:
        runtime_trig = [ln for ln in lines if ":|" in ln]
        if static_any:
            lines = [ln for ln in lines if ":|" not in ln]
        elif not runtime_trig:
            raise Skip("none of the |-conditions holds on this host")
    if len(lines) > 8:
        raise Skip("%d conditions (limit 8)" % len(lines))
    return lines


_FIRSTBOOT = ("first-boot wizard; a schema-init install or migration has made the user "
              "already, and its done-marker is often missing, so it would run")
_DENY = {"rsyslog": "journal-sink owns /dev/log under schema-init",
         "abrtd": "schema-coredump owns kernel core dumps under schema-init",
         "plasma-setup": _FIRSTBOOT, "initial-setup": _FIRSTBOOT,
         "initial-setup-graphical": _FIRSTBOOT, "gnome-initial-setup": _FIRSTBOOT}


def _denied(name):
    if name in _DENY:
        return _DENY[name]
    if name.startswith("abrt-"):
        return _DENY["abrtd"]
    return None


_UNIT_ALIAS = {"dbus-broker": "dbus", "polkit": "polkitd", "NetworkManager": "network-manager",
               "bluetooth": "bluetoothd", "systemd-logind": "schema-logind",
               "systemd-journald": "journal-sink"}


def dep_lines(name, sections, known):
    """Requires=/Requisite=/BindsTo= on a service or socket that has a schema
    .svc (known) -> dep= lines; Type=dbus also waits for the bus, as systemd's
    implicit After=dbus.socket, and for polkitd: a bus daemon that asks for
    PolicyKit1 before polkitd.svc owns it gets a bus-activated polkitd, and
    the supervised one then loses the name and crash-loops. Plain After= is ordering only in systemd and
    never keeps a unit from starting, while dep= waits for the dep to settle,
    so it is not translated. Returns (lines, notes)."""
    if not known:
        return [], []
    cands = []
    if _get_last(sections.get("Service", []), "Type").lower() == "dbus":
        cands += ["dbus", "polkitd"]
    unit = sections.get("Unit", [])
    for k in ("Requires", "Requisite", "BindsTo"):
        for v in _get_all(unit, k):
            for u in v.split():
                if u.endswith((".service", ".socket")) and "@" not in u:
                    base = u.rsplit(".", 1)[0]
                    cands.append(_UNIT_ALIAS.get(base, base))
    deps = []
    for d in cands:
        if d != name and d in known and d not in deps:
            deps.append(d)
    notes = []
    if len(deps) > 8:
        notes.append("dropped dep on %s (limit 8)" % " ".join(deps[8:]))
        deps = deps[:8]
    return ["dep=%s" % d for d in deps], notes


def unit_to_svc(name, sections, sock=None, lazy=False, known=None):
    """Translate parsed unit sections into schema .svc text. sock: the parsed
    .socket that activates it; lazy: start on the first connection, not at boot.

    Returns the .svc file body (str). Raises Skip(reason) for units we refuse
    to half-translate (Type=forking without an absolute PIDFile=, Type=dbus
    without BusName, template, no ExecStart).
    """
    if "@" in name:
        raise Skip("template unit (%s) — instances unsupported" % name)

    svc = sections.get("Service", [])
    inst = sections.get("Install", [])

    stype = _get_last(svc, "Type").lower()
    pidfile = _get_last(svc, "PIDFile")
    if stype == "forking" and not (pidfile.startswith("/") and not any(c in pidfile for c in "%$")):
        raise Skip("Type=forking without an absolute PIDFile= — nothing to "
                   "find the daemon's main PID by")
    busname = _get_last(svc, "BusName")
    if stype == "dbus" and not busname:
        raise Skip("Type=dbus without BusName= — nothing to wait for")

    execstarts = _get_all(svc, "ExecStart")
    execstart = execstarts[-1] if execstarts else ""
    argv = _exec_tokens(execstart)
    if not argv:
        raise Skip("no usable ExecStart")

    env_pairs = _env_pairs(_get_all(svc, "Environment"))
    env_files, env_file_notes = [], []
    for ef in _get_all(svc, "EnvironmentFile"):
        if ef == "":
            env_files = []
        elif "%" in ef or not ef.lstrip("-").startswith("/"):
            env_file_notes.append("dropped EnvironmentFile=%s" % ef)
        else:
            env_files.append(ef)
    if len(env_files) > 4:
        env_file_notes.append("dropped EnvironmentFile=%s (limit 4)" % " ".join(env_files[4:]))
        env_files = env_files[:4]
    pres = _exec_pre(svc)
    expand = bool(env_files) and any("$" in a for a in argv[1:] + [w for _, pa in pres for w in pa])
    dropped_args = 0
    env_map = dict(p.split("=", 1) for p in env_pairs)
    if env_files and "$" in argv[0]:
        exe, _ = _expand_argv(argv[:1], env_map)
        if not exe:
            raise Skip("ExecStart binary path uses a variable from an EnvironmentFile")
        argv = exe + argv[1:]
    if not env_files:
        argv, dropped_args = _expand_argv(argv, env_map)
        if not argv:
            raise Skip("ExecStart is entirely unresolved variables")
        expanded = []
        for pre, pa in pres:
            pa, d = _expand_argv(pa, env_map)
            dropped_args += d
            if pa:
                expanded.append((pre, pa))
        pres = expanded

    lines = ["name=%s" % name, "exec=%s" % argv[0]]
    for a in argv[1:]:
        lines.append("args=%s" % a)

    for e in env_pairs:
        lines.append("env=%s" % e)
    for ef in env_files:
        lines.append("env_file=%s" % ef)
    if expand:
        lines.append("expand_args=1")
    for pre, pa in pres:
        lines.append("exec_pre=%s%s" % (pre, " ".join(_cmd_quote(w) for w in pa)))

    if stype == "oneshot":
        lines.append("oneshot=1")
    elif stype in ("notify", "notify-reload"):
        lines.append("notify=1")
    elif stype == "dbus":
        lines.append("ready_bus_name=%s" % busname)
    elif stype == "forking":
        lines.append("pid_file=%s" % pidfile)

    # systemd defaults Restart=no; only always/on-* opt into auto-restart.
    restart = _get_last(svc, "Restart").lower()
    if restart in ("", "no"):
        lines.append("no_restart=1")

    user = _get_last(svc, "User")
    if user and user != "root":
        lines.append("user=%s" % user)
    else:
        lines.append("needs_root=1")

    lines.append("critical=0")

    hard, warns = _hardening(svc)
    seen_plain = False
    for pre, _ in pres:
        if "+" not in pre:
            seen_plain = True
        elif seen_plain:
            warns.append("ExecStartPre order changed: '+' lines run before the others")
            break
    lines.extend(hard)
    lines.extend(condition_lines(sections))
    deps, dep_notes = dep_lines(name, sections, known)
    lines.extend(deps)

    sock_notes = []
    if sock is not None:
        sl, sock_notes = socket_lines(sock)
        lines.extend(sl)
        if lazy:
            lines.append("listen_lazy=1")

    wd = _get_last(svc, "WatchdogSec")
    if wd:
        sec = _timespan_sec(wd)
        if sec is None:
            warns.append("WatchdogSec=%s unrecognised, no service watchdog" % wd)
        elif sec > 0 and stype in ("notify", "notify-reload"):
            lines.append("watchdog_sec=%d" % sec)
        elif sec > 0:
            warns.append("WatchdogSec= without Type=notify has no socket to pet over, dropped")

    # [Install] presence is why the unit was queued; note if it's missing.
    notes = []
    if dropped_args:
        notes.append("dropped %d unresolved $VAR arg(s)" % dropped_args)
    if not inst and sock is None:
        notes.append("no [Install] section")
    notes += env_file_notes
    notes += dep_notes
    notes += ["socket: " + n for n in sock_notes]
    for k in ("WorkingDirectory",):
        if _get_last(svc, k):
            notes.append("dropped %s" % k)
    if notes:
        lines.insert(0, "# schema-import: " + "; ".join(notes))
    for w in reversed(warns):
        lines.insert(0, "%s%s" % (WARN_PREFIX, w))

    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Filesystem drain (impure) — thin wrapper around the core above.
# ---------------------------------------------------------------------------

def _root():
    return os.environ.get("MIGRATE_ROOT") or "/"


def state_dir():
    e = os.environ.get("SCHEMA_STATE_DIR")
    return e if e else os.path.join(_root(), "var/lib/schema-init")


def svc_dir():
    e = os.environ.get("SCHEMA_SVC_DIR")
    return e if e else os.path.join(_root(), "etc/schema-init/services")


def queue_path():
    return os.path.join(state_dir(), "pending.list")


def _unit_dirs():
    r = _root()
    return [os.path.join(r, d.lstrip("/")) for d in (
        "etc/systemd/system", "run/systemd/system",
        "usr/lib/systemd/system", "lib/systemd/system")]


def find_unit(name):
    """Locate a unit file by bare or .service name; None if not found."""
    cands = [name] if name.endswith(".service") else [name + ".service", name]
    for d in _unit_dirs():
        for c in cands:
            p = os.path.join(d, c)
            if os.path.exists(p):
                return p
    return None


def _read_queue():
    try:
        with open(queue_path()) as f:
            return [ln.strip() for ln in f if ln.strip()]
    except FileNotFoundError:
        return []


def skip_stub(name, path, reason, text):
    """A <name>.svc.skipped note for a unit we refused to translate: why, and
    the unit itself, so the admin can write the .svc by hand. PID 1 loads only
    *.svc, so the stub is inert."""
    lines = ["# schema-import skipped %s: %s" % (name, reason),
             "# Write %s.svc by hand if it should run under schema-init." % name,
             "# Original unit (%s):" % path, "#"]
    lines += ["# " + ln if ln else "#" for ln in text.splitlines()]
    return "\n".join(lines) + "\n"


def _timer_for(name):
    """The .timer that starts name.service (same name or Unit=), if any."""
    for d in _unit_dirs():
        try:
            entries = os.listdir(d)
        except OSError:
            continue
        for e in entries:
            if not e.endswith(".timer") or "@" in e:
                continue
            if e == name + ".timer":
                return e
            try:
                with open(os.path.join(d, e)) as f:
                    t = parse_unit(f.read())
            except OSError:
                continue
            if _get_last(t.get("Timer", []), "Unit") == name + ".service":
                return e
    return None


def _service_enabled(name):
    """name.service has a .wants/ symlink of its own (systemd enabled it)."""
    import glob
    return any(glob.glob(os.path.join(d, "*.wants", name + ".service"))
               for d in _unit_dirs()[:2])


_RECLAIMED = ("systemd-", "dbus", "NetworkManager-wait-online")


def enabled_units():
    """Every service/timer/socket systemd enabled into a target, minus the
    pieces schema-init itself replaces and template instances."""
    import glob
    out = []
    for p in sorted(glob.glob(os.path.join(_unit_dirs()[0], "*.target.wants", "*"))):
        u = os.path.basename(p)
        if not u.endswith((".service", ".timer", ".socket")) or "@" in u \
                or u.startswith(_RECLAIMED) or u in out:
            continue
        out.append(u)
    return out


_INTERPRETERS = {"sh", "bash", "dash", "env", "python3", "python", "perl", "true", "busybox"}


def _exec_owner(body, out):
    """Another long-running .svc already running this daemon's binary
    (usr-merge aliases resolved), or None. Oneshots and timers share binaries
    with daemons legitimately."""
    def real(exe):
        return os.path.realpath(os.path.join(_root(), exe.lstrip("/")))

    def daemon(lines):
        return not any(ln.startswith(("oneshot=1", "on_calendar=", "on_boot_sec=", "on_active_sec="))
                       for ln in lines)

    exe = next((ln[5:] for ln in body.splitlines() if ln.startswith("exec=")), None)
    if not exe or os.path.basename(exe) in _INTERPRETERS or not daemon(body.splitlines()):
        return None
    mine = real(exe)
    try:
        entries = sorted(os.listdir(svc_dir()))
    except OSError:
        return None
    for e in entries:
        p = os.path.join(svc_dir(), e)
        if not e.endswith(".svc") or p == out:
            continue
        try:
            with open(p) as f:
                olines = f.read().splitlines()
        except OSError:
            continue
        other = [ln[5:].strip() for ln in olines if ln.startswith("exec=")]
        if other and daemon(olines) and real(other[-1]) == mine:
            return e
    return None


_TIMER_KEYS = ("on_calendar=", "on_boot_sec=", "on_active_sec=")


def _svc_graph():
    """{name: (deps, is_timer)} for the .svc files in the service dir."""
    g = {}
    try:
        entries = os.listdir(svc_dir())
    except OSError:
        return g
    for e in entries:
        if not e.endswith(".svc"):
            continue
        try:
            with open(os.path.join(svc_dir(), e)) as f:
                lines = f.read().splitlines()
        except OSError:
            continue
        g[e[:-4]] = ([ln[4:].strip() for ln in lines if ln.startswith("dep=")],
                     any(ln.startswith(_TIMER_KEYS) for ln in lines))
    return g


def _known_svcs(queued):
    """Schema services a dep= may name: the .svc files there, plus the units
    being imported in the same drain. A timer's .svc is the timer job, never
    something to wait on."""
    g = _svc_graph()
    known = {n for n, (_, timer) in g.items() if not timer}
    for q in queued:
        if not q.endswith(".timer"):
            known.add(re.sub(r"\.(service|socket)$", "", q))
    return known


def _drop_cycles(name, body):
    """Drop each dep= of name's body that would close a dependency loop with
    the .svc files already written: PID 1 refuses to boot a cyclic graph."""
    g = _svc_graph()
    g[name] = ([], False)
    kept, dropped = [], []
    for ln in body.splitlines():
        if not ln.startswith("dep="):
            kept.append(ln)
            continue
        d = ln[4:]
        seen, stack, loop = set(), [d], False
        while stack and not loop:
            n = stack.pop()
            if n == name:
                loop = True
            elif n not in seen:
                seen.add(n)
                stack.extend(g.get(n, ([], False))[0])
        if loop:
            dropped.append(d)
        else:
            kept.append(ln)
            g[name][0].append(d)
    if dropped:
        kept.insert(0, "# schema-import: dropped dep on %s (would make a dependency loop)"
                    % " ".join(dropped))
    return "\n".join(kept) + "\n"


def import_one(name, force=False, queued=()):
    """Translate one queued unit. Returns (status, detail) where status is one
    of: imported, exists, not-found, skipped, error. queued: the other names
    in the same drain (an enabled .service makes its socket eager)."""
    timer = name.endswith(".timer")
    socket = name.endswith(".socket")
    name = name[:-len(".service")] if name.endswith(".service") else name
    name = name[:-len(".timer")] if timer else name
    sock_sections = None
    if socket:
        name = name[:-len(".socket")]
        spath = find_unit(name + ".socket")
        if not spath:
            return ("not-found", name + ".socket")
        try:
            with open(spath) as f:
                stext = f.read()
        except OSError as e:
            return ("error", str(e))
        sock_sections = parse_unit(stext)
        try:
            socket_lines(sock_sections)
        except Skip as s:
            return ("skipped", (str(s), skip_stub(name, spath, str(s), stext),
                                os.path.join(svc_dir(), name + ".svc")))
        target = _get_last(sock_sections.get("Socket", []), "Service") or name + ".service"
        name = target[:-len(".service")] if target.endswith(".service") else target
    else:
        spath = find_unit(name + ".socket") if not timer else None
        if spath and spath.endswith(name + ".socket"):
            try:
                with open(spath) as f:
                    cand = parse_unit(f.read())
                svc_target = _get_last(cand.get("Socket", []), "Service") or name + ".service"
                if svc_target == name + ".service":
                    socket_lines(cand)
                    sock_sections = cand
            except (OSError, Skip):
                pass
    out = os.path.join(svc_dir(), name + ".svc")
    if os.path.exists(out) and not force:
        return ("exists", out)
    path = find_unit(name + ".timer" if timer else name)
    if not path:
        return ("not-found", name)
    known = _known_svcs(queued)
    lazy = socket and not _service_enabled(name) and name + ".service" not in queued \
        and name not in queued
    try:
        with open(path) as f:
            text = f.read()
        sections = parse_unit(text)
        if "@" in name and sock_sections is not None:
            raise Skip("socket starts template %s — instances unsupported" % name)
        if _denied(name):
            raise Skip(_denied(name))
        if timer:
            target = _get_last(sections.get("Timer", []), "Unit") or name + ".service"
            if "@" in target:
                raise Skip("timer starts template instance %s — instances unsupported" % target)
            spath = find_unit(target)
            if not spath:
                raise Skip("timer's %s not found" % target)
            with open(spath) as f:
                body = timer_to_svc(name, sections, parse_unit(f.read()), known)
        else:
            if not sections.get("Install") and _timer_for(name):
                raise Skip("started by %s, which owns %s.svc — enable the timer"
                           % (_timer_for(name), name))
            body = unit_to_svc(name, sections, sock_sections, lazy, known)
        body = _drop_cycles(name, body)
        owner = _exec_owner(body, out)
        if owner:
            raise Skip("its binary already runs as %s — importing it would start a second copy"
                       % owner)
    except Skip as s:
        return ("skipped", (str(s), skip_stub(name, path, str(s), text), out))
    except OSError as e:
        return ("error", str(e))
    return ("imported", (out, body))


def drain(units=None, force=False, dry_run=False, log=print):
    """Drain the queue (or the given unit list). Writes .svc files, then rewrites
    the queue keeping only transient (not-found) entries. Returns a counts dict."""
    from_queue = units is None
    items = units if units is not None else _read_queue()
    counts = {"imported": 0, "exists": 0, "not-found": 0, "skipped": 0, "error": 0}
    keep = []
    for name in items:
        status, detail = import_one(name, force=force, queued=items)
        counts[status] += 1
        if status == "imported":
            out, body = detail
            log("import  %s -> %s" % (name, out))
            for ln in body.splitlines():
                if ln.startswith(WARN_PREFIX):
                    log("WARN    %s: %s" % (name, ln[len(WARN_PREFIX):]))
            if not dry_run:
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with open(out, "w") as f:
                    f.write(body)
        elif status == "exists":
            log("have    %s (%s exists; --force to overwrite)" % (name, detail))
        elif status == "not-found":
            log("MISS    %s (no unit file; left queued)" % name)
            keep.append(name)
        elif status == "skipped":
            reason, stub, out = detail
            stub_path = out + ".skipped"
            log("skip    %s: %s (see %s)" % (name, reason, stub_path))
            if not dry_run:
                try:
                    os.makedirs(os.path.dirname(stub_path), exist_ok=True)
                    with open(stub_path, "w") as f:
                        f.write(stub)
                except OSError as e:
                    log("WARN    %s: could not write %s: %s" % (name, stub_path, e))
        else:
            log("ERROR   %s: %s" % (name, detail))
            keep.append(name)
    if from_queue and not dry_run:
        os.makedirs(state_dir(), exist_ok=True)
        # Re-read so intents the shim appended while we processed survive, and
        # rewrite atomically (tmp + rename) so a crash can't truncate the queue.
        processed = set(items) - set(keep)
        remaining = [n for n in _read_queue() if n not in processed]
        tmp = queue_path() + ".tmp"
        with open(tmp, "w") as f:
            for n in remaining:
                f.write(n + "\n")
        os.replace(tmp, queue_path())
    return counts


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="schema-import",
        description="Drain the systemctl-shim enable queue into schema .svc files.")
    ap.add_argument("units", nargs="*",
                    help="unit(s) to import directly; default drains pending.list")
    ap.add_argument("-n", "--dry-run", action="store_true",
                    help="show what would happen; touch nothing")
    ap.add_argument("-f", "--force", action="store_true",
                    help="overwrite an existing .svc")
    ap.add_argument("-e", "--enabled", action="store_true",
                    help="import every unit systemd enabled (/etc/systemd/system/*.target.wants)")
    args = ap.parse_args(argv)
    units = args.units or None
    if args.enabled:
        units = (units or []) + [u for u in enabled_units() if u not in (units or [])]
    counts = drain(units=units, force=args.force, dry_run=args.dry_run)
    print("imported=%(imported)d exists=%(exists)d not-found=%(not-found)d "
          "skipped=%(skipped)d error=%(error)d" % counts)
    return 1 if counts["error"] else 0


if __name__ == "__main__":
    sys.exit(main())
