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
ROOT = os.environ.get("DOCTOR_ROOT") or "/"
GREEN, AMBER, RED = "GREEN", "AMBER", "RED"


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
    grade: str = SAFE


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


def run_checks(checks, heal, force, flap=None, now=None):
    if now is None:
        now = time.time()
    results = {}
    order = []
    clean = []          # checks that detected healthy — watched for collateral
    aborted = False
    for c in checks:
        order.append(c.name)
        if aborted:
            results[c.name] = CheckResult(c.name, c.summary, "reported",
                                          "not run — earlier heal was rolled back",
                                          "", "run aborted", grade=c.grade)
            continue
        f = c.detect()
        if f is None:
            results[c.name] = CheckResult(c.name, c.summary, "clean", grade=c.grade)
            clean.append(c)
            if flap is not None:
                flap.recovered(c.name)
            continue
        will_heal = heal and f.healable and (c.grade == SAFE or c.name in force)
        if not will_heal:
            results[c.name] = CheckResult(c.name, c.summary, "reported",
                                          c.explain(f), f.oracle_said,
                                          "left as found (deferred)" if c.grade == DEFERRED
                                          else "detect-only", grade=c.grade)
            continue
        if flap is not None and c.grade == SAFE and c.name not in force:
            if not flap.should_heal(c.name, now):
                results[c.name] = CheckResult(
                    c.name, c.summary, "chronic", c.explain(f), f.oracle_said,
                    f"chronic — {FLAP_THRESHOLD}+ heals in {FLAP_WINDOW // 60}m, not re-healing",
                    grade=c.grade)
                continue
        snap = c.snapshot()
        c.heal(f)
        # collateral: did any previously-clean check just break?
        broke = next((x for x in clean if x.detect() is not None), None)
        if broke is not None:
            c.back_out(snap)
            results[c.name] = CheckResult(c.name, c.summary, "reported",
                                          c.explain(f), f.oracle_said,
                                          f"rolled back — heal broke {broke.name}", grade=c.grade)
            aborted = True
            continue
        if c.verify():
            results[c.name] = CheckResult(c.name, c.summary, "healed",
                                          c.explain(f), f.oracle_said, "healed", grade=c.grade)
            clean.append(c)
            if flap is not None:
                flap.record_heal(c.name, now)
        else:
            c.back_out(snap)
            results[c.name] = CheckResult(c.name, c.summary, "reported",
                                          c.explain(f), f.oracle_said,
                                          "heal did not resolve — rolled back", grade=c.grade)
    return [results[n] for n in order]


def result_color(r):
    if r.state == "clean":
        return GREEN
    if r.state == "healed":
        return AMBER
    if r.state == "chronic":
        return RED
    return AMBER if r.grade == DEFERRED else RED   # reported


def overall_color(results):
    rank = {GREEN: 0, AMBER: 1, RED: 2}
    worst = GREEN
    for r in results:
        c = result_color(r)
        if rank[c] > rank[worst]:
            worst = c
    return worst


FLAP_THRESHOLD = 3
FLAP_WINDOW = 1800   # seconds


def _boot_id():
    try:
        return open(os.path.join(ROOT, "proc/sys/kernel/random/boot_id")).read().strip()
    except OSError:
        return ""


class FlapState:
    PATH = "var/lib/schema-init/doctor-state"

    def __init__(self, data):
        self.data = data

    @classmethod
    def load(cls):
        try:
            data = json.load(open(os.path.join(ROOT, cls.PATH)))
            if not isinstance(data, dict):
                data = {}
        except (OSError, ValueError):
            data = {}
        data.setdefault("version", 1)
        data.setdefault("last_overall", GREEN)
        data.setdefault("last_run", 0)
        data.setdefault("checks", {})
        if not isinstance(data["checks"], dict):
            data["checks"] = {}
        bid = _boot_id()
        if data.get("boot_id") != bid:            # fresh boot -> fresh flap history
            data["boot_id"] = bid
            try:
                for c in data["checks"].values():
                    c["heals"] = []
                    c["chronic"] = False
            except (AttributeError, TypeError):
                data["checks"] = {}
        return cls(data)

    def save(self, now=None):
        self.data["last_run"] = int(now if now is not None else time.time())
        p = os.path.join(ROOT, self.PATH)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p + ".tmp", "w") as fh:
            json.dump(self.data, fh)
        os.replace(p + ".tmp", p)
        os.chmod(p, 0o644)

    def _c(self, name):
        return self.data["checks"].setdefault(
            name, {"heals": [], "chronic": False, "last_state": GREEN})

    def should_heal(self, name, now):
        c = self._c(name)
        c["heals"] = [t for t in c["heals"] if now - t < FLAP_WINDOW]
        if len(c["heals"]) >= FLAP_THRESHOLD:
            c["chronic"] = True
            return False
        c["chronic"] = False
        return True

    def record_heal(self, name, now):
        self._c(name)["heals"].append(int(now))

    def recovered(self, name):
        c = self._c(name)
        c["heals"] = []
        c["chronic"] = False

    def is_chronic(self, name):
        return self._c(name).get("chronic", False)


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


DEDUP_GUARD_REL = ".config/plasma-workspace/env/zzzz-dedup-xdg-data-dirs.sh"
DEDUP_GUARD = """#!/bin/sh
# schema-doctor: collapse duplicate XDG_DATA_DIRS entries (snapd double-add).
# Named zzzz-* so it sources LAST, after the env.d replay re-adds the double.
if [ -n "$XDG_DATA_DIRS" ]; then
    _out=""
    _oldifs=$IFS
    IFS=:
    for _d in $XDG_DATA_DIRS; do
        case ":$_out:" in
            *":$_d:"*) ;;
            *) _out="${_out:+$_out:}$_d" ;;
        esac
    done
    IFS=$_oldifs
    export XDG_DATA_DIRS="$_out"
fi
"""


class XdgDataDirsDup(Check):
    name = "xdg-data-dirs-dup"
    summary = "XDG_DATA_DIRS carries no duplicate entries (the cause under the ksycoca stutter)"
    grade = SAFE

    def _plasma(self):
        return next((p for p in _proc_table() if "plasmashell" in p["cmd"]), None)

    def _guard(self, home):
        return os.path.join(home, DEDUP_GUARD_REL)

    def detect(self):
        p = self._plasma()
        if not p:
            return None
        home = p["env"].get("HOME")
        parts = [d for d in p["env"].get("XDG_DATA_DIRS", "").split(":") if d]
        if len(parts) == len(set(parts)):
            return None
        if home and os.path.exists(self._guard(home)):
            return None
        dupes = sorted({d for d in parts if parts.count(d) > 1})
        return Finding(
            detail=f"plasmashell's XDG_DATA_DIRS repeats {', '.join(dupes)} — snapd is "
                   "added twice, so session apps keep the doubled value while deduped "
                   "ones do not; that fingerprint split drives the ksycoca rebuild loop. "
                   "A persistent dedup guard fixes it from the next login",
            oracle_said="systemd --user sources environment.d once, without doubling",
            healable=bool(home))

    def snapshot(self):
        p = self._plasma()
        home = p["env"].get("HOME") if p else None
        g = self._guard(home) if home else None
        return {"guard": g, "existed": bool(g and os.path.exists(g))}

    def heal(self, f):
        p = self._plasma()
        home = p["env"].get("HOME") if p else None
        if not home:
            return
        g = self._guard(home)
        os.makedirs(os.path.dirname(g), exist_ok=True)
        with open(g, "w") as fh:
            fh.write(DEDUP_GUARD)
        os.chmod(g, 0o755)

    def verify(self):
        p = self._plasma()
        home = p["env"].get("HOME") if p else None
        return bool(home and os.path.exists(self._guard(home)))

    def back_out(self, snap):
        if snap and snap.get("guard") and not snap.get("existed"):
            try:
                os.remove(snap["guard"])
            except OSError:
                pass


REGISTRY.append(XdgDataDirsDup())


NVIDIA_EGL_DIR = "usr/share/egl/egl_external_platform.d"
NVIDIA_WAYLAND_JSON = NVIDIA_EGL_DIR + "/10_nvidia_wayland.json"
NVIDIA_GBM_JSON = NVIDIA_EGL_DIR + "/15_nvidia_gbm.json"
NVIDIA_WAYLAND_ICD = {
    "file_format_version": "1.0.0",
    "ICD": {"library_path": "libnvidia-egl-wayland.so.1"},
}


class NvidiaWaylandEgl(Check):
    name = "nvidia-wayland-egl"
    summary = "NVIDIA's Wayland EGL platform is registered (no llvmpipe CPU burn)"
    grade = SAFE

    def _p(self, rel):
        return os.path.join(ROOT, rel)

    def detect(self):
        if not os.path.exists(self._p("dev/nvidia0")):
            return None                              # no NVIDIA → irrelevant
        if not os.path.exists(self._p(NVIDIA_GBM_JSON)):
            return None                              # egl-wayland not installed → not this wound
        if os.path.exists(self._p(NVIDIA_WAYLAND_JSON)):
            return None
        return Finding(
            detail="the packaged egl_external_platform.d/10_nvidia_wayland.json is "
                   "missing while its 15_nvidia_gbm.json sibling is present — EGL has no "
                   "NVIDIA provider for Wayland, so GL clients fall back to the llvmpipe "
                   "software rasteriser and burn CPU. Restoring the registration puts new "
                   "windows back on the GPU",
            oracle_said="egl-wayland ships this file; rpm -V lists it as missing",
            healable=True)

    def snapshot(self):
        return {"existed": os.path.exists(self._p(NVIDIA_WAYLAND_JSON))}

    def heal(self, f):
        p = self._p(NVIDIA_WAYLAND_JSON)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as fh:
            json.dump(NVIDIA_WAYLAND_ICD, fh, indent=4)
            fh.write("\n")
        os.chmod(p, 0o644)

    def verify(self):
        return os.path.exists(self._p(NVIDIA_WAYLAND_JSON))

    def back_out(self, snap):
        if snap and not snap.get("existed"):
            try:
                os.remove(self._p(NVIDIA_WAYLAND_JSON))
            except OSError:
                pass


REGISTRY.append(NvidiaWaylandEgl())


def read_config():
    heal = True
    disabled = set()
    notify = True
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
                elif k == "notify" and v.lower() in ("no", "off", "0", "false"):
                    notify = False
    except FileNotFoundError:
        pass
    return heal, disabled, notify


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


def render_status(results, mode):
    ov = overall_color(results)
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    lines = [f"schema-doctor: {ov}   {ts}   mode={mode}"]
    for r in results:
        tail = r.action or r.detail or ""
        lines.append(f"  {result_color(r):<6} {r.name:<22} {tail}")
    return "\n".join(lines) + "\n"


def status_json(results, mode):
    return json.dumps({
        "overall": overall_color(results),
        "mode": mode,
        "ts": int(time.time()),
        "checks": [{"name": r.name, "color": result_color(r), "state": r.state,
                    "detail": r.detail, "action": r.action} for r in results],
    }, indent=2)


def write_status(text):
    d = os.path.join(ROOT, "run/schema-init")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "doctor-status")
    with open(p, "w") as fh:
        fh.write(text)
    os.chmod(p, 0o644)
    return p


def read_status():
    try:
        return open(os.path.join(ROOT, "run/schema-init/doctor-status")).read()
    except OSError:
        return "schema-doctor: no status yet (run --heal or wait for the periodic timer)\n"


def write_status_json(text):
    d = os.path.join(ROOT, "run/schema-init")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "doctor-status.json")
    with open(p, "w") as fh:
        fh.write(text)
    os.chmod(p, 0o644)
    return p


def read_status_json():
    try:
        return open(os.path.join(ROOT, "run/schema-init/doctor-status.json")).read()
    except OSError:
        return json.dumps({"overall": "UNKNOWN", "mode": "", "ts": 0, "checks": []})


def active_session_env():
    uid = active_uid()
    for p in _proc_table():
        if p["env"].get("DBUS_SESSION_BUS_ADDRESS"):
            return uid, p["env"]
    return uid, None


def _notify_argv(uid, summary, body):
    return ["setpriv", "--reuid", str(uid), "--regid", str(uid), "--clear-groups",
            "notify-send", "-a", "schema-doctor", "--", summary, body]


def notify_send(uid, env, summary, body):
    if uid is None or not env or not env.get("DBUS_SESSION_BUS_ADDRESS"):
        return
    child = {"DBUS_SESSION_BUS_ADDRESS": env["DBUS_SESSION_BUS_ADDRESS"],
             "DISPLAY": env.get("DISPLAY", ""),
             "WAYLAND_DISPLAY": env.get("WAYLAND_DISPLAY", ""),
             "XDG_RUNTIME_DIR": env.get("XDG_RUNTIME_DIR", f"/run/user/{uid}"),
             "PATH": "/usr/bin:/bin"}
    try:
        subprocess.run(_notify_argv(uid, summary, body), env=child,
                       timeout=5, check=False)
    except (OSError, subprocess.SubprocessError):
        pass


def notify_transitions(results, flap, enabled):
    events = []
    for r in results:
        col = result_color(r)
        prev = flap._c(r.name).get("last_state", GREEN)
        if col == RED and prev != RED:
            events.append((f"schema-doctor: {r.name}",
                           r.action or r.detail or "needs attention"))
        flap._c(r.name)["last_state"] = col          # bookkeeping, always
    new_overall = overall_color(results)
    if new_overall == GREEN and flap.data.get("last_overall", GREEN) != GREEN:
        events.append(("schema-doctor: all clear", "all seams healthy again"))
    flap.data["last_overall"] = new_overall
    if enabled and events:
        uid, env = active_session_env()
        for summ, body in events:
            notify_send(uid, env, summ, body)


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


def _safe_detect_all(checks, heal, force, flap=None):
    try:
        return run_checks(checks, heal, force, flap=flap)
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
    ap.add_argument("--periodic", action="store_true", help="flap-aware run + notify (timer)")
    ap.add_argument("--status", action="store_true", help="print the last health status")
    args = ap.parse_args(argv)

    cfg_heal, disabled, cfg_notify = read_config()
    checks = [c for c in REGISTRY if c.name not in disabled]

    if args.status:
        print(read_status_json() if args.json else read_status())
        return 0

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
    flap = FlapState.load() if args.periodic else None
    results = _safe_detect_all(checks, heal, set(args.force), flap)

    mode = "periodic" if args.periodic else ("boot" if args.wait else "manual")
    try:
        write_status(render_status(results, mode))
    except Exception:
        pass
    try:
        write_status_json(status_json(results, mode))
    except Exception:
        pass
    if flap is not None:
        try:
            notify_transitions(results, flap, cfg_notify)
            flap.save()
        except Exception:
            pass

    out = render_json(results) if args.json else render_report(results)
    write_report(render_report(results))
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
