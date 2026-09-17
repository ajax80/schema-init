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

# --- ratholes -> Skip ---
def skipped(name, txt):
    try:
        si.unit_to_svc(name, si.parse_unit(txt))
        return False
    except si.Skip:
        return True
check("Type=notify skipped", skipped("n", "[Service]\nType=notify\nExecStart=/bin/n\n"))
check("Type=forking skipped", skipped("f", "[Service]\nType=forking\nExecStart=/bin/f\n"))
check("Type=dbus skipped", skipped("d", "[Service]\nType=dbus\nExecStart=/bin/d\n"))
check("template skipped", skipped("t@", "[Service]\nExecStart=/bin/t\n"))
check("no ExecStart skipped", skipped("e", "[Service]\nType=simple\n"))

# --- drain against a temp tree ---
tmp = tempfile.mkdtemp()
os.environ["MIGRATE_ROOT"] = tmp
os.environ["SCHEMA_STATE_DIR"] = os.path.join(tmp, "state")
os.environ["SCHEMA_SVC_DIR"] = os.path.join(tmp, "svc")
unitdir = os.path.join(tmp, "usr/lib/systemd/system")
os.makedirs(unitdir); os.makedirs(os.environ["SCHEMA_STATE_DIR"])
open(os.path.join(unitdir, "good.service"), "w").write(
    "[Service]\nExecStart=/usr/bin/good -D\n[Install]\nWantedBy=multi-user.target\n")
open(os.path.join(unitdir, "noisy.service"), "w").write(
    "[Service]\nType=notify\nExecStart=/usr/bin/noisy\n[Install]\nWantedBy=x\n")
open(si.queue_path(), "w").write("good\nnoisy\nghost\n")

counts = si.drain(log=lambda *_: None)
check("one imported", counts["imported"] == 1)
check("one skipped", counts["skipped"] == 1)
check("one not-found", counts["not-found"] == 1)
check("good.svc written", os.path.exists(os.path.join(os.environ["SCHEMA_SVC_DIR"], "good.svc")))
check("noisy.svc NOT written", not os.path.exists(os.path.join(os.environ["SCHEMA_SVC_DIR"], "noisy.svc")))
remaining = [l.strip() for l in open(si.queue_path()) if l.strip()]
check("queue keeps only the transient miss", remaining == ["ghost"])

# re-drain the queue: only the transient miss remains
counts2 = si.drain(log=lambda *_: None)
check("re-drain: only ghost, still missing", counts2["not-found"] == 1 and counts2["imported"] == 0)
# direct re-import of an already-present unit hits the 'exists' path, writes nothing
counts3 = si.drain(units=["good"], log=lambda *_: None)
check("existing .svc not overwritten", counts3["exists"] == 1 and counts3["imported"] == 0)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
