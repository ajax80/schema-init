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
