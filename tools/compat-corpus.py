#!/usr/bin/env python3
# Compatibility corpus: real systemd units through schema-import, directive by
# directive. For each unit: imported or skipped (and why); for each directive in
# it: mapped (the importer read it, no warning names it), warned (read, and a
# warning says what is lost), or silent (the importer never looked at it: lost
# without a word, the case that matters most). Each imported .svc then goes
# through schema-init --check, so "mapped" also means "PID 1's loader takes it".
#
#   tools/compat-corpus.py [--json OUT] [--label NAME] [--foreign [--security S]] DIR [DIR...]
#
# DIRs hold *.service files (a distro's /usr/lib/systemd/system, or an extracted
# package tree from tools/compat-fetch.py). Output: a summary on stdout, the full
# per-directive record as JSON (machine-readable, for diffing runs and distros).
import argparse, collections, glob, importlib.util, json, os, re, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location(
    "schema_import", os.path.join(REPO, "distros/fedora-installer/migrate/schema-import.py"))
si = importlib.util.module_from_spec(spec)
spec.loader.exec_module(si)


# a directive name that remembers whether the importer ever matched it: ==, !=,
# `in (...)`, dict/set membership, startswith and endswith all land here
class Key(str):
    def __new__(cls, s):
        k = str.__new__(cls, s)
        k.hit = False
        return k

    def __eq__(self, other):
        r = str.__eq__(self, other)
        if r is True:
            self.hit = True
        return r

    def __ne__(self, other):
        r = str.__ne__(self, other)
        if r is False:
            self.hit = True
        return r

    __hash__ = str.__hash__

    def startswith(self, *a):
        r = str.startswith(self, *a)
        if r:
            self.hit = True
        return r

    def endswith(self, *a):
        r = str.endswith(self, *a)
        if r:
            self.hit = True
        return r


# no runtime effect in the .svc: descriptive, or the [Install] keys only the enable
# step reads (Alias= is not here: dbus-org.* aliases add the dbus dep=). Wants=:
# --enabled imports what it names and every .svc starts on its own; with no
# After= systemd orders nothing either, and After= is read for after=
INERT = {("Unit", "Description"), ("Unit", "Documentation"), ("Unit", "Wants"), ("Install", "WantedBy"),
         ("Install", "RequiredBy"), ("Install", "UpheldBy"), ("Install", "Also"),
         ("Install", "DefaultInstance")}


def inert(sec, key):
    return (sec, key) in INERT


# systemd's drop-ins for this tree: <unit>.d/*.conf in name order, after the unit.
# read_unit() merges the host's unit dirs, which is wrong for a foreign tree
def unit_text(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    for conf in sorted(glob.glob(path + ".d/*.conf")):
        text += "\n" + open(conf, encoding="utf-8", errors="replace").read()
    return text


# a sibling .socket activates the service when the service names it (Sockets=,
# Requires=, Wants=, BindsTo=) or the socket is enabled by default
def socket_for(path, sections):
    sp = path[:-len(".service")] + ".socket"
    if not os.path.exists(sp):
        return None
    sock = tracked(unit_text(sp))
    name = os.path.basename(sp)
    named = any(name in v.split() for sec, k in (("Unit", "Requires"), ("Unit", "Wants"), ("Unit", "BindsTo"),
                                                 ("Service", "Sockets"))
                for kk, v in sections.get(sec, []) if str.__eq__(kk, k))
    enabled = any(str.__eq__(k, "WantedBy") and "sockets.target" in v.split() for k, v in sock.get("Install", []))
    return sock if named or enabled else None


def tracked(text):
    return {sec: [(Key(k), v) for k, v in pairs] for sec, pairs in si.parse_unit(text).items()}


# PID 1's own loader on the generated .svc: None when it loads, else its reason
def check_svc(body, name, scratch):
    d = os.path.join(scratch, name, "etc/schema-init/services")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, name + ".svc")
    with open(p, "w") as f:
        f.write(body)
    os.chmod(p, 0o644)
    r = subprocess.run([os.path.join(REPO, "schema-init"), "--check", "--root",
                        os.path.join(scratch, name), "--cmdline", ""], capture_output=True, text=True)
    errs = [ln for ln in r.stdout.splitlines() if ln.startswith("error: ")]
    if errs:
        return errs[0][7:]
    # usage, path-too-long and crashes say so only on stderr, or not at all
    if r.returncode not in (0, 1):
        return "--check exited %d: %s" % (r.returncode, r.stderr.strip()[:200])
    return None


def run_unit(path, scratch, known):
    name = os.path.basename(path)[:-len(".service")]
    rec = {"unit": os.path.basename(path), "status": None, "reason": None, "warnings": [],
           "directives": [], "loader": None}
    try:
        text = unit_text(path)
    except OSError as e:
        rec.update(status="unreadable", reason=str(e))
        return rec
    sections = tracked(text)
    # deps resolve against the other services here, as --enabled resolves them
    # against what it imports
    sock = socket_for(path, sections)
    try:
        denied = si._denied(name)
        if denied:
            raise si.Skip(denied)
        body = si.unit_to_svc(name, sections, sock=sock, known=known)
        rec["status"] = "imported"
    except si.Skip as e:
        rec.update(status="skipped", reason=str(e))
        body = None
    except Exception as e:
        rec.update(status="error", reason="%s: %s" % (type(e).__name__, e))
        body = None
    if body:
        rec["warnings"] = [ln[len(si.WARN_PREFIX):] for ln in body.splitlines() if ln.startswith(si.WARN_PREFIX)]
        rec["loader"] = check_svc(body, name, scratch)
    # a warning is about the directive it leads with ("WatchdogSec= without
    # Type=notify" is WatchdogSec's, not Type's), or the list after "not enforced:"
    warned_keys = set()
    for w in rec["warnings"]:
        m = re.match(r"([A-Z][A-Za-z]+)[= ]", w)
        if m:
            warned_keys.add(m.group(1))
        m = re.search(r"not enforced: ([A-Za-z ]+)$", w)
        if m:
            warned_keys.update(m.group(1).split())
    for sec, pairs in sections.items():
        for k, v in pairs:
            if inert(sec, str(k)):
                st = "inert"
            elif rec["status"] != "imported":
                # a skip stops reading at its reason: what comes after says nothing
                st = "skipped-unit"
            elif not k.hit:
                st = "silent"
            elif str(k) in warned_keys:
                st = "warned"
            else:
                st = "mapped"
            rec["directives"].append({"section": sec, "key": str(k), "value": v, "status": st})
    return rec


# --foreign: the tree isn't this host's, so judge it against one fixed machine,
# not whatever box runs the tool: bare-metal x86-64, cgroup v2, outside any user
# namespace, the distro's own security modules. A bare ExecStart name is taken
# as found on PATH, as it would be on the distro the unit ships for
REFERENCE = {"virt": "none", "virt_kind": None, "userns": False, "cgroup_v2": True,
             "controllers": {"cpuset", "cpu", "io", "memory", "hugetlb", "pids", "rdma", "misc"},
             "cpus": 4, "arch": "x86-64", "cpu_vendor": "GenuineIntel", "cpu_flags": set()}


def foreign(security):
    si._FACTS = dict(REFERENCE, security=set(security.split(",")) - {""})
    si._which = lambda cmd: None if "/" in cmd else "/usr/bin/" + cmd


def main():
    ap = argparse.ArgumentParser(description="Real systemd units through schema-import, "
                                 "directive by directive: mapped, warned, silent, inert.")
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--json", help="write the per-unit, per-directive record here")
    ap.add_argument("--label", default="corpus")
    ap.add_argument("--skip-systemd", action="store_true", help="leave out systemd-*.service (systemd's own)")
    ap.add_argument("--foreign", action="store_true",
                    help="judge machine-fixed conditions against a fixed reference host, not this one")
    ap.add_argument("--security", default="audit",
                    help="--foreign: the reference host's security modules (debian/ubuntu: apparmor,audit)")
    a = ap.parse_args()
    if a.foreign:
        foreign(a.security)
    if not os.access(os.path.join(REPO, "schema-init"), os.X_OK):
        sys.exit("compat-corpus: build ./schema-init first (make schema-init)")
    units = sorted({p for d in a.dirs for p in glob.glob(os.path.join(d, "*.service")) if not os.path.islink(p)})
    if a.skip_systemd:
        units = [u for u in units if not os.path.basename(u).startswith("systemd-")]
    with tempfile.TemporaryDirectory() as scratch:
        known = {os.path.basename(u)[:-len(".service")] for u in units}
        recs = [run_unit(u, scratch, known) for u in units]

    st = collections.Counter(r["status"] for r in recs)
    reasons = collections.Counter(re.sub(r"=\S*|\s+\S*/\S*", "", r["reason"] or "").strip()[:80]
                                  for r in recs if r["status"] == "skipped")
    per = collections.defaultdict(collections.Counter)
    for r in recs:
        for d in r["directives"]:
            per["%s.%s" % (d["section"], d["key"])][d["status"]] += 1
    loader_bad = [(r["unit"], r["loader"]) for r in recs if r["loader"]]
    print("%s: %d units  %s" % (a.label, len(recs), dict(st)))
    if loader_bad:
        print("\nIMPORTED BUT PID 1'S LOADER REFUSES (%d):" % len(loader_bad))
        for u, why in loader_bad:
            print("  %s: %s" % (u, why))
    print("\nskip reasons (top 15):")
    for why, n in reasons.most_common(15):
        print("  %4d  %s" % (n, why))
    silent = sorted(((c["silent"], k) for k, c in per.items() if c["silent"]), reverse=True)
    print("\nSILENTLY IGNORED directives in imported units (read by nothing, no warning), top 30:")
    for n, k in silent[:30]:
        print("  %4d  %s" % (n, k))
    warned = sorted(((c["warned"], k) for k, c in per.items() if c["warned"]), reverse=True)
    print("\nwarned (lossy, said out loud), top 15:")
    for n, k in warned[:15]:
        print("  %4d  %s" % (n, k))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"label": a.label, "units": recs,
                       "directives": {k: dict(c) for k, c in sorted(per.items())}}, f, indent=1)
        print("\nfull record: %s" % a.json)


if __name__ == "__main__":
    main()
