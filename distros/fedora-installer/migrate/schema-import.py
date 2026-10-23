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
import shutil
import subprocess
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


def _specifiers(argv, name):
    spec = {"n": name + ".service", "N": name, "p": name, "t": "/run", "%": "%"}
    if _USER:
        spec.update({"h": _USER["home"], "t": _USER["runtime"], "S": _USER["state"],
                     "C": _USER["cache"], "E": _USER["config"], "u": _USER["user"],
                     "U": str(_USER["uid"]), "T": "/tmp", "V": "/var/tmp"})
    out = []
    for w in argv:
        def rep(m):
            if m.group(1) not in spec:
                raise Skip("specifier %%%s has no schema equivalent" % m.group(1))
            return spec[m.group(1)]
        out.append(re.sub(r"%(.?)", rep, w))
    return out


def _exec_pre(svc, name):
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
        out.append(("".join(sorted(set(pre))), _specifiers(argv, name)))
    if len(out) > 8:
        raise Skip("%d ExecStartPre= lines (limit 8)" % len(out))
    return out


class Skip(Exception):
    """Raised when a unit falls in a known rathole and must not be translated."""


_TRUE = ("1", "yes", "true", "on")
_FALSE = ("", "0", "no", "false", "off")


_LIMITS = ("CPU", "FSIZE", "DATA", "STACK", "CORE", "RSS", "NOFILE", "AS", "NPROC",
           "MEMLOCK", "LOCKS", "SIGPENDING", "MSGQUEUE", "NICE", "RTPRIO", "RTTIME")
_LIMIT_VAL = re.compile(r"^(infinity|\d+[KMGT]?|[-+]\d+)$")
_INF = float("inf")


def _limit_num(x, nice):
    """A Limit value as PID 1 reads it, or None if PID 1 would reject it."""
    if not _LIMIT_VAL.match(x):
        return None
    if x == "infinity":
        return _INF
    if x[0] in "+-":
        return 20 - int(x) if nice and -20 <= int(x) <= 19 else None
    n = int(x.rstrip("KMGT")) << (10 * ("KMGT".index(x[-1]) + 1) if x[-1] in "KMGT" else 0)
    return n if n < 2 ** 64 - 1 else None


def limit_lines(svc):
    """Limit<X>=VALUE[:HARD] -> limit_<x>=. Values PID 1 can't parse (time
    spans like LimitCPU=30s, P/E suffixes) are dropped with a warning."""
    lines, warns = [], []
    for name in _LIMITS:
        v = _get_last(svc, "Limit" + name)
        if not v:
            continue
        parts = v.split(":")
        nums = [_limit_num(x, name == "NICE") for x in parts]
        if len(parts) > 2 or None in nums or (
                len(nums) == 2 and nums[1] != _INF and nums[0] > nums[1]):
            warns.append("Limit%s=%s not translated (unsupported value)" % (name, v))
            continue
        lines.append("limit_%s=%s" % (name.lower(), v))
    return lines, warns


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


def _check_line_len(lines):
    for ln in lines:
        if not ln.lstrip().startswith(("#", ";")) and len(ln.encode()) > 511:
            raise Skip("%s= line is %d bytes (limit 511)" % (ln.split("=", 1)[0], len(ln.encode())))


def timer_to_svc(name, timer_sections, svc_sections, known=None, unit=None):
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
    body = unit_to_svc(name, svc_sections, known=known, unit=unit)
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
    _check_line_len(out + tlines)
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
            v = v.replace("%t", _USER["runtime"] if _USER else "/run")
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
                "CPUs", "FirstBoot", "Architecture", "User")
def _sys_uid_max():
    # systemd's @system: uid <= SYS_UID_MAX from login.defs (999 when unset)
    try:
        with open(os.path.join(os.environ.get("MIGRATE_ROOT") or "/", "etc/login.defs")) as f:
            for ln in f:
                w = ln.split()
                if len(w) >= 2 and w[0] == "SYS_UID_MAX" and w[1].isdigit():
                    return int(w[1])
    except OSError:
        pass
    return 999
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
    cpu = dict(ln.split(":", 1) for ln in reversed(_read("/proc/cpuinfo").splitlines()) if ":" in ln)
    cpu = {k.strip(): v.strip() for k, v in cpu.items()}
    _FACTS = {"virt": virt, "virt_kind": kind, "security": sec,
              "cgroup_v2": ctl is not None, "controllers": set((ctl or "").split()),
              "cpus": len(os.sched_getaffinity(0)),
              "arch": _ARCH.get(platform.machine(), platform.machine()),
              "cpu_vendor": cpu.get("vendor_id", ""), "cpu_flags": set(cpu.get("flags", "").split())}
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
    if name == "User":
        # the manager's user: the importing user under --user, root for PID 1
        user, uid = (_USER["user"], _USER["uid"]) if _USER else ("root", 0)
        a = {"%u": user, "%U": str(uid)}.get(arg.strip(), arg.strip())
        if "%" in a:
            return None
        if a == "@system":
            return uid <= _sys_uid_max()
        return a == str(uid) if a.isdigit() else a == user
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


_QUIRKS = {"intel_lpmd": lambda f: None if f.get("cpu_vendor") == "GenuineIntel" and "hybrid_cpu" in f.get("cpu_flags", ())
           else "intel_lpmd supports only Intel hybrid CPUs and exits on this one"}


def _denied(name):
    if name in _DENY:
        return _DENY[name]
    if name.startswith("abrt-"):
        return _DENY["abrtd"]
    if name in _QUIRKS:
        return _QUIRKS[name](host_facts())
    return None


_UNIT_ALIAS = {"dbus-broker": "dbus", "polkit": "polkitd", "NetworkManager": "network-manager",
               "bluetooth": "bluetoothd", "systemd-logind": "schema-logind",
               "systemd-journald": "journal-sink"}


def dep_lines(name, sections, known):
    """Requires=/Requisite=/BindsTo= on a service or socket that has a schema
    .svc (known) -> dep= lines; Type=dbus also waits for the bus, as systemd's
    implicit After=dbus.socket, and for polkitd: a bus daemon that asks for
    PolicyKit1 before polkitd.svc owns it gets a bus-activated polkitd, and
    the supervised one then loses the name and crash-loops. Bus daemons that
    are not Type=dbus (firewalld) count too: BusName=, Alias=dbus-org.*, or
    PartOf=dbus.service. Plain After= is ordering only in systemd and
    never keeps a unit from starting, while dep= waits for the dep to settle,
    so it is not translated. Returns (lines, notes)."""
    if not known:
        return [], []
    cands = []
    unit = sections.get("Unit", [])
    svc = sections.get("Service", [])
    tokens = lambda sec, k: " ".join(_get_all(sec, k)).split()
    part_of = {_UNIT_ALIAS.get(u.rsplit(".", 1)[0], u.rsplit(".", 1)[0])
               for u in tokens(unit, "PartOf") if u.endswith((".service", ".socket"))}
    if (_get_last(svc, "Type").lower() == "dbus" or _get_last(svc, "BusName")
            or any(a.startswith("dbus-org.") for a in tokens(sections.get("Install", []), "Alias"))
            or "dbus" in part_of):
        cands += ["dbus", "polkitd"]
    for k in ("Requires", "Requisite", "BindsTo"):
        for u in tokens(unit, k):
            if u.endswith((".service", ".socket")) and "@" not in u:
                base = u.rsplit(".", 1)[0]
                cands.append(_UNIT_ALIAS.get(base, base))
    raw = {v: k for k, v in _UNIT_ALIAS.items()}
    self_names = {name, _UNIT_ALIAS.get(name, name)}
    deps = []
    for d in cands:
        d = d if d in known else raw.get(d, d)
        if d not in self_names and _UNIT_ALIAS.get(d, d) not in self_names and d in known and d not in deps:
            deps.append(d)
    return _cap_deps(deps)


def _cap_deps(deps):
    """PID 1 refuses a ninth dep=: keep the first 8, note the rest."""
    notes = ["dropped dep on %s (limit 8)" % " ".join(deps[8:])] if len(deps) > 8 else []
    return ["dep=%s" % d for d in deps[:8]], notes


def unit_to_svc(name, sections, sock=None, lazy=False, known=None, unit=None):
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
    if _get_last(svc, "StandardInput").lower().startswith("tty"):
        raise Skip("StandardInput=tty: schema-init gives services /dev/null on stdin; "
                   "write a .svc that names the terminal (agetty ... tty1)")
    busname = _get_last(svc, "BusName")
    if stype == "dbus" and not busname:
        raise Skip("Type=dbus without BusName= — nothing to wait for")

    execstarts = _get_all(svc, "ExecStart")
    execstart = execstarts[-1] if execstarts else ""
    argv = _exec_tokens(execstart)
    if not argv:
        raise Skip("no usable ExecStart")
    argv = _specifiers(argv, unit or name)

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
    pres = _exec_pre(svc, unit or name)
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

    if len(argv) > 63:
        raise Skip("ExecStart= has %d arguments (limit 62)" % (len(argv) - 1))
    env_pairs = list({p.split("=", 1)[0]: p for p in env_pairs}.values())
    if len(env_pairs) > 16:
        raise Skip("Environment= sets %d variables (limit 16)" % len(env_pairs))
    for pre, pa in pres:
        if len(pa) > 32:
            raise Skip("ExecStartPre= has %d words (limit 32)" % len(pa))

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
    else:
        rsec = _timespan_sec(_get_last(svc, "RestartSec"))
        if rsec:
            lines.append("restart_sec=%d" % min(rsec, 3600))

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
    lim, lim_warns = limit_lines(svc)
    lines.extend(lim)
    warns.extend(lim_warns)
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

    # TimeoutStopSec= (or TimeoutSec=, which sets start and stop; the later of
    # the two wins, as in systemd) is how long systemd waits after SIGTERM;
    # schema-init's shutdown grace is 3 s unless stop_timeout_sec= says
    # otherwise, capped at 300.
    skey, stop = "", ""
    for k, v in svc:
        if k in ("TimeoutStopSec", "TimeoutSec"):
            skey, stop = k, v
    if stop:
        sec = _timespan_sec(stop)
        if stop.strip().lower() == "infinity" or sec == 0:
            lines.append("stop_timeout_sec=300")
            warns.append("%s=%s (no limit) capped at 300 s" % (skey, stop))
        elif sec is None:
            warns.append("%s=%s unrecognised, default stop grace" % (skey, stop))
        elif sec > 300:
            lines.append("stop_timeout_sec=300")
            warns.append("%s=%s capped at 300 s" % (skey, stop))
        else:
            lines.append("stop_timeout_sec=%d" % sec)

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

    _check_line_len(lines)
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
    if _USER:
        dirs = [os.path.join(_USER["config"], "systemd/user"),
                os.path.join(r, "etc/systemd/user")]
        if _USER["runtime"]:
            dirs.append(os.path.join(_USER["runtime"], "systemd/user"))
        dirs += [os.path.join(r, "run/systemd/user"), os.path.join(_USER["data"], "systemd/user")]
        return dirs + [os.path.join(r, d) for d in _USER_SYS_DIRS]
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
            known.add(re.sub(r"\.(service|socket)$", "", q.rsplit("/", 1)[-1]))
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


INITD = "/etc/init.d/"
LSB_DEPS = {"$local_fs": "mount-fstab", "$remote_fs": "mount-fstab", "$syslog": "journal-sink",
            "$network": "network-manager", "$time": "chrony", "udev": "udevd", "eudev": "udevd",
            "ssh": "sshd", "procps": "sysctl"}
OWNED = "its binary already runs as"


def initd_dir():
    return os.path.join(_root(), INITD.lstrip("/"))


def lsb_header(text):
    """Key -> value of the script's ### BEGIN INIT INFO block."""
    hdr, inside = {}, False
    for ln in text.splitlines():
        s = ln.strip()
        if s == "### BEGIN INIT INFO":
            inside = True
        elif s == "### END INIT INFO":
            break
        elif inside and s.startswith("#") and ":" in s:
            k, v = s.lstrip("# ").split(":", 1)
            hdr[k.strip()] = v.strip()
    return hdr


def lsb_to_svc(name, text, known=None):
    """An init script with no systemd unit, run the way sysvinit runs it:
    `start` once, no supervision (a pid_file would restart-loop a script its
    /etc/default turns off)."""
    deps = []
    for d in lsb_header(text).get("Required-Start", "").split():
        svc = LSB_DEPS.get(d) or _UNIT_ALIAS.get(d, d)
        if svc != name and svc not in deps and (known is None or svc in known):
            deps.append(svc)
    lines = ["# schema-import: from %s%s (init script; no usable systemd unit)" % (INITD, name),
             "name=" + name, "exec=" + INITD + name, "args=start", "oneshot=1", "needs_root=1"]
    dl, notes = _cap_deps(deps)
    lines += ["# schema-import: " + n for n in notes] + dl
    return "\n".join(lines) + "\n"


def _import_lsb(name, force, queued):
    out = os.path.join(svc_dir(), name + ".svc")
    if os.path.exists(out) and not force:
        return ("exists", out)
    path = os.path.join(initd_dir(), name)
    try:
        with open(path) as f:
            text = f.read()
    except FileNotFoundError:
        return ("not-found", INITD + name)
    except OSError as e:
        return ("error", str(e))
    denied = _denied(name)
    if denied:
        return ("skipped", (denied, skip_stub(name, path, denied, text), out))
    body = _drop_cycles(name, lsb_to_svc(name, text, _known_svcs(queued)))
    return ("imported", (out, body))


def _runlevel():
    try:
        with open(os.path.join(_root(), "etc/inittab")) as f:
            for ln in f:
                if ln.lstrip().startswith("#"):
                    continue
                p = ln.split(":")
                if len(p) >= 3 and p[2] == "initdefault":
                    return p[1]
    except OSError:
        pass
    return "2"


def _runnable_unit(unit):
    """unit has an ExecStart and an [Install] target boot pulls it into. A
    masked unit (-> /dev/null) reads empty: Debian masks it when the init
    script is the real thing. A unit with no WantedBy=/RequiredBy= is started
    by something else on systemd (udev, a socket, D-Bus), so the init script,
    with its own checks, is what sysvinit actually runs."""
    p = find_unit(unit)
    if not p:
        return False
    try:
        with open(p) as f:
            sec = parse_unit(f.read())
    except OSError:
        return False
    inst = sec.get("Install", [])
    return bool(_get_last(sec.get("Service", []), "ExecStart")) and \
        bool(_get_last(inst, "WantedBy") or _get_last(inst, "RequiredBy"))


def rc_units(skip=()):
    """What sysvinit starts in the default runlevel (/etc/rcN.d S links): the
    same-named systemd unit where the package ships one, else the init script
    itself (INITD + name). skip: script names to leave out."""
    d = os.path.join(_root(), "etc/rc%s.d" % _runlevel())
    try:
        links = sorted(os.listdir(d))
    except OSError:
        return []
    out = []
    for ln in links:
        m = re.match(r"S\d\d(.+)$", ln)
        if not m or m.group(1) in skip:
            continue
        script = m.group(1)
        item = script + ".service" if _runnable_unit(script + ".service") else INITD + script
        if item not in out:
            out.append(item)
    return out


SV = "/etc/sv/"
RUNSVDIR = "etc/runit/runsvdir/default"
runit_after = None
RUNIT_READY = {"dbus": "ready_path=/run/dbus/system_bus_socket"}


def runit_units(skip=()):
    """What runit starts at boot: the services the default runsvdir links to
    in /etc/sv, minus those with a down file. skip: service names to leave out."""
    d = os.path.join(_root(), RUNSVDIR)
    try:
        links = sorted(os.listdir(d))
    except OSError:
        return []
    out = []
    for n in links:
        p = os.path.join(d, n)
        t = os.readlink(p) if os.path.islink(p) else os.path.join("/", RUNSVDIR, n)
        t = os.path.normpath(t if t.startswith("/") else os.path.join("/", RUNSVDIR, t))
        name = os.path.basename(t)
        if os.path.dirname(t) + "/" != SV:
            print("runit: %s -> %s is outside %s, not imported" % (n, t, SV), file=sys.stderr)
            continue
        if name in skip or SV + name in out or os.path.exists(os.path.join(_root(), t.lstrip("/"), "down")):
            continue
        out.append(SV + name)
    return out


def runit_to_svc(name, text, known=None, after=None, check=False):
    """A runit service run the way runsv runs it: ./run from its own directory,
    restarted when it dies. `sv check X` / `sv start X` lines become dep=X;
    after: the service standing in for runit's stage 1, which all of them wait for.
    Ready as soon as it runs, as `sv check` sees it, unless it has a ./check
    script: then a known readiness signal, else one second."""
    deps = [after] if after and after != name else []
    for m in re.finditer(r"\bsv\s+(?:-\S+\s+(?:\d+\s+)?)*(?:check|start|status|up)\s+([^\s;|&)]+)", text):
        d = m.group(1).rstrip("/").rsplit("/", 1)[-1]
        if d != name and d not in deps and (known is None or d in known):
            deps.append(d)
    lines = ["# schema-import: from %s%s (runit)" % (SV, name),
             "name=" + name, "exec=/bin/sh", "args=-c",
             "args=cd %s%s && exec ./run" % (SV, name), "needs_root=1"]
    if not check:
        lines.append("stable_secs=0")
    elif name in RUNIT_READY:
        lines.append(RUNIT_READY[name])
    else:
        lines += ["# schema-import: %s%s/check not translated; ready after 1 s" % (SV, name),
                  "stable_secs=1"]
    dl, notes = _cap_deps(deps)
    lines += ["# schema-import: " + n for n in notes] + dl
    return "\n".join(lines) + "\n"


def _import_runit(name, force, queued):
    out = os.path.join(svc_dir(), name + ".svc")
    if os.path.exists(out) and not force:
        return ("exists", out)
    path = os.path.join(_root(), SV.lstrip("/"), name, "run")
    try:
        with open(path) as f:
            text = f.read()
    except FileNotFoundError:
        return ("not-found", SV + name)
    except OSError as e:
        return ("error", str(e))
    check = os.path.exists(os.path.join(os.path.dirname(path), "check"))
    return ("imported", (out, _drop_cycles(name, runit_to_svc(name, text, _known_svcs(queued),
                                                              runit_after, check))))


def read_skip(path):
    try:
        with open(path) as f:
            return {w for ln in f for w in ln.split("#", 1)[0].split()}
    except OSError:
        return set()


def import_one(name, force=False, queued=()):
    """Translate one queued unit. Returns (status, detail) where status is one
    of: imported, exists, not-found, skipped, error. queued: the other names
    in the same drain (an enabled .service makes its socket eager)."""
    if name.startswith(INITD):
        return _import_lsb(name[len(INITD):], force, queued)
    if name.startswith(SV):
        return _import_runit(name[len(SV):], force, queued)
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
        denied = _denied(name)
        if denied:
            raise Skip(denied)
        if timer:
            target = _get_last(sections.get("Timer", []), "Unit") or name + ".service"
            if "@" in target:
                raise Skip("timer starts template instance %s — instances unsupported" % target)
            spath = find_unit(target)
            if not spath:
                raise Skip("timer's %s not found" % target)
            with open(spath) as f:
                body = timer_to_svc(name, sections, parse_unit(f.read()), known,
                                    target[:-len(".service")] if target.endswith(".service") else target)
        else:
            if not sections.get("Install") and _timer_for(name):
                raise Skip("started by %s, which owns %s.svc — enable the timer"
                           % (_timer_for(name), name))
            body = unit_to_svc(name, sections, sock_sections, lazy, known)
        body = _drop_cycles(name, body)
        owner = _exec_owner(body, out)
        if owner:
            raise Skip(OWNED + " %s — importing it would start a second copy"
                       % owner)
    except Skip as s:
        return ("skipped", (str(s), skip_stub(name, path, str(s), text), out))
    except OSError as e:
        return ("error", str(e))
    return ("imported", (out, body))


def drain(units=None, force=False, dry_run=False, log=print, written=None, lsb_fallback=()):
    """Drain the queue (or the given unit list). Writes .svc files, then rewrites
    the queue keeping only transient (not-found) entries. Returns a counts dict."""
    from_queue = units is None
    items = units if units is not None else _read_queue()
    counts = {"imported": 0, "exists": 0, "not-found": 0, "skipped": 0, "error": 0}
    keep = []
    for name in items:
        status, detail = import_one(name, force=force, queued=items)
        if status == "skipped" and name in lsb_fallback and not detail[0].startswith(OWNED):
            log("unit    %s: %s; using %s%s" % (name, detail[0], INITD, name[:-len(".service")]))
            status, detail = _import_lsb(name[:-len(".service")], force, items)
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
                if written is not None:
                    written.append(out)
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


BUS_MARK = "# generated by schema-import --bus-overrides"
BUS_HELPER = "/usr/libexec/schema-init/schema-bus-activate"


def parse_activation(text):
    sec = parse_unit(text).get("D-BUS Service", [])
    return {k: _get_last(sec, k) for k in ("Name", "Exec", "User", "SystemdService")}


def bus_override(act, svc):
    user, exe = act.get("User"), act.get("Exec") or "/bin/false"
    if user != "root" and not (not user and exe in ("/bin/false", "/usr/bin/false")):
        return None
    lines = [BUS_MARK, "[D-BUS Service]", "Name=" + act["Name"],
             "Exec=%s %s %s %s" % (BUS_HELPER, svc, act["Name"], exe),
             "User=root"]
    if act.get("SystemdService"):
        lines.append("SystemdService=" + act["SystemdService"])
    return "\n".join(lines) + "\n"


def _svc_for_unit(unit):
    p = find_unit(unit)
    if not p:
        return None
    stem = os.path.basename(os.path.realpath(p))
    if not stem.endswith(".service") or "@" in stem:
        return None
    stem = stem[:-len(".service")]
    for svc in (stem, _UNIT_ALIAS.get(stem)):
        if svc and os.path.exists(os.path.join(svc_dir(), svc + ".svc")):
            return svc
    return None


def act_dir():
    return os.path.join(_root(), "usr/share/dbus-1/system-services")


def bus_dir():
    e = os.environ.get("SCHEMA_BUS_OVERRIDE_DIR")
    return e if e else os.path.join(_root(), "usr/local/share/dbus-1/system-services")


def _generated(path):
    try:
        with open(path) as f:
            return f.readline().rstrip("\n") == BUS_MARK
    except (OSError, ValueError):
        return False


def bus_overrides(dry_run=False, log=print):
    want = {}
    try:
        names = sorted(os.listdir(act_dir()))
    except OSError:
        names = []
    for fn in names:
        if not fn.endswith(".service"):
            continue
        try:
            with open(os.path.join(act_dir(), fn)) as f:
                act = parse_activation(f.read())
        except (OSError, ValueError):
            continue
        if not act.get("Name") or not act.get("SystemdService"):
            continue
        svc = _svc_for_unit(act["SystemdService"])
        if not svc:
            continue
        body = bus_override(act, svc)
        if body:
            want[act["Name"] + ".service"] = body
    out = bus_dir()
    try:
        have = {fn for fn in os.listdir(out) if _generated(os.path.join(out, fn))}
    except OSError:
        have = set()
    counts = {"written": 0, "kept": 0, "removed": 0, "skipped": 0}
    for fn, body in sorted(want.items()):
        p = os.path.join(out, fn)
        if os.path.exists(p) and fn not in have:
            counts["skipped"] += 1
            continue
        try:
            with open(p) as f:
                same = f.read() == body
        except (OSError, ValueError):
            same = False
        if same:
            counts["kept"] += 1
            continue
        counts["written"] += 1
        log("bus-override  %s" % p)
        if not dry_run:
            try:
                os.makedirs(out, exist_ok=True)
                with open(p + ".tmp", "w") as f:
                    f.write(body)
                os.replace(p + ".tmp", p)
            except OSError as e:
                log("bus-override  %s: %s" % (p, e))
    for fn in sorted(have - set(want)):
        counts["removed"] += 1
        log("bus-override  remove %s" % os.path.join(out, fn))
        if not dry_run:
            try:
                os.remove(os.path.join(out, fn))
            except OSError:
                pass
    if not dry_run and (counts["written"] or counts["removed"]) and _root() == "/":
        try:
            subprocess.run(["dbus-send", "--system", "--type=method_call", "--dest=org.freedesktop.DBus",
                            "/org/freedesktop/DBus", "org.freedesktop.DBus.ReloadConfig"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        except (OSError, subprocess.SubprocessError):
            pass
    return counts


# ---------------------------------------------------------------------------
# User mode (--user): the user's own enabled units, for schema-user.
# ---------------------------------------------------------------------------

_USER = None
# after ~/.config, /etc, $XDG_RUNTIME_DIR, /run and $XDG_DATA_HOME (systemd's order)
_USER_SYS_DIRS = ("usr/local/share/systemd/user", "usr/share/systemd/user",
                  "usr/local/lib/systemd/user", "usr/lib/systemd/user")
USER_MARK = "# generated by schema-import --user"
# schema-user is unprivileged: it cannot unshare a mount namespace, so a unit
# carrying these would fail at every start
_USER_NO_HARDEN = ("private_tmp=", "protect_system=", "protect_home=")
# Phase 1 reads only what the user enabled: distro defaults under /etc and
# /usr/lib include dbus.socket, which would start a second session bus.
_USER_ROOTS = ("default.target", "sockets.target", "timers.target", "basic.target")
_SESSION_TARGETS = ("graphical-session.target", "graphical-session-pre.target",
                    "plasma-*.target", "gnome-session*.target")
_UNIT_SUFFIXES = (".service", ".socket", ".timer", ".path", ".target", ".mount")
# The session script runs the session bus; a generated copy would listen on
# $XDG_RUNTIME_DIR/bus too. Never generated, whatever pulls them in.
_NEVER = ("dbus.socket", "dbus.service", "dbus-broker.service")


class Masked(Exception):
    pass


def user_context(home, uid, user, env):
    def xdg(var, default):
        v = env.get(var)
        return v if v and os.path.isabs(v) else os.path.join(home, default)
    return {"home": home, "uid": uid, "user": user,
            "config": xdg("XDG_CONFIG_HOME", ".config"),
            "state": xdg("XDG_STATE_HOME", ".local/state"),
            "cache": xdg("XDG_CACHE_HOME", ".cache"),
            "data": xdg("XDG_DATA_HOME", ".local/share"),
            "runtime": env.get("XDG_RUNTIME_DIR", "")}


def set_user_mode(ctx):
    global _USER
    _USER = ctx


def _masked(path):
    if os.path.islink(path) and os.readlink(path) == "/dev/null":
        return True
    try:
        return os.path.getsize(path) == 0
    except OSError:
        return False


def user_unit_text(name):
    # winning file + drop-ins in systemd's order; None = nowhere; Masked = /dev/null or empty
    text = None
    for d in _unit_dirs():
        p = os.path.join(d, name)
        if os.path.lexists(p):
            if _masked(p):
                raise Masked(name)
            try:
                with open(p) as f:
                    text = f.read()
            except OSError:
                return None
            break
    if text is None:
        return None
    drop = {}
    for d in _unit_dirs():
        dd = os.path.join(d, name + ".d")
        try:
            for e in os.listdir(dd):
                if e.endswith(".conf"):
                    drop.setdefault(e, os.path.join(dd, e))
        except OSError:
            continue
    for e in sorted(drop):
        try:
            with open(drop[e]) as f:
                text += "\n" + f.read()
        except OSError:
            pass
    return text


def _unit_name(n):
    return n if n.endswith(_UNIT_SUFFIXES) else n + ".service"


def read_skip_list(paths):
    return {_unit_name(n) for p in paths for n in read_skip(p)}


def _wants_dir_units(dirs, unit):
    out = []
    for d in dirs:
        for kind in (".wants", ".requires"):
            try:
                out += sorted(os.listdir(os.path.join(d, unit + kind)))
            except OSError:
                pass
    return out


def user_enabled_set(skip):
    import fnmatch
    cfg = _unit_dirs()[0]
    # (unit, enabled directly by a .wants link rather than reached by Wants=)
    queue = []
    for root in _USER_ROOTS:
        queue += [(u, True) for u in _wants_dir_units([cfg], root)]
    units, skipped, seen = [], [], set()
    while queue:
        u, direct = queue.pop(0)
        if u in seen:
            continue
        seen.add(u)
        if u in _NEVER:
            skipped.append((u, "the session bus is started by the session"))
            continue
        if u.endswith(".target"):
            # only the user's own .wants of a target, never its Wants= or the
            # distro's .wants (graphical-session.target -> basic.target ->
            # /etc sockets.target.wants/dbus.socket)
            if any(fnmatch.fnmatch(u, pat) for pat in _SESSION_TARGETS):
                skipped.append((u, "phase 2 session target"))
            else:
                queue += [(w, True) for w in _wants_dir_units([cfg], u)]
            continue
        if not u.endswith((".service", ".socket", ".timer", ".path")):
            if direct:
                skipped.append((u, "unsupported unit type"))
            continue
        if u in skip:
            skipped.append((u, "handled elsewhere (skip list)"))
            continue
        if "@" in u:
            skipped.append((u, "template unit"))
            continue
        if u.endswith(".path"):
            skipped.append((u, "path unit"))
            continue
        try:
            text = user_unit_text(u)
        except Masked:
            skipped.append((u, "masked"))
            continue
        except ValueError as e:
            skipped.append((u, "unreadable: %s" % e))
            continue
        if text is None:
            skipped.append((u, "not found"))
            continue
        parsed = parse_unit(text)
        unit = parsed.get("Unit", [])
        for key in ("Wants", "Requires"):
            for v in _get_all(unit, key):
                queue += [(w, False) for w in v.split()]
        # a socket's or timer's .svc runs its service, so that service's hard
        # deps must be in the set or their dep= lines are dropped. Not its
        # Wants=: systemd starts those at first activation, here they would
        # start at every login.
        try:
            act = _user_activates(u, parsed, skip)
        except Skip:
            act = None
        if act and "@" not in act:
            try:
                atext = user_unit_text(act)
            except (Masked, ValueError):
                atext = None
            if atext:
                aunit = parse_unit(atext).get("Unit", [])
                for key in ("Requires", "BindsTo"):
                    for v in _get_all(aunit, key):
                        queue += [(w, False) for w in v.split()]
                try:
                    queue += [(w, True) for w in sorted(os.listdir(os.path.join(cfg, act + ".requires")))]
                except OSError:
                    pass
        queue += [(w, True) for w in _wants_dir_units([cfg], u)]
        units.append(u)
    try:
        entries = sorted(os.listdir(cfg))
    except OSError:
        entries = []
    for e in entries:
        tgt = e.rsplit(".", 1)[0]
        if not e.endswith((".wants", ".requires")) or \
                not any(fnmatch.fnmatch(tgt, pat) for pat in _SESSION_TARGETS):
            continue
        for u in _wants_dir_units([cfg], tgt):
            if u not in seen:
                seen.add(u)
                skipped.append((u, "phase 2 session target"))
    return units, skipped


def _user_sections(unit):
    try:
        text = user_unit_text(unit)
    except Masked:
        raise Skip("%s is masked" % unit)
    if text is None:
        raise Skip("%s not found" % unit)
    return parse_unit(text)


def _base(unit):
    return unit.rsplit(".", 1)[0]


def _activation_guard(target, skip):
    if target in _NEVER:
        raise Skip("activates %s: the session bus is started by the session" % target)
    if target in skip:
        raise Skip("activates skip-listed %s" % target)


def _user_activates(unit, sections, skip):
    """The service a .socket or .timer runs, or None for other units. Raises
    Skip where _user_translate would skip the unit."""
    if unit.endswith(".socket"):
        socket_lines(sections)
        target = _get_last(sections.get("Socket", []), "Service") or _base(unit) + ".service"
    elif unit.endswith(".timer"):
        target = _get_last(sections.get("Timer", []), "Unit") or _base(unit) + ".service"
        if "@" in target:
            raise Skip("timer starts template instance %s — instances unsupported" % target)
    else:
        return None
    _activation_guard(target, skip)
    return target


NET_ONLINE = "network-online"


def _after_online(*sections):
    return any("network-online.target" in " ".join(_get_all(sec.get("Unit", []), "After")).split()
               for sec in sections)


def _net_online_svc():
    # schema-user has no network-online.target: one oneshot that After= units
    # dep= on, as NetworkManager-wait-online. Only where the system runs NM:
    # otherwise nm-online waits out its whole timeout for an NM that never comes.
    nm, true = _which("nm-online"), _which("true")
    if not (nm and true and os.path.exists(
            os.path.join(_root(), "etc/schema-init/services/network-manager.svc"))):
        return None
    # - : a timed-out wait still lets the dependents start, as in systemd
    return "%s\nname=%s\nexec=%s\nexec_pre=-%s -s -q -t 60\noneshot=1\ncritical=0\n" % (
        USER_MARK, NET_ONLINE, true, nm)


def _user_translate(unit, known, enabled, skip=(), net=False):
    sections = _user_sections(unit)
    name = _base(unit)
    svc_sections = sections
    target = _user_activates(unit, sections, skip)
    if unit.endswith(".socket"):
        name = _base(target)
        svc_sections = _user_sections(target)
        body = unit_to_svc(name, svc_sections, sections, target not in enabled, known)
    elif unit.endswith(".timer"):
        svc_sections = _user_sections(target)
        body = timer_to_svc(name, sections, svc_sections, known, _base(target))
    else:
        body = unit_to_svc(name, sections, None, False, known)
    lines = []
    if _after_online(sections, svc_sections):
        deps = sum(1 for ln in body.splitlines() if ln.startswith("dep="))
        if not net:
            lines.append("%sAfter=network-online.target not waited for: no NetworkManager"
                         % WARN_PREFIX)
        elif deps >= 8:
            lines.append("%sAfter=network-online.target not waited for: 8 dep= already"
                         % WARN_PREFIX)
        else:
            body = body.rstrip("\n") + "\ndep=%s\n" % NET_ONLINE
    for ln in _drop_cycles(name, body).splitlines():
        if ln == "needs_root=1":
            continue
        if ln.startswith(_USER_NO_HARDEN):
            k, v = ln.split("=", 1)
            if v != "0":
                lines.insert(0, "%s%s=%s dropped: schema-user cannot set up a mount "
                             "namespace" % (WARN_PREFIX, k, v))
            continue
        if ln.startswith("user="):
            lines.insert(0, "%sUser=%s dropped: user services run as the user"
                         % (WARN_PREFIX, ln[5:]))
            continue
        lines.append(ln)
    return name, USER_MARK + "\n" + "\n".join(lines) + "\n"


def _write_svc(path, body):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(body)


def _keep_hand_written(out_dir, stage):
    # schema-ctl --user add and hand-written .svc files live in the same dir
    try:
        entries = os.listdir(out_dir)
    except OSError:
        return
    for e in entries:
        src, dst = os.path.join(out_dir, e), os.path.join(stage, e)
        if os.path.exists(dst):
            continue
        if os.path.isdir(src):
            shutil.copytree(src, dst, symlinks=True)
            continue
        try:
            with open(src) as f:
                if f.readline().rstrip("\n") == USER_MARK:
                    continue
        except (OSError, ValueError):
            pass
        shutil.copy2(src, dst, follow_symlinks=False)


USER_BUS_MARK = "# generated by schema-import --user (bus activation)"


# unit -> the .svc that runs it, for bus routing and schema-systemd1-session
# (written to <runtime>/schema-user/units). A socket's .svc is named after
# its service and runs it. Left out: timers, and a lazy socket's .svc, where
# "start" only arms the listener -- routing to it would wait on a name nobody
# brings up.
def user_unit_map(done, bodies):
    m = {}
    for n, u in done.items():
        # a timer's .svc is the timer: "start" would fire the job
        if u.endswith(".timer") or "\nlisten_lazy=1\n" in bodies.get(n, ""):
            continue
        m[u] = n
        if u.endswith(".socket"):
            m[n + ".service"] = n
    return m
# the session bus's own search order (schema-dbus-session-run.sh), minus the
# routed dir itself: the first file to define a Name wins
_SESSION_ACT_DIRS = ("usr/local/lib/schema/dbus-1/services", "usr/share/dbus-1/services")


def _session_act_dirs():
    return [os.path.join(_USER["data"], "dbus-1/services")] + \
           [os.path.join(_root(), d) for d in _SESSION_ACT_DIRS]


# Route session activation of a unit schema-user runs through
# schema-bus-activate --user, so the bus never spawns a second, unsupervised
# copy. unit_svc: unit name -> generated .svc name. True when `out` changed.
def user_bus_overrides(unit_svc, out, dry_run=False, log=print):
    want, seen = {}, set()
    for d in _session_act_dirs():
        try:
            names = sorted(os.listdir(d))
        except OSError:
            continue
        for fn in names:
            if not fn.endswith(".service"):
                continue
            try:
                with open(os.path.join(d, fn)) as f:
                    act = parse_activation(f.read())
            except (OSError, ValueError):
                continue
            name, unit = act.get("Name"), act.get("SystemdService")
            if not name or name in seen:
                continue
            seen.add(name)
            if not unit:
                continue
            p = find_unit(unit)
            # SystemdService= is often an Alias= (dbus-org.*): follow the link
            svc = unit_svc.get(unit) or (p and unit_svc.get(os.path.basename(os.path.realpath(p))))
            if not svc:
                continue
            want[name + ".service"] = "\n".join([
                USER_BUS_MARK, "[D-BUS Service]", "Name=" + name,
                "Exec=%s --user %s %s %s" % (BUS_HELPER, svc, name, act.get("Exec") or "/bin/false"),
                "SystemdService=" + unit]) + "\n"
    try:
        have = {fn for fn in os.listdir(out) if _generated_user_bus(os.path.join(out, fn))}
    except OSError:
        have = set()
    changed = False
    for fn, body in sorted(want.items()):
        p = os.path.join(out, fn)
        try:
            with open(p) as f:
                if f.read() == body:
                    continue
        except (OSError, ValueError):
            pass
        changed = True
        log("bus-route  %s" % fn)
        if not dry_run:
            os.makedirs(out, mode=0o700, exist_ok=True)
            with open(p + ".tmp", "w") as f:
                f.write(body)
            os.replace(p + ".tmp", p)
    for fn in sorted(have - set(want)):
        changed = True
        log("bus-route  remove %s" % fn)
        if not dry_run:
            os.unlink(os.path.join(out, fn))
    return changed


def _generated_user_bus(path):
    try:
        with open(path) as f:
            return f.readline().rstrip("\n") == USER_BUS_MARK
    except (OSError, ValueError):
        return False


def _session_bus_reload(runtime):
    # the bus read its service dirs at start; a running one needs telling
    sock = os.path.join(runtime, "bus")
    if not os.path.exists(sock) or not shutil.which("dbus-send"):
        return
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS="unix:path=" + sock)
    try:
        subprocess.run(["dbus-send", "--session", "--type=method_call", "--print-reply",
                        "--dest=org.freedesktop.DBus", "/org/freedesktop/DBus",
                        "org.freedesktop.DBus.ReloadConfig"],
                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
    except (OSError, subprocess.SubprocessError):
        pass


def run_user_import(ctx, out_dir, skip_paths, dry_run=False, log=print):
    # built in out_dir.new and swapped in, so a failure leaves out_dir as it was
    out_dir = out_dir.rstrip("/") or "/"
    old = out_dir + ".old"
    # killed between the two renames of a previous swap: put the old set back,
    # or _keep_hand_written finds nothing and its hand-written .svc are lost
    if not os.path.isdir(out_dir) and os.path.isdir(old) and not dry_run:
        os.rename(old, out_dir)
    set_user_mode(ctx)
    stage = out_dir + ".new"
    saved = os.environ.get("SCHEMA_SVC_DIR")
    counts = {"generated": 0, "skipped": 0, "error": 0}
    try:
        skip = read_skip_list(skip_paths)
        units, skipped = user_enabled_set(skip)
        enabled = set(units)
        known = {_base(u) for u in units if not u.endswith(".timer")}
        net_svc = None if NET_ONLINE in known else _net_online_svc()
        if not dry_run:
            shutil.rmtree(stage, ignore_errors=True)
            os.makedirs(stage, mode=0o700)
        os.environ["SCHEMA_SVC_DIR"] = stage
        done, bodies, by_name = {}, [], {}
        # sockets, then timers: a socket's .svc carries its service, and a
        # timer's schedule must not lose to its same-name service
        order = lambda u: 0 if u.endswith(".socket") else 1 if u.endswith(".timer") else 2
        for u in sorted(units, key=order):
            try:
                name, body = _user_translate(u, known, enabled, skip, net_svc is not None)
                if name in done:
                    if done[name] == name + ".socket" and u == name + ".service":
                        continue   # runs through its socket's .svc
                    raise Skip("started by %s" % done[name])
            except Skip as e:
                skipped.append((u, str(e)))
                continue
            except ValueError as e:
                skipped.append((u, "unreadable: %s" % e))
                continue
            done[name] = u
            bodies.append(body)
            by_name[name] = body
            log("import  %s -> %s.svc" % (u, name))
            if not dry_run:
                _write_svc(os.path.join(stage, name + ".svc"), body)
            counts["generated"] += 1
        if net_svc and any("\ndep=%s\n" % NET_ONLINE in b for b in bodies):
            log("import  network-online.target -> %s.svc" % NET_ONLINE)
            if not dry_run:
                _write_svc(os.path.join(stage, NET_ONLINE + ".svc"), net_svc)
            counts["generated"] += 1
        counts["skipped"] = len(skipped)
        manifest = "".join("%s\t%s\n" % (u, r) for u, r in skipped)
        unit_svc = user_unit_map(done, by_name)
        bus_out = os.path.join(os.path.dirname(out_dir), "dbus-services")
        if dry_run:
            log(manifest, end="")
            user_bus_overrides(unit_svc, bus_out, dry_run=True, log=log)
            return counts
        _keep_hand_written(out_dir, stage)
        shutil.rmtree(old, ignore_errors=True)
        if os.path.isdir(out_dir):
            os.rename(out_dir, old)
        try:
            os.rename(stage, out_dir)
        except OSError:
            if os.path.isdir(old):
                os.rename(old, out_dir)
            raise
        shutil.rmtree(old, ignore_errors=True)
        mpath = os.path.join(os.path.dirname(out_dir), "skipped")
        fd = os.open(mpath + ".new", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(manifest)
        os.rename(mpath + ".new", mpath)
        upath = os.path.join(os.path.dirname(out_dir), "units")
        fd = os.open(upath + ".new", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write("".join("%s\t%s\n" % kv for kv in sorted(unit_svc.items())))
        os.rename(upath + ".new", upath)
        # the services are in place: a routing failure is logged, not an import failure
        try:
            if user_bus_overrides(unit_svc, bus_out, log=log) and ctx["runtime"]:
                _session_bus_reload(ctx["runtime"])
        except OSError as e:
            log("bus-route  failed: %s" % e)
    except OSError as e:
        print("user import failed: %s" % e, file=sys.stderr)
        counts["error"] += 1
        shutil.rmtree(stage, ignore_errors=True)
    finally:
        if saved is None:
            os.environ.pop("SCHEMA_SVC_DIR", None)
        else:
            os.environ["SCHEMA_SVC_DIR"] = saved
        set_user_mode(None)
    return counts


def _user_main(args):
    import pwd
    xdg = os.environ.get("XDG_RUNTIME_DIR")
    if not xdg:
        print("schema-import --user: XDG_RUNTIME_DIR is not set", file=sys.stderr)
        return 2
    pw = pwd.getpwuid(os.getuid())
    ctx = user_context(os.environ.get("HOME") or pw.pw_dir, pw.pw_uid, pw.pw_name, os.environ)
    out = args.out or os.path.join(xdg, "schema-user/services")
    skips = [os.path.join(_root(), "etc/schema-init/user-skip"),
             os.path.join(ctx["config"], "schema-user/skip")]
    c = run_user_import(ctx, out, skips, dry_run=args.dry_run)
    print("user-import generated=%(generated)d skipped=%(skipped)d error=%(error)d" % c)
    return 1 if c["error"] else 0


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
    ap.add_argument("-b", "--bus-overrides", action="store_true",
                    help="only regenerate the D-Bus activation overrides that route bus names to PID 1")
    ap.add_argument("--rc", action="store_true",
                    help="import what sysvinit starts in the default runlevel (/etc/rcN.d)")
    ap.add_argument("--runit", action="store_true",
                    help="import what runit starts at boot (/etc/runit/runsvdir/default)")
    ap.add_argument("--after", metavar="SVC",
                    help="with --runit: every imported service also waits for SVC (the boot oneshot)")
    ap.add_argument("--start", action="store_true",
                    help="when schema-init is PID 1, load and start each new .svc now (schema-ctl add)")
    ap.add_argument("--rc-skip", metavar="FILE",
                    help="init script or runit service names (whitespace-separated, # comments) "
                         "--rc or --runit leaves out")
    ap.add_argument("--user", action="store_true",
                    help="generate this user's enabled user units into "
                         "$XDG_RUNTIME_DIR/schema-user/services for schema-user")
    ap.add_argument("--out", metavar="DIR",
                    help="with --user: write the .svc files here instead")
    args = ap.parse_args(argv)
    if args.user:
        return _user_main(args)
    if args.bus_overrides:
        c = bus_overrides(dry_run=args.dry_run)
        print("bus-overrides written=%(written)d kept=%(kept)d removed=%(removed)d skipped=%(skipped)d" % c)
        return 0
    units = args.units or None
    if args.enabled:
        units = (units or []) + [u for u in enabled_units() if u not in (units or [])]
    fallback = ()
    if args.rc:
        skip = read_skip(args.rc_skip) if args.rc_skip else set()
        rc = rc_units(skip)
        fallback = {u for u in rc if u.endswith(".service")}
        units = (units or []) + [u for u in rc if u not in (units or [])]
    if args.runit:
        global runit_after
        runit_after = args.after
        skip = read_skip(args.rc_skip) if args.rc_skip else set()
        units = (units or []) + [u for u in runit_units(skip) if u not in (units or [])]
    written = []
    counts = drain(units=units, force=args.force, dry_run=args.dry_run, written=written,
                   lsb_fallback=fallback)
    if args.start and written and _read("/proc/1/comm").strip() == "schema-init":
        for out in written:
            try:
                if subprocess.run(["schema-ctl", "add", out]).returncode:
                    print("schema-ctl add %s failed" % out, file=sys.stderr)
            except OSError as e:
                print("schema-ctl add %s: %s" % (out, e), file=sys.stderr)
    print("imported=%(imported)d exists=%(exists)d not-found=%(not-found)d "
          "skipped=%(skipped)d error=%(error)d" % counts)
    try:
        bus_overrides(dry_run=args.dry_run)
    except Exception as e:
        print("bus-overrides failed: %s" % e, file=sys.stderr)
    return 1 if counts["error"] else 0


if __name__ == "__main__":
    sys.exit(main())
