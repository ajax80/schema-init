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


def active_uid():
    d = os.path.join(ROOT, "run/systemd/sessions")
    try:
        names = [n for n in os.listdir(d) if n != "31" and not n.endswith(".ref")]
    except FileNotFoundError:
        return None
    for n in sorted(names):
        try:
            for line in open(os.path.join(d, n)):
                if line.startswith("UID="):
                    return int(line.strip()[4:])
        except (OSError, ValueError):
            continue
    return None


def _proc_table():
    tbl = []
    for d in sorted(glob.glob(os.path.join(ROOT, "proc/[0-9]*"))):
        try:
            cmd = open(os.path.join(d, "cmdline"), "rb").read() \
                    .replace(b"\0", b" ").decode(errors="replace").strip()
        except OSError:
            continue
        env = {}
        try:
            for kv in open(os.path.join(d, "environ"), "rb").read().split(b"\0"):
                k, sep, v = kv.partition(b"=")
                if sep:
                    env[k.decode(errors="replace")] = v.decode(errors="replace")
        except OSError:
            pass
        tbl.append({"pid": os.path.basename(d), "cmd": cmd, "env": env})
    return tbl


def _running(needle, tbl=None):
    return any(needle in p["cmd"] for p in (tbl if tbl is not None else _proc_table()))


class CardInputAcl(Check):
    name = "card-input-acl"
    summary = "the logged-in user can open the GPU and input devices"
    grade = SAFE

    def _nodes(self):
        pats = ["dev/dri/card*", "dev/dri/renderD*", "dev/input/event*"]
        out = []
        for p in pats:
            out += glob.glob(os.path.join(ROOT, p))
        return sorted(out)

    def _getfacl(self, path):
        return subprocess.run(["getfacl", "-pn", path],
                              capture_output=True, text=True).stdout

    def _has_rw(self, path, uid):
        return f"user:{uid}:rw" in self._getfacl(path)

    def detect(self):
        uid = active_uid()
        if uid is None or uid == 0:
            return None
        missing = [n for n in self._nodes() if not self._has_rw(n, uid)]
        if not missing:
            return None
        return Finding(
            detail=f"uid {uid} lacks rw on: {', '.join(os.path.basename(m) for m in missing)}",
            oracle_said="systemd uaccess grants the active-seat user rw on these",
            healable=True)

    def snapshot(self):
        return {n: self._getfacl(n) for n in self._nodes()}

    def heal(self, f):
        uid = active_uid()
        for n in self._nodes():
            if not self._has_rw(n, uid):
                subprocess.run(["setfacl", "-m", f"u:{uid}:rw", n], check=False)

    def back_out(self, snap):
        for n, acl in snap.items():
            subprocess.run(["setfacl", "--set-file=-", n],
                           input=acl, text=True, check=False)


REGISTRY.append(CardInputAcl())


def _active_vtnr():
    rel = os.environ.get("SCHEMA_DOCTOR_ACTIVE_VT", "sys/class/tty/tty0/active")
    try:
        val = open(os.path.join(ROOT, rel)).read().strip()   # e.g. "tty1"
        return int(val[3:]) if val.startswith("tty") else None
    except (OSError, ValueError):
        return None


def _sessions():
    d = os.path.join(ROOT, "run/systemd/sessions")
    out = {}
    try:
        names = [n for n in os.listdir(d) if not n.endswith(".ref")]
    except FileNotFoundError:
        return out
    for n in names:
        kv = {}
        try:
            for line in open(os.path.join(d, n)):
                k, _, v = line.strip().partition("=")
                kv[k] = v
        except OSError:
            continue
        out[n] = kv
    return out


class SessionSingle(Check):
    name = "session-single"
    summary = "one real desktop session, no orphaned placeholder"
    grade = DEFERRED

    def detect(self):
        sess = _sessions()
        if not sess:
            return None
        vtnr = _active_vtnr()
        orphan = "31" in sess and sess["31"].get("VTNR", "0") == "0"
        backed = any(s.get("VTNR") == str(vtnr) for s in sess.values()) if vtnr else False
        active_is_orphan = orphan and (len(sess) == 1 or not backed)
        if active_is_orphan:
            return Finding(
                detail="the active session is the synthesised placeholder #31 "
                       "(VTNR=0) — session registration lost the boot-time race",
                oracle_said=f"a real session should own the live VT (tty{vtnr})",
                healable=False)
        if vtnr and not backed:
            return Finding(
                detail=f"no registered session owns the active VT (tty{vtnr})",
                oracle_said=f"the logged-in session should carry VTNR={vtnr}",
                healable=False)
        return None


REGISTRY.append(SessionSingle())


class Login1Power(Check):
    name = "login1-power"
    summary = "login1 answers power, suspend, and inhibitor queries"
    grade = DEFERRED

    METHODS = ["CanPowerOff", "CanReboot", "CanSuspend", "CanHibernate", "ListInhibitors"]

    def probe(self, method):
        cmd = os.environ.get("SCHEMA_DOCTOR_BUSCTL", "busctl")
        try:
            r = subprocess.run(
                [cmd, "call", "org.freedesktop.login1", "/org/freedesktop/login1",
                 "org.freedesktop.login1.Manager", method],
                capture_output=True, text=True, timeout=5)
            return r.returncode, (r.stdout + r.stderr).strip()
        except Exception as e:
            return 1, str(e)

    def detect(self):
        failed = [m for m in self.METHODS if self.probe(m)[0] != 0]
        if not failed:
            return None
        return Finding(
            detail=f"login1 did not answer: {', '.join(failed)} "
                   "(this is why PowerDevil says settings could not be loaded)",
            oracle_said="systemd-logind answers all of these on the Manager interface",
            healable=False)


REGISTRY.append(Login1Power())


# linux/vt.h
VT_GETMODE = 0x5601
VT_AUTO, VT_PROCESS = 0x00, 0x01


class VtMediation(Check):
    name = "vt-mediation"
    summary = "Ctrl+Alt+F-keys are mediated (VT switching won't freeze the screen)"
    grade = SAFE

    def vt_mode(self):
        env = os.environ.get("SCHEMA_DOCTOR_VT_MODE")
        if env:
            return env
        try:
            fd = os.open(os.path.join(ROOT, "dev/tty0"), os.O_RDONLY | os.O_NOCTTY)
        except OSError:
            return "unknown"
        try:
            buf = fcntl.ioctl(fd, VT_GETMODE, struct.pack("bbhhh", 0, 0, 0, 0, 0))
            mode = struct.unpack("bbhhh", buf)[0]
            return "process" if mode == VT_PROCESS else "auto"
        except OSError:
            return "unknown"
        finally:
            os.close(fd)

    def rearm(self):
        cmd = os.environ.get("SCHEMA_DOCTOR_BUSCTL", "busctl")
        subprocess.run(
            [cmd, "call", "org.freedesktop.login1", "/org/freedesktop/login1",
             "org.schema.logind1.Manager", "RearmVtMediation"],
            capture_output=True, text=True, check=False, timeout=5)

    def detect(self):
        m = self.vt_mode()
        if m in ("process", "unknown"):     # unknown = can't prove broken; don't cry wolf
            return None
        return Finding(
            detail="the active VT is in kernel-native switching (VT_AUTO), not "
                   "mediated by schema-logind — switching consoles can freeze the screen",
            oracle_said="logind puts the session's VT in VT_PROCESS mode",
            healable=True)

    def heal(self, f):
        try:
            self.rearm()
        except Exception:
            pass

    def verify(self):
        return self.vt_mode() == "process"


REGISTRY.append(VtMediation())


class PowerDevilRunning(Check):
    name = "powerdevil-running"
    summary = "PowerDevil is running so power and screen-lock settings load"
    grade = DEFERRED

    def detect(self):
        if active_uid() in (None, 0):
            return None
        if _running("org_kde_powerdevil"):
            return None
        return Finding(
            detail="PowerDevil is not running — its /etc/xdg/autostart entry carries "
                   "X-systemd-skip=true and there is no systemd --user to start it, so "
                   "the power and screen-lock settings report the service is not running "
                   "and cannot load",
            oracle_said="systemd --user starts plasma-powerdevil.service at login",
            healable=False)


REGISTRY.append(PowerDevilRunning())


class KsycocaLoop(Check):
    name = "ksycoca-loop"
    summary = "Plasma processes agree on the menu prefix (no ksycoca rebuild storm)"
    grade = DEFERRED

    def detect(self):
        tbl = _proc_table()
        pl = next((p for p in tbl if "plasmashell" in p["cmd"]), None)
        kd = next((p for p in tbl if "kded6" in p["cmd"]), None)
        if not pl or not kd:
            return None
        a = pl["env"].get("XDG_MENU_PREFIX", "")
        b = kd["env"].get("XDG_MENU_PREFIX", "")
        if a == b:
            return None
        return Finding(
            detail=f"plasmashell and kded6 disagree on XDG_MENU_PREFIX "
                   f"(plasmashell={a or 'unset'!r}, kded6={b or 'unset'!r}) — each "
                   "rebuilds ksycoca to its own menu view about once a second, pinning "
                   "a CPU core; that is the ~2-second desktop and video stutter",
            oracle_said="systemd --user gives every session process one consistent "
                        "XDG_MENU_PREFIX",
            healable=False)


REGISTRY.append(KsycocaLoop())


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
