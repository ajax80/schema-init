#!/usr/bin/env python3
"""schema-migrate — transition an existing Fedora KDE box onto schema-init
in place, non-destructively, with a fallback boot entry and post-reboot heal.

Stdlib only. MIGRATE_ROOT prefixes filesystem paths (tests inject a temp tree);
MIGRATE_REPO points at the schema-init tree on the USB.
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.environ.get("MIGRATE_ROOT") or "/"
_MODDIR = os.path.dirname(os.path.abspath(__file__))


def P(rel):
    return os.path.join(ROOT, rel.lstrip("/"))


def repo():
    return os.environ.get("MIGRATE_REPO") or os.path.dirname(os.path.dirname(os.path.dirname(_MODDIR)))


def load_prevent_set(path=None):
    if path is None:
        path = os.path.join(_MODDIR, "prevent-set.list")
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
