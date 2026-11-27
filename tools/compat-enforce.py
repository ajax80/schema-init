#!/usr/bin/env python3
# Compat corpus stage 3: is what the importer mapped actually IN FORCE?
#
# Each imported unit's daemon is replaced by tests/qemu-ci/enforce-probe, which
# reports the execution context it got (uid, groups, caps, NoNewPrivs, seccomp,
# umask, cwd, rlimits, nice, oom, env, namespaces, read-only mounts, and the
# outcome of actually trying a W+X mmap, a personality switch, SCHED_FIFO, a
# setuid chmod and unshare). All of them run at once in one QEMU guest with the
# real schema-init as PID 1. Then each systemd directive the unit carried is
# judged against what the probe saw:
#
#   enforced      the process has what the unit asked for
#   NOT-ENFORCED  it doesn't (for a warned directive the warning already said so)
#   unobservable  the probe can't tell here (a non-root process is refused
#                 SCHED_FIFO or unshare whatever the unit asked)
#   unchecked     no oracle for this directive yet
#   not-run       the service never reported
#
# Lifecycle keys (exec, deps, ordering, conditions, readiness, restart) are
# stripped from the .svc: they're the QEMU pid1-boot job's, and a missing real
# daemon or dependency would keep the probe from running at all.
#
#   tools/compat-enforce.py [--foreign [--security S]] [--json OUT] DIR [DIR...]
#
# A DIR may hold a rootfs/ overlay copied into the guest (EnvironmentFile=s).
import argparse, base64, collections, glob, importlib.util, io, json, os, re, shlex, shutil, subprocess
import sys, tarfile, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_spec = importlib.util.spec_from_file_location("compat_corpus", os.path.join(REPO, "tools/compat-corpus.py"))
cc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cc)
si = cc.si

# lifecycle, not execution context: see the header
DROP = {"exec", "args", "exec_pre", "exec_reload", "exec_stop", "pid_file", "notify", "main_pid_notify",
        "ready_bus_name", "ready_path", "condition", "dep", "after", "before", "bind", "critical",
        "restart", "restart_sec", "no_restart", "max_restarts", "watch_path", "watchdog_sec", "stable_secs",
        "oneshot", "expand_args", "listen", "listen_lazy", "socket_mode", "socket_user", "socket_group",
        "trigger_limit_burst", "trigger_limit_interval_sec", "stop_timeout_sec", "start_timeout_sec",
        "reload_signal", "kill_mode"}

CAPS = ("chown dac_override dac_read_search fowner fsetid kill setgid setuid setpcap linux_immutable "
        "net_bind_service net_broadcast net_admin net_raw ipc_lock ipc_owner sys_module sys_rawio sys_chroot "
        "sys_ptrace sys_pacct sys_admin sys_boot sys_nice sys_resource sys_time sys_tty_config mknod lease "
        "audit_write audit_control setfcap mac_override mac_admin syslog wake_alarm block_suspend audit_read "
        "perfmon bpf checkpoint_restore").split()
CAP = {"CAP_" + n.upper(): i for i, n in enumerate(CAPS)}
LIMITS = {"LimitNOFILE": "nofile", "LimitNPROC": "nproc", "LimitCORE": "core", "LimitMEMLOCK": "memlock"}
DIRS = {"StateDirectory": "/var/lib", "RuntimeDirectory": "/run", "CacheDirectory": "/var/cache",
        "LogsDirectory": "/var/log", "ConfigurationDirectory": "/etc"}
CONTROL = "enforce-control"
# what ExecStart=+ switches off (systemd.exec: "the process is executed with full privileges")
SANDBOX = {"NoNewPrivileges", "PrivateTmp", "ProtectHome", "ProtectSystem", "ProtectKernelTunables",
           "ProtectControlGroups", "ProtectKernelModules", "PrivateDevices", "PrivateNetwork", "ProtectHostname",
           "MemoryDenyWriteExecute", "LockPersonality", "RestrictRealtime", "RestrictSUIDSGID",
           "RestrictNamespaces", "SystemCallFilter", "CapabilityBoundingSet", "ReadWritePaths", "ReadOnlyPaths"}
BATCH = 480


def last(sections, sec, key):
    v = None
    for k, val in sections.get(sec, []):
        if str.__eq__(k, key):
            v = val
    return v


def all_of(sections, sec, key):
    out = []
    for k, val in sections.get(sec, []):
        if str.__eq__(k, key):
            out = [] if val == "" else out + [val]
    return out


def truthy(v):
    return v is not None and v.lower() in ("1", "yes", "true", "on")


# ---- import ----------------------------------------------------------------

def translate(path, known):
    name = os.path.basename(path)[:-len(".service")]
    sections = cc.tracked(cc.unit_text(path))
    try:
        if si._denied(name):
            return name, sections, None, None
        body = si.unit_to_svc(name, sections, sock=cc.socket_for(path, sections), known=known)
    except si.Skip:
        return name, sections, None, None
    except Exception as e:
        return name, sections, None, "%s: %s" % (type(e).__name__, e)
    warned = cc.warned_of([ln[len(si.WARN_PREFIX):] for ln in body.splitlines() if ln.startswith(si.WARN_PREFIX)])
    return name, sections, (body, warned), None


def paths_to_check(sections):
    out = []
    wd = last(sections, "Service", "WorkingDirectory")
    if wd and wd.lstrip("-") not in ("~", "") and wd.lstrip("-").startswith("/"):
        out.append(wd.lstrip("-"))
    for d, base in DIRS.items():
        for v in all_of(sections, "Service", d):
            out += [os.path.join(base, p.split(":")[0]) for p in v.split() if "%" not in p]
    for k in ("ReadWritePaths", "ReadOnlyPaths"):
        for v in all_of(sections, "Service", k):
            out += [p.lstrip("-+") for p in v.split() if p.lstrip("-+").startswith("/") and "%" not in p]
    return list(dict.fromkeys(out))


def probe_svc(name, body, paths, tamper):
    keep = []
    for ln in body.splitlines():
        if not ln or ln.startswith("#") or "=" not in ln:
            continue
        k = ln.split("=", 1)[0]
        # the importer's own *Directory= creation is an install -d exec_pre: kept
        if (k in DROP and not (k == "exec_pre" and "/usr/bin/install -d" in ln)) or (tamper and ln in tamper):
            continue
        keep.append(ln)
    keep += ["exec=/bin/enforce-probe", "args=" + name] + ["args=" + p for p in paths]
    keep += ["oneshot=1", "after=enforce-setup"]
    return "\n".join(keep) + "\n"


# ---- guest -----------------------------------------------------------------

def users_of(units):
    users, groups = {}, {}
    for _, sections, _ in units:
        u = last(sections, "Service", "User")
        g = last(sections, "Service", "Group")
        if u and not u.isdigit() and "%" not in u and u != "root":
            users.setdefault(u, 2000 + len(users))
        for gg in [g] + " ".join(all_of(sections, "Service", "SupplementaryGroups")).split():
            if gg and not gg.isdigit() and "%" not in gg and gg != "root":
                groups.setdefault(gg, 3000 + len(groups))
    return users, groups


def build_guest(work, units, tamper, overlays, busybox, users, groups):
    root = os.path.join(work, "root")
    for d in ("bin", "sbin", "usr/bin", "etc/schema-init/services", "proc", "sys", "dev", "run", "tmp",
              "var/tmp", "var/lib", "var/cache", "var/log", "boot", "home/enforce", "root"):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    for ov in overlays:
        shutil.copytree(ov, root, dirs_exist_ok=True, symlinks=True)
    os.chmod(os.path.join(root, "tmp"), 0o1777)
    for m in ("tmp/.enforce-marker", "home/enforce/.marker", "root/.marker"):
        open(os.path.join(root, m), "w").close()
    shutil.copy(os.path.join(REPO, "schema-init"), os.path.join(root, "sbin/schema-init"))
    shutil.copy(busybox, os.path.join(root, "bin/busybox"))
    for a in ("sh", "mkdir", "chmod", "ls", "sleep", "tar", "gzip", "base64", "poweroff", "cat", "stty", "test"):
        os.symlink("busybox", os.path.join(root, "bin", a))
    os.symlink("/bin/busybox", os.path.join(root, "usr/bin/install"))
    subprocess.run(["cc", "-static", "-O2", "-std=c11", "-D_GNU_SOURCE", "-o", os.path.join(root, "bin/enforce-probe"),
                    os.path.join(REPO, "tests/qemu-ci/enforce-probe.c")], check=True)

    with open(os.path.join(root, "etc/passwd"), "w") as f:
        f.write("root:x:0:0::/root:/bin/sh\n")
        f.writelines("%s:x:%d:%d::/:/bin/sh\n" % (u, uid, groups[u]) for u, uid in users.items())
    with open(os.path.join(root, "etc/group"), "w") as f:
        f.write("root:x:0:\n")
        f.writelines("%s:x:%d:\n" % (g, gid) for g, gid in groups.items())

    svcdir = os.path.join(root, "etc/schema-init/services")
    names = []
    for name, sections, (body, _) in units:
        paths = paths_to_check(sections)
        # what a unit only names must exist, as on the real distro; *Directory=
        # paths are PID 1's to create, so they're left for it
        made = {os.path.join(b, p.split(":")[0]) for d, b in DIRS.items()
                for v in all_of(sections, "Service", d) for p in v.split()}
        for p in paths:
            if p not in made and not p.startswith(("/run", "/proc", "/sys", "/dev")):
                os.makedirs(os.path.join(root, p.lstrip("/")), exist_ok=True)
        # a required EnvironmentFile= ships with its package; empty stands in for it
        for v in all_of(sections, "Service", "EnvironmentFile"):
            ef = os.path.join(root, v.lstrip("/"))
            if not v.startswith("-") and "%" not in v and not os.path.exists(ef):
                os.makedirs(os.path.dirname(ef), exist_ok=True)
                open(ef, "w").close()
        with open(os.path.join(svcdir, name + ".svc"), "w") as f:
            f.write(probe_svc(name, body, paths, tamper.get(name)))
        names.append(name)
    for name, extra in (("enforce-setup", "oneshot=1\nexec=/bin/sh\nargs=-c\nargs=mkdir -p -m 1777 /run/probe\n"),
                        (CONTROL, "oneshot=1\nexec=/bin/enforce-probe\nargs=%s\nafter=enforce-setup\n" % CONTROL)):
        with open(os.path.join(svcdir, name + ".svc"), "w") as f:
            f.write("name=%s\n%sno_new_privs=0\nprivate_tmp=0\nprotect_system=0\nprotect_home=0\nsyscall_deny=0\n"
                    % (name, extra))
    names.append(CONTROL)
    # results leave on the second serial port, base64: the console carries PID 1's
    # and the kernel's lines, which would split a payload mid-line
    with open(os.path.join(root, "bin/enforce-collect"), "w") as f:
        # done when all reported, or the count held still for 15 s after the first
        # report (a slow TCG boot can take longer than that to get any): a service that
        # can't start (its namespace fails) never will. PID 1's per-service logs
        # ride along, so a not-run unit says why
        f.write("#!/bin/sh\nn=0 still=0 last=-1\nwhile [ $n -lt 240 ] && [ $still -lt 15 ]; do\n"
                "  c=$(ls /run/probe | wc -l)\n  [ $c -ge %d ] && break\n"
                "  [ $c -gt 0 ] && [ $c = $last ] && still=$((still+1)) || still=0\n  last=$c; sleep 1; n=$((n+1))\ndone\n"
                "stty -F /dev/ttyS1 raw 2>/dev/null\n"
                "{ echo ENFORCE-BEGIN; cd / && tar c run/probe $(ls -d var/log/schema-init run/log/schema-init "
                "2>/dev/null) | gzip -1 | base64; echo ENFORCE-END; } > /dev/ttyS1\n"
                "poweroff -f\n" % len(names))
    os.chmod(os.path.join(root, "bin/enforce-collect"), 0o755)
    os.symlink("busybox", os.path.join(root, "bin/wc"))
    with open(os.path.join(svcdir, "enforce-collect.svc"), "w") as f:
        f.write("name=enforce-collect\nexec=/bin/enforce-collect\noneshot=1\nafter=enforce-setup\nstart_timeout_sec=600\n"
                "no_new_privs=0\nprivate_tmp=0\nprotect_system=0\nprotect_home=0\nsyscall_deny=0\n")
    for dp, dns, fns in os.walk(svcdir):
        for fn in fns:
            os.chmod(os.path.join(dp, fn), 0o644)

    initrd = os.path.join(work, "initramfs.gz")
    with open(initrd, "wb") as out:
        find = subprocess.Popen(["find", "."], cwd=root, stdout=subprocess.PIPE)
        cpio = subprocess.Popen(["cpio", "-o", "-H", "newc", "-R", "0:0"], cwd=root, stdin=find.stdout,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        subprocess.run(["gzip", "-1"], stdin=cpio.stdout, stdout=out, check=True)
    return initrd


def boot(work, initrd, kernel, timeout):
    accel, cpu = ("kvm", "host") if os.access("/dev/kvm", os.W_OK) else ("tcg", "max")
    data = os.path.join(work, "data.out")
    console = os.path.join(work, "console.log")
    subprocess.run(["timeout", str(timeout), "qemu-system-x86_64", "-accel", accel, "-cpu", cpu, "-m", "1024",
                    "-smp", "2", "-no-reboot", "-display", "none", "-nic", "none",
                    "-serial", "file:" + console, "-serial", "file:" + data,
                    "-kernel", kernel, "-initrd", initrd,
                    "-append", "console=ttyS0 rdinit=/sbin/schema-init panic=-1 loglevel=4"],
                   stdin=subprocess.DEVNULL)
    raw = open(data, "rb").read().decode("ascii", "replace").replace("\r", "")
    m = re.search(r"ENFORCE-BEGIN\n(.*?)ENFORCE-END", raw, re.S)
    if not m:
        sys.exit("compat-enforce: no results from the guest; console log:\n" + open(console, errors="replace").read()[-4000:])
    reports, logs = {}, {}
    with tarfile.open(fileobj=io.BytesIO(base64.b64decode(m.group(1)))) as t:
        for mem in t:
            if mem.isfile() and "/log/schema-init/" in mem.name and mem.name.endswith(".log"):
                logs[os.path.basename(mem.name)[:-4]] = t.extractfile(mem).read().decode(errors="replace")
            elif mem.isfile() and mem.name.startswith("run/probe/") and not os.path.basename(mem.name).startswith("."):
                rep = collections.defaultdict(list)
                for ln in t.extractfile(mem).read().decode(errors="replace").splitlines():
                    k, _, v = ln.partition("=")
                    rep[k].append(v)
                reports[os.path.basename(mem.name)] = rep
    return reports, logs


# ---- oracle ----------------------------------------------------------------

def caps_mask(values):
    # systemd: lines accumulate; "~" inverts a line; an empty line resets
    mask = None
    for v in values:
        if v == "":
            mask = None
            continue
        inv = v.startswith("~")
        names = v.lstrip("~").split()
        bits = sum(1 << CAP[n] for n in names if n in CAP)
        if inv:
            mask = (mask if mask is not None else (1 << len(CAPS)) - 1) & ~bits
        else:
            mask = (mask or 0) | bits
    return mask


def limit(v, nr_open):
    def one(x):
        x = x.strip().lower()
        if x == "infinity":
            return nr_open if nr_open is not None else -1
        m = re.fullmatch(r"(\d+)([kmgtpe]?)", x)
        return int(m.group(1)) * 1024 ** " kmgtpe".index(m.group(2) or " ") if m else None
    soft, _, hard = v.partition(":")
    return one(soft), one(hard or soft)


# {(section, key): (verdict, detail)} for each directive with an oracle
def judge(sections, rep, ctl, users, groups, overlays):
    one = lambda k: rep.get(k, ["?"])[0].strip()
    out = {}
    uid = int(one("uid")) if one("uid").lstrip("-").isdigit() else None
    root = uid == 0
    sv = lambda k: last(sections, "Service", k)

    def put(key, ok, want, got, observable=True):
        out[("Service", key)] = ("unobservable" if not observable else "enforced" if ok else "NOT-ENFORCED",
                                 "want %s got %s" % (want, got))

    ids = lambda n, table: 0 if n in (None, "root") else int(n) if n.isdigit() else table.get(n)
    # ExecStart=! or + : systemd doesn't switch to User=/Group= for the command
    starts = all_of(sections, "Service", "ExecStart")
    # the importer's rule: !! is only the no-ambient-caps fallback, so it runs as User=
    pfx = re.match(r"[@\-:+!]*", starts[0]).group(0) if starts else ""
    elevated = "+" in pfx or ("!" in pfx and "!!" not in pfx)
    # + : systemd applies no sandboxing at all, so none is expected
    unsandboxed = "+" in pfx
    u, g = sv("User"), sv("Group")
    if u:
        want = 0 if elevated else ids(u, users)
        put("User", uid == want, want, uid)
    if g:
        want = 0 if elevated else ids(g, groups)
        put("Group", one("gid") == str(want), want, one("gid"))
    du = sv("DynamicUser")
    if du is not None and not elevated:
        if truthy(du):   # an allocated uid, or the static User= when it names one
            put("DynamicUser", uid not in (None, 0) and (not u or u == "root" or uid == ids(u, users)),
                "non-root uid", uid)
        else:
            put("DynamicUser", uid == ids(u, users), ids(u, users), uid)
    sg = " ".join(all_of(sections, "Service", "SupplementaryGroups")).split()
    if sg and not elevated:
        want = {str(int(x) if x.isdigit() else groups.get(x)) for x in sg}
        got = set(one("groups").split(","))
        put("SupplementaryGroups", want <= got, sorted(want), sorted(got))
    if sv("NoNewPrivileges") is not None:
        put("NoNewPrivileges", one("nnp") == ("1" if truthy(sv("NoNewPrivileges")) else "0"),
            int(truthy(sv("NoNewPrivileges"))), one("nnp"))
    if sv("UMask"):
        put("UMask", int(one("umask"), 8) == int(sv("UMask"), 8), sv("UMask"), one("umask"))
    wd = sv("WorkingDirectory")
    if wd and wd.lstrip("-").startswith("/"):
        put("WorkingDirectory", one("cwd") == wd.lstrip("-").rstrip("/") or one("cwd") == wd.lstrip("-"),
            wd.lstrip("-"), one("cwd"))
    env = dict(e.split("=", 1) for e in rep.get("env", []) if "=" in e)
    # systemd's order: Environment= lines, then each EnvironmentFile= over them,
    # a later file over an earlier one. Each key is judged under the directive
    # whose value wins
    want_env = {}
    for v in all_of(sections, "Service", "Environment"):
        try:
            words = shlex.split(v)
        except ValueError:
            continue
        want_env.update((w.split("=", 1)[0], (w.split("=", 1)[1], "Environment"))
                        for w in words if "=" in w and "%" not in w and "$" not in w)
    for v in all_of(sections, "Service", "EnvironmentFile"):
        p = v.lstrip("-")
        src = next((os.path.join(o, p.lstrip("/")) for o in overlays if os.path.exists(os.path.join(o, p.lstrip("/")))), None)
        if not src:
            continue   # absent in the guest as on most hosts: nothing to load
        for ln in open(src).read().splitlines():
            if "=" in ln and not ln.lstrip().startswith("#"):
                k, val = ln.split("=", 1)
                want_env[k.strip()] = (val.strip().strip('"\''), "EnvironmentFile")
    for d in ("Environment", "EnvironmentFile"):
        mine = {k: v for k, (v, src) in want_env.items() if src == d}
        if mine:
            bad = {k: (v, env.get(k)) for k, v in mine.items() if env.get(k) != v}
            put(d, not bad, "all set", bad or "all set")
    for d, k in LIMITS.items():
        if sv(d):
            want = limit(sv(d), int(one("nr_open")) if d == "LimitNOFILE" else None)
            got = tuple(int(x) for x in one(k).split(":"))
            put(d, want == got, "%s:%s" % want, "%s:%s" % got)
    if sv("Nice"):
        put("Nice", one("nice") == sv("Nice").strip(), sv("Nice"), one("nice"))
    if sv("OOMScoreAdjust"):
        put("OOMScoreAdjust", one("oom") == sv("OOMScoreAdjust").strip(), sv("OOMScoreAdjust"), one("oom"))
    if truthy(sv("PrivateTmp")):
        put("PrivateTmp", one("tmp_marker") == "0", "host /tmp hidden", "visible" if one("tmp_marker") == "1" else "hidden")
    ph = (sv("ProtectHome") or "").lower()
    if ph in ("1", "yes", "true", "on", "read-only", "tmpfs"):
        hidden = one("home_marker") == "0" and one("root_marker") == "0"
        put("ProtectHome", hidden, "/home and /root hidden", "hidden" if hidden else "visible")
    ps = (sv("ProtectSystem") or "").lower()
    if ps in ("1", "yes", "true", "on", "full", "strict"):
        want = ["ro_usr", "ro_boot"] + (["ro_etc"] if ps in ("full", "strict") else [])
        got = {k: one(k) for k in want}
        put("ProtectSystem", all(v == "1" for v in got.values()), "read-only " + " ".join(want), got)
    for d, k in (("ProtectKernelTunables", "ro_procsys"), ("ProtectControlGroups", "ro_cgroup")):
        if truthy(sv(d)) or (d == "ProtectControlGroups" and (sv(d) or "").lower() == "strict"):
            put(d, one(k) == "1", "read-only", one(k))
    if truthy(sv("ProtectKernelModules")):
        bnd = int(one("capbnd"), 16)
        put("ProtectKernelModules", not bnd & (1 << CAP["CAP_SYS_MODULE"]), "no CAP_SYS_MODULE", hex(bnd))
    if truthy(sv("PrivateDevices")):
        put("PrivateDevices", one("dev_mem") == "0", "/dev/mem hidden", "hidden" if one("dev_mem") == "0" else "visible",
            ctl["dev_mem"][0] == "1")
    for d, k in (("PrivateNetwork", "ns_net"), ("ProtectHostname", "ns_uts")):
        if truthy(sv(d)) or (d == "ProtectHostname" and (sv(d) or "").lower().startswith("private")):
            put(d, one(k) != ctl[k][0], "own namespace", "own" if one(k) != ctl[k][0] else "host's")
    # behavioural: judged only where the control (root, no hardening) succeeds,
    # and as root, where a refusal can only be the hardening's
    for d, k in (("MemoryDenyWriteExecute", "mdwe"), ("LockPersonality", "lockpers"),
                 ("RestrictRealtime", "rt_refused"), ("RestrictSUIDSGID", "suid_refused")):
        if truthy(sv(d)):
            observable = ctl[k][0] == "0" and (root or d in ("MemoryDenyWriteExecute", "LockPersonality",
                                                              "RestrictSUIDSGID"))
            put(d, one(k) == "1", "refused", "refused" if one(k) == "1" else "allowed", observable)
    rn = sv("RestrictNamespaces")
    if rn is not None and rn.strip():
        v = rn.strip().lower()
        denies_uts = truthy(v) or (v.startswith("~") and "uts" in v.split()) or \
            (not v.startswith("~") and v not in ("no", "false", "0", "off") and "uts" not in v.split())
        if denies_uts:
            put("RestrictNamespaces", one("unshare_refused") == "1", "unshare(UTS) refused",
                "refused" if one("unshare_refused") == "1" else "allowed", root and ctl["unshare_refused"][0] == "0")
    scf = all_of(sections, "Service", "SystemCallFilter")
    if scf:
        # judged on swapoff (@swap): denied by an allow-list without it, or a
        # deny-list naming it. Seccomp: 2 alone would also come from
        # LockPersonality= & co, which the engine builds as seccomp too
        allow = [v for v in scf if not v.startswith("~")]
        names = set(" ".join(v.lstrip("~") for v in scf).split())
        denied = (allow and not names & {"@swap", "swapoff"}) or \
            any(v.startswith("~") and set(v[1:].split()) & {"@swap", "swapoff"} for v in scf)
        blocked = one("swapoff") != ctl["swapoff"][0]
        put("SystemCallFilter", blocked, "swapoff filtered", "%s (control %s)" % (one("swapoff"), ctl["swapoff"][0]),
            bool(denied))
    cb = all_of(sections, "Service", "CapabilityBoundingSet")
    if cb:
        want = caps_mask(cb)
        if want is not None:
            got = int(one("capbnd"), 16)
            put("CapabilityBoundingSet", got & ~want == 0, hex(want), hex(got))
    amb = " ".join(all_of(sections, "Service", "AmbientCapabilities")).split()
    if amb and not any(a.startswith("~") for a in amb):
        want = sum(1 << CAP[a] for a in amb if a in CAP)
        # root holds them effective after exec anyway; ambient only carries them for a non-root uid
        got = int(one("capeff" if root else "capamb"), 16)
        put("AmbientCapabilities", got & want == want, hex(want), hex(got))
    paths = {ln.split(" ")[0]: ln.split(" ")[1:] for ln in rep.get("path", [])}
    for d, base in DIRS.items():
        for v in all_of(sections, "Service", d):
            for p in v.split():
                if "%" in p:
                    continue
                full = os.path.join(base, p.split(":")[0])
                got = paths.get(full, ["missing"])
                # systemd hands each to User=/Group= (whatever ExecStart= runs as), except
                # a config dir, which stays root's; with no Group=, User='s own group
                mine = d != "ConfigurationDirectory" and not truthy(sv("DynamicUser"))
                # a guest user's primary group shares its name (build_guest); a numeric
                # User= has no passwd entry there, so only its uid is judged
                gid = ids(g, groups) if g else 0 if u in (None, "root") else None if u.isdigit() else groups.get(u)
                owner = "%s %s" % (ids(u, users), gid) if mine else "0 0"
                ok = got[0] != "missing" and (truthy(sv("DynamicUser")) or " ".join(got[:2]) == owner
                                              or (gid is None and got[0] == owner.split()[0]))
                put(d, ok, "%s owned by %s" % (full, owner), " ".join(got))
    for k, want_ro in (("ReadWritePaths", "0"), ("ReadOnlyPaths", "1")):
        for v in all_of(sections, "Service", k):
            for p in v.split():
                q = p.lstrip("-+")
                if not q.startswith("/") or "%" in q:
                    continue
                got = paths.get(q, ["missing"])
                if got[0] == "missing" and p.startswith("-"):
                    continue
                put(k, got[0] != "missing" and got[-1] == want_ro, "%s ro=%s" % (q, want_ro), " ".join(got))
    if unsandboxed:
        out = {k: v for k, v in out.items() if k[1] not in SANDBOX}
    return out


def main():
    ap = argparse.ArgumentParser(description="Is what schema-import mapped in force? Probes every imported unit "
                                 "under the real PID 1 in a QEMU guest.")
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--json")
    ap.add_argument("--label", default="enforce")
    ap.add_argument("--foreign", action="store_true")
    ap.add_argument("--security")
    ap.add_argument("--skip-systemd", action="store_true")
    ap.add_argument("--kernel", default=os.environ.get("KERNEL") or "/boot/vmlinuz-" + os.uname().release)
    ap.add_argument("--busybox", default=os.environ.get("BUSYBOX") or shutil.which("busybox"))
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--console", help="keep the guest's console log here")
    ap.add_argument("--tamper", action="append", default=[],
                    help="UNIT:LINE drop LINE from UNIT's .svc (the anti-false-green control)")
    a = ap.parse_args()
    if a.security is not None and not a.foreign:
        ap.error("--security only applies with --foreign")
    if a.foreign:
        cc.foreign(a.security or "audit")
    if not os.access(os.path.join(REPO, "schema-init"), os.X_OK):
        sys.exit("compat-enforce: build ./schema-init first (make schema-init)")
    # the guest has no libc: a dynamic PID 1 only shows as a kernel panic
    if not re.search(r"static(ally|-pie) linked", subprocess.run(["file", "-L", os.path.join(REPO, "schema-init")],
                                                                 capture_output=True, text=True).stdout):
        sys.exit("compat-enforce: ./schema-init is not static; make clean && make schema-init")
    tamper = collections.defaultdict(set)
    for t in a.tamper:
        u, _, ln = t.partition(":")
        tamper[u].add(ln)

    paths = sorted({p for d in a.dirs for p in glob.glob(os.path.join(d, "*.service")) if not os.path.islink(p)})
    if a.skip_systemd:
        paths = [p for p in paths if not os.path.basename(p).startswith("systemd-")]
    overlays = [os.path.join(d, "rootfs") for d in a.dirs if os.path.isdir(os.path.join(d, "rootfs"))]
    known = {os.path.basename(p)[:-len(".service")] for p in paths}
    units = []
    for p in paths:
        name, sections, res, err = translate(p, known)
        if err:
            print("compat-enforce: %s: %s" % (name, err), file=sys.stderr)
        if res:
            units.append((name, sections, res))
    users, groups = users_of(units)
    groups.update({u: uid for u, uid in users.items() if u not in groups})
    # PID 1 loads at most MAX_SERVICES (service.h, 512) .svc files: one boot per
    # batch, each with the three harness services and its own control probe
    # units that claim the same *Directory= (chrony and chronyd-restricted, which
    # Conflicts= keeps apart on a real host) go to different boots: run together,
    # whichever creates the directory last decides its owner for both
    batches = []
    for un in units:
        mine = set(paths_to_check(un[1])) & {os.path.join(b, p.split(":")[0]) for d, b in DIRS.items()
                                             for v in all_of(un[1], "Service", d) for p in v.split()}
        for b in batches:
            if len(b[0]) < BATCH and not (b[1] & mine):
                b[0].append(un)
                b[1].update(mine)
                break
        else:
            batches.append(([un], set(mine)))
    reports, logs, ctl_of = {}, {}, {}
    for i, (batch, _) in enumerate(batches):
        with tempfile.TemporaryDirectory() as work:
            initrd = build_guest(work, batch, tamper, overlays, a.busybox, users, groups)
            r, lg = boot(work, initrd, a.kernel, a.timeout)
            if a.console:
                shutil.copy(os.path.join(work, "console.log"), "%s.%d" % (a.console, i))
        if CONTROL not in r:
            sys.exit("compat-enforce: the control probe never reported (batch %d)" % i)
        for name, _, _ in batch:
            ctl_of[name] = r[CONTROL]
        reports.update(r)
        logs.update(lg)

    recs, per = [], collections.defaultdict(collections.Counter)
    for name, sections, (body, warned) in units:
        rep = reports.get(name)
        verdicts = judge(sections, rep, ctl_of[name], users, groups, overlays) if rep else {}
        rec = {"unit": name + ".service", "ran": rep is not None, "directives": []}
        if rep is None:
            rec["log"] = "\n".join(logs.get(name, "").strip().splitlines()[-5:])
        for sec, pairs in sections.items():
            seen = set()
            for kk, v in pairs:
                k = str(kk)
                # only what the importer consulted: a silent directive was never mapped
                if (sec, k) in seen or cc.inert(sec, k) or not kk.hit:
                    continue
                seen.add((sec, k))
                vd, detail = verdicts.get((sec, k), ("not-run" if rep is None else "unchecked", ""))
                rec["directives"].append({"section": sec, "key": k, "warned": k in warned,
                                          "verdict": vd, "detail": detail})
                per["%s.%s" % (sec, k)][vd] += 1
        recs.append(rec)

    ran = sum(r["ran"] for r in recs)
    print("%s: %d imported, %d probed under PID 1" % (a.label, len(recs), ran))
    notrun = [r["unit"] for r in recs if not r["ran"]]
    if notrun:
        print("\nNEVER REPORTED (%d):" % len(notrun))
        for r in recs:
            if not r["ran"]:
                print("  %s: %s" % (r["unit"], (r["log"].splitlines() or ["(no log)"])[-1][:160]))
    bad = [(r["unit"], d) for r in recs for d in r["directives"] if d["verdict"] == "NOT-ENFORCED"]
    print("\nNOT ENFORCED, mapped without a warning (%d):" % sum(1 for _, d in bad if not d["warned"]))
    for u, d in bad:
        if not d["warned"]:
            print("  %s %s.%s: %s" % (u, d["section"], d["key"], d["detail"]))
    print("\nNOT ENFORCED, warned (said out loud) (%d)" % sum(1 for _, d in bad if d["warned"]))
    print("\nby directive (enforced / NOT / unobservable / unchecked):")
    for k, c in sorted(per.items(), key=lambda kv: -sum(kv[1].values())):
        if c["enforced"] or c["NOT-ENFORCED"] or c["unobservable"]:
            print("  %-40s %4d %4d %4d %4d" % (k, c["enforced"], c["NOT-ENFORCED"], c["unobservable"], c["unchecked"]))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"label": a.label, "units": recs, "directives": {k: dict(c) for k, c in sorted(per.items())}},
                      f, indent=1)
        print("\nfull record: %s" % a.json)


if __name__ == "__main__":
    main()
