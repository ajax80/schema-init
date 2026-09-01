#!/usr/bin/env python3
"""kernel-install plugin tests: the schema-init per-kernel BLS entry generator.

Drives distros/shared/kernel-install/99-schema-init.install with the arguments
and env kernel-install passes (COMMAND KERNEL_VERSION, KERNEL_INSTALL_BOOT_ROOT,
KERNEL_INSTALL_ENTRY_TOKEN), against a temp /boot tree. A stub grub2-editenv on
PATH records the saved_entry the hook would set, so the marker-gated default
tracking is checked without a real bootloader.

  ./tests/test_kernel_install_hook.py    exit 0 all pass, 1 any fail
"""
import os
import sys
import stat
import tempfile
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOOK = os.path.join(REPO, 'distros/shared/kernel-install/99-schema-init.install')
TOKEN = '8ac661a02a5647aaa4f14e6f78f77879'
VER = '7.1.12-200.fc44.x86_64'

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def new_tree(marker=True, extras=True):
    root = tempfile.mkdtemp(prefix='schema-ki-')
    boot = os.path.join(root, 'boot')
    entries = os.path.join(boot, 'loader/entries')
    os.makedirs(entries)
    with open(os.path.join(entries, f'{TOKEN}-{VER}.conf'), 'w') as f:
        f.write(
            f'title Fedora Linux ({VER}) 44 (KDE Plasma Desktop Edition)\n'
            f'version {VER}\n'
            f'linux /vmlinuz-{VER}\n'
            f'initrd /initramfs-{VER}.img $tuned_initrd\n'
            f'options root=UUID=abc ro rootflags=subvol=root rhgb quiet $tuned_params\n'
            f'grub_users $grub_users\ngrub_arg --unrestricted\ngrub_class fedora\n')
    # a rescue + a stock older kernel that must NOT be picked as the clone source
    with open(os.path.join(entries, f'{TOKEN}-0-rescue.conf'), 'w') as f:
        f.write('title rescue\nversion 0-rescue\noptions ro\n')

    conf_root = os.path.join(root, 'etc/schema-init')
    os.makedirs(conf_root)
    if extras:
        os.makedirs(os.path.join(conf_root, 'kernel-cmdline.d'))
        with open(os.path.join(conf_root, 'kernel-cmdline.d/10-radeon.conf'), 'w') as f:
            f.write('# optiplex GPU\nmodprobe.blacklist=radeon\n')
    if marker:
        open(os.path.join(conf_root, 'boot-default'), 'w').close()

    # stub grub2-editenv on PATH that records "set saved_entry=..."
    bind = os.path.join(root, 'bin')
    os.makedirs(bind)
    rec = os.path.join(root, 'grubenv-set.log')
    stub = os.path.join(bind, 'grub2-editenv')
    with open(stub, 'w') as f:
        f.write('#!/bin/sh\nfor a in "$@"; do case "$a" in saved_entry=*) '
                f'echo "$a" >> "{rec}";; esac; done\nexit 0\n')
    os.chmod(stub, 0o755)
    # a schema-init on PATH so the auto-resolve path (empty SCHEMA_INIT_BIN) has
    # something deterministic to find
    si = os.path.join(bind, 'schema-init')
    open(si, 'w').close(); os.chmod(si, 0o755)
    return root, boot, entries, conf_root, rec, bind


def run(cmd, ver, boot, conf_root, bind, extra_env=None):
    env = dict(os.environ)
    env['KERNEL_INSTALL_BOOT_ROOT'] = boot
    env['KERNEL_INSTALL_ENTRY_TOKEN'] = TOKEN
    env['SCHEMA_INIT_CONF_ROOT'] = conf_root
    env['SCHEMA_INIT_BIN'] = '/usr/bin/schema-init'   # deterministic; host-independent
    env['PATH'] = bind + os.pathsep + env['PATH']
    if extra_env:
        env.update(extra_env)
    return subprocess.run(['sh', HOOK, cmd, ver, os.path.join(boot, 'x')],
                          env=env, capture_output=True, text=True)


def test_add_full():
    root, boot, entries, conf_root, rec, bind = new_tree()
    r = run('add', VER, boot, conf_root, bind)
    check('add: exits 0', r.returncode == 0, r.stderr)
    conf = os.path.join(entries, f'schema-{VER}.conf')
    check('add: schema entry created', os.path.isfile(conf))
    body = open(conf).read() if os.path.isfile(conf) else ''
    check('add: title suffixed', '(schema-init)' in body and 'title ' in body)
    opts = next((l for l in body.splitlines() if l.startswith('options ')), '')
    check('add: init= injected', 'init=/usr/bin/schema-init' in opts, opts)
    check('add: host extra args appended', 'modprobe.blacklist=radeon' in opts, opts)
    check('add: original args preserved', 'rootflags=subvol=root' in opts, opts)
    check('add: no double spaces in options', '  ' not in opts, opts)
    check('add: kernel image copied', f'linux /vmlinuz-{VER}' in body)
    check('add: default repointed (marker present)',
          os.path.isfile(rec) and f'saved_entry=schema-{VER}' in open(rec).read(),
          open(rec).read() if os.path.isfile(rec) else '<no grubenv call>')


def test_no_marker_leaves_default():
    root, boot, entries, conf_root, rec, bind = new_tree(marker=False)
    run('add', VER, boot, conf_root, bind)
    check('no-marker: schema entry still created',
          os.path.isfile(os.path.join(entries, f'schema-{VER}.conf')))
    check('no-marker: default NOT touched', not os.path.isfile(rec),
          open(rec).read() if os.path.isfile(rec) else '')


def test_no_extras():
    root, boot, entries, conf_root, rec, bind = new_tree(extras=False)
    run('add', VER, boot, conf_root, bind)
    opts = next((l for l in open(os.path.join(entries, f'schema-{VER}.conf'))
                 if l.startswith('options ')), '')
    check('no-extras: init= present', 'init=/usr/bin/schema-init' in opts)
    check('no-extras: no stray radeon', 'radeon' not in opts, opts)


def test_idempotent_add():
    root, boot, entries, conf_root, rec, bind = new_tree()
    run('add', VER, boot, conf_root, bind)
    run('add', VER, boot, conf_root, bind)
    opts = next((l for l in open(os.path.join(entries, f'schema-{VER}.conf'))
                 if l.startswith('options ')), '')
    check('idempotent: init= not duplicated', opts.count('init=/usr/bin/schema-init') == 1, opts)


def test_init_path_override():
    root, boot, entries, conf_root, rec, bind = new_tree()
    run('add', VER, boot, conf_root, bind,
        extra_env={'SCHEMA_INIT_BIN': '/sbin/schema-init'})
    opts = next((l for l in open(os.path.join(entries, f'schema-{VER}.conf'))
                 if l.startswith('options ')), '')
    check('override: uses SCHEMA_INIT_BIN path', 'init=/sbin/schema-init' in opts, opts)
    check('override: not the default path', 'init=/usr/bin/schema-init' not in opts, opts)


def test_no_double_init():
    # a stock entry that already carries a (differently-pathed) schema init must
    # not get a second init= appended
    root, boot, entries, conf_root, rec, bind = new_tree(extras=False)
    stock = os.path.join(entries, f'{TOKEN}-{VER}.conf')
    body = open(stock).read().replace(
        'options root=UUID=abc ro',
        'options root=UUID=abc ro init=/sbin/schema-init')
    open(stock, 'w').write(body)
    run('add', VER, boot, conf_root, bind)
    opts = next((l for l in open(os.path.join(entries, f'schema-{VER}.conf'))
                 if l.startswith('options ')), '')
    check('no-double: exactly one init=', opts.count('init=') == 1, opts)


def test_auto_resolve_from_path():
    root, boot, entries, conf_root, rec, bind = new_tree(extras=False)
    run('add', VER, boot, conf_root, bind, extra_env={'SCHEMA_INIT_BIN': ''})
    opts = next((l for l in open(os.path.join(entries, f'schema-{VER}.conf'))
                 if l.startswith('options ')), '')
    check('auto-resolve: uses schema-init found on PATH',
          f'init={os.path.join(bind, "schema-init")}' in opts, opts)


def test_remove():
    root, boot, entries, conf_root, rec, bind = new_tree()
    run('add', VER, boot, conf_root, bind)
    conf = os.path.join(entries, f'schema-{VER}.conf')
    check('remove: entry exists before', os.path.isfile(conf))
    run('remove', VER, boot, conf_root, bind)
    check('remove: schema entry deleted', not os.path.isfile(conf))
    check('remove: stock entry untouched',
          os.path.isfile(os.path.join(entries, f'{TOKEN}-{VER}.conf')))


def main():
    print('schema-init kernel-install hook tests\n')
    for fn in (test_add_full, test_no_marker_leaves_default, test_no_extras,
               test_idempotent_add, test_init_path_override, test_no_double_init,
               test_auto_resolve_from_path, test_remove):
        print(fn.__name__)
        fn()
        print()
    passed = sum(1 for r in results if r)
    print(f"{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
