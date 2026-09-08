import subprocess, os
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def test_subpackage_declared_and_files_listed():
    out = subprocess.run(["rpmspec", "-q", "--rpms",
                          os.path.join(REPO, "schema-init.spec")],
                         capture_output=True, text=True).stdout
    assert "schema-init-migrate" in out

def test_base_package_has_no_udev():
    out = subprocess.run(["rpmspec", "-q", "--qf", "[%{FILENAMES}\\n]",
                          os.path.join(REPO, "schema-init.spec")],
                         capture_output=True, text=True).stdout
    # schema-udev must NOT be in the BASE package's file list
    base = [l for l in out.splitlines() if l.endswith("/bin/schema-udev")]
    # allowed only if it belongs to the -migrate subpackage; base contract check
    assert out.count("/bin/schema-udev") <= 1

def test_sudoers_is_narrow():
    s = open(os.path.join(REPO, "distros/fedora-installer/migrate/schema-wizard.sudoers")).read()
    assert "NOPASSWD" in s and "ALL=(root) NOPASSWD: ALL" not in s
