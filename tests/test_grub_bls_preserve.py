#!/usr/bin/env python3
"""grub.d BLS preserve tests: grub2-mkconfig must not clobber schema-init entries.

Fedora's /etc/grub.d/10_linux (update_bls_cmdline) rewrites the options line of
EVERY entry in /boot/loader/entries to GRUB_CMDLINE_LINUX: init= is dropped and
a snapshot entry's rootflags=subvol=<snapshot> is replaced by the live root.
scripts/09_schema_bls_save runs before it and stashes the schema-init entries;
scripts/11_schema_bls_restore runs after it and puts them back. The 10_linux
rewrite is simulated here against a temp entries dir.

  ./tests/test_grub_bls_preserve.py    exit 0 all pass, 1 any fail
"""
import os
import sys
import tempfile
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAVE = os.path.join(REPO, 'scripts/09_schema_bls_save')
RESTORE = os.path.join(REPO, 'scripts/11_schema_bls_restore')
HOOK = os.path.join(REPO, 'distros/shared/kernel-install/99-schema-init.install')
TOKEN = '8ac661a02a5647aaa4f14e6f78f77879'
VER = '7.1.12-200.fc44.x86_64'
OLD_STOCK = 'root=UUID=abc ro rootflags=subvol=root rhgb quiet'
NEW_STOCK = 'root=UUID=abc ro rootflags=subvol=root rhgb quiet mitigations=off'
INIT = 'init=/usr/bin/schema-init'

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def write(path, title, options, version=VER):
    with open(path, 'w') as f:
        f.write(f'title {title}\nversion {version}\nlinux /vmlinuz-{VER}\n'
                f'initrd /initramfs-{VER}.img\noptions {options}\n'
                f'grub_users $grub_users\ngrub_arg --unrestricted\ngrub_class fedora\n')


def opts(path):
    return next((l[len('options '):].rstrip('\n') for l in open(path) if l.startswith('options ')), None)


def new_tree(extras=True, marker=True):
    root = tempfile.mkdtemp(prefix='schema-bls-')
    entries = os.path.join(root, 'entries')
    os.makedirs(entries)
    conf_root = os.path.join(root, 'etc/schema-init')
    os.makedirs(conf_root)
    if extras:
        os.makedirs(os.path.join(conf_root, 'kernel-cmdline.d'))
        with open(os.path.join(conf_root, 'kernel-cmdline.d/10-radeon.conf'), 'w') as f:
            f.write('# optiplex GPU\nmodprobe.blacklist=radeon\n')
    if marker:
        open(os.path.join(conf_root, 'boot-default'), 'w').close()
    e = {
        'stock': os.path.join(entries, f'{TOKEN}-{VER}.conf'),
        'twin': os.path.join(entries, f'schema-{VER}.conf'),
        'snap': os.path.join(entries, 'schema-snap-20261011-200415-post-update.conf'),
        'custom': os.path.join(entries, f'{TOKEN}-7.0.12-cachyos1-schema.fc44.x86_64.conf'),
        'rescue': os.path.join(entries, f'{TOKEN}-0-rescue.conf'),
    }
    write(e['stock'], f'Fedora Linux ({VER})', OLD_STOCK)
    write(e['twin'], f'Fedora Linux ({VER}) (schema-init)', f'{OLD_STOCK} {INIT} modprobe.blacklist=radeon')
    write(e['snap'], 'schema-init snapshot', f'root=UUID=abc ro rootflags=subvol=schema-snap-20261011 {INIT}',
          version='snap')
    write(e['custom'], 'SchemaKernel', f'root=PARTUUID=3e07 ro nowatchdog init=/sbin/schema-init',
          version='7.0.12-cachyos1-schema.fc44.x86_64')
    write(e['rescue'], 'rescue', OLD_STOCK, version='0-rescue')
    return root, entries, conf_root, e


def env_for(root, entries, conf_root):
    env = dict(os.environ)
    env['SCHEMA_INIT_GRUB_ENTRIES'] = entries
    env['SCHEMA_INIT_BLS_STASH'] = os.path.join(root, 'stash')
    env['SCHEMA_INIT_CONF_ROOT'] = conf_root
    env['SCHEMA_INIT_BIN'] = '/usr/bin/schema-init'
    env['SCHEMA_INIT_KI_HOOK'] = HOOK
    env['KERNEL_INSTALL_ENTRY_TOKEN'] = TOKEN
    env.pop('GRUB_CMDLINE_LINUX', None)
    env.pop('GRUB_CMDLINE_LINUX_DEFAULT', None)
    return env


def run(script, env):
    return subprocess.run(['sh', script], env=env, capture_output=True, text=True)


def ten_linux(entries, cmdline):
    for n in os.listdir(entries):
        if not n.endswith('.conf'):
            continue
        p = os.path.join(entries, n)
        lines = open(p).read().splitlines(True)
        with open(p, 'w') as f:
            f.writelines(f'options {cmdline}\n' if l.startswith('options') else l for l in lines)


def mkconfig(root, entries, conf_root, cmdline=NEW_STOCK, save=True):
    env = env_for(root, entries, conf_root)
    env['GRUB_CMDLINE_LINUX'] = cmdline   # grub2-mkconfig exports it to every grub.d script
    s = run(SAVE, env) if save else None
    ten_linux(entries, cmdline)
    r = run(RESTORE, env)
    return s, r


def test_mkconfig_preserves_entries():
    root, entries, conf_root, e = new_tree()
    s, r = mkconfig(root, entries, conf_root)
    check('save: exits 0', s.returncode == 0, s.stderr)
    check('restore: exits 0', r.returncode == 0, r.stderr)
    check('save: nothing on stdout (it lands in grub.cfg)', s.stdout == '', repr(s.stdout))
    check('restore: nothing on stdout (it lands in grub.cfg)', r.stdout == '', repr(r.stdout))
    check('twin: rebuilt from the new stock line + init + extras',
          opts(e['twin']) == f'{NEW_STOCK} {INIT} modprobe.blacklist=radeon', opts(e['twin']))
    check('snapshot: rootflags=subvol=<snapshot> kept verbatim',
          opts(e['snap']) == f'root=UUID=abc ro rootflags=subvol=schema-snap-20261011 {INIT}', opts(e['snap']))
    check('custom schema-init entry (non schema-* name): kept verbatim',
          opts(e['custom']) == 'root=PARTUUID=3e07 ro nowatchdog init=/sbin/schema-init', opts(e['custom']))
    check('stock entry: left with the new mkconfig line', opts(e['stock']) == NEW_STOCK, opts(e['stock']))
    check('rescue entry: untouched by us', opts(e['rescue']) == NEW_STOCK, opts(e['rescue']))
    body = open(e['twin']).read()
    check('twin: title and other lines unchanged',
          f'title Fedora Linux ({VER}) (schema-init)\n' in body and 'grub_class fedora\n' in body, body)
    check('stash: removed after restore', not os.path.exists(os.path.join(root, 'stash')))


def test_global_init_cmdline_still_restores_snapshot():
    # blakbox puts init= in GRUB_CMDLINE_LINUX itself: after 10_linux the
    # snapshot still has init= but has lost its subvol — must still be restored
    root, entries, conf_root, e = new_tree()
    mkconfig(root, entries, conf_root, cmdline=f'{NEW_STOCK} init=/sbin/schema-init')
    check('global init=: snapshot subvol restored',
          'rootflags=subvol=schema-snap-20261011' in (opts(e['snap']) or ''), opts(e['snap']))
    o = opts(e['twin']) or ''
    check('global init=: twin has exactly one init=', o.count('init=') == 1, o)
    new = f'{NEW_STOCK} init=/sbin/schema-init'
    check('global init=: stock entry takes the new mkconfig line', opts(e['stock']) == new, opts(e['stock']))
    check('global init=: non schema-* entry is mkconfig\'s to rewrite', opts(e['custom']) == new, opts(e['custom']))


def test_no_marker_twin_verbatim():
    # a box that never opted in: the hook does not own the twin, so it comes
    # back exactly as it was
    root, entries, conf_root, e = new_tree(marker=False)
    mkconfig(root, entries, conf_root)
    check('no marker: twin options restored verbatim',
          opts(e['twin']) == f'{OLD_STOCK} {INIT} modprobe.blacklist=radeon', opts(e['twin']))


def test_interrupted_run_keeps_stash():
    # mkconfig died after 10_linux, before the restore: the next run's save
    # must not throw the only good copy away
    root, entries, conf_root, e = new_tree()
    env = env_for(root, entries, conf_root)
    run(SAVE, env)
    ten_linux(entries, NEW_STOCK)
    mkconfig(root, entries, conf_root)
    check('interrupted run: snapshot subvol recovered on the next mkconfig',
          'rootflags=subvol=schema-snap-20261011' in (opts(e['snap']) or ''), opts(e['snap']))


def test_restore_without_stash_fixes_twin():
    # a box already clobbered before the hook shipped (DBox, 10-17): no stash,
    # the twin is still rebuilt from its stock entry
    root, entries, conf_root, e = new_tree()
    ten_linux(entries, NEW_STOCK)
    r = run(RESTORE, env_for(root, entries, conf_root))
    check('no stash: exits 0', r.returncode == 0, r.stderr)
    check('no stash: twin init= restored',
          opts(e['twin']) == f'{NEW_STOCK} {INIT} modprobe.blacklist=radeon', opts(e['twin']))


def test_trailing_space_from_10_linux():
    # 10_linux writes "options ${cmdline} " -- an empty GRUB_CMDLINE_LINUX_DEFAULT
    # leaves a trailing space (Eli, 10-17)
    root, entries, conf_root, e = new_tree()
    mkconfig(root, entries, conf_root, cmdline=NEW_STOCK + ' ')
    o = opts(e['twin']) or ''
    check('trailing space: no double space in rebuilt twin', '  ' not in o, repr(o))


def test_no_extras():
    root, entries, conf_root, e = new_tree(extras=False)
    mkconfig(root, entries, conf_root)
    check('no extras: twin = stock + init', opts(e['twin']) == f'{NEW_STOCK} {INIT}', opts(e['twin']))


def test_idempotent():
    root, entries, conf_root, e = new_tree()
    mkconfig(root, entries, conf_root)
    before = {k: open(p).read() for k, p in e.items()}
    mkconfig(root, entries, conf_root)
    check('second mkconfig: every entry identical', before == {k: open(p).read() for k, p in e.items()})


def test_missing_entries_dir():
    root = tempfile.mkdtemp(prefix='schema-bls-')
    env = env_for(root, os.path.join(root, 'nope'), os.path.join(root, 'etc'))
    s, r = run(SAVE, env), run(RESTORE, env)
    check('no entries dir: save exits 0 silently', s.returncode == 0 and s.stdout == '', s.stderr)
    check('no entries dir: restore exits 0 silently', r.returncode == 0 and r.stdout == '', r.stderr)


def test_entry_removed_between():
    root, entries, conf_root, e = new_tree()
    env = env_for(root, entries, conf_root)
    run(SAVE, env)
    os.remove(e['snap'])
    ten_linux(entries, NEW_STOCK)
    r = run(RESTORE, env)
    check('entry deleted mid-run: restore exits 0', r.returncode == 0, r.stderr)
    check('entry deleted mid-run: not recreated', not os.path.exists(e['snap']))


def test_odd_characters_survive():
    root, entries, conf_root, e = new_tree()
    weird = 'root=UUID=abc ro acpi_osi="!Windows 2020" foo=a\\b&c$d/e;f init=/usr/bin/schema-init'
    write(e['snap'], 'snap', weird, version='snap')
    mkconfig(root, entries, conf_root)
    check('quotes, backslash, & $ / ; kept byte-for-byte', opts(e['snap']) == weird, opts(e['snap']))


def main():
    print('schema-init grub.d BLS preserve tests\n')
    for fn in (test_mkconfig_preserves_entries, test_global_init_cmdline_still_restores_snapshot,
               test_restore_without_stash_fixes_twin, test_trailing_space_from_10_linux, test_no_extras,
               test_no_marker_twin_verbatim, test_interrupted_run_keeps_stash, test_idempotent,
               test_missing_entries_dir, test_entry_removed_between, test_odd_characters_survive):
        print(fn.__name__)
        fn()
        print()
    passed = sum(1 for r in results if r)
    print(f"{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
