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
import argparse, hashlib, io, json, lzma, os, re, shutil, sys, tarfile, urllib.request

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
                if f["NAME"] in out:   # first repo listed wins, as in deb_index
                    continue
                out[f["NAME"]] = (f["VERSION"], "%s/os/x86_64/%s" % (repo, f["FILENAME"]), f["SHA256SUM"])
    return out


def resolve(lock, path):
    for name, d in lock.items():
        idx = deb_index(d) if d["format"] == "deb" else pkg_index(d)
        missing = [p for p in d["want"] if p not in idx]
        d["packages"] = [{"name": p, "version": idx[p][0], "path": idx[p][1], "sha256": idx[p][2]}
                         for p in d["want"] if p in idx]
        print("%s: %d pinned%s" % (name, len(d["packages"]),
                                   ", not in snapshot: " + " ".join(missing) if missing else ""))
    with open(path, "w") as f:
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
# A unit-file link is recreated, never followed: compat-corpus skips links, and
# an absolute target (/dev/null for a masked unit) must not resolve on this host.
# A linked <unit>.d is dropped: files under it would be written through the link.
# A name shipped twice (lib/ and usr/lib/, or two packages that conflict, like
# Ubuntu's mariadb-server aliasing mysql.service and mysql-server-8.0's real one):
# a real file replaces a link, else the first copy wins, said out loud either way
# so the result never hangs on lock order unseen
def extract_units(tarblob, dest, pkg):
    n = 0
    with tarfile.open(fileobj=io.BytesIO(tarblob)) as t:
        for m in t:
            p = m.name.removeprefix("./")
            rel = next((p[len(u):] for u in UNIT_DIRS if p.startswith(u)), None)
            parts = rel.split("/") if rel else []
            if not rel or len(parts) > 2 or (len(parts) == 2 and not parts[0].endswith(".d")) \
                    or ".." in parts or not (m.isfile() or m.islnk() or m.issym()) \
                    or (m.issym() and (len(parts) == 2 or rel.endswith(".d"))):
                continue
            out = os.path.join(dest, rel)
            if os.path.lexists(out):
                if os.path.islink(out) and not m.issym():
                    print("compat-fetch: %s: %s replaces an alias link" % (pkg, rel), file=sys.stderr)
                    os.unlink(out)
                else:
                    print("compat-fetch: %s: %s already extracted, kept the first" % (pkg, rel), file=sys.stderr)
                    continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
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
        # rebuilt from the cache each run, so a package dropped from the lock leaves nothing behind
        shutil.rmtree(dest, ignore_errors=True)
        os.makedirs(dest)
        total = 0
        for p in d["packages"]:
            cp = os.path.join(cache, p["sha256"])
            blob = open(cp, "rb").read() if os.path.exists(cp) else get("%s/%s" % (d["base"], p["path"]))
            if hashlib.sha256(blob).hexdigest() != p["sha256"]:
                sys.exit("compat-fetch: %s %s %s: sha256 mismatch" % (name, p["name"], p["version"]))
            if not os.path.exists(cp):
                with open(cp, "wb") as f:
                    f.write(blob)
            total += extract_units(deb_data(blob) if d["format"] == "deb" else blob, dest, p["name"])
        print("%s: %d packages, %d unit files -> %s" % (name, len(d["packages"]), total, dest))


def main():
    ap = argparse.ArgumentParser(description="Fetch pinned Debian/Ubuntu/Arch unit trees for compat-corpus.")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--resolve", action="store_true", help="re-pin the lock from each snapshot")
    ap.add_argument("--lock", default=LOCK)
    a = ap.parse_args()
    lock = json.load(open(a.lock))
    if a.resolve:
        resolve(lock, a.lock)
    elif a.out:
        # Arch packages and Ubuntu's data.tar are zstd: tarfile reads it from 3.14
        if "zst" not in tarfile.TarFile.OPEN_METH:
            sys.exit("compat-fetch: this Python's tarfile can't read zstd (needs 3.14 with zstd)")
        fetch(lock, a.out)
    else:
        ap.error("give OUT or --resolve")


if __name__ == "__main__":
    main()
