#!/usr/bin/env python3
"""boot-entry-integrity tests — schema BLS entries keep init=schema-init and
host kernel-cmdline.d args; saved_entry stays on a real schema entry.

  ./tests/test_doctor_boot_entry_integrity.py    exit 0 all pass, 1 any fail
"""
import os
import sys
import tempfile
import importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def new_root(entries=True, extras=True):
    root = tempfile.mkdtemp(prefix='schema-doctor-bei-')
    ent_dir = os.path.join(root, 'boot/loader/entries')
    os.makedirs(ent_dir)
    conf_root = os.path.join(root, 'etc/schema-init')
    os.makedirs(conf_root)
    if extras:
        os.makedirs(os.path.join(conf_root, 'kernel-cmdline.d'))
        with open(os.path.join(conf_root, 'kernel-cmdline.d/10-radeon.conf'), 'w') as f:
            f.write('# optiplex GPU\nmodprobe.blacklist=radeon\n')
    bind = os.path.join(root, 'bin')
    os.makedirs(bind)
    grubenv_state = os.path.join(root, 'grubenv-state.txt')
    stub = os.path.join(bind, 'grub2-editenv-stub.sh')
    with open(stub, 'w') as f:
        f.write(f'''#!/bin/sh
case "$2" in
  list) [ -f "{grubenv_state}" ] && cat "{grubenv_state}" ;;
  set)
    shift 2
    for a in "$@"; do
      case "$a" in saved_entry=*) echo "$a" > "{grubenv_state}" ;; esac
    done
    ;;
esac
exit 0
''')
    os.chmod(stub, 0o755)
    return root, ent_dir, conf_root, bind, stub, grubenv_state


def write_entry(ent_dir, name, options):
    with open(os.path.join(ent_dir, f'{name}.conf'), 'w') as f:
        f.write(f'title Fedora Linux\nversion x\noptions {options}\n')


def load_module(root, stub):
    os.environ['DOCTOR_ROOT'] = root
    os.environ['SCHEMA_DOCTOR_GRUB2_EDITENV'] = stub
    spec = importlib.util.spec_from_file_location(
        'schema_doctor', os.path.join(REPO, 'scripts', 'schema-doctor.py'))
    sd = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sd)
    return sd


def test_clean_entry_detects_none():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rootflags=subvol=root init=/sbin/schema-init '
                'modprobe.blacklist=radeon')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('clean entry: detect returns None', f is None, f.detail if f else '')


def test_missing_init_detected():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rootflags=subvol=root modprobe.blacklist=radeon')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('missing init=: detected', f is not None)
    check('missing init=: names the entry', 'schema-7.1.12' in (f.detail if f else ''), f.detail if f else '')


def test_missing_extra_only_detected():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rootflags=subvol=root init=/sbin/schema-init')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('missing extra only (init= intact): still detected', f is not None)


def test_tonights_actual_shape_all_entries_broken():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    for name in ('schema-ssd-7.1.12-200.fc44.x86_64', 'schema-7.1.12-200.fc44.x86_64',
                 'schema-7.1.10-200.fc44.x86_64', 'schema-7.1.8-200.fc44.x86_64'):
        write_entry(ent_dir, name, 'root=/dev/sda2 ro rootflags=subvol=root rhgb quiet')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('all 4 entries stripped: detected', f is not None)
    for name in ('schema-ssd-7.1.12', 'schema-7.1.12', 'schema-7.1.10', 'schema-7.1.8'):
        check(f'all-stripped: {name} named in detail', name in (f.detail if f else ''), f.detail if f else '')


def test_no_schema_entries_clean():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    c = sd.BootEntryIntegrity()
    check('no schema-*.conf at all (greybox/eli model): clean', c.detect() is None)


def main():
    print('boot-entry-integrity tests\n')
    for fn in (test_clean_entry_detects_none, test_missing_init_detected,
               test_missing_extra_only_detected, test_tonights_actual_shape_all_entries_broken,
               test_no_schema_entries_clean):
        print(fn.__name__)
        fn()
        print()
    passed = sum(1 for r in results if r)
    print(f"{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
