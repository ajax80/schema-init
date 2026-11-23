#!/usr/bin/env python3
"""ISO first-boot wizard: no dialog until Plasma's first start has settled.

A "Switch and restart" while plasmashell is still building its first panel
shuts it down mid-build, and the half-made panel (no tray, no clock) is what
every later login loads (v0.4.31 isotest). The wizard waits for the layout file
to exist and stand still with plasmashell running, capped so it never hangs.
"""
import os, shutil, subprocess, sys, tempfile, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIZARD = os.path.join(REPO, 'distros', 'fedora-installer', 'firstboot-flip-wizard.sh')
results = []
def check(name, ok, detail=''):
    results.append(ok)
    print(('  PASS  ' if ok else '  FAIL  ') + name + ('' if ok else '  — ' + detail))

def first_dialog_after(layout_age=None, plasmashell=True, settle=2, cap=6):
    tmp = tempfile.mkdtemp(prefix='schema-fbw-settle-')
    fake, home = os.path.join(tmp, 'fake'), os.path.join(tmp, 'home')
    os.makedirs(fake); os.makedirs(os.path.join(home, '.local/state/schema'))
    open(os.path.join(fake, 'yad'), 'w').write('#!/bin/sh\n[ -f "$FAKE/t" ] || date +%s.%N > "$FAKE/t"\nexit 3\n')
    open(os.path.join(fake, 'pgrep'), 'w').write('#!/bin/sh\nexit %d\n' % (0 if plasmashell else 1))
    open(os.path.join(fake, 'sudo'), 'w').write('#!/bin/sh\nexit 1\n')
    for n in ('yad', 'pgrep', 'sudo'):
        os.chmod(os.path.join(fake, n), 0o755)
    os.makedirs(os.path.join(home, '.config/autostart'))
    open(os.path.join(home, '.config/autostart/schema-firstboot.desktop'), 'w').close()
    open(os.path.join(home, '.local/state/schema/firstboot.state'), 'w').write('welcome\n')
    lay = os.path.join(home, '.config/plasma-org.kde.plasma.desktop-appletsrc')
    if layout_age is not None:
        open(lay, 'w').write('[Containments][1]\nplugin=org.kde.panel\n')
        t = time.time() - layout_age
        os.utime(lay, (t, t))
    open(os.path.join(fake, 'boot_id'), 'w').write('b\n')
    env = dict(os.environ, HOME=home, FAKE=fake, PATH=fake + ':' + os.environ['PATH'],
               SCHEMA_BOOT_ID_FILE=os.path.join(fake, 'boot_id'),
               SCHEMA_PLASMA_SETTLE_SEC=str(settle), SCHEMA_PLASMA_SETTLE_MAX=str(cap))
    env.pop('XDG_STATE_HOME', None)
    start = time.time()
    subprocess.run(['bash', WIZARD], env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
    tf = os.path.join(fake, 't')
    d = float(open(tf).read()) - start if os.path.exists(tf) else None
    shutil.rmtree(tmp, ignore_errors=True)
    return d

d = first_dialog_after(layout_age=0)
check('fresh layout, plasmashell up: waits for it to stand still (>= 2 s)', d is not None and d >= 1.9, str(d))
d = first_dialog_after(layout_age=3600)
check('layout long settled (a later login): no wait (< 1.5 s)', d is not None and d < 1.5, str(d))
d = first_dialog_after(layout_age=None, plasmashell=False, cap=3)
check('no plasmashell, no layout: gives up at the cap and still shows (3-5 s)', d is not None and 2.9 <= d < 5, str(d))
print('test_firstboot_wizard_settle: %d/%d' % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
