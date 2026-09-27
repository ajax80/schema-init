#!/usr/bin/env python3
"""grub2-mkconfig targets: never the ESP stub that configfile's /boot/grub2."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

def load(root):
    os.environ["MIGRATE_ROOT"] = root
    spec = importlib.util.spec_from_file_location("sm_grub", os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py"))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

def tree(esp_body):
    r = tempfile.mkdtemp()
    os.makedirs(os.path.join(r, "boot/grub2")); os.makedirs(os.path.join(r, "boot/efi/EFI/fedora"))
    open(os.path.join(r, "boot/grub2/grub.cfg"), "w").write("menuentry x {}\n")
    open(os.path.join(r, "boot/efi/EFI/fedora/grub.cfg"), "w").write(esp_body)
    return r

r = tree("search --no-floppy --fs-uuid --set=dev abc\nset prefix=($dev)/grub2\nexport $prefix\nconfigfile $prefix/grub.cfg\n")
check("modern ESP stub is not a target", load(r)._grub_cfg_targets() == ["/boot/grub2/grub.cfg"])

r = tree("menuentry legacy {}\n")
check("full legacy ESP config is a target", load(r)._grub_cfg_targets() == ["/boot/grub2/grub.cfg", "/boot/efi/EFI/fedora/grub.cfg"])

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
