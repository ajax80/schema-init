#!/usr/bin/env python3
"""package + make-install tests — inject the runner, no real dnf/make."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

calls = []
# pretend egl-wayland is already installed, the others are not
def fake_run(argv, **kw):
    calls.append(argv)
    rc = 0 if (argv[:2] == ["rpm", "-q"] and "egl-wayland" in argv) else (0 if argv[:2] != ["rpm", "-q"] else 1)
    return type("R", (), {"returncode": rc, "stdout": ""})()

m = sm.Manifest()
installed = sm.install_packages(m, run=fake_run)
check("installs the missing packages", "libavcodec-freeworld" in installed)
check("skips the already-present package", "egl-wayland" not in installed)
check("records only newly installed", "egl-wayland" not in m.packages and "libavcodec-freeworld" in m.packages)
check("dnf install actually called", any(c[:2] == ["dnf", "install"] for c in calls))

calls.clear()
sm.run_make_install(run=fake_run)
check("make install targets DESTDIR + PREFIX",
      any(c[0] == "make" and any("DESTDIR=" in x for x in c) and "PREFIX=/usr" in c for c in calls))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
