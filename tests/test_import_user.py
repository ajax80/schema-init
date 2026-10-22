#!/usr/bin/env python3
"""schema-import --user: resolution of the user's enabled units, translation,
output swap and skip manifest. Temp trees only (MIGRATE_ROOT + temp HOME)."""
import os, sys, tempfile, shutil, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-import.py")
spec = importlib.util.spec_from_file_location("schema_import", MOD)
si = importlib.util.module_from_spec(spec); spec.loader.exec_module(si)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

OBSIDIAN = ("[Unit]\nDescription=Obsidian vault tunnel to blakbox (Kim MCP)\nAfter=network-online.target\n\n"
            "[Service]\nExecStart=/usr/bin/ssh -N -o ServerAliveInterval=30 -o ServerAliveCountMax=3 "
            "-o ExitOnForwardFailure=yes -L 27124:127.0.0.1:27124 blakbox\nRestart=always\nRestartSec=5\n\n"
            "[Install]\nWantedBy=default.target\n")
SIMPLE = "[Service]\nExecStart=/usr/bin/sleep infinity\nRestart=always\n"


class Tree:
    """A fake root (system unit dirs) and a fake home (~/.config/systemd/user)."""
    def __init__(self):
        self.base = tempfile.mkdtemp()
        self.root = os.path.join(self.base, "root")
        self.home = os.path.join(self.base, "home")
        self.run = os.path.join(self.base, "run")
        os.makedirs(self.root); os.makedirs(self.home); os.makedirs(self.run, mode=0o700)
        os.environ["MIGRATE_ROOT"] = self.root
        self.ctx = si.user_context(self.home, 1000, "tess",
                                   {"XDG_RUNTIME_DIR": self.run})
        si.set_user_mode(self.ctx)

    def cfg(self, rel):
        return os.path.join(self.ctx["config"], "systemd/user", rel)

    def sys(self, d, rel):
        return os.path.join(self.root, d.lstrip("/"), rel)

    def write(self, path, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
        return path

    def link(self, path, target):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        os.symlink(target, path)

    def enable(self, unit, target="default.target", kind="wants", src=None):
        self.link(self.cfg("%s.%s/%s" % (target, kind, unit)), src or self.cfg(unit))

    def done(self):
        si.set_user_mode(None)
        os.environ.pop("MIGRATE_ROOT", None)
        shutil.rmtree(self.base)


def gen(skip=()):
    units, skipped = si.user_enabled_set(set(skip))
    return sorted(units), sorted(skipped)


# --- precedence: ~/.config beats /usr/lib ---
t = Tree()
t.write(t.cfg("foo.service"), "[Service]\nExecStart=/bin/mine\n")
t.write(t.sys("/usr/lib/systemd/user", "foo.service"), "[Service]\nExecStart=/bin/distro\n")
check("precedence", "/bin/mine" in si.user_unit_text("foo.service")
      and "/bin/distro" not in si.user_unit_text("foo.service"))
t.done()

# --- mask: enabled masked unit listed; un-enabled mask ignored ---
t = Tree()
t.write(t.sys("/usr/lib/systemd/user", "foo.service"), SIMPLE)
t.write(t.sys("/usr/lib/systemd/user", "bar.service"), SIMPLE)
t.link(t.cfg("foo.service"), "/dev/null")
t.link(t.cfg("bar.service"), "/dev/null")
t.enable("foo.service", src=t.sys("/usr/lib/systemd/user", "foo.service"))
try:
    si.user_unit_text("foo.service"); ok = False
except si.Masked:
    ok = True
check("mask: raises Masked", ok)
check("mask: enabled masked listed, un-enabled ignored",
      gen() == ([], [("foo.service", "masked")]))
t.done()

# --- drop-ins: every dir, lexical by file name, later appended ---
t = Tree()
t.write(t.sys("/usr/lib/systemd/user", "foo.service"), "[Service]\nExecStart=/bin/a\n")
t.write(t.sys("/usr/lib/systemd/user", "foo.service.d/10-a.conf"), "[Service]\nEnvironment=A=1\n")
t.write(t.cfg("foo.service.d/20-b.conf"), "[Service]\nEnvironment=B=2\n")
t.write(t.sys("/usr/lib/systemd/user", "foo.service.d/30-c.conf"), "[Service]\nEnvironment=C=lib\n")
t.write(t.cfg("foo.service.d/30-c.conf"), "[Service]\nEnvironment=C=cfg\n")
txt = si.user_unit_text("foo.service")
check("dropin_order", txt.index("A=1") < txt.index("B=2") < txt.index("C=cfg"))
check("dropin same name: higher dir wins", "C=lib" not in txt)
t.done()

# --- drop-in ExecStart reset kept in order (translation asserted in Task 2) ---
t = Tree()
t.write(t.cfg("foo.service"), "[Service]\nExecStart=/bin/old\n")
t.write(t.cfg("foo.service.d/override.conf"), "[Service]\nExecStart=\nExecStart=/bin/new\n")
txt = si.user_unit_text("foo.service")
check("dropin_execstart_reset text order", txt.index("/bin/old") < txt.index("ExecStart=\n") < txt.index("/bin/new"))
t.done()

# --- closure: .wants then Wants=/Requires= recursively ---
t = Tree()
t.write(t.cfg("a.service"), "[Unit]\nWants=b.service\n" + SIMPLE)
t.write(t.sys("/usr/lib/systemd/user", "b.service"), "[Unit]\nRequires=c.socket\n" + SIMPLE)
t.write(t.sys("/usr/lib/systemd/user", "c.socket"), "[Socket]\nListenStream=%t/c.sock\n")
t.write(t.sys("/usr/lib/systemd/user", "c.service"), SIMPLE)
t.enable("a.service")
check("closure", gen() == (["a.service", "b.service", "c.socket"], []))
t.done()

# --- distro/admin .wants never read ---
t = Tree()
t.write(t.sys("/usr/lib/systemd/user", "dbus.socket"), "[Socket]\nListenStream=%t/bus\n")
t.write(t.sys("/usr/lib/systemd/user", "x.service"), SIMPLE)
t.link(t.sys("/etc/systemd/user", "sockets.target.wants/dbus.socket"),
       t.sys("/usr/lib/systemd/user", "dbus.socket"))
t.link(t.sys("/usr/lib/systemd/user", "default.target.wants/x.service"), "../x.service")
check("distro_wants_ignored", gen() == ([], []))
t.done()

# --- session-target wants: skipped, phase 2 ---
t = Tree()
t.write(t.cfg("gammastep.service"), SIMPLE)
t.enable("gammastep.service", target="graphical-session.target")
check("session_target", gen() == ([], [("gammastep.service", "phase 2 session target")]))
t.done()

# --- skip list: system + user file, bare names, comments ---
t = Tree()
sysskip = t.write(t.sys("/etc/schema-init", "user-skip"), "# defaults\n\nsyncthing\n")
userskip = t.write(os.path.join(t.ctx["config"], "schema-user/skip"), "other.socket  \n# x\n")
check("read_skip_list", si.read_skip_list([sysskip, userskip, "/nonexistent"])
      == {"syncthing.service", "other.socket"})
t.write(t.sys("/usr/lib/systemd/user", "syncthing.service"), SIMPLE)
t.enable("syncthing.service", src=t.sys("/usr/lib/systemd/user", "syncthing.service"))
check("skip_list", gen(si.read_skip_list([sysskip])) ==
      ([], [("syncthing.service", "handled elsewhere (skip list)")]))
t.done()

# --- template and path units ---
t = Tree()
t.write(t.cfg("foo@.service"), SIMPLE)
t.write(t.cfg("bar.path"), "[Path]\nPathModified=/tmp/x\n")
t.enable("foo@x.service", src=t.cfg("foo@.service"))
t.enable("bar.path")
check("template_and_path", gen() == ([], [("bar.path", "path unit"), ("foo@x.service", "template unit")]))
t.done()

# --- not found ---
t = Tree()
t.link(t.cfg("default.target.wants/gone.service"), t.cfg("gone.service"))
check("not_found", gen() == ([], [("gone.service", "not found")]))
t.done()

# --- fleet: Optiplex ---
t = Tree()
t.write(t.cfg("obsidian-tunnel.service"), OBSIDIAN)
t.enable("obsidian-tunnel.service")
check("fleet_optiplex", gen() == (["obsidian-tunnel.service"], []))
t.done()

# --- fleet: blakbox ---
t = Tree()
t.write(t.sys("/usr/lib/systemd/user", "syncthing.service"), SIMPLE)
t.write(t.sys("/usr/lib/systemd/user", "akonadi_control.service"), SIMPLE)
t.link(t.cfg("akonadi_control.service"), "/dev/null")
t.enable("syncthing.service", src=t.sys("/usr/lib/systemd/user", "syncthing.service"))
skip = t.write(t.sys("/etc/schema-init", "user-skip"), "pipewire.service\nsyncthing.service\n")
check("fleet_blakbox", gen(si.read_skip_list([skip])) ==
      ([], [("syncthing.service", "handled elsewhere (skip list)")]))
t.done()

# --- review fix: deps never reach distro .wants or dbus.socket ---
t = Tree()
L = "/usr/lib/systemd/user"
t.write(t.sys(L, "dbus.socket"), "[Socket]\nListenStream=%t/bus\n")
t.write(t.sys(L, "dbus.service"), SIMPLE)
t.write(t.sys(L, "mpris-proxy.service"), "[Unit]\nWants=dbus.socket\n" + SIMPLE)
t.enable("mpris-proxy.service", src=t.sys(L, "mpris-proxy.service"))
u, sk = gen()
check("deps: mpris-proxy Wants=dbus.socket -> no dbus", u == ["mpris-proxy.service"]
      and ("dbus.socket", "the session bus is started by the session") in sk)
t.done()

t = Tree()
t.write(t.sys(L, "dbus.socket"), "[Socket]\nListenStream=%t/bus\n")
t.write(t.sys(L, "uresourced.service"), SIMPLE)
t.write(t.sys(L, "graphical-session.target"), "[Unit]\nRequires=basic.target\n")
t.write(t.sys(L, "basic.target"), "[Unit]\nWants=sockets.target timers.target\n")
t.write(t.sys(L, "sockets.target"), "[Unit]\n")
t.link(t.sys("/etc/systemd/user", "sockets.target.wants/dbus.socket"), t.sys(L, "dbus.socket"))
t.link(t.sys(L, "graphical-session.target.wants/uresourced.service"), "../uresourced.service")
t.write(t.cfg("mine.service"), "[Unit]\nWants=graphical-session.target network-online.target foo.slice\n" + SIMPLE)
t.enable("mine.service")
u, sk = gen()
check("deps: target chain never expanded", u == ["mine.service"]
      and sk == [("graphical-session.target", "phase 2 session target")])
t.done()

t = Tree()
t.write(t.cfg("a.service"), SIMPLE)
t.write(t.sys(L, "x.service"), SIMPLE)
t.link(t.sys(L, "a.service.wants/x.service"), "../x.service")
t.enable("a.service")
check("deps: unit .wants read from ~/.config only", gen() == (["a.service"], []))
t.done()

t = Tree()
t.write(t.cfg("dbus.socket"), "[Socket]\nListenStream=%t/bus\n")
t.enable("dbus.socket", target="sockets.target")
check("deps: dbus.socket never generated even if user-enabled",
      gen() == ([], [("dbus.socket", "the session bus is started by the session")]))
t.done()

# =========================== Task 2: translation + output ===========================

def run(t, skip_paths=()):
    out = os.path.join(t.run, "schema-user/services")
    c = si.run_user_import(t.ctx, out, list(skip_paths))
    return c, out


def svc(out, name):
    with open(os.path.join(out, name + ".svc")) as f:
        return f.read()


def files_under(base):
    return sorted(os.path.relpath(os.path.join(dp, f), base)
                  for dp, _, fs in os.walk(base) for f in fs)


# --- specifiers ---
t = Tree()
t.write(t.cfg("s.service"), "[Service]\nExecStart=/bin/x %h/b --run %t/sock --s %S --c %C --e %E "
        "--u %u --U %U --tmp %T %V\n")
t.enable("s.service")
c, out = run(t)
b = svc(out, "s")
h = t.home
check("specifiers", all(("args=%s\n" % a) in b for a in (
    h + "/b", t.run + "/sock", h + "/.local/state", h + "/.cache", h + "/.config",
    "tess", "1000", "/tmp", "/var/tmp")))
t.done()

# --- no needs_root / user= ---
t = Tree()
t.write(t.cfg("a.service"), SIMPLE)
t.write(t.cfg("n.service"), "[Service]\nExecStart=/bin/n\nUser=nobody\n")
t.enable("a.service"); t.enable("n.service")
c, out = run(t)
check("no_root_lines", "needs_root" not in svc(out, "a") and "needs_root" not in svc(out, "n")
      and "\nuser=" not in svc(out, "n"))
check("User= warned", any(ln.startswith(si.WARN_PREFIX) and "User=" in ln
                          for ln in svc(out, "n").splitlines()))
t.done()

# --- socket %t -> runtime dir ---
t = Tree()
t.write(t.cfg("c.socket"), "[Socket]\nListenStream=%t/foo.sock\n")
t.write(t.cfg("c.service"), SIMPLE)
t.enable("c.socket", target="sockets.target")
c, out = run(t)
check("socket_runtime", ("listen=stream:%s/foo.sock\n" % t.run) in svc(out, "c"))
t.done()

# --- deps only within the user set ---
t = Tree()
t.write(t.cfg("a.service"), "[Unit]\nRequires=b.service network-online.target\nAfter=network-online.target\n" + SIMPLE)
t.write(t.cfg("b.service"), SIMPLE)
t.enable("a.service")
c, out = run(t)
check("deps_in_set_only", "dep=b\n" in svc(out, "a") and "network" not in
      "".join(l for l in svc(out, "a").splitlines(True) if l.startswith("dep=")))
t.done()

# --- cycle dropped ---
t = Tree()
t.write(t.cfg("a.service"), "[Unit]\nRequires=b.service\n" + SIMPLE)
t.write(t.cfg("b.service"), "[Unit]\nRequires=a.service\n" + SIMPLE)
t.enable("a.service")
c, out = run(t)
check("cycle_dropped", ("dep=b\n" in svc(out, "a")) + ("dep=a\n" in svc(out, "b")) <= 1)
t.done()

# --- Optiplex obsidian-tunnel ---
t = Tree()
t.write(t.cfg("obsidian-tunnel.service"), OBSIDIAN)
t.enable("obsidian-tunnel.service")
c, out = run(t)
b = svc(out, "obsidian-tunnel")
check("obsidian_svc", "exec=/usr/bin/ssh\n" in b and "args=-N\n" in b and "args=blakbox\n" in b
      and "no_restart=1" not in b and c == {"generated": 1, "skipped": 0, "error": 0})
t.done()

# --- drop-in ExecStart reset wins ---
t = Tree()
t.write(t.cfg("foo.service"), "[Service]\nExecStart=/bin/old\n")
t.write(t.cfg("foo.service.d/override.conf"), "[Service]\nExecStart=\nExecStart=/bin/new\n")
t.enable("foo.service")
c, out = run(t)
check("dropin_execstart_reset", "exec=/bin/new\n" in svc(out, "foo"))
t.done()

# --- stale .svc removed on the next run ---
t = Tree()
t.write(t.cfg("a.service"), SIMPLE)
t.enable("a.service")
c, out = run(t)
os.unlink(t.cfg("default.target.wants/a.service"))
c, out = run(t)
check("stale_svc_removed", not os.path.exists(os.path.join(out, "a.svc")))
t.done()

# --- a failed run keeps the previous dir; next good run leaves no staging ---
t = Tree()
t.write(t.cfg("a.service"), SIMPLE)
t.write(t.cfg("b.service"), SIMPLE)
t.enable("a.service"); t.enable("b.service")
c, out = run(t)
before = sorted(os.listdir(out))
real, n = si._write_svc, [0]
def boom(path, body):
    n[0] += 1
    if n[0] > 1:
        raise OSError("disk full")
    real(path, body)
si._write_svc = boom
os.unlink(t.cfg("default.target.wants/b.service"))
t.write(t.cfg("c.service"), SIMPLE); t.enable("c.service")
c, out = run(t)
si._write_svc = real
check("crash_leaves_old_dir", sorted(os.listdir(out)) == before and c["error"] == 1)
c, out = run(t)
check("no staging left", not os.path.exists(out + ".new") and not os.path.exists(out + ".old")
      and sorted(os.listdir(out)) == ["a.svc", "c.svc"])
t.done()

# --- manifest, blakbox shape ---
t = Tree()
t.write(t.sys("/usr/lib/systemd/user", "syncthing.service"), SIMPLE)
t.enable("syncthing.service", src=t.sys("/usr/lib/systemd/user", "syncthing.service"))
skip = t.write(t.sys("/etc/schema-init", "user-skip"), "syncthing.service\n")
c, out = run(t, [skip])
with open(os.path.join(t.run, "schema-user/skipped")) as f:
    check("manifest", f.read() == "syncthing.service\thandled elsewhere (skip list)\n")
check("manifest counts", c == {"generated": 0, "skipped": 1, "error": 0} and os.listdir(out) == [])
t.done()

# --- translation Skip lands in the manifest ---
t = Tree()
t.write(t.cfg("x.service"), "[Service]\nType=oneshot\n")
t.enable("x.service")
c, out = run(t)
with open(os.path.join(t.run, "schema-user/skipped")) as f:
    check("translation skip in manifest", f.read().startswith("x.service\t"))
t.done()

# --- nothing written outside out/ and the manifest ---
t = Tree()
t.write(t.cfg("a.service"), SIMPLE)
t.enable("a.service")
before_root, before_home = files_under(t.root), files_under(t.home)
c, out = run(t)
check("no_writes_outside", files_under(t.root) == before_root and files_under(t.home) == before_home
      and files_under(t.run) == ["schema-user/services/a.svc", "schema-user/skipped"])
check("svc mode 0600", (os.stat(os.path.join(out, "a.svc")).st_mode & 0o777) == 0o600)
t.done()

# --- review fix: one unreadable unit does not abort the import ---
t = Tree()
t.write(t.cfg("a.service"), SIMPLE)
with open(t.cfg("b.service"), "wb") as f:
    f.write(b"[Service]\nExecStart=/bin/caf\xe9\n")
t.enable("a.service"); t.enable("b.service")
c, out = run(t)
with open(os.path.join(t.run, "schema-user/skipped")) as f:
    m = f.read()
check("unreadable unit skipped, rest generated", os.listdir(out) == ["a.svc"]
      and m.startswith("b.service\tunreadable: ") and c["error"] == 0)
t.done()

# --- review fix: a socket never starts a skip-listed service ---
t = Tree()
t.write(t.cfg("foo.socket"), "[Socket]\nListenStream=%t/foo.sock\n")
t.write(t.cfg("foo.service"), SIMPLE)
t.enable("foo.socket", target="sockets.target")
skip = t.write(os.path.join(t.ctx["config"], "schema-user/skip"), "foo\n")
c, out = run(t, [skip])
with open(os.path.join(t.run, "schema-user/skipped")) as f:
    m = f.read()
check("socket of skip-listed service not generated", os.listdir(out) == []
      and "foo.socket\tactivates skip-listed foo.service\n" in m)
t.done()

# --- CLI: no XDG_RUNTIME_DIR -> 2 ---
saved = os.environ.pop("XDG_RUNTIME_DIR", None)
check("cli_no_runtime", si.main(["--user"]) == 2)
if saved is not None:
    os.environ["XDG_RUNTIME_DIR"] = saved

print("%d/%d passed" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
