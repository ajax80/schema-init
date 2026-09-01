#!/usr/bin/env python3
"""locale1 backing-store tests for schema-logind.

Exercises the /etc/locale.conf, /etc/vconsole.conf and
/etc/X11/xorg.conf.d/00-keyboard.conf read/write helpers that back the
org.freedesktop.locale1 interface — no bus, no live state. SCHEMA_LOGIND_ETC
points the module's backing files at a temp tree before import.

  ./tests/test_logind_locale1.py        exit 0 all pass, 1 any fail
"""
import os
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ETC = tempfile.mkdtemp(prefix='schema-logind-locale-')
os.environ['SCHEMA_LOGIND_ETC'] = ETC

sys.path.insert(0, os.path.join(REPO, 'scripts'))
import importlib.util
spec = importlib.util.spec_from_file_location(
    'schema_logind', os.path.join(REPO, 'scripts', 'schema-logind.py'))
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def read(path):
    with open(path) as f:
        return f.read()


def test_locale_empty_when_absent():
    for f in (L.LOCALE_CONF,):
        if os.path.exists(f):
            os.remove(f)
    check('locale: empty list when no file', L.get_locale() == [])


def test_locale_roundtrip():
    L.set_locale(['LANG=en_US.UTF-8', 'LC_TIME=en_GB.UTF-8'])
    got = L.get_locale()
    check('locale: LANG round-trips', 'LANG=en_US.UTF-8' in got, str(got))
    check('locale: LC_TIME round-trips', 'LC_TIME=en_GB.UTF-8' in got, str(got))
    body = read(L.LOCALE_CONF)
    check('locale: file has LANG line', 'LANG=en_US.UTF-8' in body, body)


def test_locale_replaces_not_appends():
    L.set_locale(['LANG=de_DE.UTF-8'])
    got = L.get_locale()
    check('locale: replaced clean (only LANG now)', got == ['LANG=de_DE.UTF-8'], str(got))


def test_locale_rejects_bad_key():
    try:
        L.set_locale(['NOTALOCALE=x'])
        check('locale: rejects unknown var', False, 'no exception raised')
    except Exception:
        check('locale: rejects unknown var', True)


def test_locale_rejects_malformed():
    try:
        L.set_locale(['LANG'])
        check('locale: rejects missing =', False, 'no exception raised')
    except Exception:
        check('locale: rejects missing =', True)


def test_vconsole_roundtrip():
    L.set_vconsole('us', 'grp:alt_shift_toggle')
    check('vconsole: keymap round-trips', L.get_vconsole()[0] == 'us')
    check('vconsole: toggle round-trips', L.get_vconsole()[1] == 'grp:alt_shift_toggle')
    body = read(L.VCONSOLE_CONF)
    check('vconsole: KEYMAP written', 'KEYMAP=us' in body, body)


def test_vconsole_empty_when_absent():
    if os.path.exists(L.VCONSOLE_CONF):
        os.remove(L.VCONSOLE_CONF)
    check('vconsole: empty when no file', L.get_vconsole() == ('', ''))


def test_x11_roundtrip_creates_dir():
    L.set_x11_keyboard('us', 'pc105', 'dvorak', 'ctrl:nocaps')
    lay, model, variant, opts = L.get_x11_keyboard()
    check('x11: layout round-trips', lay == 'us', lay)
    check('x11: model round-trips', model == 'pc105', model)
    check('x11: variant round-trips', variant == 'dvorak', variant)
    check('x11: options round-trip', opts == 'ctrl:nocaps', opts)
    check('x11: keyboard conf created under X11 dir',
          os.path.isfile(L.X11_KEYMAP_CONF), L.X11_KEYMAP_CONF)
    body = read(L.X11_KEYMAP_CONF)
    check('x11: uses InputClass evdev stanza',
          'MatchIsKeyboard' in body and 'XkbLayout' in body, body)


def test_x11_empty_when_absent():
    if os.path.exists(L.X11_KEYMAP_CONF):
        os.remove(L.X11_KEYMAP_CONF)
    check('x11: empty tuple when no file', L.get_x11_keyboard() == ('', '', '', ''))


def main():
    print('schema-logind locale1 backing-store tests\n')
    for fn in (test_locale_empty_when_absent, test_locale_roundtrip,
               test_locale_replaces_not_appends, test_locale_rejects_bad_key,
               test_locale_rejects_malformed, test_vconsole_roundtrip,
               test_vconsole_empty_when_absent, test_x11_roundtrip_creates_dir,
               test_x11_empty_when_absent):
        print(fn.__name__)
        fn()
        print()
    passed = sum(1 for r in results if r)
    print(f"{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
