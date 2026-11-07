#!/usr/bin/env python3
# Migration imports what an enabled unit needs, as systemd would start it. Found
# on a real boot: nfs-server.svc failed because rpcbind and rpc.mountd never came.
import importlib.util, os, shutil, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-import.py")
root = tempfile.mkdtemp(prefix="impdeps.")
os.environ["MIGRATE_ROOT"] = root
spec = importlib.util.spec_from_file_location("schema_import_deps", MOD)
si = importlib.util.module_from_spec(spec)
spec.loader.exec_module(si)
results = []


def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  ok  " if cond else "  FAIL ") + name + ("" if cond else "  " + str(detail)))


LIB = root + "/usr/lib/systemd/system"
ETC = root + "/etc/systemd/system"
os.makedirs(LIB)
os.makedirs(ETC)
os.makedirs(root + "/etc/schema-init/services")


def unit(name, unit_lines="", body="ExecStart=/usr/bin/true", section="Service"):
    with open(os.path.join(LIB, name), "w") as f:
        f.write("[Unit]\n%s\n[%s]\n%s\n" % (unit_lines, section, body))


def enable(name, target="multi-user"):
    d = os.path.join(ETC, target + ".target.wants")
    os.makedirs(d, exist_ok=True)
    os.symlink(os.path.join(LIB, name), os.path.join(d, name))


unit("nfs-server.service", "Requires=nfs-mountd.service proc-fs-nfsd.mount\nWants=rpcbind.socket nfs-idmapd.service\nRequires=dbus.service")
enable("nfs-server.service")
unit("nfs-mountd.service", "Wants=chained.service")
unit("chained.service", "Requires=chain-end.service")
unit("chain-end.service")
unit("nfs-idmapd.service")
unit("rpcbind.service")
unit("rpcbind.socket", body="ListenStream=/run/rpcbind.sock", section="Socket")
# already runs under another name, reclaimed, a template, masked, missing, not a service
unit("polkit.service")
unit("systemd-thing.service")
unit("tpl@.service")
unit("masked-one.service")
os.symlink("/dev/null", os.path.join(ETC, "masked-one.service"))
unit("modem.service", "Requires=polkit.service dbus.service\nWants=systemd-thing.service tpl@x.service masked-one.service gone.service network-online.target")
enable("modem.service")
# a socket's own deps come now; its service's Wants wait for first activation
unit("cups.socket", "Wants=sock-want.service", body="ListenStream=/run/cups.sock", section="Socket")
unit("sock-want.service")
unit("cups.service", "Requires=cups-hard.service\nWants=cups-soft.service")
unit("cups-hard.service")
unit("cups-soft.service")
enable("cups.socket", "sockets")
unit("tick.timer", "", body="OnCalendar=daily", section="Timer")
unit("tick.service", "Requires=tick-hard.service\nWants=tick-soft.service")
unit("tick-hard.service")
unit("tick-soft.service")
enable("tick.timer", "timers")
# a loop must end
unit("loop-a.service", "Wants=loop-b.service")
unit("loop-b.service", "Wants=loop-a.service")
enable("loop-a.service")
# a unit's own .wants directory
unit("host.service")
unit("guest.service")
enable("host.service")
os.makedirs(os.path.join(ETC, "host.service.wants"))
os.symlink(os.path.join(LIB, "guest.service"), os.path.join(ETC, "host.service.wants/guest.service"))

got = si.enabled_units()
for u, why in (("nfs-mountd.service", "Requires="), ("rpcbind.socket", "Wants= socket"), ("nfs-idmapd.service", "Wants="),
               ("chained.service", "through a dep"), ("chain-end.service", "through a dep's dep"),
               ("sock-want.service", "a socket's own Wants="), ("cups-hard.service", "a socket's service's Requires="),
               ("tick-hard.service", "a timer's service's Requires="), ("loop-b.service", "loop member"),
               ("guest.service", "a unit's .wants dir")):
    check("%s comes along (%s)" % (u, why), u in got, got)
for u, why in (("polkit.service", "polkitd.svc runs it"), ("dbus.service", "reclaimed"), ("systemd-thing.service", "reclaimed"),
               ("tpl@x.service", "template"), ("masked-one.service", "masked"), ("gone.service", "no unit file"),
               ("network-online.target", "not a service"), ("proc-fs-nfsd.mount", "not a service"),
               ("cups-soft.service", "starts at first activation"), ("tick-soft.service", "starts at first activation")):
    check("%s stays out (%s)" % (u, why), u not in got, got)
check("no duplicates", len(got) == len(set(got)), got)
check("directly enabled units still there first", got.index("nfs-server.service") < got.index("nfs-mountd.service"))

logs = []
counts = si.drain(units=got, log=logs.append)
S = root + "/etc/schema-init/services/"
body = open(S + "nfs-server.svc").read()
check("Requires= becomes dep=", "dep=nfs-mountd\n" in body, body)
check("Wants= does not", "dep=rpcbind" not in body and "dep=nfs-idmapd" not in body, body)
check("the socket's service gets its listener", "listen=" in open(S + "rpcbind.svc").read())
check("nothing errored", counts["error"] == 0, logs)
check("no duplicate polkit", not os.path.exists(S + "polkit.svc"))

# what can't be met is said out loud, not dropped
unit("needs.service", "Requires=absent.service dbus.service systemd-thing.service polkit.service proc-fs-nfsd.mount tpl@x.service")
st, det = si.import_one("needs.service", queued=["needs.service"])
check("single import ok", st == "imported", det)
text = det[1] if st == "imported" else ""
warns = [l for l in text.splitlines() if l.startswith(si.WARN_PREFIX)]
check("unmet Requires= warns", any("absent.service" in w for w in warns), text)
check("reclaimed deps don't warn", not any("dbus" in w or "systemd-thing" in w for w in warns), warns)
check("aliased deps don't warn", not any("polkit" in w for w in warns), warns)
check("other unit types don't warn here", not any(".mount" in w or "tpl@" in w for w in warns), warns)

# queued but then skipped: a dep= was written and nothing will ever satisfy it
unit("wants-forky.service", "Requires=forky.service")
unit("forky.service", body="Type=forking\nExecStart=/usr/bin/true")
logs2 = []
si.drain(units=si.with_deps(["wants-forky.service"]), log=logs2.append)
hit = [l for l in logs2 if l.startswith("WARN") and "wants-forky" in l and "forky.service" in l]
check("a hard dep the importer skipped is reported", hit, logs2)
check("the report says why", hit and "Type=forking" in hit[0], hit)
unit("wants-ok.service", "Requires=ok-dep.service")
unit("ok-dep.service")
logs3 = []
si.drain(units=si.with_deps(["wants-ok.service"]), log=logs3.append)
check("a dep that imported isn't reported", not [l for l in logs3 if l.startswith("WARN") and "wants-ok" in l], logs3)

# the queue a later `systemctl enable` feeds gets the same walk
shutil.rmtree(S)
os.makedirs(S)
unit("late.service", "Requires=late-hard.service\nWants=late-soft.service")
unit("late-hard.service")
unit("late-soft.service")
os.makedirs(os.path.dirname(si.queue_path()), exist_ok=True)
with open(si.queue_path(), "w") as f:
    f.write("late.service\n")
si.drain(log=logs.append)
check("queued unit's Requires= comes along", os.path.exists(S + "late-hard.svc"), os.listdir(S))
check("queued unit's Wants= comes along", os.path.exists(S + "late-soft.svc"), os.listdir(S))
check("queued unit gets its dep=", "dep=late-hard\n" in open(S + "late.svc").read())
check("queue emptied", open(si.queue_path()).read() == "", open(si.queue_path()).read())

# --- from review, each on its own clean tree ---
def reset():
    shutil.rmtree(root)
    os.makedirs(LIB)
    os.makedirs(ETC)
    os.makedirs(S)
    os.makedirs(os.path.dirname(si.queue_path()))


# an alias resolves to the real unit, or it walks past every name check
reset()
unit("rsyslog.service", body="ExecStart=/usr/sbin/rsyslogd")
os.symlink("rsyslog.service", os.path.join(LIB, "syslog.service"))
unit("dbus-broker.service", body="ExecStart=/usr/bin/dbus-broker-launch")
os.symlink("dbus-broker.service", os.path.join(LIB, "messagebus.service"))
unit("real.service")
os.symlink("real.service", os.path.join(LIB, "nick.service"))
unit("p.service", "Wants=syslog.service messagebus.service nick.service")
enable("p.service")
got = si.enabled_units()
check("alias of a denied unit stays out", "syslog.service" not in got and "rsyslog.service" not in got, got)
check("alias of a reclaimed bus stays out", "messagebus.service" not in got and "dbus-broker.service" not in got, got)
check("an ordinary alias imports as the real unit", "real.service" in got and "nick.service" not in got, got)
si.drain(units=got, log=lambda *_: None)
check("no rsyslog service written", not os.path.exists(S + "syslog.svc") and not os.path.exists(S + "rsyslog.svc"), os.listdir(S))
check("no second broker written", not os.path.exists(S + "messagebus.svc"), os.listdir(S))

# a pulled-in dep that fails stays queued for the next drain
reset()
unit("q.service", "Requires=qdep.service")
with open(os.path.join(LIB, "qdep.service"), "wb") as f:
    f.write(b"[Service]\nExecStart=/usr/bin/\xff\n")
with open(si.queue_path(), "w") as f:
    f.write("q.service\n")
si.drain(log=lambda *_: None)
check("failed pulled-in dep stays queued", "qdep.service" in open(si.queue_path()).read(), open(si.queue_path()).read())
check("its parent leaves the queue", "q.service" not in open(si.queue_path()).read().replace("qdep.service", ""))
with open(si.queue_path(), "w") as f:
    f.write("")

# a skipped socket doesn't mean its service is missing
reset()
unit("b.service", "Requires=rb.service")
unit("rb.service")
unit("rb.socket", body="\n".join("ListenStream=/run/rb%d" % i for i in range(5)), section="Socket")
logs4 = []
si.drain(units=si.with_deps(["b.service", "rb.socket"]), log=logs4.append)
check("service imported despite its socket being skipped", os.path.exists(S + "rb.svc"), logs4)
check("so no false warning", not [l for l in logs4 if l.startswith("WARN") and "needs rb" in l], logs4)

# one .svc name, one job: a timer's schedule must not be replaced by its service
reset()
unit("t.timer", body="OnCalendar=daily", section="Timer")
unit("t.service", body="ExecStart=/usr/bin/true\n[Install]\nWantedBy=multi-user.target")
unit("c.service", "Requires=t.service")
enable("t.timer", "timers")
enable("c.service")
got = si.enabled_units()
check("enabled timer keeps its service out", "t.service" not in got, got)
si.drain(units=got, force=True, log=lambda *_: None)
check("the schedule survives --force", "on_calendar=" in open(S + "t.svc").read(), open(S + "t.svc").read())

# systemd would start a wanted timer
reset()
unit("w.timer", body="OnCalendar=daily", section="Timer")
unit("w.service")
unit("e.service", "Wants=w.timer")
enable("e.service")
check("a wanted timer comes along", "w.timer" in si.enabled_units(), si.enabled_units())
unit("hard.service", "Requires=absent.timer")
st, det = si.import_one("hard.service", queued=["hard.service"])
check("an unmet required timer warns", st == "imported" and "absent.timer" in det[1], det)

# queue entries are checked before anything reads them
reset()
outside = os.path.join(root, "evil.service")
with open(outside, "w") as f:
    f.write("[Unit]\nWants=tempt.service\n[Service]\nExecStart=/usr/bin/true\n")
unit("tempt.service")
got = si.with_deps(["../../../evil.service"])
check("a bad queue name isn't read", "tempt.service" not in got, got)
check("and isn't dropped: import_one reports it", got == ["../../../evil.service"], got)

shutil.rmtree(root)
print("test_import_deps: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
