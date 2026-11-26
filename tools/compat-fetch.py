#!/usr/bin/env python3
# Fetch the compat corpus's foreign unit trees: real packages from Debian,
# Ubuntu and Arch at pinned versions, out of dated snapshot archives whose URLs
# never change, so a run today and a run next year read the same bytes.
#
#   tools/compat-fetch.py --resolve      re-pin tools/compat-corpus.lock.json
#                                        (versions + sha256) from each snapshot
#   tools/compat-fetch.py OUT            download, verify sha256, extract each
#                                        package's systemd unit dir to OUT/<distro>/
#
# Nothing is vendored: the lock holds names, versions, paths and hashes only.
# A package whose hash doesn't match the lock is an error, never a re-resolve.
import argparse, hashlib, io, json, lzma, os, re, sys, tarfile, urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(REPO, "tools/compat-corpus.lock.json")
UNIT_DIRS = ("usr/lib/systemd/system/", "lib/systemd/system/")


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "compat-fetch"}),
                                timeout=300) as r:
        return r.read()


# Debian-style Packages index -> {name: (version, path, sha256)}. One suite, no
# updates pocket: the snapshot date alone fixes what's in it
def deb_index(d):
    out = {}
    for comp in d["components"]:
        raw = lzma.decompress(get("%s/dists/%s/%s/binary-amd64/Packages.xz" % (d["base"], d["suite"], comp)))
        for stanza in raw.decode("utf-8", "replace").split("\n\n"):
            f = dict(m.groups() for m in re.finditer(r"^([A-Za-z0-9-]+): (.*)$", stanza, re.M))
            if "Package" in f and f["Package"] not in out:
                out[f["Package"]] = (f["Version"], f["Filename"], f["SHA256"])
    return out


# Arch repo db: a tarball of <pkg>-<ver>/desc files, %FIELD%\nvalue blocks
def pkg_index(d):
    out = {}
    for repo in d["repos"]:
        with tarfile.open(fileobj=io.BytesIO(get("%s/%s/os/x86_64/%s.db" % (d["base"], repo, repo)))) as t:
            for m in t:
                if not m.name.endswith("/desc"):
                    continue
                f = dict(re.findall(r"%([A-Z0-9]+)%\n([^\n]*)", t.extractfile(m).read().decode()))
                out[f["NAME"]] = (f["VERSION"], "%s/os/x86_64/%s" % (repo, f["FILENAME"]), f["SHA256SUM"])
    return out


def resolve(lock):
    for name, d in lock.items():
        idx = deb_index(d) if d["format"] == "deb" else pkg_index(d)
        missing = [p for p in d["want"] if p not in idx]
        d["packages"] = [{"name": p, "version": idx[p][0], "path": idx[p][1], "sha256": idx[p][2]}
                         for p in d["want"] if p in idx]
        print("%s: %d pinned%s" % (name, len(d["packages"]),
                                   ", not in snapshot: " + " ".join(missing) if missing else ""))
    with open(LOCK, "w") as f:
        json.dump(lock, f, indent=1)
        f.write("\n")


# a .deb is an ar archive; its data.tar.{gz,xz,zst} member holds the files
def deb_data(blob):
    if blob[:8] != b"!<arch>\n":
        raise ValueError("not an ar archive")
    i = 8
    while i + 60 <= len(blob):
        hdr = blob[i:i + 60]
        size = int(hdr[48:58])
        if hdr[:16].strip().startswith(b"data.tar"):
            return blob[i + 60:i + 60 + size]
        i += 60 + size + (size & 1)
    raise ValueError("no data.tar member")


# only <unit dir>/<file> and <unit dir>/<unit>.d/<file>, flattened into dest.
# Links are recreated, never followed: compat-corpus skips them, and an
# absolute target (/dev/null for a masked unit) must not resolve on this host
def extract_units(tarblob, dest):
    n = 0
    with tarfile.open(fileobj=io.BytesIO(tarblob)) as t:
        for m in t:
            p = m.name.lstrip("./")
            rel = next((p[len(u):] for u in UNIT_DIRS if p.startswith(u)), None)
            parts = rel.split("/") if rel else []
            if not rel or len(parts) > 2 or (len(parts) == 2 and not parts[0].endswith(".d")) \
                    or ".." in parts or not (m.isfile() or m.issym()):
                continue
            out = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            if os.path.lexists(out):
                os.unlink(out)
            if m.issym():
                os.symlink(m.linkname, out)
            else:
                with open(out, "wb") as f:
                    f.write(t.extractfile(m).read())
                n += 1
    return n


def fetch(lock, out):
    cache = os.path.join(out, ".cache")
    os.makedirs(cache, exist_ok=True)
    for name, d in lock.items():
        dest = os.path.join(out, name)
        os.makedirs(dest, exist_ok=True)
        total = 0
        for p in d["packages"]:
            cp = os.path.join(cache, p["sha256"])
            blob = open(cp, "rb").read() if os.path.exists(cp) else get("%s/%s" % (d["base"], p["path"]))
            if hashlib.sha256(blob).hexdigest() != p["sha256"]:
                sys.exit("compat-fetch: %s %s %s: sha256 mismatch" % (name, p["name"], p["version"]))
            if not os.path.exists(cp):
                with open(cp, "wb") as f:
                    f.write(blob)
            total += extract_units(deb_data(blob) if d["format"] == "deb" else blob, dest)
        print("%s: %d packages, %d unit files -> %s" % (name, len(d["packages"]), total, dest))


def main():
    ap = argparse.ArgumentParser(description="Fetch pinned Debian/Ubuntu/Arch unit trees for compat-corpus.")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--resolve", action="store_true", help="re-pin the lock from each snapshot")
    ap.add_argument("--lock", default=LOCK)
    a = ap.parse_args()
    lock = json.load(open(a.lock))
    if a.resolve:
        resolve(lock)
    elif a.out:
        fetch(lock, a.out)
    else:
        ap.error("give OUT or --resolve")


if __name__ == "__main__":
    main()
