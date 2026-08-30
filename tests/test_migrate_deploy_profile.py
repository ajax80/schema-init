#!/usr/bin/env python3
"""prevent-set copy tests — fake repo tree + fake prevent-set."""
import os, sys, tempfile, importlib.util
root = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root
fakerepo = tempfile.mkdtemp(); os.environ["MIGRATE_REPO"] = fakerepo
# minimal fake repo layout
for d in ("distros/fedora-kde/scripts", "distros/fedora-kde/config/plasma-env",
          "distros/fedora-installer/rail/services", "distros/fedora-installer/migrate"):
    os.makedirs(os.path.join(fakerepo, d))
open(os.path.join(fakerepo, "distros/fedora-kde/scripts/plasma-session-start.sh"), "w").write("#!/bin/sh\n")
open(os.path.join(fakerepo, "distros/fedora-kde/config/plasma-env/env.sh"), "w").write("x=1\n")
open(os.path.join(fakerepo, "distros/fedora-installer/rail/services/dbus.svc"), "w").write("name=dbus\n")
# a tiny prevent-set naming just those + an excluded item
psl = os.path.join(fakerepo, "distros/fedora-installer/migrate/prevent-set.list")
open(psl, "w").write("script plasma-session-start.sh\nconfig plasma-env\nservice dbus\nexclude frigate\n")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
spec = importlib.util.spec_from_file_location("schema_migrate", MOD)
sm = importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)
sys.modules["schema_migrate"] = sm
# point the module's prevent-set loader at the fake list
sm._PREVENT_LIST_OVERRIDE = psl
results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

m = sm.Manifest()
dst = sm.deploy_prevent_set(m)
check("copied the script", os.path.exists(os.path.join(root, "usr/local/lib/schema-init/scripts/plasma-session-start.sh")))
check("copied the config dir", os.path.exists(os.path.join(root, "etc/schema-init/config/plasma-env/env.sh")))
check("copied the service", os.path.exists(os.path.join(root, "etc/schema-init/services/dbus.svc")))
check("nothing frigate copied", not any("frigate" in p for p in dst))
check("manifest recorded the copies", any("plasma-session-start.sh" in f for f in m.files))

# dry-run writes nothing
root2 = tempfile.mkdtemp(); os.environ["MIGRATE_ROOT"] = root2
spec2 = importlib.util.spec_from_file_location("schema_migrate_2", MOD)
sm2 = importlib.util.module_from_spec(spec2); spec2.loader.exec_module(sm2)
sm2._PREVENT_LIST_OVERRIDE = psl
m2 = sm2.Manifest(); plan = sm2.deploy_prevent_set(m2, dry_run=True)
check("dry-run returns a plan", len(plan) >= 3)
check("dry-run wrote nothing", not os.path.exists(os.path.join(root2, "etc/schema-init")))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
