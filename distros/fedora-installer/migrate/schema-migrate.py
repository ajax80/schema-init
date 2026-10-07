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
import time
import importlib.util as _ilu

ROOT = os.environ.get("MIGRATE_ROOT") or "/"
_MODDIR = os.path.dirname(os.path.abspath(__file__))
_PREVENT_LIST_OVERRIDE = None  # tests may set this to a path
_stage_path = os.path.join(_MODDIR, "stage.py")
if not os.path.exists(_stage_path):
    _stage_path = "/usr/libexec/schema-init/stage.py"
if not os.path.exists(_stage_path):
    raise SystemExit("schema-migrate: stage.py not found (looked in %s and "
                     "/usr/libexec/schema-init) — is schema-init-migrate installed?"
                     % _MODDIR)
_spec = _ilu.spec_from_file_location("stage", _stage_path)
stage = _ilu.module_from_spec(_spec); _spec.loader.exec_module(stage)


def P(rel):
    return os.path.join(ROOT, rel.lstrip("/"))


def repo():
    return os.environ.get("MIGRATE_REPO") or os.path.dirname(os.path.dirname(os.path.dirname(_MODDIR)))


# On an RPM box schema-migrate lives in /usr/bin, so repo() resolves to "/" and
# no source tree exists. The -migrate package mirrors the repo-relative asset
# layout under here, so find_source() falls back to it when MIGRATE_REPO and the
# real repo tree are absent.
_DATA_DIR = "/usr/share/schema-init/migrate"


def _source_roots():
    roots = [repo()]
    if os.path.isdir(_DATA_DIR) and _DATA_DIR not in roots:
        roots.append(_DATA_DIR)
    return roots


def find_source(relpath):
    for root in _source_roots():
        cand = os.path.join(root, relpath)
        if os.path.exists(cand):
            return cand
    return None


def load_prevent_set(path=None):
    if path is None:
        path = _PREVENT_LIST_OVERRIDE or os.path.join(_MODDIR, "prevent-set.list")
        if not os.path.exists(path):
            path = "/usr/share/schema-init/migrate/prevent-set.list"
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


def _adv():
    return [a for a in os.environ.get("MIGRATE_ADV", "").split(",") if a]


DOCTOR_TIMERS = {"schema-doctor-periodic"}


def deploy_prevent_set(manifest, dry_run=False):
    ps = load_prevent_set()
    skip = DOCTOR_TIMERS if "no-doctor-timers" in _adv() else set()
    written = []
    for name in ps["script"]:
        src = find_source("distros/fedora-kde/scripts/" + name)
        dst = P("usr/local/lib/schema-init/scripts/" + name)
        if src:
            _copy_into(src, dst, manifest, dry_run)
            written.append(dst)
            if not dry_run:
                manifest.add_file("/usr/local/lib/schema-init/scripts/" + name)
    for name in ps["config"]:
        src = find_source("distros/fedora-kde/config/" + name)
        dst = P("etc/schema-init/config/" + name)
        if src:
            _copy_into(src, dst, manifest, dry_run)
            written.append(dst)
            if not dry_run:
                manifest.add_file("/etc/schema-init/config/" + name)
    for name in ps["service"]:
        if name in skip:
            continue
        for base in ("distros/fedora-installer/rail/services",
                     "distros/fedora-kde/services"):
            src = find_source(base + "/" + name + ".svc")
            if src:
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


LEGACY_ENTRY = "schema-init.conf"
INIT_PATH = "/usr/bin/schema-init"
HOOK_REL = "usr/lib/kernel/install.d/99-schema-init.install"
HOOK_SRC = "distros/shared/kernel-install/99-schema-init.install"
BOOT_DEFAULT = "etc/schema-init/boot-default"


def stock_entries():
    out = {}
    for e in sorted(_glob.glob(P("boot/loader/entries/*.conf"))):
        name = os.path.basename(e)
        if name.startswith("schema-") or "rescue" in name:
            continue
        for line in open(e, errors="ignore"):
            if line.startswith("version "):
                v = line.split(None, 1)[1].strip()
                if name.endswith("-%s.conf" % v):
                    out[v] = name[:-len(".conf")]
                break
    return out


def _hook_src():
    for cand in (find_source(HOOK_SRC),
                 os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(_MODDIR))), HOOK_SRC)):
        if cand and os.path.exists(cand):
            return cand
    return None


def install_kernel_hook(manifest):
    dst = P(HOOK_REL)
    if os.path.exists(dst):
        return dst
    src = _hook_src()
    if src is None:
        raise RuntimeError("kernel-install hook %s not found — without it every kernel "
                           "update drops the box back to its old init" % HOOK_SRC)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    os.chmod(dst, 0o755)
    manifest.add_file("/" + HOOK_REL)
    return dst


def seed_boot_entries(kernel, manifest, run=subprocess.run):
    had_hook = os.path.exists(P(HOOK_REL))
    hook = install_kernel_hook(manifest)
    before = set(os.listdir(P("boot/loader/entries")))
    marker = P(BOOT_DEFAULT)
    had_marker = os.path.exists(marker)
    if not had_marker:
        os.makedirs(os.path.dirname(marker), exist_ok=True)
        open(marker, "w").close()
        manifest.add_file("/" + BOOT_DEFAULT)
    try:
        os.remove(P("boot/loader/entries/" + LEGACY_ENTRY))
    except OSError:
        pass
    # the hook only clones; migrate picks the default itself (the running
    # kernel, known to boot — not the newest, which may never have booted)
    env = dict(os.environ, KERNEL_INSTALL_BOOT_ROOT=P("boot"),
               SCHEMA_INIT_CONF_ROOT=P("etc/schema-init"),
               SCHEMA_INIT_BIN=INIT_PATH, SCHEMA_INIT_NO_DEFAULT="1")
    for v in stock_entries():
        subprocess.run(["sh", hook, "add", v, "/boot/vmlinuz-" + v], env=env, check=False)
    entry = P("boot/loader/entries/schema-%s.conf" % kernel)
    if not os.path.exists(entry):
        # no manifest gets saved on this path: leave nothing behind, and above
        # all no marker, or the next kernel update flips the default anyway
        for name in set(os.listdir(P("boot/loader/entries"))) - before:
            os.remove(P("boot/loader/entries/" + name))
        for made, path in ((had_hook, HOOK_REL), (had_marker, BOOT_DEFAULT)):
            if not made:
                os.remove(P(path))
                manifest.files = [f for f in manifest.files if f != "/" + path]
        raise RuntimeError("no schema boot entry was made for the running kernel %s" % kernel)
    manifest.grub["saved_entry_was"] = _grubenv_get("saved_entry", run)
    run(["grub2-editenv", "-", "set", "saved_entry=schema-" + kernel], check=False)
    return entry


def remove_boot_entries(saved_was=None, run=subprocess.run):
    stock = stock_entries()
    ents = P("boot/loader/entries")
    for name in [LEGACY_ENTRY] + ["schema-%s.conf" % v for v in stock]:
        try:
            os.remove(os.path.join(ents, name))
        except OSError:
            pass
    saved = _grubenv_get("saved_entry", run)
    if not saved or not saved.startswith("schema-") or not stock:
        return
    target = stock.get(saved[len("schema-"):])
    if not target and saved_was and os.path.exists(os.path.join(ents, saved_was + ".conf")):
        target = saved_was
    if not target:
        target = stock[sorted(stock, key=_vkey)[-1]]
    run(["grub2-editenv", "-", "set", "saved_entry=" + target], check=False)


def _vkey(v):
    return [int(x) if x.isdigit() else x for x in re.split(r"(\d+)", v)]


FALLBACK_STASH = "var/lib/schema-init/boot-entries.orig"


def hide_fallback_entries(manifest):
    moved = []
    for e in sorted(_glob.glob(P("boot/loader/entries/*.conf"))):
        if os.path.basename(e).startswith("schema-"):
            continue
        os.makedirs(P(FALLBACK_STASH), exist_ok=True)
        shutil.move(e, os.path.join(P(FALLBACK_STASH), os.path.basename(e)))
        moved.append("/" + os.path.relpath(e, ROOT))
    manifest.grub["hidden_entries"] = moved
    return moved


def take_snapshot(run=subprocess.run, dry_run=False):
    r = run(["findmnt", "-no", "FSTYPE", ROOT], capture_output=True, text=True)
    if (getattr(r, "stdout", "") or "").strip() != "btrfs":
        print("snapshot: / is not btrfs — skipped")
        return None
    dst = P(".schema-migrate-snapshot-" + time.strftime("%Y%m%d-%H%M%S"))
    if dry_run:
        return dst
    r = run(["btrfs", "subvolume", "snapshot", "-r", ROOT, dst], capture_output=True, text=True)
    if getattr(r, "returncode", 0) != 0:
        raise RuntimeError("btrfs snapshot of / failed — refusing to change anything "
                           "without it (--advanced-no-snapshot skips it)")
    print("snapshot: " + dst)
    return dst


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
        if not os.path.exists(P(p)):
            continue
        # Modern Fedora's ESP grub.cfg is a stub that `configfile`s the real
        # /boot/grub2/grub.cfg; grub2-mkconfig refuses to overwrite it (older
        # grub would destroy it). Only a full legacy ESP config is a target.
        if p.startswith("boot/efi/"):
            try:
                if "configfile" in open(P(p), errors="ignore").read():
                    continue
            except OSError:
                continue
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
            elif key == "GRUB_DEFAULT":
                # saved_entry is how schema becomes the default; a fixed
                # GRUB_DEFAULT would silently keep booting the old init
                ln, _ = "GRUB_DEFAULT=saved", seen.add(key)
            lines.append(ln)
        if "GRUB_TIMEOUT" not in seen:
            lines.append("GRUB_TIMEOUT=5")
        if "GRUB_TIMEOUT_STYLE" not in seen:
            lines.append("GRUB_TIMEOUT_STYLE=menu")
        if "GRUB_DEFAULT" not in seen:
            lines.append("GRUB_DEFAULT=saved")
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
    for rel in grub.get("hidden_entries", []):
        src = os.path.join(P(FALLBACK_STASH), os.path.basename(rel))
        if os.path.exists(src):
            shutil.move(src, P(rel.lstrip("/")))
    prev = grub.get("menu_auto_hide_was")
    if prev:
        run(["grub2-editenv", "-", "set", "menu_auto_hide=" + prev], check=False)
    for t in grub.get("cfg_targets", []):
        run(["grub2-mkconfig", "-o", P(t.lstrip("/"))], check=False)


class Manifest:
    PATH = "var/lib/schema-init/migrate-manifest.json"

    def __init__(self, files=None, packages=None, boot_entry=None, grub=None, links=None,
                 snapshot=None):
        self.files = list(files or [])
        self.packages = list(packages or [])
        self.boot_entry = boot_entry
        self.grub = dict(grub or {})
        self.links = dict(links or {})
        self.snapshot = snapshot

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
                       "boot_entry": self.boot_entry, "grub": self.grub,
                       "links": self.links, "snapshot": self.snapshot}, fh, indent=2)
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
        return cls(d.get("files"), d.get("packages"), d.get("boot_entry"), d.get("grub"),
                   d.get("links"), d.get("snapshot"))


def _rpm_owned(rel):
    try:
        return subprocess.run(["rpm", "--root", ROOT, "-qf", "/" + rel.lstrip("/")],
                              capture_output=True).returncode == 0
    except OSError:
        return False


def uninstall(run=subprocess.run):
    m = Manifest.load()
    restore_grub(m.grub, run=run)
    removed = 0
    for rel in m.files:
        if _rpm_owned(rel):
            continue
        try:
            os.remove(P(rel))
            removed += 1
        except OSError:
            pass
    for rel, target in m.links.items():
        try:
            if os.path.lexists(P(rel)):
                os.remove(P(rel))
            os.symlink(target, P(rel))
        except OSError:
            pass
    if m.snapshot and os.path.isdir(P(m.snapshot)):
        run(["btrfs", "subvolume", "delete", P(m.snapshot)], check=False)
    if m.packages:
        run(["dnf", "remove", "-y"] + m.packages, check=False)
    remove_boot_entries(m.grub.get("saved_entry_was"), run=run)
    for rel in (Manifest.PATH, stage.STAGE_PATH):
        try:
            os.remove(P(rel))
        except OSError:
            pass
    return {"files_removed": removed, "packages": m.packages}


def _mount_slug(target):
    if target == "/":
        return "root"
    return target.strip("/").replace("/", "-")


SKIP_MOUNT_TARGETS = {"/"}
NO_HARDENING = "no_new_privs=0\nprivate_tmp=0\nprotect_system=0\nprotect_home=0\n"


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
                + "".join("dep=%s\n" % d for d in deps)
                + NO_HARDENING)
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
        cand = find_source(os.path.join(d, base))
        if cand:
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
        if "/usr/local/bin/" + name in manifest.files:
            continue
        src = _find_helper_src(name)
        if src is None:
            continue
        dst = P("usr/local/bin/" + name)
        if not _rpm_owned("/usr/local/bin/" + name):
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
                         "critical=0\n" + NO_HARDENING)
    manifest.add_file("/etc/schema-init/services/coldplug-modules.svc")
    return mods


UDEVD_PATHS = ["/usr/lib/systemd/systemd-udevd", "/lib/systemd/systemd-udevd",
               "/usr/libexec/systemd-udevd"]


UDEVD_LAUNCH = "/usr/local/bin/schema-udevd-launch.sh"
UDEV_LIVE_FLAG = "/etc/schema-init/schema-udev.live"


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
    # The armed LIVE flag picks the owner at boot, as schema-sysprep.sh does on
    # the ISO: schema-udev does its own coldplug and writes its ready file;
    # otherwise stock systemd-udevd plus udevadm trigger/settle.
    launch = P(UDEVD_LAUNCH.lstrip("/"))
    os.makedirs(os.path.dirname(launch), exist_ok=True)
    open(launch, "w").write("#!/bin/sh\n"
                            "if [ -e %s ] && [ -x /usr/bin/schema-udev ]; then\n"
                            "    mkdir -p /run/schema-udev\n"
                            "    exec /usr/bin/schema-udev\n"
                            "fi\n"
                            "exec %s\n" % (UDEV_LIVE_FLAG, udevd))
    os.chmod(launch, 0o755)
    manifest.add_file(UDEVD_LAUNCH)
    svc = P("etc/schema-init/services/udevd.svc")
    os.makedirs(os.path.dirname(svc), exist_ok=True)
    open(svc, "w").write("name=udevd\nexec=%s\nneeds_root=1\ncritical=0\n" % UDEVD_LAUNCH + NO_HARDENING)
    manifest.add_file("/etc/schema-init/services/udevd.svc")
    script = P("usr/local/bin/schema-udev-trigger.sh")
    os.makedirs(os.path.dirname(script), exist_ok=True)
    open(script, "w").write("#!/bin/sh\n"
                            "if [ -e %s ] && [ -x /usr/bin/schema-udev ]; then\n"
                            "    i=0; while [ $i -lt 60 ]; do [ -e /run/schema-udev/ready ] && exit 0; i=$((i+1)); sleep 0.5; done\n"
                            "    exit 0\n"
                            "fi\n"
                            "udevadm trigger --action=add --type=subsystems\n"
                            "udevadm trigger --action=add --type=devices\n"
                            "udevadm settle --timeout=30\n" % UDEV_LIVE_FLAG)
    os.chmod(script, 0o755)
    manifest.add_file("/usr/local/bin/schema-udev-trigger.sh")
    tsvc = P("etc/schema-init/services/udev-trigger.svc")
    open(tsvc, "w").write("name=udev-trigger\n"
                          "exec=/usr/local/bin/schema-udev-trigger.sh\n"
                          "dep=udevd\noneshot=1\nneeds_root=1\ncritical=0\n" + NO_HARDENING)
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
    if os.path.islink(rc) and ("systemd/resolve" in os.readlink(rc) or not os.path.exists(rc)):
        manifest.links["/etc/resolv.conf"] = os.readlink(rc)
        os.unlink(rc)
    script = P("usr/local/bin/schema-resolv-unstub.sh")
    os.makedirs(os.path.dirname(script), exist_ok=True)
    open(script, "w").write("#!/bin/sh\n"
                            "[ -L /etc/resolv.conf ] && [ ! -e /etc/resolv.conf ] && rm -f /etc/resolv.conf\n"
                            "exit 0\n")
    os.chmod(script, 0o755)
    manifest.add_file("/usr/local/bin/schema-resolv-unstub.sh")
    svc = P("etc/schema-init/services/resolv-unstub.svc")
    os.makedirs(os.path.dirname(svc), exist_ok=True)
    open(svc, "w").write("name=resolv-unstub\n"
                         "exec=/usr/local/bin/schema-resolv-unstub.sh\n"
                         "oneshot=1\nneeds_root=1\ncritical=0\n" + NO_HARDENING)
    manifest.add_file("/etc/schema-init/services/resolv-unstub.svc")
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
PREBUILT_BINS = ["schema-init", "schema-ctl", "schema-subreaper"]

FLIP_HELPER = "/usr/libexec/schema-init/schema-flip-apply"
SEATBELT_HELPER = "/usr/libexec/schema-init/schema-udev-flip-healthcheck.sh"
DBUS_SEATBELT_HELPER = "/usr/libexec/schema-init/schema-dbus-flip-healthcheck.sh"
AUTOSTART = "etc/xdg/autostart/schema-wizard.desktop"
USER_AUTOSTART = ".config/autostart/schema-wizard.desktop"

# What the ISO kickstart lays down around the Plasma launch chain, from the
# same sources. plasma-session-start.sh sources plasma-env/ and fires the
# XDG-autostart runner from /usr/local/lib/schema; without them a migrated
# box has no autostart apps, no ssh-agent and no plasmashell watchdog.
SESSION_SUPPORT = [
    ("distros/fedora-kde/scripts/schema-autostart-runner.sh", "usr/local/lib/schema/schema-autostart-runner.sh", 0o755),
    ("distros/fedora-kde/scripts/schema-plasma-watchdog.sh", "usr/local/lib/schema/schema-plasma-watchdog.sh", 0o755),
    ("distros/fedora-kde/config/plasma-env/zzz-environment-d.sh", "usr/local/lib/schema/zzz-environment-d.sh", 0o644),
    ("distros/fedora-kde/config/plasma-env/05-kdedefaults.sh", "usr/local/lib/schema/plasma-env/05-kdedefaults.sh", 0o644),
    ("distros/fedora-kde/config/plasma-env/no-app-scope.sh", "usr/local/lib/schema/plasma-env/no-app-scope.sh", 0o644),
    ("distros/fedora-kde/config/plasma-env/ssh-agent-sock.sh", "usr/local/lib/schema/plasma-env/ssh-agent-sock.sh", 0o644),
    ("distros/fedora-kde/config/plasma-workspace/env/zz-schema-autostart.sh", "usr/local/lib/schema/plasma-env/zz-schema-autostart.sh", 0o644),
]


def install_session_support(profile, manifest, dry_run=False):
    done = []
    for src_rel, dst_rel, mode in SESSION_SUPPORT:
        src = find_source(src_rel)
        if not src or _rpm_owned(dst_rel):
            continue
        done.append("/" + dst_rel)
        if dry_run:
            continue
        dst = P(dst_rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        os.chmod(dst, mode)
        manifest.add_file("/" + dst_rel)
    # The runner sweeps only ~/.config/autostart, so the wizard's system entry
    # never fires under schema-init; same filename overrides it under systemd.
    user, uid = profile.get("user"), profile.get("uid")
    home = P("home/%s" % user) if user else None
    src = P(AUTOSTART)
    if home and os.path.isdir(home) and os.path.exists(src) and not dry_run:
        dst = os.path.join(home, USER_AUTOSTART)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        for d in (os.path.dirname(os.path.dirname(dst)), os.path.dirname(dst), dst):
            try:
                os.chown(d, uid, uid)
            except (OSError, TypeError):
                pass
        manifest.add_file("/home/%s/%s" % (user, USER_AUTOSTART))
        done.append("/home/%s/%s" % (user, USER_AUTOSTART))
    return done

def _default_flip(*a):
    return subprocess.run([FLIP_HELPER, *a], capture_output=True, text=True)

def install_flip_seatbelt(manifest, dry_run=False):
    # headless backstop: a schema-init oneshot that runs every boot and, if a
    # flip is armed, rolls it back when /dev comes up unusable or the desktop
    # never confirms. Laid down dormant at deploy; the helper gates itself on
    # the armed state, so it no-ops until `schema-flip-apply arm` (R2).
    rel = "etc/schema-init/services/schema-udev-healthcheck.svc"
    if dry_run:
        return P(rel)
    dst = P(rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    # No dep=: a dep that crash-loops goes DORMANT, never EXCISED, and blocks
    # its dependents forever — a broken udev would stop the seatbelt ever
    # running. The healthcheck waits for /dev itself (bounded), so it needs
    # start_timeout_sec above the ~90s oneshot default.
    body = ("name=schema-udev-healthcheck\n"
            "exec=" + SEATBELT_HELPER + "\n"
            "oneshot=1\n"
            "start_timeout_sec=300\n"
            "needs_root=1\n"
            "critical=0\n" + NO_HARDENING)
    open(dst, "w").write(body)
    manifest.add_file("/" + rel)
    drel = "etc/schema-init/services/schema-dbus-healthcheck.svc"
    open(P(drel), "w").write("name=schema-dbus-healthcheck\n"
                             "exec=" + DBUS_SEATBELT_HELPER + "\n"
                             "oneshot=1\n"
                             "start_timeout_sec=300\n"
                             "needs_root=1\n"
                             "critical=0\n" + NO_HARDENING)
    manifest.add_file("/" + drel)
    return dst

def teardown(root="/"):
    paths = [os.path.join(root, AUTOSTART)]
    home = os.path.join(root, "home")
    if os.path.isdir(home):
        paths += [os.path.join(home, u, USER_AUTOSTART) for u in os.listdir(home)]
    for p in paths:
        try:
            os.remove(p)
        except OSError:
            pass

def arm_flip(root="/", flip=_default_flip):
    if stage.read_stage(root) != stage.R1_HEAL:
        raise RuntimeError("arm-flip requires stage R1_HEAL")
    if not udev_offered(root):
        stage.transition(stage.DONE, root=root)
        if not dbus_offered(root):
            teardown(root)
        return 0
    if flip("arm").returncode != 0:
        return 1
    stage.transition(stage.R2_PENDING, root=root)
    return 0

def _boot_id():
    try:
        return open("/proc/sys/kernel/random/boot_id").read().strip()
    except OSError:
        return ""

def dbus_awaiting_reboot(root="/"):
    return (stage.read_stage(root) == stage.R3_PENDING
            and stage.read_extra(root).get("dbus_armed_boot") == _boot_id())

def udev_offered(root="/"):
    return "no-udev-flip" not in stage.read_extra(root).get("adv", [])

def dbus_offered(root="/"):
    return "no-dbus-broker" not in stage.read_extra(root).get("adv", [])

def advance_finish(root="/", flip=_default_flip):
    cur = stage.read_stage(root)
    if cur == stage.R1_PENDING:
        stage.transition(stage.R1_HEAL, root=root)
        return stage.R1_HEAL
    if cur == stage.R2_PENDING:
        authoritative = flip("is-authoritative").returncode == 0
        if authoritative:
            flip("confirm")
        new = stage.DONE if authoritative else stage.ROLLED_BACK
        stage.transition(new, root=root)
        if not (authoritative and dbus_offered(root)):
            teardown(root)
        return new
    return cur

def arm_dbus(root="/", flip=_default_flip):
    if stage.read_stage(root) != stage.DONE or not dbus_offered(root):
        return 1
    if flip("dbus-arm").returncode != 0:
        return 1
    stage.transition(stage.R3_PENDING, root=root, extra={"dbus_armed_boot": _boot_id()})
    return 0

def confirm_dbus(root="/", flip=_default_flip):
    if stage.read_stage(root) != stage.R3_PENDING or dbus_awaiting_reboot(root):
        return stage.read_stage(root)
    if flip("dbus-is-authoritative").returncode == 0:
        flip("dbus-confirm")
        new = stage.R3_DONE
    elif (flip("dbus-state").stdout or "").strip() == "skipped":
        new = stage.R3_ROLLED_BACK
    else:
        flip("dbus-rollback")
        stage.transition(stage.R3_ROLLED_BACK, root=root)
        teardown(root)
        flip("reboot")
        return stage.R3_ROLLED_BACK
    stage.transition(new, root=root)
    teardown(root)
    return new

def skip_dbus(root="/"):
    if stage.read_stage(root) != stage.DONE:
        return 1
    adv = stage.read_extra(root).get("adv", [])
    stage.write_stage(stage.DONE, root=root, extra={"adv": sorted(set(adv) | {"no-dbus-broker"})})
    teardown(root)
    return 0

RECOVERY_TEXT = (
    "HOW TO GET YOUR COMPUTER BACK\n"
    "=============================\n\n"
    "Your computer is about to restart to finish setting up schema.\n\n"
    "If the screen stays BLACK for more than 2 minutes after the restart:\n\n"
    "  1. Hold the power button until the computer turns off.\n"
    "  2. Press it again to turn it back on.\n"
    "  3. At the start-up menu, use the arrow keys to choose the entry that\n"
    "     does NOT say \"(schema-init)\".\n"
    "  4. Press Enter.\n\n"
    "Your computer will start exactly as it does today. Nothing is lost.\n"
)


def recovery_text(snapshot=None, fallback=True):
    t = RECOVERY_TEXT
    if not fallback:
        t = t.replace(
            "  3. At the start-up menu, use the arrow keys to choose the entry that\n"
            "     does NOT say \"(schema-init)\".\n"
            "  4. Press Enter.\n\n"
            "Your computer will start exactly as it does today. Nothing is lost.\n",
            "You chose not to keep the old start-up entry, so there is no menu\n"
            "option to go back to. Start from a Fedora USB stick and run\n"
            "\"sudo schema-migrate --uninstall\" on your installed system.\n")
    if snapshot:
        t += ("\nA read-only copy of your system from before the change is saved at\n"
              "  %s\n"
              "\"sudo schema-migrate --uninstall\" removes schema and this copy.\n" % snapshot)
    return t


def write_recovery_card(profile, root="/", text=RECOVERY_TEXT):
    written = []
    user = profile.get("user")
    uid = profile.get("uid")
    targets = []
    # only place a card in the user's home if that home already exists — never
    # create /home/<user> ourselves (as root it would be root-owned and break
    # the user's later home setup).
    if user and os.path.isdir(os.path.join(root, "home", user)):
        targets.append(("home/%s/schema-recovery.txt" % user, uid))
    targets.append(("boot/schema-recovery.txt", None))
    for rel, owner in targets:
        dst = os.path.join(root, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w") as fh:
            fh.write(text)
        os.chmod(dst, 0o644)
        if owner is not None:
            try:
                os.chown(dst, owner, owner)
            except OSError:
                pass
        written.append("/" + rel)
    return written


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


BUILD_PACKAGES = ["gcc", "make", "glibc-static", "libacl-devel", "dbus-devel", "pkgconf-pkg-config"]


def ensure_build_toolchain(manifest, run=subprocess.run, dry_run=False):
    missing = [p for p in BUILD_PACKAGES if not _installed(p, run)]
    if not missing or dry_run:
        return missing
    run(["dnf", "install", "-y"] + missing, check=False)
    for p in missing:
        if _installed(p, run):
            manifest.add_package(p)
    return missing


def provision_binaries(manifest, run=subprocess.run, dry_run=False, prebuilt=False):
    if dry_run:
        return
    if prebuilt:
        missing = [b for b in PREBUILT_BINS if not os.path.exists(P("usr/bin/" + b))]
        if missing:
            raise RuntimeError("prebuilt mode: %s absent — install the schema-init "
                               "package first" % ", ".join("/usr/bin/" + b for b in missing))
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


def run_make_install(manifest, run=subprocess.run, dry_run=False, prebuilt=False):
    return provision_binaries(manifest, run=run, dry_run=dry_run, prebuilt=prebuilt)


IMPORTED_LIST = "var/lib/schema-init/migrate-imported.list"


def _import_script():
    src = find_source("distros/fedora-installer/migrate/schema-import.py")
    if src:
        return src
    rpm = P("usr/bin/schema-import")
    return rpm if os.path.exists(rpm) else None


def import_enabled_units(manifest, run=subprocess.run, dry_run=False):
    script = _import_script()
    if dry_run or not script:
        if not script:
            print("WARN: schema-import not found; enabled units not imported")
        return []
    svcd = P("etc/schema-init/services")
    before = set(os.listdir(svcd)) if os.path.isdir(svcd) else set()
    env = dict(os.environ, MIGRATE_ROOT=ROOT)
    r = run([sys.executable, script, "--enabled"], capture_output=True, text=True, env=env)
    log = P("var/log/schema-init/migrate-import.log")
    os.makedirs(os.path.dirname(log), exist_ok=True)
    with open(log, "w") as f:
        f.write((getattr(r, "stdout", "") or "") + (getattr(r, "stderr", "") or ""))
    if r.returncode != 0:
        print("WARN: some enabled units did not import; see /var/log/schema-init/migrate-import.log")
    added = sorted(set(os.listdir(svcd)) - before) if os.path.isdir(svcd) else []
    rec = P(IMPORTED_LIST)
    try:
        prior = open(rec).read().split()
    except OSError:
        prior = []
    names = sorted(set(prior) | set(added))
    os.makedirs(os.path.dirname(rec), exist_ok=True)
    with open(rec, "w") as f:
        f.write("".join(n + "\n" for n in names))
    manifest.add_file("/" + IMPORTED_LIST)
    for name in names:
        if os.path.exists(os.path.join(svcd, name)):
            manifest.add_file("/etc/schema-init/services/" + name)
    return added


def do_deploy(run=subprocess.run, dry_run=False, prebuilt=False):
    profile = build_profile(run=run)
    if profile["kernel"] not in stock_entries():
        raise RuntimeError("no boot entry for the running kernel %s in /boot/loader/entries "
                           "— refusing to migrate a box whose boot menu can't be read"
                           % profile["kernel"])
    snap = take_snapshot(run=run, dry_run=dry_run) if "no-snapshot" not in _adv() else None
    if not dry_run:
        write_profile(profile)
    m = Manifest()
    if snap and not dry_run:
        m.snapshot = "/" + os.path.relpath(snap, ROOT)
    run_make_install(m, run=run, dry_run=dry_run, prebuilt=prebuilt)
    deploy_prevent_set(m, dry_run=dry_run)
    generate_host_units(profile, m, dry_run=dry_run)
    generate_module_load(m, dry_run=dry_run)
    generate_udev_units(m, dry_run=dry_run)
    install_flip_seatbelt(m, dry_run=dry_run)
    generate_nm_config(m, dry_run=dry_run)
    ensure_user_groups(profile, run=run, dry_run=dry_run)
    _add_unit_dep("network-manager", "coldplug-modules", dry_run=dry_run)
    _add_unit_dep("network-manager", "udev-trigger", dry_run=dry_run)
    _add_unit_dep("network-manager", "resolv-unstub", dry_run=dry_run)
    install_unit_helpers(m, dry_run=dry_run)
    install_session_support(profile, m, dry_run=dry_run)
    install_packages(m, run=run, dry_run=dry_run)
    import_enabled_units(m, run=run, dry_run=dry_run)
    # grub2-mkconfig FIRST: Fedora's BLS sync rewrites every loader entry's
    # options from the canonical cmdline, so it must run before the schema
    # entry is written or it strips the init= override we just added.
    ensure_grub_menu_visible(m, run=run, dry_run=dry_run)
    entry = seed_boot_entries(profile["kernel"], m, run=run) if not dry_run else None
    if not dry_run and "no-fallback-entry" in _adv():
        hide_fallback_entries(m)
    if not dry_run:
        write_recovery_card(profile, root=ROOT, text=recovery_text(
            m.snapshot, fallback="no-fallback-entry" not in _adv()))
        m.set_boot_entry("/" + os.path.relpath(entry, ROOT))
        m.save()
        stage.transition(stage.R1_PENDING, root=ROOT, extra={"adv": _adv()})
    return m


def finish_report():
    try:
        prof = json.load(open(P("var/lib/schema-init/migrate-profile.json")))
    except (OSError, ValueError):
        prof = {}
    leftover = [u for u in prof.get("services", {}).get("leftover", [])
                if not os.path.exists(P("etc/schema-init/services/%s.svc" % u))]
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
        lines.append("Run `sudo schema-import <unit>.service` to carry one over "
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
    ap.add_argument("--arm-flip", action="store_true", help="R2: arm the udev flip")
    ap.add_argument("--arm-dbus", action="store_true", help="R3: arm the schema-dbus flip")
    ap.add_argument("--confirm-dbus", action="store_true", help="R3: judge the dbus flip from the desktop")
    ap.add_argument("--skip-dbus", action="store_true", help="R3: decline the dbus flip")
    ap.add_argument("--prebuilt", action="store_true",
                    help="consume RPM-installed binaries; never compile")
    ap.add_argument("--stage", action="store_true", help="print the current wizard stage")
    ap.add_argument("--advanced-no-fallback-entry", action="store_true")
    ap.add_argument("--advanced-no-snapshot", action="store_true")
    ap.add_argument("--advanced-no-udev-flip", action="store_true")
    ap.add_argument("--advanced-no-dbus-broker", action="store_true")
    ap.add_argument("--advanced-no-doctor-timers", action="store_true")
    args = ap.parse_args(argv)

    os.environ.setdefault("MIGRATE_ADV", ",".join(
        k for k in ("no-fallback-entry", "no-snapshot", "no-udev-flip", "no-dbus-broker", "no-doctor-timers")
        if getattr(args, "advanced_" + k.replace("-", "_"))))

    if args.stage:
        print(stage.read_stage(ROOT)); return 0

    if args.arm_flip:
        return arm_flip(root=ROOT)

    if args.arm_dbus:
        return arm_dbus(root=ROOT)

    if args.confirm_dbus:
        print(confirm_dbus(root=ROOT))
        return 0

    if args.skip_dbus:
        return skip_dbus(root=ROOT)

    if args.finish:
        if os.path.exists(P("run/schema-init/migrate-finished")):
            print("migrate-finish already ran")
            return 0
        new = advance_finish(root=ROOT)
        print("stage: " + new)
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

    if not (args.deploy or args.dry_run):
        ap.print_help(sys.stderr)
        print("\nrefusing: no action given — pass --deploy to run the migration "
              "(or --discover / --dry-run to preview). Bare `schema-migrate` no "
              "longer deploys.", file=sys.stderr)
        return 2

    prebuilt = args.prebuilt or os.environ.get("MIGRATE_PREBUILT") == "1"
    do_deploy(run=run, dry_run=args.dry_run, prebuilt=prebuilt)
    print("dry-run complete — nothing changed" if args.dry_run
          else "deploy complete — reboot; schema-init is now the default boot entry")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
