#!/usr/bin/env python3
"""schema-doctor — diagnose-and-heal the schema-init logind seam.

Runs late (after a session exists) as a critical=0 oneshot and as a CLI.
Each invariant is a Check; the run loop snapshots before every SAFE heal and
backs out on failure or collateral so the box is never left worse. systemd,
still installed, is the oracle. Stdlib only — dbus is reached via busctl.
"""
import fcntl
import glob
import json
import os
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Any, Optional

SAFE, DEFERRED = "SAFE", "DEFERRED"
ROOT = os.environ.get("DOCTOR_ROOT", "")


@dataclass
class Finding:
    detail: str
    oracle_said: str = ""
    healable: bool = True


@dataclass
class CheckResult:
    name: str
    summary: str
    state: str          # clean | healed | reported
    detail: str = ""
    oracle_said: str = ""
    action: str = ""


class Check:
    name = "check"
    summary = ""
    grade = SAFE

    def detect(self) -> Optional[Finding]:
        raise NotImplementedError

    def explain(self, f: Finding) -> str:
        return f.detail

    def snapshot(self) -> Any:
        return None

    def heal(self, f: Finding) -> None:
        pass

    def verify(self) -> bool:
        return self.detect() is None

    def back_out(self, snap: Any) -> None:
        pass


def run_checks(checks, heal, force):
    results = {}
    order = []
    clean = []          # checks that detected healthy — watched for collateral
    aborted = False
    for c in checks:
        order.append(c.name)
        if aborted:
            results[c.name] = CheckResult(c.name, c.summary, "reported",
                                          "not run — earlier heal was rolled back",
                                          "", "run aborted")
            continue
        f = c.detect()
        if f is None:
            results[c.name] = CheckResult(c.name, c.summary, "clean")
            clean.append(c)
            continue
        will_heal = heal and f.healable and (c.grade == SAFE or c.name in force)
        if not will_heal:
            results[c.name] = CheckResult(c.name, c.summary, "reported",
                                          c.explain(f), f.oracle_said,
                                          "left as found (deferred)" if c.grade == DEFERRED
                                          else "detect-only")
            continue
        snap = c.snapshot()
        c.heal(f)
        # collateral: did any previously-clean check just break?
        broke = next((x for x in clean if x.detect() is not None), None)
        if broke is not None:
            c.back_out(snap)
            results[c.name] = CheckResult(c.name, c.summary, "reported",
                                          c.explain(f), f.oracle_said,
                                          f"rolled back — heal broke {broke.name}")
            aborted = True
            continue
        if c.verify():
            results[c.name] = CheckResult(c.name, c.summary, "healed",
                                          c.explain(f), f.oracle_said, "healed")
            clean.append(c)
        else:
            c.back_out(snap)
            results[c.name] = CheckResult(c.name, c.summary, "reported",
                                          c.explain(f), f.oracle_said,
                                          "heal did not resolve — rolled back")
    return [results[n] for n in order]


REGISTRY: list = []


def read_config():
    heal = True
    disabled = set()
    path = os.path.join(ROOT, "etc/schema-init/doctor.conf")
    try:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                k, _, v = line.partition("=")
                k, v = k.strip(), v.strip()
                if k == "heal" and v.lower() in ("no", "off", "0", "false"):
                    heal = False
                elif k == "disable":
                    disabled |= {x.strip() for x in v.split(",") if x.strip()}
    except FileNotFoundError:
        pass
    return heal, disabled


def render_report(results):
    lines = ["=== schema-doctor report ===", ""]
    for r in results:
        lines.append(f"[{r.state.upper()}] {r.name} — {r.summary}")
        if r.detail:
            lines.append(f"    found:  {r.detail}")
        if r.oracle_said:
            lines.append(f"    systemd would: {r.oracle_said}")
        if r.action:
            lines.append(f"    action: {r.action}")
        lines.append("")
    return "\n".join(lines)


def render_json(results):
    return json.dumps([r.__dict__ for r in results], indent=2)


def write_report(text):
    d = os.path.join(ROOT, "var/log/schema-init")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "doctor-report.txt")
    with open(p, "w") as fh:
        fh.write(text)
    os.chmod(p, 0o644)
    return p


def wait_for_session(timeout):
    d = os.path.join(ROOT, "run/systemd/sessions")
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if any(n != "31" and not n.endswith(".ref") for n in os.listdir(d)):
                return True
        except FileNotFoundError:
            pass
        time.sleep(0.25)
    return False


def _safe_detect_all(checks, heal, force):
    try:
        return run_checks(checks, heal, force)
    except Exception as e:                       # never propagate — never block boot
        return [CheckResult("schema-doctor", "internal", "reported",
                            f"doctor aborted: {e}", "", "logged, exit 0")]


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog="schema-doctor")
    ap.add_argument("--check", action="store_true", help="detect + report, no heal")
    ap.add_argument("--heal", action="store_true", help="detect + heal + re-check")
    ap.add_argument("--explain", metavar="NAME", help="plain-language why for one check")
    ap.add_argument("--dry-run", action="store_true", help="show heals, change nothing")
    ap.add_argument("--force", action="append", default=[], metavar="NAME",
                    help="run a DEFERRED check's heal anyway")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--wait", type=float, default=0.0, help="wait N s for a session first")
    args = ap.parse_args(argv)

    cfg_heal, disabled = read_config()
    checks = [c for c in REGISTRY if c.name not in disabled]

    if args.explain:
        for c in checks:
            if c.name == args.explain:
                f = None
                try:
                    f = c.detect()
                except Exception as e:
                    print(f"{c.name}: detect failed: {e}"); return 0
                print(c.explain(f) if f else f"{c.name}: healthy")
                return 0
        print(f"unknown check: {args.explain}"); return 0

    if args.wait:
        wait_for_session(args.wait)

    heal = (args.heal or (not args.check and not args.dry_run)) and cfg_heal and not args.dry_run
    results = _safe_detect_all(checks, heal, set(args.force))

    out = render_json(results) if args.json else render_report(results)
    write_report(render_report(results))
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
