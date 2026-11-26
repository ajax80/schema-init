#!/usr/bin/env python3
# tools/compat-fetch.py: a pinned package is verified against its lock hash and
# only its unit dir comes out (drop-ins kept, links recreated not followed,
# nothing else). compat-corpus --foreign judges a foreign tree against the fixed
# reference host, not the machine running it. Offline: file:// archives.
import hashlib, io, json, os, subprocess, sys, tarfile, tempfile
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FETCH = os.path.join(REPO, "tools/compat-fetch.py")


def tarball(files, mode):
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode=mode) as t:
        for name, data in files.items():
            ti = tarfile.TarInfo(name)
            if data.startswith("->"):
                ti.type, ti.linkname = tarfile.SYMTYPE, data[2:]
                t.addfile(ti)
            else:
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data.encode()))
    return b.getvalue()


def ar(members):
    out = b"!<arch>\n"
    for name, data in members:
        out += b"%-16s%-12d%-6d%-6d%-8s%-10d`\n" % (name.encode(), 0, 0, 0, b"100644", len(data)) + data
        out += b"\n" if len(data) & 1 else b""
    return out


UNIT = "[Service]\nExecStart=/usr/bin/x\n"
with tempfile.TemporaryDirectory() as d:
    deb = ar([("debian-binary", b"2.0\n"), ("control.tar.xz", tarball({"./control": "x"}, "w:xz")),
              ("data.tar.xz", tarball({"./lib/systemd/system/a.service": UNIT,
                                       "./lib/systemd/system/a.service.d/o.conf": "[Service]\nUser=nobody\n",
                                       "./lib/systemd/system/masked.service": "->/dev/null",
                                       "./lib/systemd/system/user/u.service": UNIT,
                                       "./lib/systemd/system/..": "->/etc",
                                       "./lib/systemd/system/deep/x/y.service": UNIT,
                                       "./usr/bin/x": "binary"}, "w:xz"))])
    pkg = tarball({".PKGINFO": "x", "usr/lib/systemd/system/b.service": UNIT,
                   "usr/lib/systemd/system-preset/p.preset": "x"}, "w:gz")
    for n, blob in (("a.deb", deb), ("b.pkg.tar.gz", pkg)):
        open(os.path.join(d, n), "wb").write(blob)
    sha = lambda b: hashlib.sha256(b).hexdigest()
    lock = {"deb": {"format": "deb", "base": "file://" + d, "packages": [
                {"name": "a", "version": "1", "path": "a.deb", "sha256": sha(deb)}]},
            "pkg": {"format": "pkg", "base": "file://" + d, "packages": [
                {"name": "b", "version": "1", "path": "b.pkg.tar.gz", "sha256": sha(pkg)}]}}
    lp = os.path.join(d, "lock.json")
    json.dump(lock, open(lp, "w"))
    out = os.path.join(d, "out")
    r = subprocess.run([sys.executable, FETCH, "--lock", lp, out], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    got = sorted(os.path.relpath(os.path.join(dp, f), out) for dp, _, fs in os.walk(out)
                 for f in fs if ".cache" not in dp)
    assert got == ["deb/a.service", "deb/a.service.d/o.conf", "deb/masked.service", "pkg/b.service"], got
    assert os.readlink(os.path.join(out, "deb/masked.service")) == "/dev/null"
    assert open(os.path.join(out, "deb/a.service.d/o.conf")).read().startswith("[Service]")

    lock["pkg"]["packages"][0]["sha256"] = "0" * 64
    json.dump(lock, open(lp, "w"))
    r = subprocess.run([sys.executable, FETCH, "--lock", lp, os.path.join(d, "out2")], capture_output=True, text=True)
    assert r.returncode != 0 and "sha256 mismatch" in r.stderr, (r.returncode, r.stderr)

    # --foreign: the reference host is bare metal with no apparmor unless told;
    # a bare ExecStart name counts as found
    t = os.path.join(d, "tree")
    os.makedirs(t)
    open(os.path.join(t, "bare.service"), "w").write(
        "[Unit]\nConditionVirtualization=no\nConditionVirtualization=!private-users\n"
        "[Service]\nExecStart=surely-not-on-this-host-x9 --run\n")
    open(os.path.join(t, "aa.service"), "w").write(
        "[Unit]\nConditionSecurity=apparmor\n[Service]\nExecStart=/usr/bin/aa\n")

    def run(*a):
        r = subprocess.run([sys.executable, os.path.join(REPO, "tools/compat-corpus.py"), "--json",
                            os.path.join(d, "r.json")] + list(a) + [t], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        return {u["unit"]: u for u in json.load(open(os.path.join(d, "r.json")))["units"]}
    rec = run("--foreign")
    assert rec["bare.service"]["status"] == "imported", rec["bare.service"]
    assert rec["aa.service"]["status"] == "skipped", rec["aa.service"]
    assert run("--foreign", "--security", "apparmor,audit")["aa.service"]["status"] == "imported"
    assert run()["bare.service"]["status"] == "skipped"
print("ok compat_fetch")
