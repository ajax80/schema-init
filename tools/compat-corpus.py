#!/usr/bin/env python3
# Compatibility corpus: real systemd units through schema-import, directive by
# directive. For each unit: imported or skipped (and why); for each directive in
# it: mapped (the importer read it, no warning names it), warned (read, and a
# warning says what is lost), or silent (the importer never looked at it: lost
# without a word, the case that matters most). Each imported .svc then goes
# through schema-init --check, so "mapped" also means "PID 1's loader takes it".
#
#   tools/compat-corpus.py [--json OUT] [--label NAME] DIR [DIR...]
#
# DIRs hold *.service files (a distro's /usr/lib/systemd/system, or an extracted
# package tree). Output: a summary on stdout, the full per-directive record as
# JSON (machine-readable, for diffing runs and distros).
import argparse, collections, glob, importlib.util, json, os, re, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location(
    "schema_import", os.path.join(REPO, "distros/fedora-installer/migrate/schema-import.py"))
si = importlib.util.module_from_spec(spec)
spec.loader.exec_module(si)


class Key(str):
    """A directive name that remembers whether the importer ever matched it:
    ==, `in (...)`, dict membership and startswith all end in __eq__/startswith."""
    def __new__(cls, s):
        k = str.__new__(cls, s)
        k.hit = False
        return k

    def __eq__(self, other):
        r = str.__eq__(self, other)
        if r is True:
            self.hit = True
        return r

    __hash__ = str.__hash__

    def startswith(self, *a):
        r = str.startswith(self, *a)
        if r:
            self.hit = True
        return r


# no runtime effect in the .svc: descriptive, or [Install] (the enable step reads
# that, not unit_to_svc)
INERT = {("Unit", "Description"), ("Unit", "Documentation")}


def inert(sec, key):
    return sec == "Install" or (sec, key) in INERT


def tracked(text):
    return {sec: [(Key(k), v) for k, v in pairs] for sec, pairs in si.parse_unit(text).items()}


def check_svc(body, name, scratch):
    """PID 1's own loader on the generated .svc: None when it loads, else its reason."""
    d = os.path.join(scratch, name, "etc/schema-init/services")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, name + ".svc")
    with open(p, "w") as f:
        f.write(body)
    os.chmod(p, 0o644)
    r = subprocess.run([os.path.join(REPO, "schema-init"), "--check", "--root",
                        os.path.join(scratch, name), "--cmdline", ""], capture_output=True, text=True)
    errs = [ln for ln in r.stdout.splitlines() if ln.startswith("error: ")]
    return errs[0][7:] if errs else None


def run_unit(path, scratch, known):
    name = os.path.basename(path)[:-len(".service")]
    rec = {"unit": os.path.basename(path), "status": None, "reason": None, "warnings": [],
           "directives": [], "loader": None}
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError as e:
        rec.update(status="unreadable", reason=str(e))
        return rec
    sections = tracked(text)
    # as --enabled imports: deps resolve against the other services here, and a
    # sibling .socket is the activating socket
    sock = None
    sp = path[:-len(".service")] + ".socket"
    if os.path.exists(sp):
        sock = tracked(open(sp, encoding="utf-8", errors="replace").read())
    try:
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
    warned_text = " ".join(rec["warnings"]) + " " + (rec["reason"] or "")
    for sec, pairs in sections.items():
        for k, v in pairs:
            if inert(sec, str(k)):
                st = "inert"
            elif rec["status"] != "imported":
                # a skip stops reading at its reason: what comes after says nothing
                st = "skipped-unit"
            elif not k.hit:
                st = "silent"
            elif re.search(r"(?<![A-Za-z])%s(?![A-Za-z])" % re.escape(str(k)), warned_text):
                st = "warned"
            else:
                st = "mapped"
            rec["directives"].append({"section": sec, "key": str(k), "value": v, "status": st})
    return rec


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--json", help="write the per-unit, per-directive record here")
    ap.add_argument("--label", default="corpus")
    ap.add_argument("--skip-systemd", action="store_true", help="leave out systemd-*.service (systemd's own)")
    a = ap.parse_args()
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
