#!/usr/bin/env python3
"""ISO first-boot wizard: the schema-dbus phases, driven with fake yad/sudo.

yad answers come from a queue of exit codes; the fake sudo plays the flip
helper, logging each subcommand and answering from per-command exit codes.

  ./tests/test_firstboot_wizard_dbus.py     exit 0 all pass, 1 any fail
"""
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIZARD = os.path.join(REPO, 'distros', 'fedora-installer', 'firstboot-flip-wizard.sh')

YAD = '''#!/bin/sh
q="$FAKE/yad.queue"
rc=0
if [ -s "$q" ]; then rc=$(head -1 "$q"); sed -i 1d "$q"; fi
exit "$rc"
'''
SUDO = '''#!/bin/sh
cmd="$2"
echo "$cmd" >> "$FAKE/calls"
[ -f "$FAKE/out.$cmd" ] && cat "$FAKE/out.$cmd"
exit "$(cat "$FAKE/rc.$cmd" 2>/dev/null || echo 0)"
'''

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))


def run(phase, yad=(), rc=None, out=None):
    tmp = tempfile.mkdtemp(prefix='schema-fbw-')
    fake = os.path.join(tmp, 'fake')
    home = os.path.join(tmp, 'home')
    os.makedirs(fake)
    os.makedirs(os.path.join(home, '.local/state/schema'))
    for name, body in (('yad', YAD), ('sudo', SUDO)):
        p = os.path.join(fake, name)
        open(p, 'w').write(body)
        os.chmod(p, 0o755)
    open(os.path.join(fake, 'yad.queue'), 'w').write(''.join(f'{c}\n' for c in yad))
    for k, v in (rc or {}).items():
        open(os.path.join(fake, 'rc.' + k), 'w').write(str(v))
    for k, v in (out or {}).items():
        open(os.path.join(fake, 'out.' + k), 'w').write(v)
    autostart = os.path.join(home, '.config/autostart/schema-firstboot.desktop')
    os.makedirs(os.path.dirname(autostart))
    open(autostart, 'w').close()
    state = os.path.join(home, '.local/state/schema/firstboot.state')
    open(state, 'w').write(phase + '\n')
    env = dict(os.environ, HOME=home, FAKE=fake, PATH=fake + ':' + os.environ['PATH'])
    env.pop('XDG_STATE_HOME', None)
    subprocess.run(['bash', WIZARD], env=env, stdin=subprocess.DEVNULL,
                   capture_output=True, timeout=30)
    calls = open(os.path.join(fake, 'calls')).read().split() if os.path.exists(os.path.join(fake, 'calls')) else []
    final = open(state).read().strip()
    kept = os.path.exists(autostart)
    shutil.rmtree(tmp, ignore_errors=True)
    run.autostart_kept = kept
    return calls, final


def main():
    print("-- udev confirmed -> dbus offer -> arm -> reboot --")
    calls, st = run('armed', yad=[0, 0, 0])
    check('udev confirmed first', calls[:3] == ['root-state', 'is-authoritative', 'confirm'], str(calls))
    check('dbus checked then armed', 'dbus-check' in calls and calls.index('dbus-check') < calls.index('dbus-arm'), str(calls))
    check('reboots after arming', calls[-1] == 'reboot', str(calls))
    check('state dbus_armed', st == 'dbus_armed', st)
    check('autostart kept for the confirm login', 'resolve' not in calls and run.autostart_kept, str(calls))

    print("-- dbus offer skipped --")
    calls, st = run('dbus_offer', yad=[3])
    check('no dbus-arm', 'dbus-arm' not in calls, str(calls))
    check('state done', st == 'done', st)
    check('autostart removed', 'resolve' in calls and not run.autostart_kept, str(calls))

    for phase in ('welcome', 'dbus_offer'):
        for code, why in ((143, 'killed by shutdown/logout'), (1, "couldn't open")):
            print(f"-- {phase}: yad {why} (rc {code}) is not Skip --")
            calls, st = run(phase, yad=[code])
            check('state unchanged', st == phase, st)
            check('autostart kept, nothing resolved', run.autostart_kept and 'resolve' not in calls, str(calls))
    print("-- dbus armed, 'Restarting' notice killed: still reboots --")
    calls, st = run('dbus_offer', yad=[0, 0, 143])
    check('armed then rebooted', 'dbus-arm' in calls and calls[-1] == 'reboot', str(calls))
    print("-- dbus arm fails, error notice killed: still rolls back --")
    calls, st = run('dbus_offer', yad=[0, 0, 143], rc={'dbus-arm': 1})
    check('rollback ran', 'dbus-rollback' in calls, str(calls))

    print("-- welcome: window closed (252) is a choice --")
    calls, st = run('welcome', yad=[252])
    check('state skipped', st == 'skipped', st)

    print("-- dbus preflight fails --")
    calls, st = run('dbus_offer', yad=[0, 0], rc={'dbus-check': 1}, out={'dbus-check': 'broker did not answer'})
    check('no dbus-arm', 'dbus-arm' not in calls, str(calls))
    check('stays retryable', st == 'dbus_offer', st)
    check('no reboot', 'reboot' not in calls, str(calls))

    print("-- dbus-arm fails --")
    calls, st = run('dbus_offer', yad=[0, 0, 0], rc={'dbus-arm': 1})
    check('rolls back the partial arm', 'dbus-rollback' in calls, str(calls))
    check('no reboot', 'reboot' not in calls, str(calls))

    print("-- armed boot, broker authoritative --")
    calls, st = run('dbus_armed', yad=[0])
    check('confirms', 'dbus-confirm' in calls, str(calls))
    check('no rollback, no reboot', 'dbus-rollback' not in calls and 'reboot' not in calls, str(calls))
    check('state done', st == 'done', st)
    check('user autostart removed once done', not run.autostart_kept)

    print("-- armed boot, seatbelt already rolled back --")
    calls, st = run('dbus_armed', yad=[0], rc={'dbus-is-authoritative': 1}, out={'dbus-state': 'skipped'})
    check('does not roll back again or reboot', 'dbus-rollback' not in calls and 'reboot' not in calls, str(calls))
    check('state done', st == 'done', st)

    print("-- armed boot, not authoritative, seatbelt has not acted --")
    calls, st = run('dbus_armed', yad=[0], rc={'dbus-is-authoritative': 1}, out={'dbus-state': 'armed'})
    check('rolls back', 'dbus-rollback' in calls, str(calls))
    check('reboots', calls[-1] == 'reboot', str(calls))
    check('never confirms', 'dbus-confirm' not in calls, str(calls))

    print(f">> {sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
