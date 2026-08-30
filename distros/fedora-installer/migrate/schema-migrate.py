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
