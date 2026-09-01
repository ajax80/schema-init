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
import re
import shutil
import subprocess
import sys
import tempfile

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


GRUB_DEFAULT = "etc/default/grub"
GRUB_BACKUP = "etc/default/grub.schema-migrate.orig"


def _grubenv_get(key, run):
    try:
        r = run(["grub2-editenv", "list"], capture_output=True, text=True)
        for line in (r.stdout or "").splitlines():
            k, _, v = line.partition("=")
            if k.strip() == key:
                return v.strip()
    except Exception:
        pass
    return None


def _grub_cfg_targets():
    targets = []
    for p in ("boot/grub2/grub.cfg", "boot/efi/EFI/fedora/grub.cfg"):
        if os.path.exists(P(p)):
            targets.append("/" + p)
    return targets


def ensure_grub_menu_visible(manifest, run=subprocess.run, dry_run=False):
    if dry_run:
        return {}
    changed = {}
    src = P(GRUB_DEFAULT)
    if os.path.exists(src):
        orig = open(src).read()
        if not os.path.exists(P(GRUB_BACKUP)):
            open(P(GRUB_BACKUP), "w").write(orig)
            manifest.add_file("/" + GRUB_BACKUP)
        lines, seen = [], set()
        for ln in orig.splitlines():
            key = ln.split("=", 1)[0].strip()
            if key == "GRUB_TIMEOUT":
                ln, _ = "GRUB_TIMEOUT=5", seen.add(key)
            elif key == "GRUB_TIMEOUT_STYLE":
                ln, _ = "GRUB_TIMEOUT_STYLE=menu", seen.add(key)
            lines.append(ln)
        if "GRUB_TIMEOUT" not in seen:
            lines.append("GRUB_TIMEOUT=5")
        if "GRUB_TIMEOUT_STYLE" not in seen:
            lines.append("GRUB_TIMEOUT_STYLE=menu")
        open(src, "w").write("\n".join(lines) + "\n")
        changed["default_backup"] = "/" + GRUB_BACKUP
    # menu_auto_hide hides the GRUB menu on a single-OS box after a clean boot;
    # remember its prior value, then clear it so a human sees the menu and can
    # pick the "(schema-init)" fallback by hand.
    changed["menu_auto_hide_was"] = _grubenv_get("menu_auto_hide", run)
    run(["grub2-editenv", "-", "unset", "menu_auto_hide"], check=False)
    changed["cfg_targets"] = _grub_cfg_targets()
    for t in changed["cfg_targets"]:
        run(["grub2-mkconfig", "-o", P(t.lstrip("/"))], check=False)
    manifest.grub = changed
    return changed


def restore_grub(grub, run=subprocess.run):
    if not grub:
        return
    backup = grub.get("default_backup")
    if backup and os.path.exists(P(backup)):
        try:
            open(P(GRUB_DEFAULT), "w").write(open(P(backup)).read())
        except OSError:
            pass
    prev = grub.get("menu_auto_hide_was")
    if prev:
        run(["grub2-editenv", "-", "set", "menu_auto_hide=" + prev], check=False)
    for t in grub.get("cfg_targets", []):
        run(["grub2-mkconfig", "-o", P(t.lstrip("/"))], check=False)


class Manifest:
    PATH = "var/lib/schema-init/migrate-manifest.json"

    def __init__(self, files=None, packages=None, boot_entry=None, grub=None):
        self.files = list(files or [])
        self.packages = list(packages or [])
        self.boot_entry = boot_entry
        self.grub = dict(grub or {})

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
                       "boot_entry": self.boot_entry, "grub": self.grub}, fh, indent=2)
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
        return cls(d.get("files"), d.get("packages"), d.get("boot_entry"), d.get("grub"))


def uninstall(run=subprocess.run):
    m = Manifest.load()
    restore_grub(m.grub, run=run)
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


SKIP_MOUNT_TARGETS = {"/"}


def _parent_mount_target(target, targets):
    best = None
    for t in targets:
        if t in SKIP_MOUNT_TARGETS or t == target:
            continue
        if target.startswith(t.rstrip("/") + "/"):
            if best is None or len(t) > len(best):
                best = t
    return best


def generate_host_units(profile, manifest, dry_run=False):
    written = []
    mounts = profile.get("mounts", [])
    targets = [m["target"] for m in mounts]
    for mnt in mounts:
        if mnt["target"] in SKIP_MOUNT_TARGETS:
            continue
        slug = _mount_slug(mnt["target"])
        rel = "etc/schema-init/services/mount-%s.svc" % slug
        # schema-init takes ONE argv element per args= line (it never splits on
        # whitespace) — a single combined line reaches /bin/mount as one garbage
        # argument and the mount silently fails.
        margs = ["-t", mnt["fstype"], "-o", mnt.get("opts", "defaults"),
                 mnt["src"], mnt["target"]]
        deps = ["udev-trigger"]
        parent = _parent_mount_target(mnt["target"], targets)
        if parent:
            deps.append("mount-" + _mount_slug(parent))
        body = ("name=mount-%s\nexec=/bin/mount\n" % slug
                + "".join("args=%s\n" % a for a in margs)
                + "oneshot=1\nneeds_root=1\ncritical=0\n"
                + "".join("dep=%s\n" % d for d in deps))
        written.append(P(rel))
        if not dry_run:
            os.makedirs(os.path.dirname(P(rel)), exist_ok=True)
            open(P(rel), "w").write(body)
            manifest.add_file("/" + rel)
    return written


HELPER_DIRS = ["scripts",
               "distros/fedora-installer/rail/scripts",
               "distros/fedora-kde/scripts"]
HELPER_ALIASES = {"schema-doctor": "schema-doctor.py"}
_LOCALBIN_RE = re.compile(r"/usr/local/bin/([A-Za-z0-9._-]+)")


def _find_helper_src(name):
    base = HELPER_ALIASES.get(name, name)
    for d in HELPER_DIRS:
        cand = os.path.join(repo(), d, base)
        if os.path.exists(cand):
            return cand
    return None


def install_unit_helpers(manifest, dry_run=False):
    svc_dir = P("etc/schema-init/services")
    queue, seen, installed = [], set(), []
    if os.path.isdir(svc_dir):
        for fn in os.listdir(svc_dir):
            if fn.endswith(".svc"):
                for m in _LOCALBIN_RE.finditer(open(os.path.join(svc_dir, fn)).read()):
                    queue.append(m.group(1))
    while queue:
        name = queue.pop()
        if name in seen:
            continue
        seen.add(name)
        src = _find_helper_src(name)
        if src is None:
            continue
        dst = P("usr/local/bin/" + name)
        if not dry_run:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            os.chmod(dst, 0o755)
            manifest.add_file("/usr/local/bin/" + name)
        installed.append(name)
        try:
            for m in _LOCALBIN_RE.finditer(open(src, encoding="utf-8", errors="ignore").read()):
                if m.group(1) not in seen:
                    queue.append(m.group(1))
        except OSError:
            pass
    return installed


def _add_unit_dep(unit, dep, dry_run=False):
    path = P("etc/schema-init/services/" + unit + ".svc")
    if dry_run or not os.path.exists(path):
        return
    lines = open(path).read().splitlines()
    for ln in lines:
        if ln.strip() == "dep=" + dep:
            return
    with open(path, "a") as fh:
        fh.write("dep=" + dep + "\n")


MODULE_LOAD_DENY = {"binfmt_misc"}


def generate_module_load(manifest, dry_run=False):
    try:
        mods = [ln.split()[0] for ln in open("/proc/modules") if ln.strip()]
    except OSError:
        mods = []
    mods = [m for m in mods if m not in MODULE_LOAD_DENY]
    if not mods or dry_run:
        return mods
    script = P("usr/local/bin/schema-coldplug-modules.sh")
    os.makedirs(os.path.dirname(script), exist_ok=True)
    with open(script, "w") as fh:
        fh.write("#!/bin/sh\n")
        for m in mods:
            fh.write('modprobe %s 2>/dev/null || true\n' % m)
    os.chmod(script, 0o755)
    manifest.add_file("/usr/local/bin/schema-coldplug-modules.sh")
    svc = P("etc/schema-init/services/coldplug-modules.svc")
    os.makedirs(os.path.dirname(svc), exist_ok=True)
    open(svc, "w").write("name=coldplug-modules\n"
                         "exec=/usr/local/bin/schema-coldplug-modules.sh\n"
                         "oneshot=1\n"
                         "needs_root=1\n"
                         "critical=0\n")
    manifest.add_file("/etc/schema-init/services/coldplug-modules.svc")
    return mods


UDEVD_PATHS = ["/usr/lib/systemd/systemd-udevd", "/lib/systemd/systemd-udevd",
               "/usr/libexec/systemd-udevd"]


def _udevd_path():
    for p in UDEVD_PATHS:
        if os.path.exists(P(p.lstrip("/"))):
            return p
    return None


def generate_udev_units(manifest, dry_run=False):
    if dry_run:
        return
    udevd = _udevd_path()
    if not udevd:
        return
    svc = P("etc/schema-init/services/udevd.svc")
    os.makedirs(os.path.dirname(svc), exist_ok=True)
    open(svc, "w").write("name=udevd\nexec=%s\nneeds_root=1\ncritical=0\n" % udevd)
    manifest.add_file("/etc/schema-init/services/udevd.svc")
    script = P("usr/local/bin/schema-udev-trigger.sh")
    os.makedirs(os.path.dirname(script), exist_ok=True)
    open(script, "w").write("#!/bin/sh\n"
                            "udevadm trigger --action=add --type=subsystems\n"
                            "udevadm trigger --action=add --type=devices\n"
                            "udevadm settle --timeout=30\n")
    os.chmod(script, 0o755)
    manifest.add_file("/usr/local/bin/schema-udev-trigger.sh")
    tsvc = P("etc/schema-init/services/udev-trigger.svc")
    open(tsvc, "w").write("name=udev-trigger\n"
                          "exec=/usr/local/bin/schema-udev-trigger.sh\n"
                          "dep=udevd\noneshot=1\nneeds_root=1\ncritical=0\n")
    manifest.add_file("/etc/schema-init/services/udev-trigger.svc")


def generate_nm_config(manifest, dry_run=False):
    if dry_run:
        return
    conf = P("etc/NetworkManager/conf.d/10-schema-managed.conf")
    os.makedirs(os.path.dirname(conf), exist_ok=True)
    # rc-manager=file: without systemd-resolved under schema-init, /etc/resolv.conf
    # is a dangling symlink to resolved's stub and every DNS lookup fails. Tell NM
    # to write resolv.conf directly as a real file from DHCP.
    open(conf, "w").write("[device]\nmatch-device=*\nmanaged=1\n\n"
                          "[main]\nrc-manager=file\n")
    manifest.add_file("/etc/NetworkManager/conf.d/10-schema-managed.conf")
    rc = P("etc/resolv.conf")
    if os.path.islink(rc) and not os.path.exists(rc):
        os.unlink(rc)
    key = P("etc/NetworkManager/system-connections/schema-wired.nmconnection")
    os.makedirs(os.path.dirname(key), exist_ok=True)
    open(key, "w").write("[connection]\nid=schema-wired\ntype=ethernet\n"
                         "autoconnect=true\nautoconnect-priority=100\n\n"
                         "[ethernet]\n\n[ipv4]\nmethod=auto\n\n[ipv6]\nmethod=auto\n")
    os.chmod(key, 0o600)
    manifest.add_file("/etc/NetworkManager/system-connections/schema-wired.nmconnection")


DESKTOP_GROUPS = ["video", "render", "input", "audio"]


def ensure_user_groups(profile, run=subprocess.run, dry_run=False):
    user = profile.get("user")
    if not user or dry_run:
        return []
    have = set()
    try:
        for line in open(P("etc/group")):
            have.add(line.split(":", 1)[0])
    except OSError:
        pass
    groups = [g for g in DESKTOP_GROUPS if g in have]
    if groups:
        run(["usermod", "-aG", ",".join(groups), user], check=False)
    return groups


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


BUILD_PACKAGES = ["gcc", "make", "glibc-static", "libacl-devel"]


def ensure_build_toolchain(manifest, run=subprocess.run, dry_run=False):
    missing = [p for p in BUILD_PACKAGES if not _installed(p, run)]
    if not missing or dry_run:
        return missing
    run(["dnf", "install", "-y"] + missing, check=False)
    for p in missing:
        if _installed(p, run):
            manifest.add_package(p)
    return missing


def run_make_install(manifest, run=subprocess.run, dry_run=False):
    if dry_run:
        return
    ensure_build_toolchain(manifest, run=run, dry_run=dry_run)
    if shutil.which("make") is None or shutil.which("gcc") is None:
        raise RuntimeError("build toolchain unavailable after dnf install "
                           "(need %s) — cannot build schema-init" % ", ".join(BUILD_PACKAGES))
    staging = tempfile.mkdtemp()
    try:
        r = run(["make", "-C", repo(), "install", "DESTDIR=" + staging, "PREFIX=/usr"])
        if getattr(r, "returncode", 0) != 0:
            raise RuntimeError("schema-init build failed (make exited %s)" % r.returncode)
        if not os.path.exists(os.path.join(staging, "usr/bin/schema-init")):
            raise RuntimeError("build produced no /usr/bin/schema-init — refusing to "
                               "install a boot entry that would panic")
        for dirpath, _dirs, files in os.walk(staging):
            for f in files:
                full = os.path.join(dirpath, f)
                rel = os.path.relpath(full, staging)
                dst = P(rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(full, dst)
                manifest.add_file("/" + rel)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def do_deploy(run=subprocess.run, dry_run=False):
    profile = build_profile(run=run)
    if not dry_run:
        write_profile(profile)
    m = Manifest()
    run_make_install(m, run=run, dry_run=dry_run)
    deploy_prevent_set(m, dry_run=dry_run)
    generate_host_units(profile, m, dry_run=dry_run)
    generate_module_load(m, dry_run=dry_run)
    generate_udev_units(m, dry_run=dry_run)
    generate_nm_config(m, dry_run=dry_run)
    ensure_user_groups(profile, run=run, dry_run=dry_run)
    _add_unit_dep("network-manager", "coldplug-modules", dry_run=dry_run)
    _add_unit_dep("network-manager", "udev-trigger", dry_run=dry_run)
    install_unit_helpers(m, dry_run=dry_run)
    install_packages(m, run=run, dry_run=dry_run)
    # grub2-mkconfig FIRST: Fedora's BLS sync rewrites every loader entry's
    # options from the canonical cmdline, so it must run before the schema
    # entry is written or it strips the init= override we just added.
    ensure_grub_menu_visible(m, run=run, dry_run=dry_run)
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
