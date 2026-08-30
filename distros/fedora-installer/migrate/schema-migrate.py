#!/usr/bin/env python3
"""schema-migrate — transition an existing Fedora KDE box onto schema-init
in place, non-destructively, with a fallback boot entry and post-reboot heal.

Stdlib only. MIGRATE_ROOT prefixes filesystem paths (tests inject a temp tree);
MIGRATE_REPO points at the schema-init tree on the USB.
"""
import glob
import glob as _glob
import json
import os
import shutil
import subprocess
import sys

ROOT = os.environ.get("MIGRATE_ROOT") or "/"
_MODDIR = os.path.dirname(os.path.abspath(__file__))
_PREVENT_LIST_OVERRIDE = None  # tests may set this to a path


def P(rel):
    return os.path.join(ROOT, rel.lstrip("/"))


def repo():
    return os.environ.get("MIGRATE_REPO") or os.path.dirname(os.path.dirname(os.path.dirname(_MODDIR)))


def load_prevent_set(path=None):
    if path is None:
        path = _PREVENT_LIST_OVERRIDE or os.path.join(_MODDIR, "prevent-set.list")
    out = {"script": [], "config": [], "service": [], "exclude": []}
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            cat, _, name = line.partition(" ")
            name = name.strip()
            if cat in out and name:
                out[cat].append(name)
    return out


def _copy_into(src, dst, manifest, dry_run):
    if dry_run:
        return dst
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.isdir(src):
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        shutil.copy2(src, dst)
    return dst


def deploy_prevent_set(manifest, dry_run=False):
    ps = load_prevent_set()
    r = repo()
    written = []
    for name in ps["script"]:
        src = os.path.join(r, "distros/fedora-kde/scripts", name)
        dst = P("usr/local/lib/schema-init/scripts/" + name)
        if os.path.exists(src):
            _copy_into(src, dst, manifest, dry_run)
            written.append(dst)
            if not dry_run:
                manifest.add_file("/usr/local/lib/schema-init/scripts/" + name)
    for name in ps["config"]:
        src = os.path.join(r, "distros/fedora-kde/config", name)
        dst = P("etc/schema-init/config/" + name)
        if os.path.exists(src):
            _copy_into(src, dst, manifest, dry_run)
            written.append(dst)
            if not dry_run:
                manifest.add_file("/etc/schema-init/config/" + name)
    for name in ps["service"]:
        for base in ("distros/fedora-installer/rail/services",
                     "distros/fedora-kde/services"):
            src = os.path.join(r, base, name + ".svc")
            if os.path.exists(src):
                dst = P("etc/schema-init/services/" + name + ".svc")
                _copy_into(src, dst, manifest, dry_run)
                written.append(dst)
                if not dry_run:
                    manifest.add_file("/etc/schema-init/services/" + name + ".svc")
                break
    return written


def _os_release():
    kv = {}
    try:
        for line in open(P("etc/os-release")):
            k, _, v = line.strip().partition("=")
            kv[k] = v.strip().strip('"')
    except OSError:
        pass
    return kv


def detect_platform():
    osr = _os_release()
    if osr.get("ID") != "fedora":
        return None, "this box is not Fedora (os-release ID=%s) — v1 supports Fedora only" % osr.get("ID", "unknown")
    if not (os.path.exists(P("usr/bin/plasmashell")) or os.path.exists(P("usr/bin/sddm"))):
        return None, "no KDE found (plasmashell/sddm absent) — v1 supports Fedora KDE only"
    return "fedora-kde", ""


SCHEMA_OWNED = {
    "systemd-udevd", "systemd-journald", "systemd-logind", "systemd-resolved",
    "crond", "cron", "systemd-timesyncd", "systemd-userdbd",
}

UNIT_TO_SVC = {
    "NetworkManager": "network-manager",
    "sshd": "sshd",
    "bluetooth": "bluetoothd",
    "polkit": "polkitd",
    "dbus": "dbus",
    "dbus-broker": "dbus",
    "seatd": "seatd",
}


def running_services(run=subprocess.run):
    try:
        r = run(["systemctl", "list-units", "--type=service", "--state=running",
                 "--no-legend", "--plain"], capture_output=True, text=True)
        out = r.stdout
    except Exception:
        out = ""
    units = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        unit = line.split()[0]
        if unit.endswith(".service"):
            units.append(unit[:-len(".service")])
    return units


def classify_services(units, prevent):
    covered, owned, leftover = set(), set(), set()
    svc_set = set(prevent["service"])
    for u in units:
        if u in SCHEMA_OWNED:
            owned.add(u)
        elif u in UNIT_TO_SVC and UNIT_TO_SVC[u] in svc_set:
            covered.add(UNIT_TO_SVC[u])
        else:
            leftover.add(u)
    return {"covered": sorted(covered), "schema_owned": sorted(owned),
            "leftover": sorted(leftover)}


SKIP_FSTYPES = {"proc", "sysfs", "devpts", "tmpfs", "devtmpfs", "cgroup",
                "cgroup2", "mqueue", "hugetlbfs", "debugfs", "swap", "efivarfs"}


def read_fstab():
    out = []
    try:
        lines = open(P("etc/fstab")).read().splitlines()
    except OSError:
        return out
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = line.split()
        if len(f) < 3 or f[2] in SKIP_FSTYPES:
            continue
        out.append({"src": f[0], "target": f[1], "fstype": f[2],
                    "opts": f[3] if len(f) > 3 else "defaults"})
    return out


def primary_user():
    try:
        for line in open(P("etc/passwd")):
            f = line.split(":")
            if len(f) >= 3 and f[2] == "1000":
                return f[0], 1000
    except OSError:
        pass
    return None, None


def bootloader_kind():
    if glob.glob(P("boot/loader/entries/*.conf")):
        return "bls"
    return "unknown"


def build_profile(run=subprocess.run):
    plat, _ = detect_platform()
    user, uid = primary_user()
    prevent = load_prevent_set()
    services = classify_services(running_services(run=run), prevent)
    return {
        "platform": plat,
        "user": user,
        "uid": uid,
        "services": services,
        "mounts": read_fstab(),
        "bootloader": bootloader_kind(),
        "kernel": os.environ.get("MIGRATE_KERNEL") or os.uname().release,
    }


def write_profile(profile):
    p = P("var/lib/schema-init/migrate-profile.json")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as fh:
        json.dump(profile, fh, indent=2)
    os.chmod(p, 0o644)
    return p


SCHEMA_ENTRY_ID = "schema-init"
INIT_PATH = "/usr/bin/schema-init"


def _active_entry(kernel):
    entries = sorted(_glob.glob(P("boot/loader/entries/*.conf")))
    entries = [e for e in entries if os.path.basename(e) != SCHEMA_ENTRY_ID + ".conf"]
    if not entries:
        return None
    for e in entries:
        if kernel and kernel in os.path.basename(e):
            return e
    for e in entries:
        if kernel and kernel in open(e).read():
            return e
    return entries[-1]


def add_boot_entry(kernel):
    src = _active_entry(kernel)
    dst = P("boot/loader/entries/%s.conf" % SCHEMA_ENTRY_ID)
    lines = open(src).read().splitlines() if src else ["options ro"]
    out = []
    for line in lines:
        if line.startswith("title "):
            out.append(line.rstrip() + " (schema-init)")
        elif line.startswith("options "):
            if ("init=" + INIT_PATH) not in line:
                line = line.rstrip() + " init=" + INIT_PATH
            out.append(line)
        else:
            out.append(line)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w") as fh:
        fh.write("\n".join(out) + "\n")
    return dst


def remove_boot_entry():
    dst = P("boot/loader/entries/%s.conf" % SCHEMA_ENTRY_ID)
    try:
        os.remove(dst)
    except OSError:
        pass


class Manifest:
    PATH = "var/lib/schema-init/migrate-manifest.json"

    def __init__(self, files=None, packages=None, boot_entry=None):
        self.files = list(files or [])
        self.packages = list(packages or [])
        self.boot_entry = boot_entry

    def add_file(self, path):
        if path not in self.files:
            self.files.append(path)

    def add_package(self, name):
        if name not in self.packages:
            self.packages.append(name)

    def set_boot_entry(self, path):
        self.boot_entry = path

    def save(self):
        p = P(self.PATH)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as fh:
            json.dump({"files": self.files, "packages": self.packages,
                       "boot_entry": self.boot_entry}, fh, indent=2)
        os.chmod(p, 0o644)
        return p

    @classmethod
    def load(cls):
        try:
            d = json.load(open(P(cls.PATH)))
            if not isinstance(d, dict):
                d = {}
        except (OSError, ValueError):
            d = {}
        return cls(d.get("files"), d.get("packages"), d.get("boot_entry"))


def uninstall(run=subprocess.run):
    m = Manifest.load()
    removed = 0
    for rel in m.files:
        try:
            os.remove(P(rel))
            removed += 1
        except OSError:
            pass
    if m.packages:
        run(["dnf", "remove", "-y"] + m.packages, check=False)
    remove_boot_entry()
    try:
        os.remove(P(Manifest.PATH))
    except OSError:
        pass
    return {"files_removed": removed, "packages": m.packages}


def _mount_slug(target):
    if target == "/":
        return "root"
    return target.strip("/").replace("/", "-")


def generate_host_units(profile, manifest, dry_run=False):
    written = []
    for mnt in profile.get("mounts", []):
        slug = _mount_slug(mnt["target"])
        rel = "etc/schema-init/services/mount-%s.svc" % slug
        body = ("name=mount-%s\n"
                "exec=/bin/mount\n"
                "args=-t %s -o %s %s %s\n"
                "oneshot=1\n"
                "needs_root=1\n"
                "critical=0\n") % (slug, mnt["fstype"], mnt.get("opts", "defaults"),
                                   mnt["src"], mnt["target"])
        written.append(P(rel))
        if not dry_run:
            os.makedirs(os.path.dirname(P(rel)), exist_ok=True)
            open(P(rel), "w").write(body)
            manifest.add_file("/" + rel)
    return written


PREVENT_PACKAGES = ["libavcodec-freeworld", "egl-wayland", "seatd"]


def _installed(pkg, run):
    try:
        return run(["rpm", "-q", pkg], capture_output=True, text=True).returncode == 0
    except Exception:
        return False


def install_packages(manifest, run=subprocess.run, dry_run=False):
    done = []
    for pkg in PREVENT_PACKAGES:
        if _installed(pkg, run):
            continue
        done.append(pkg)
        if not dry_run:
            run(["dnf", "install", "-y", pkg], check=False)
            manifest.add_package(pkg)
    return done


def run_make_install(run=subprocess.run, dry_run=False):
    if dry_run:
        return
    run(["make", "-C", repo(), "install", "DESTDIR=" + ROOT.rstrip("/"), "PREFIX=/usr"],
        check=False)


def do_deploy(run=subprocess.run, dry_run=False):
    profile = build_profile(run=run)
    if not dry_run:
        write_profile(profile)
    m = Manifest()
    run_make_install(run=run, dry_run=dry_run)
    deploy_prevent_set(m, dry_run=dry_run)
    generate_host_units(profile, m, dry_run=dry_run)
    install_packages(m, run=run, dry_run=dry_run)
    entry = add_boot_entry(profile["kernel"]) if not dry_run else None
    if not dry_run:
        m.set_boot_entry("/" + os.path.relpath(entry, ROOT))
        m.save()
    return m


def finish_report():
    try:
        prof = json.load(open(P("var/lib/schema-init/migrate-profile.json")))
    except (OSError, ValueError):
        prof = {}
    leftover = prof.get("services", {}).get("leftover", [])
    try:
        doctor = open(P("run/schema-init/doctor-status")).read().strip()
    except OSError:
        doctor = "(schema-doctor status not found)"
    lines = ["=== schema-migrate: first-boot report ===", "",
             "platform: " + str(prof.get("platform", "?")), "",
             "schema-doctor: " + doctor, ""]
    if leftover:
        lines.append("Services still running under the old system that were NOT ported:")
        lines += ["  - " + s for s in leftover]
        lines.append("")
        lines.append("Run `schema-migrate --translate` to carry these over "
                     "(simple units auto-convert; complex ones are named, not dropped).")
    else:
        lines.append("No un-ported services — nothing left to translate.")
    marker = P("run/schema-init/migrate-finished")
    os.makedirs(os.path.dirname(marker), exist_ok=True)
    open(marker, "w").write("done\n")
    return "\n".join(lines)


def main(argv, run=subprocess.run):
    import argparse
    ap = argparse.ArgumentParser(prog="schema-migrate")
    ap.add_argument("--discover", action="store_true", help="preview the profile, change nothing")
    ap.add_argument("--deploy", action="store_true", help="run the flip")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, change nothing")
    ap.add_argument("--uninstall", action="store_true", help="reverse a prior migration")
    ap.add_argument("--finish", action="store_true", help="post-reboot report + translate offer")
    args = ap.parse_args(argv)

    if args.finish:
        if os.path.exists(P("run/schema-init/migrate-finished")):
            print("migrate-finish already ran")
            return 0
        print(finish_report())
        return 0

    if args.uninstall:
        res = uninstall(run=run)
        print("uninstalled: %d files, packages=%s" % (res["files_removed"], res["packages"]))
        return 0

    plat, why = detect_platform()
    if plat is None:
        print("refusing: " + why)
        return 2

    if args.discover:
        profile = build_profile(run=run)
        path = write_profile(profile)
        print(json.dumps(profile, indent=2))
        print("profile written to " + path)
        return 0

    if os.path.exists(P(Manifest.PATH)) and not args.dry_run:
        print("already migrated (manifest present) — run --uninstall to reverse")
        return 0

    do_deploy(run=run, dry_run=args.dry_run)
    print("dry-run complete — nothing changed" if args.dry_run
          else "deploy complete — reboot and pick the '(schema-init)' entry")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
