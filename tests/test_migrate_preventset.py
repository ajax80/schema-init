#!/usr/bin/env python3
"""prevent-set.list parser tests."""
import builtins, io, os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

ps = sm.load_prevent_set()
check("returns the four categories",
      set(ps) == {"script", "config", "service", "exclude"})
check("scripts include plasma-session-start.sh", "plasma-session-start.sh" in ps["script"])
check("services include schema-logind", "schema-logind" in ps["service"])
check("config includes plasma-workspace", any("plasma-workspace" in c for c in ps["config"]))
check("frigate is excluded, not a service",
      "frigate" in ps["exclude"] and "frigate" not in ps["service"])
check("comments and blanks ignored", "" not in ps["service"] and "#" not in "".join(ps["service"]))

# Installed-layout fallback: schema-migrate lives in /usr/bin, prevent-set.list
# in /usr/share/schema-init/migrate — a different dir. When the _MODDIR
# sibling is absent, load_prevent_set() must fall back to that installed path
# instead of crashing.
_FALLBACK = "/usr/share/schema-init/migrate/prevent-set.list"
_moddir_orig = sm._MODDIR
_real_exists = os.path.exists
_real_open = builtins.open
sm._MODDIR = tempfile.mkdtemp()  # no prevent-set.list here


def _exists_shim(p):
    return True if p == _FALLBACK else _real_exists(p)


def _open_shim(p, *a, **kw):
    return io.StringIO("service fallback-demo\n") if p == _FALLBACK else _real_open(p, *a, **kw)


os.path.exists = _exists_shim
builtins.open = _open_shim
try:
    ps_fallback = sm.load_prevent_set()
    check("falls back to the installed share path when MODDIR sibling is missing",
          "fallback-demo" in ps_fallback["service"])
finally:
    os.path.exists = _real_exists
    builtins.open = _real_open
    sm._MODDIR = _moddir_orig

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
