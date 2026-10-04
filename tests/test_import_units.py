#!/usr/bin/env python3
"""schema-import unit tests — pure translation + a temp-tree drain. No systemctl."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-import.py")
spec = importlib.util.spec_from_file_location("schema_import", MOD)
si = importlib.util.module_from_spec(spec); spec.loader.exec_module(si)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

# --- parse_unit ---
s = si.parse_unit(
    "[Unit]\nDescription=x\n\n[Service]\n# comment\nType=simple\n"
    "ExecStart=/usr/bin/foo \\\n  --flag a\nEnvironment=A=1\nEnvironment=B=2\n"
    "[Install]\nWantedBy=multi-user.target\n")
check("sections parsed", set(s) == {"Unit", "Service", "Install"})
check("line continuation joined", si._get_last(s["Service"], "ExecStart") == "/usr/bin/foo --flag a")
check("duplicate keys collected", si._get_all(s["Service"], "Environment") == ["A=1", "B=2"])

# --- unit_to_svc: simple daemon, no Restart, no User ---
body = si.unit_to_svc("foo", si.parse_unit(
    "[Service]\nExecStart=/usr/bin/foo -D --x\n[Install]\nWantedBy=multi-user.target\n"))
check("exec extracted", "exec=/usr/bin/foo\n" in body)
check("args split one per line", "args=-D\n" in body and "args=--x\n" in body)
check("no User -> needs_root", "needs_root=1\n" in body)
check("Restart absent -> no_restart", "no_restart=1\n" in body)
check("critical default", "critical=0\n" in body)

# --- Restart / Type / User / Environment ---
b2 = si.unit_to_svc("bar", si.parse_unit(
    "[Service]\nType=oneshot\nRestart=on-failure\nUser=nobody\n"
    "Environment=\"FOO=a b\" BAR=c\nExecStart=/bin/bar\n[Install]\nWantedBy=x\n"))
check("Type=oneshot", "oneshot=1\n" in b2)
check("Restart=on-failure -> restart kept", "no_restart" not in b2)
check("User=nobody -> user=, no needs_root", "user=nobody\n" in b2 and "needs_root" not in b2)
check("quoted env pair", "env=FOO=a b\n" in b2)
check("second env pair", "env=BAR=c\n" in b2)

# --- ExecStart prefix stripping ---
b3 = si.unit_to_svc("p", si.parse_unit("[Service]\nExecStart=@-/bin/p arg\n[Install]\nWantedBy=x\n"))
check("prefix chars stripped", "exec=/bin/p\n" in b3 and "args=arg\n" in b3)

# --- $VAR expansion / dropping ---
b4 = si.unit_to_svc("v", si.parse_unit(
    "[Service]\nEnvironment=OPTS=-x\nExecStart=/bin/v $OPTS $UNSET -z\n[Install]\nWantedBy=x\n"))
check("known $VAR expanded", "args=-x\n" in b4)
check("unresolved $VAR dropped", "$UNSET" not in b4 and "args=-z\n" in b4)
check("drop noted", "dropped 1 unresolved" in b4)

# --- hardening knobs: always all four, explicit ---
def hard(txt):
    return si.unit_to_svc("h", si.parse_unit("[Service]\nExecStart=/bin/h\n" + txt + "[Install]\nWantedBy=x\n"))
h0 = hard("")
check("no directives -> four explicit =0",
      all(k + "=0\n" in h0 for k in ("no_new_privs", "private_tmp", "protect_system", "protect_home")))
check("no directives -> no warning", si.WARN_PREFIX not in h0)
h1 = hard("NoNewPrivileges=yes\nPrivateTmp=true\nProtectSystem=yes\nProtectHome=yes\n")
check("all yes -> four =1",
      all(k + "=1\n" in h1 for k in ("no_new_privs", "private_tmp", "protect_system", "protect_home")))
check("all yes -> no warning", si.WARN_PREFIX not in h1)
check("ProtectSystem=full", "protect_system=full\n" in hard("ProtectSystem=full\n"))
hs = hard("ProtectSystem=strict\n")
check("ProtectSystem=strict -> full + warning",
      "protect_system=full\n" in hs and si.WARN_PREFIX + "ProtectSystem=strict" in hs)
for v in ("read-only", "tmpfs"):
    hh = hard("ProtectHome=%s\n" % v)
    check("ProtectHome=%s -> 0 + warning" % v,
          "protect_home=0\n" in hh and si.WARN_PREFIX + "ProtectHome=" + v in hh)
hn = hard("NoNewPrivileges=maybe\n")
check("unknown bool -> 0 + warning", "no_new_privs=0\n" in hn and "NoNewPrivileges=maybe" in hn)
check("last assignment wins", "private_tmp=0\n" in hard("PrivateTmp=yes\nPrivateTmp=no\n"))
hw = hard("ProtectSystem=strict\nProtectHome=tmpfs\n")
check("warnings lead the file", hw.startswith(si.WARN_PREFIX) and hw.count(si.WARN_PREFIX) == 2)

# --- ratholes -> Skip ---
def skipped(name, txt):
    try:
        si.unit_to_svc(name, si.parse_unit(txt))
        return False
    except si.Skip:
        return True
bn = si.unit_to_svc("n", si.parse_unit("[Service]\nType=notify\nExecStart=/bin/n\n"))
check("Type=notify -> notify=1", "notify=1\n" in bn and "oneshot=1" not in bn)
bnr = si.unit_to_svc("nr", si.parse_unit("[Service]\nType=notify-reload\nExecStart=/bin/nr\n"))
check("Type=notify-reload -> notify=1", "notify=1\n" in bnr)
bw = si.unit_to_svc("w", si.parse_unit("[Service]\nType=notify\nWatchdogSec=3min\nExecStart=/bin/w\n"))
check("WatchdogSec=3min -> watchdog_sec=180", "watchdog_sec=180\n" in bw)
bw = si.unit_to_svc("w", si.parse_unit("[Service]\nType=notify\nWatchdogSec=1min 30s\nExecStart=/bin/w\n"))
check("WatchdogSec=1min 30s -> 90", "watchdog_sec=90\n" in bw)
bw = si.unit_to_svc("w", si.parse_unit("[Service]\nType=notify\nWatchdogSec=500ms\nExecStart=/bin/w\n"))
check("WatchdogSec=500ms rounds up to 1", "watchdog_sec=1\n" in bw)
bw = si.unit_to_svc("w", si.parse_unit("[Service]\nType=notify\nWatchdogSec=0\nExecStart=/bin/w\n"))
check("WatchdogSec=0 -> off", "watchdog_sec" not in bw)
bw = si.unit_to_svc("w", si.parse_unit("[Service]\nType=notify\nWatchdogSec=30bogus\nExecStart=/bin/w\n"))
check("WatchdogSec bad unit -> warn, off", "watchdog_sec=" not in bw and "WatchdogSec=30bogus unrecognised" in bw)
bw = si.unit_to_svc("w", si.parse_unit("[Service]\nWatchdogSec=30\nExecStart=/bin/w\n"))
check("WatchdogSec without notify -> dropped w/ warn", "watchdog_sec=" not in bw and "without Type=notify" in bw)
bs = si.unit_to_svc("s", si.parse_unit("[Service]\nType=simple\nExecStart=/bin/s\n"))
check("Type=simple has no notify", "notify=" not in bs)
check("Type=forking skipped", skipped("f", "[Service]\nType=forking\nExecStart=/bin/f\n"))
check("Type=forking with %t PIDFile skipped", skipped("f", "[Service]\nType=forking\nPIDFile=%t/f.pid\nExecStart=/bin/f\n"))
bf = si.unit_to_svc("f", si.parse_unit("[Service]\nType=forking\nPIDFile=/run/f.pid\nExecStart=/bin/f\n"))
check("Type=forking + PIDFile -> pid_file", "pid_file=/run/f.pid\n" in bf and "notify=" not in bf)
check("Type=dbus without BusName skipped", skipped("d", "[Service]\nType=dbus\nExecStart=/bin/d\n"))
bd = si.unit_to_svc("nm", si.parse_unit("[Service]\nType=dbus\nBusName=org.freedesktop.NetworkManager\nExecStart=/usr/sbin/NetworkManager --no-daemon\n"))
check("Type=dbus -> ready_bus_name", "ready_bus_name=org.freedesktop.NetworkManager\n" in bd and "notify=" not in bd)
check("template skipped", skipped("t@", "[Service]\nExecStart=/bin/t\n"))
check("no ExecStart skipped", skipped("e", "[Service]\nType=simple\n"))

# --- .timer translation ---
for cal, want in (("daily", "00:00"), ("weekly", "Mon 00:00"), ("monthly", "1 00:00"),
                  ("Sun *-*-* 01:00:00", "Sun 01:00"), ("*-*-15 03:30", "15 03:30"),
                  ("hourly", ("interval", 3600)), ("*-*-* *:00:00", ("interval", 3600)),
                  ("Sun *-*-1..7 1:00:00", None), ("Mon..Fri 09:00", None), ("Mon *-*-15 01:00", None)):
    check("calendar %r -> %r" % (cal, want), si._calendar(cal) == want)
svcu = si.parse_unit("[Service]\nType=oneshot\nExecStart=/usr/sbin/logrotate /etc/logrotate.conf\n")
bt = si.timer_to_svc("logrotate", si.parse_unit("[Timer]\nOnCalendar=daily\nRandomizedDelaySec=1h\nPersistent=true\n"), svcu)
check("timer -> on_calendar + persistent, no oneshot/no_restart, notes drop",
      "on_calendar=00:00\n" in bt and "persistent=1\n" in bt and "oneshot=" not in bt
      and "no_restart=" not in bt and "dropped RandomizedDelaySec" in bt and "no [Install]" not in bt)
bt = si.timer_to_svc("mc", si.parse_unit("[Timer]\nOnBootSec=10min\nOnUnitInactiveSec=3h\n"), svcu)
check("OnBootSec/OnUnitInactiveSec -> on_boot_sec/on_active_sec", "on_boot_sec=600\n" in bt and "on_active_sec=10800\n" in bt)
bt = si.timer_to_svc("mc", si.parse_unit("[Timer]\nOnUnitActiveSec=1d\n"), svcu)
check("OnUnitActiveSec alone also arms the first fire", "on_boot_sec=86400\n" in bt and "on_active_sec=86400\n" in bt)
try:
    si.timer_to_svc("x", si.parse_unit("[Timer]\nOnCalendar=Mon..Fri 09:00\n"), svcu); ok = False
except si.Skip:
    ok = True
check("inexpressible OnCalendar skipped", ok)
bb = si.unit_to_svc("b", si.parse_unit("[Service]\nExecStart=/bin/bash -c 'echo $X; run ${Y}'\nEnvironment=Y=1\n"))
check("$VAR inside a word left for the shell, ${VAR} expanded", "args=echo $X; run 1\n" in bb)

# --- drain against a temp tree ---
tmp = tempfile.mkdtemp()
os.environ["MIGRATE_ROOT"] = tmp
os.environ["SCHEMA_STATE_DIR"] = os.path.join(tmp, "state")
os.environ["SCHEMA_SVC_DIR"] = os.path.join(tmp, "svc")
unitdir = os.path.join(tmp, "usr/lib/systemd/system")
os.makedirs(unitdir); os.makedirs(os.environ["SCHEMA_STATE_DIR"])
open(os.path.join(unitdir, "good.service"), "w").write(
    "[Service]\nExecStart=/usr/bin/good -D\n[Install]\nWantedBy=multi-user.target\n")
open(os.path.join(unitdir, "strict.service"), "w").write(
    "[Service]\nExecStart=/usr/bin/strict\nProtectSystem=strict\n[Install]\nWantedBy=x\n")
open(os.path.join(unitdir, "noisy.service"), "w").write(
    "[Service]\nType=forking\nExecStart=/usr/bin/noisy\n[Install]\nWantedBy=x\n")
open(os.path.join(unitdir, "tick.timer"), "w").write("[Timer]\nOnCalendar=weekly\nPersistent=true\nUnit=tock.service\n")
open(os.path.join(unitdir, "tock.service"), "w").write("[Service]\nType=oneshot\nExecStart=/usr/bin/tock\n")
open(si.queue_path(), "w").write("good\nnoisy\nghost\ntick.timer\n")

logged = []
counts = si.drain(units=["strict"], log=logged.append)
check("drain logs a WARN naming unit + directive",
      any(l.startswith("WARN    strict: ProtectSystem=strict") for l in logged))

counts = si.drain(log=lambda *_: None)
check("two imported (good + tick.timer)", counts["imported"] == 2)
tk = open(os.path.join(os.environ["SCHEMA_SVC_DIR"], "tick.svc")).read()
check("tick.timer -> tick.svc running Unit= target weekly", "exec=/usr/bin/tock\n" in tk and "on_calendar=Mon 00:00\n" in tk)
check("one skipped", counts["skipped"] == 1)
check("one not-found", counts["not-found"] == 1)
check("good.svc written", os.path.exists(os.path.join(os.environ["SCHEMA_SVC_DIR"], "good.svc")))
check("noisy.svc NOT written", not os.path.exists(os.path.join(os.environ["SCHEMA_SVC_DIR"], "noisy.svc")))
stub = os.path.join(os.environ["SCHEMA_SVC_DIR"], "noisy.svc.skipped")
st = open(stub).read() if os.path.exists(stub) else ""
check("noisy.svc.skipped written with reason + commented unit",
      st.startswith("# schema-import skipped noisy: Type=forking") and "# ExecStart=/usr/bin/noisy\n" in st
      and all(l.startswith("#") for l in st.splitlines()))
remaining = [l.strip() for l in open(si.queue_path()) if l.strip()]
check("queue keeps only the transient miss", remaining == ["ghost"])

# re-drain the queue: only the transient miss remains
counts2 = si.drain(log=lambda *_: None)
check("re-drain: only ghost, still missing", counts2["not-found"] == 1 and counts2["imported"] == 0)
# direct re-import of an already-present unit hits the 'exists' path, writes nothing
counts3 = si.drain(units=["good"], log=lambda *_: None)
check("existing .svc not overwritten", counts3["exists"] == 1 and counts3["imported"] == 0)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
