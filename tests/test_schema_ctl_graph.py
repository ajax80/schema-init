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

w("getty@.svc", "name=getty\nexec=/sbin/agetty\n")
w("getty@tty2.svc", "name=getty\nexec=/sbin/agetty\ndep=dbus\n")
w("many.svc", "name=many\nexec=/bin/true\n" + "".join(f"dep=d{i}\n" for i in range(10)))
w("bad.svc", "name=bad\nexec=/bin/true\n")
w("bad.svc.d/10.conf", "name=other\n")
w("ws.svc", "name=ws\nexec=/bin/true\ndep=dbus\n")
w("ws.svc.d/10.conf", "dep= \n")
w("mainempty.svc", "name=mainempty\nexec=/bin/true\ndep=dbus\ndep=\n")
w("noname.grp", "member=dbus\n")
w("noexec.svc", "name=noexec\n")
w("file.svc", "name=renamed\nexec=/bin/true\ndep=dbus\n")
w("q.svc", 'name=q\nexec=/bin/true\ndep=we"ird\ndep=ghost\n')
w("big.grp", "name=big\n" + "".join(f"member=d{i}\n" for i in range(9)))
rc, out, err = graph()
check("more than 8 members flagged, group kept as PID 1 holds it", '"big" [shape=box];' in out and "PID 1 holds the group EXCISED" in err)
check("template file skipped", '"getty@"' not in out and '"getty"' not in out)
check("instance named after its file", '"getty@tty2" -> "dbus";' in out)
check("more than 8 deps rejects the service, like PID 1", '"many"' not in out and '"d8"' not in out)
check("name= in a drop-in rejects the service", '"bad"' not in out and '"other"' not in out)
check("whitespace dep= in a drop-in resets", '"ws";' in out and '"ws" -> ' not in out)
check("empty dep= in the main file does not reset", '"mainempty" -> "dbus";' in out)
check("group without name= skipped", '"noname"' not in out)
check("service without exec= skipped", '"noexec"' not in out)
check("quotes escaped", '"we\\"ird"' in out)
check("missing dep declared once", out.count('"ghost" [color=red') == 1)
rc, out, _ = graph("file")
check("graph <svc> accepts the file name", rc == 0 and '"renamed" -> "dbus";' in out)

rc, _, err = graph("nosuch")
check("unknown name exits 1", rc == 1 and "nosuch" in err)
rc, _, _ = graph("../x")
check("path in name refused", rc == 1)

w("longc.svc", "name=longc\nexec=/bin/true\n#" + "x" * 700 + "\ndep=dbus\n")
w("longd.svc", "name=longd\nexec=/bin/true\ndep=" + "x" * 600 + "dep=evil\n")
w("longs.svc", "name=longs\nexec=/bin/true\n" + " " * 600 + "dep=evil\n")
w("longg.grp", "name=longg\nmember=dbus\nmember=" + "x" * 600 + "member=evil\nmember=cups\n")
rc, out, err = graph()
check("long comment line skipped, later dep kept", '"longc" -> "dbus";' in out)
check("over-long line drops the unit like PID 1", '"longd"' not in out and "longd.svc: line longer than 511 bytes" in err)
check("over-long blank line is not a comment", '"longs"' not in out)
check("group keeps its other members, like PID 1",
      '"longg" -> "dbus" [style=dashed];' in out and '"longg" -> "cups" [style=dashed];' in out and '"evil"' not in out)

w("opend.svc", "name=opend\nexec=/bin/true\ndep=dbus\n")
w("opend.svc.d/10.conf", "dep=cups\n")
os.chmod(os.path.join(d, "opend.svc.d"), 0o777)
rc, out, err = graph()
check("writable drop-in dir drops the unit like PID 1", '"opend"' not in out and "opend.svc: a drop-in directory" in err)
os.chmod(os.path.join(d, "opend.svc.d"), 0o755)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
