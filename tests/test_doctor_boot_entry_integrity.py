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


def test_substring_collision_detected():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rootflags=subvol=root init=/sbin/schema-init rd.quiet')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('substring collision (rd.quiet vs quiet): still detected as missing', f is not None)
    check('substring collision: names missing token', 'modprobe.blacklist=radeon' in (f.detail if f else ''), f.detail if f else '')


def read_options(ent_dir, name):
    with open(os.path.join(ent_dir, f'{name}.conf')) as f:
        for line in f:
            if line.startswith('options '):
                return line.rstrip('\n')
    return ''


def set_saved_entry(grubenv_state, name):
    with open(grubenv_state, 'w') as f:
        f.write(f'saved_entry={name}\n')


def test_saved_entry_on_stock_detected():
    root, ent_dir, conf_root, bind, stub, grubenv_state = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/sbin/schema-init modprobe.blacklist=radeon')
    set_saved_entry(grubenv_state, '8ac661a02a5647aaa4f14e6f78f77879-7.2.6-200.fc44.x86_64')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('saved_entry on stock kernel: detected', f is not None)
    check('saved_entry on stock kernel: named in detail', 'saved_entry' in (f.detail if f else ''), f.detail if f else '')


def test_saved_entry_dangling_detected():
    root, ent_dir, conf_root, bind, stub, grubenv_state = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/sbin/schema-init modprobe.blacklist=radeon')
    # saved_entry NAME looks like ours but the .conf file is gone (kernel removed)
    set_saved_entry(grubenv_state, 'schema-6.9.9-200.fc44.x86_64')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    check('dangling saved_entry (schema- prefix, no file): detected', f is not None)


def test_saved_entry_on_valid_schema_clean():
    root, ent_dir, conf_root, bind, stub, grubenv_state = new_root()
    sd = load_module(root, stub)
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/sbin/schema-init modprobe.blacklist=radeon')
    set_saved_entry(grubenv_state, 'schema-7.1.12-200.fc44.x86_64')
    c = sd.BootEntryIntegrity()
    check('saved_entry on a real, valid schema entry: clean', c.detect() is None)


def test_no_schema_entries_ignores_unusual_saved_entry():
    root, ent_dir, conf_root, bind, stub, grubenv_state = new_root()
    sd = load_module(root, stub)
    # no write_entry() calls — zero schema-*.conf files exist
    # but set an unusual saved_entry that looks dangling/stock
    set_saved_entry(grubenv_state, 'schema-9.9.9-200.fc44.x86_64')
    c = sd.BootEntryIntegrity()
    check('no schema entries + unusual saved_entry: guard skips check, returns clean', c.detect() is None)


def test_heal_restores_missing_init_and_extras():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    os.environ['SCHEMA_INIT_BIN'] = '/sbin/schema-init'
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rootflags=subvol=root rhgb quiet')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    c.heal(f)
    opts = read_options(ent_dir, 'schema-7.1.12-200.fc44.x86_64')
    check('heal: init= restored', 'init=/sbin/schema-init' in opts, opts)
    check('heal: extra restored', 'modprobe.blacklist=radeon' in opts, opts)
    check('heal: verify() clean', c.verify() is True)


def test_heal_strips_duplicate_stale_init():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    os.environ['SCHEMA_INIT_BIN'] = '/sbin/schema-init'
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro init=/usr/lib/systemd/systemd modprobe.blacklist=radeon')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    c.heal(f)
    opts = read_options(ent_dir, 'schema-7.1.12-200.fc44.x86_64')
    check('heal: exactly one init= token', opts.count('init=') == 1, opts)
    check('heal: the surviving init= is schema-init', 'init=/sbin/schema-init' in opts, opts)


def test_heal_idempotent():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    os.environ['SCHEMA_INIT_BIN'] = '/sbin/schema-init'
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rootflags=subvol=root rhgb quiet')
    c = sd.BootEntryIntegrity()
    c.heal(c.detect())
    c.heal(c.detect() or sd.Finding('x'))
    opts = read_options(ent_dir, 'schema-7.1.12-200.fc44.x86_64')
    check('heal idempotent: still exactly one init=', opts.count('init=') == 1, opts)
    check('heal idempotent: verify() clean', c.verify() is True)


def test_heal_preserves_rdinit():
    root, ent_dir, conf_root, bind, stub, _ = new_root()
    sd = load_module(root, stub)
    os.environ['SCHEMA_INIT_BIN'] = '/sbin/schema-init'
    write_entry(ent_dir, 'schema-7.1.12-200.fc44.x86_64',
                'root=/dev/sda2 ro rdinit=/bin/sh rhgb quiet modprobe.blacklist=radeon')
    c = sd.BootEntryIntegrity()
    f = c.detect()
    c.heal(f)
    opts = read_options(ent_dir, 'schema-7.1.12-200.fc44.x86_64')
    check('heal: rdinit= survives intact', 'rdinit=/bin/sh' in opts, opts)
    check('heal: no corrupted rd* token', 'rd' not in opts or 'rdinit=' in opts, opts)
    check('heal: init=schema-init added', 'init=/sbin/schema-init' in opts, opts)


def main():
    print('boot-entry-integrity tests\n')
    for fn in (test_clean_entry_detects_none, test_missing_init_detected,
               test_missing_extra_only_detected, test_tonights_actual_shape_all_entries_broken,
               test_no_schema_entries_clean, test_substring_collision_detected,
               test_saved_entry_on_stock_detected, test_saved_entry_dangling_detected,
               test_saved_entry_on_valid_schema_clean, test_no_schema_entries_ignores_unusual_saved_entry,
               test_heal_restores_missing_init_and_extras, test_heal_strips_duplicate_stale_init,
               test_heal_idempotent, test_heal_preserves_rdinit):
        print(fn.__name__)
        fn()
        print()
    passed = sum(1 for r in results if r)
    print(f"{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
