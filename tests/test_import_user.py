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

print("%d/%d passed" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
