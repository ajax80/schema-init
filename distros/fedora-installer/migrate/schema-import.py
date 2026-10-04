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


def timer_to_svc(name, timer_sections, svc_sections):
    """A .timer and the .service it starts as one schema timer .svc, named after
    the timer."""
    tlines, tnotes = timer_lines(timer_sections)
    body = unit_to_svc(name, svc_sections)
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


def unit_to_svc(name, sections):
    """Translate parsed unit sections into schema .svc text.

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
    expand = bool(env_files) and any("$" in a for a in argv[1:])
    dropped_args = 0
    if env_files and "$" in argv[0]:
        raise Skip("ExecStart binary path uses a variable from an EnvironmentFile")
    if not env_files:
        env_map = dict(p.split("=", 1) for p in env_pairs)
        argv, dropped_args = _expand_argv(argv, env_map)
        if not argv:
            raise Skip("ExecStart is entirely unresolved variables")

    lines = ["name=%s" % name, "exec=%s" % argv[0]]
    for a in argv[1:]:
        lines.append("args=%s" % a)

    for e in env_pairs:
        lines.append("env=%s" % e)
    for ef in env_files:
        lines.append("env_file=%s" % ef)
    if expand:
        lines.append("expand_args=1")

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
    lines.extend(hard)

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
    if not inst:
        notes.append("no [Install] section")
    notes += env_file_notes
    for k in ("ExecStartPre", "WorkingDirectory"):
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


def import_one(name, force=False):
    """Translate one queued unit. Returns (status, detail) where status is one
    of: imported, exists, not-found, skipped, error."""
    timer = name.endswith(".timer")
    name = name[:-len(".service")] if name.endswith(".service") else name
    name = name[:-len(".timer")] if timer else name
    out = os.path.join(svc_dir(), name + ".svc")
    if os.path.exists(out) and not force:
        return ("exists", out)
    path = find_unit(name + ".timer" if timer else name)
    if not path:
        return ("not-found", name)
    try:
        with open(path) as f:
            text = f.read()
        sections = parse_unit(text)
        if timer:
            target = _get_last(sections.get("Timer", []), "Unit") or name + ".service"
            if "@" in target:
                raise Skip("timer starts template instance %s — instances unsupported" % target)
            spath = find_unit(target)
            if not spath:
                raise Skip("timer's %s not found" % target)
            with open(spath) as f:
                body = timer_to_svc(name, sections, parse_unit(f.read()))
        else:
            if not sections.get("Install") and _timer_for(name):
                raise Skip("started by %s, which owns %s.svc — enable the timer"
                           % (_timer_for(name), name))
            body = unit_to_svc(name, sections)
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
        status, detail = import_one(name, force=force)
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
    args = ap.parse_args(argv)
    counts = drain(units=args.units or None, force=args.force, dry_run=args.dry_run)
    print("imported=%(imported)d exists=%(exists)d not-found=%(not-found)d "
          "skipped=%(skipped)d error=%(error)d" % counts)
    return 1 if counts["error"] else 0


if __name__ == "__main__":
    sys.exit(main())
