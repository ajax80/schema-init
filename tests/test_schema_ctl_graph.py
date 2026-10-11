#!/usr/bin/env python3
import os, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
subprocess.run(["make", "-s", "-C", REPO, "schema-ctl"], check=True)
CTL = os.path.join(REPO, "schema-ctl")

results = []
def check(n, ok, detail=""):
    results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}" + (f"  — {detail}" if detail else ""))

d = tempfile.mkdtemp()
def w(name, body):
    p = os.path.join(d, name); os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "w").write(body)
w("dbus.svc", "name=dbus\nexec=/usr/bin/dbus-daemon\n")
w("polkitd.svc", "name=polkitd\nexec=/usr/lib/polkit-1/polkitd\ndep=dbus\n")
w("desk.grp", "name=desk\nmember=dbus\nmember=polkitd\n")
w("sddm.svc", "name=sddm\nexec=/usr/bin/sddm\ndep=desk\ndep=ghost\n")
w("cups.svc", "name=cups\nexec=/usr/sbin/cupsd\ndep=dbus\n")
w("cups.svc.d/10-x.conf", "dep=\ndep=polkitd\n")
w("loner.svc", "name=loner\nexec=/bin/true\n")

def graph(*a):
    r = subprocess.run([CTL, "graph", *a], env=dict(os.environ, SCHEMA_SVC_DIR=d), capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr

rc, out, _ = graph()
check("full graph is a digraph", rc == 0 and out.startswith("digraph schema {") and out.rstrip().endswith("}"))
check("dep edge", '"polkitd" -> "dbus";' in out)
check("group is a box with dashed member edges", '"desk" [shape=box];' in out and '"desk" -> "polkitd" [style=dashed];' in out)
check("missing dep in red", '"ghost" [color=red' in out and '"sddm" -> "ghost";' in out)
check("drop-in dep= resets, then adds", '"cups" -> "polkitd";' in out and '"cups" -> "dbus";' not in out)
check("node without deps listed", '"loner";' in out)

rc, out, _ = graph("sddm")
check("graph <svc> keeps its chain", rc == 0 and all(f'"{n}"' in out for n in ("sddm", "desk", "polkitd", "dbus")))
check("graph <svc> drops the rest", '"cups"' not in out and '"loner"' not in out)

rc, _, err = graph("nosuch")
check("unknown name exits 1", rc == 1 and "nosuch" in err)
rc, _, _ = graph("../x")
check("path in name refused", rc == 1)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
