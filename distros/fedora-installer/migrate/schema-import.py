#!/usr/bin/env python3
"""schema-import — drain the systemctl shim's enable-intent queue into schema
service files. Phase 2 of the compat translator: the runtime importer.

The shim (schema-systemctl) records `enable`/`preset` intent to
pending.list; this tool reads each queued unit, parses its [Service]/[Install]
sections, and emits a schema `.svc` on 80/20 field coverage. Ratholes
(Type=notify, templates, missing ExecStart) are logged and skipped, never
half-translated.

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

# ---------------------------------------------------------------------------
# Pure translation core (no filesystem) — unit-tested in test_import_units.py.
# ---------------------------------------------------------------------------

_EXEC_PREFIX = "@-:+!"   # systemd ExecStart special-char prefixes, stripped


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
    schema-init exec()s with no shell. A token that stays unresolved (its var
    came from a dropped EnvironmentFile, say) is dropped — matching systemd,
    which expands an unset variable to nothing. Returns (argv, dropped_count)."""
    out, dropped = [], 0
    for tok in argv:
        if "$" not in tok:
            out.append(tok)
            continue
        rep = _VAR_RE.sub(lambda m: env.get(m.group(1), "\0"), tok)
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


def unit_to_svc(name, sections):
    """Translate parsed unit sections into schema .svc text.

    Returns the .svc file body (str). Raises Skip(reason) for units we refuse
    to half-translate (Type=notify, template, no ExecStart).
    """
    if "@" in name:
        raise Skip("template unit (%s) — instances unsupported" % name)

    svc = sections.get("Service", [])
    inst = sections.get("Install", [])

    stype = _get_last(svc, "Type").lower()
    if stype in ("notify", "notify-reload"):
        raise Skip("Type=%s — needs sd_notify readiness protocol" % stype)
    if stype in ("forking", "dbus"):
        raise Skip("Type=%s — schema-init tracks only the direct child, "
                   "cannot supervise a %s daemon" % (stype, stype))

    execstarts = _get_all(svc, "ExecStart")
    execstart = execstarts[-1] if execstarts else ""
    argv = _exec_tokens(execstart)
    if not argv:
        raise Skip("no usable ExecStart")

    env_pairs = _env_pairs(_get_all(svc, "Environment"))
    env_map = dict(p.split("=", 1) for p in env_pairs)
    argv, dropped_args = _expand_argv(argv, env_map)
    if not argv:
        raise Skip("ExecStart is entirely unresolved variables")

    lines = ["name=%s" % name, "exec=%s" % argv[0]]
    for a in argv[1:]:
        lines.append("args=%s" % a)

    for e in env_pairs:
        lines.append("env=%s" % e)

    if stype == "oneshot":
        lines.append("oneshot=1")

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

    # [Install] presence is why the unit was queued; note if it's missing.
    notes = []
    if dropped_args:
        notes.append("dropped %d unresolved $VAR arg(s)" % dropped_args)
    if not inst:
        notes.append("no [Install] section")
    for k in ("EnvironmentFile", "ExecStartPre", "WorkingDirectory"):
        if _get_last(svc, k):
            notes.append("dropped %s" % k)
    if notes:
        lines.insert(0, "# schema-import: " + "; ".join(notes))

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


def import_one(name, force=False):
    """Translate one queued unit. Returns (status, detail) where status is one
    of: imported, exists, not-found, skipped, error."""
    name = name[:-len(".service")] if name.endswith(".service") else name
    out = os.path.join(svc_dir(), name + ".svc")
    if os.path.exists(out) and not force:
        return ("exists", out)
    path = find_unit(name)
    if not path:
        return ("not-found", name)
    try:
        with open(path) as f:
            sections = parse_unit(f.read())
        body = unit_to_svc(name, sections)
    except Skip as s:
        return ("skipped", str(s))
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
            log("skip    %s: %s" % (name, detail))
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
