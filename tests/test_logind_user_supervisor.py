#!/usr/bin/env python3
"""schema-user lifecycle tests for schema-logind.

Runs unprivileged against a temp cgroup tree (SCHEMA_CGROUP_ROOT) and shell
stand-ins for the schema-user binary (SCHEMA_USER_BIN), so nothing touches the
live hierarchy or starts a real supervisor.

  ./tests/test_logind_user_supervisor.py        exit 0 all pass, 1 any fail
"""
import os
import pwd
import signal
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TMP = tempfile.mkdtemp(prefix='schema-logind-user-test-')
RUNDIR = os.path.join(TMP, 'run-systemd')
CG = os.path.join(TMP, 'cgroup')
LINGER = os.path.join(TMP, 'linger')
LOGS = os.path.join(TMP, 'log')
KILLSWITCH = os.path.join(TMP, 'user-supervisor.disabled')
CMDLINE = os.path.join(TMP, 'cmdline')
BIN = os.path.join(TMP, 'schema-user')
RUNUSER = os.path.join(TMP, 'run-user')
for _d in (RUNDIR + '/sessions', RUNDIR + '/seats', RUNDIR + '/users', CG, LINGER, LOGS, RUNUSER):
    os.makedirs(_d, exist_ok=True)
with open(CMDLINE, 'w') as f:
    f.write('BOOT_IMAGE=/vmlinuz root=/dev/sda1 rw quiet\n')
with open(os.path.join(CG, 'cgroup.subtree_control'), 'w') as f:
    f.write('cpu memory pids\n')

os.environ.update({
    'SCHEMA_LOGIND_RUN_DIR': RUNDIR,
    'SCHEMA_CGROUP_ROOT': CG,
    'SCHEMA_LINGER_DIR': LINGER,
    'SCHEMA_USER_BIN': BIN,
    'SCHEMA_USER_KILLSWITCH': KILLSWITCH,
    'SCHEMA_LOGIND_CMDLINE': CMDLINE,
    'SCHEMA_USER_LOG_DIR': LOGS,
    'SCHEMA_LOGIND_RUN_USER': RUNUSER,
})

sys.path.insert(0, os.path.join(REPO, 'scripts'))
import importlib.util
spec = importlib.util.spec_from_file_location(
    'schema_logind', os.path.join(REPO, 'scripts', 'schema-logind.py'))
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)

UID = os.getuid()
ME = pwd.getpwuid(UID).pw_name
results = []

SLEEPER = "#!/bin/sh\ntrap 'exit 0' TERM\nwhile :; do sleep 0.1; done\n"
STUBBORN = "#!/bin/sh\ntrap '' TERM\nwhile :; do sleep 0.1; done\n"
CRASHER = "#!/bin/sh\nexit 3\n"


def check(name, ok, detail=''):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))
    return ok


def set_bin(body):
    with open(BIN, 'w') as f:
        f.write(body)
    os.chmod(BIN, 0o755)


def svc_dir(uid=UID):
    return '%s/user.slice/user-%d.slice/user@%d.service' % (CG, uid, uid)


# A temp dir does not grow the interface files the kernel puts in every cgroup.
def fake_kernel_files(d):
    os.makedirs(d, exist_ok=True)
    for name in ('cgroup.procs', 'cgroup.subtree_control', 'cgroup.threads',
                 'cgroup.controllers', 'cgroup.kill'):
        open(os.path.join(d, name), 'a').close()


def fake_user_tree(uid=UID):
    fake_kernel_files(CG + '/user.slice')
    fake_kernel_files(CG + '/user.slice/user-%d.slice' % uid)
    fake_kernel_files(svc_dir(uid))
    fake_kernel_files(svc_dir(uid) + '/init.scope')


def wipe_user_tree():
    import shutil
    shutil.rmtree(CG + '/user.slice', ignore_errors=True)


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ''


def wait_for(fn, secs=2.0):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if fn():
            return True
        time.sleep(0.05)
    return fn()


def alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    st = read('/proc/%d/stat' % pid)
    return bool(st) and st.rsplit(') ', 1)[1].split()[0] != 'Z'


def kill_reap(pid):
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass


# The pre-exec shell carries BIN in its argv too, so only argv[1] proves the
# exec happened (a script runs as `/bin/sh <script>`).
def execd(pid):
    argv = read('/proc/%d/cmdline' % pid).split('\0')
    return len(argv) > 1 and argv[1] == BIN


def environ_of(pid):
    raw = read('/proc/%d/environ' % pid)
    return dict(kv.split('=', 1) for kv in raw.split('\0') if '=' in kv)


# -- Task 1: delegation and spawn helpers ------------------------------------

def test_cgroup_path():
    check('cgroup path is the systemd one',
          L.user_service_cgroup(1000).endswith(
              '/user.slice/user-1000.slice/user@1000.service'))


def test_delegate():
    wipe_user_tree()
    fake_user_tree()
    path = L.delegate_user_cgroup(UID)
    check('delegate returns the path', path == svc_dir(), str(path))
    for d in (CG + '/user.slice', CG + '/user.slice/user-%d.slice' % UID):
        got = read(d + '/cgroup.subtree_control').strip()
        check('controllers enabled on ' + os.path.basename(d),
              got in ('+cpu', '+memory', '+pids'), repr(got))
    for p in ('', '/cgroup.procs', '/cgroup.subtree_control', '/cgroup.threads'):
        check('owned by uid: user@' + (p or '/'),
              os.stat(svc_dir() + p).st_uid == UID)


def test_delegate_unwritable():
    global CG
    saved = L.CGROUP_ROOT
    blocker = os.path.join(TMP, 'not-a-dir')
    open(blocker, 'w').close()
    L.CGROUP_ROOT = blocker
    try:
        got = L.delegate_user_cgroup(UID)
        check('delegate on a broken root returns None', got is None, repr(got))
    except Exception as e:
        check('delegate on a broken root returns None', False, repr(e))
    finally:
        L.CGROUP_ROOT = saved


def test_spawn_moves_then_execs():
    wipe_user_tree()
    fake_user_tree()
    set_bin(SLEEPER)
    pid = L.spawn_user_supervisor(UID)
    try:
        check('spawn: pid written to user@ cgroup.procs',
              wait_for(lambda: str(pid) in read(svc_dir() + '/cgroup.procs').split()))
        check('spawn: execs the binary',
              wait_for(lambda: execd(pid)))
        env = environ_of(pid)
        check('spawn: XDG_RUNTIME_DIR', env.get('XDG_RUNTIME_DIR') == '%s/%d' % (RUNUSER, UID),
              repr(env.get('XDG_RUNTIME_DIR')))
        check('spawn: HOME/USER from passwd',
              env.get('HOME') == pwd.getpwuid(UID).pw_dir and env.get('USER') == ME)
        check('spawn: log file opened',
              os.path.exists(os.path.join(LOGS, 'schema-user-%d.log' % UID)))
        os.kill(pid, signal.SIGTERM)
        check('spawn: child reaped after SIGTERM', wait_for(
            lambda: os.waitpid(pid, os.WNOHANG)[0] == pid))
    finally:
        kill_reap(pid)


def test_find():
    wipe_user_tree()
    fake_user_tree()
    set_bin(SLEEPER)
    pid = L.spawn_user_supervisor(UID)
    try:
        wait_for(lambda: execd(pid))
        with open(svc_dir() + '/cgroup.procs', 'w') as f:
            f.write('')
        with open(svc_dir() + '/init.scope/cgroup.procs', 'w') as f:
            f.write('%d\n' % pid)
        got = L.find_user_supervisors()
        check('find: schema-user in init.scope', got == {UID: pid}, repr(got))
        with open(svc_dir() + '/init.scope/cgroup.procs', 'w') as f:
            f.write('%d\n' % os.getpid())
        got = L.find_user_supervisors()
        check('find: a non-schema-user pid is ignored', got == {}, repr(got))
    finally:
        kill_reap(pid)


def test_remove():
    wipe_user_tree()
    fake_user_tree()
    os.makedirs(svc_dir() + '/init.scope/x', exist_ok=True)
    os.makedirs(svc_dir() + '/foo.service', exist_ok=True)
    # rmdir on a real cgroup removes its interface files with it; on a temp
    # tree they are ordinary files, so clear them the way the kernel would.
    for root, _dirs, files in os.walk(svc_dir()):
        for name in files:
            os.unlink(os.path.join(root, name))
    L.remove_user_cgroup(UID)
    check('remove: user@ subtree gone', not os.path.exists(svc_dir()))
    check('remove: user-<uid>.slice kept',
          os.path.isdir(CG + '/user.slice/user-%d.slice' % UID))


# -- Task 2: lifecycle --------------------------------------------------------

def dead(pid):
    st = read('/proc/%d/stat' % pid)
    return not st or st.rsplit(') ', 1)[1].split()[0] == 'Z'


def cleanup(*svs):
    for sv in svs:
        for st in list(sv.state.values()):
            if st.get('pid'):
                kill_reap(st['pid'])
        sv.state.clear()
    for name in os.listdir(LINGER):
        os.unlink(os.path.join(LINGER, name))
    for p in (KILLSWITCH,):
        if os.path.exists(p):
            os.unlink(p)
    with open(CMDLINE, 'w') as f:
        f.write('BOOT_IMAGE=/vmlinuz root=/dev/sda1 rw quiet\n')


def fresh(body=SLEEPER):
    wipe_user_tree()
    fake_user_tree()
    set_bin(body)
    return L.UserSupervisors()


class _Stub:
    def __init__(self, record):
        self.record = record


def test_session_uids_skips_synthesised():
    reg = L.SessionRegistry.__new__(L.SessionRegistry)
    reg.sessions = {
        '31': _Stub(L.SessionRecord('31', None, synthesised=True)),
        '4': _Stub(L.SessionRecord('4', {'UID': str(UID)})),
    }
    check('session_uids: real session counted', reg.session_uids() == {UID},
          repr(reg.session_uids()))
    reg.sessions = {'31': _Stub(L.SessionRecord('31', None, synthesised=True))}
    check('session_uids: synthesised only -> empty', reg.session_uids() == set(),
          repr(reg.session_uids()))


def test_start_on_session():
    sv = fresh()
    try:
        sv.reconcile({UID}, 0)
        pid = sv.pid(UID)
        check('start: pid on session', bool(pid) and wait_for(lambda: execd(pid)))
        sv.reconcile({UID}, 1)
        check('start: no second spawn', sv.pid(UID) == pid)
    finally:
        cleanup(sv)


def test_kill_switch_file():
    sv = fresh()
    try:
        open(KILLSWITCH, 'w').close()
        sv.reconcile({UID}, 0)
        check('kill switch file: not started', sv.pid(UID) is None)
    finally:
        cleanup(sv)


def test_kill_switch_cmdline():
    sv = fresh()
    try:
        with open(CMDLINE, 'w') as f:
            f.write('quiet schema.user=0 rw\n')
        sv.reconcile({UID}, 0)
        check('kill switch cmdline: not started', sv.pid(UID) is None)
        with open(CMDLINE, 'w') as f:
            f.write('quiet schema.user=01 rw\n')
        sv.reconcile({UID}, 1)
        check('schema.user=01 is not the switch', sv.pid(UID) is not None)
    finally:
        cleanup(sv)


def test_grace():
    sv = fresh()
    try:
        sv.reconcile({UID}, 0)
        pid = sv.pid(UID)
        wait_for(lambda: execd(pid))
        sv.reconcile(set(), 10)
        sv.reconcile({UID}, 30)
        check('grace: back inside the window keeps it', sv.pid(UID) == pid and not dead(pid))
        sv.reconcile(set(), 40)
        sv.reconcile(set(), 69)
        check('grace: alive at 29 s', not dead(pid))
        sv.reconcile(set(), 70)
        check('grace: SIGTERM at 30 s', wait_for(lambda: dead(pid)))
        for name in os.listdir(svc_dir()):
            p = os.path.join(svc_dir(), name)
            if os.path.isfile(p):
                os.unlink(p)
        for name in os.listdir(svc_dir() + '/init.scope'):
            os.unlink(os.path.join(svc_dir() + '/init.scope', name))
        sv.reconcile(set(), 71)
        check('grace: uid forgotten', UID not in sv.state)
        check('grace: user@ cgroup removed', not os.path.exists(svc_dir()))
    finally:
        cleanup(sv)


def test_stop_timeout_kill():
    sv = fresh(STUBBORN)
    try:
        sv.reconcile({UID}, 0)
        pid = sv.pid(UID)
        wait_for(lambda: execd(pid))
        sv.reconcile(set(), 1)
        sv.reconcile(set(), 31)
        time.sleep(0.2)
        check('stop timeout: TERM ignored', not dead(pid))
        sv.reconcile(set(), 50)
        check('stop timeout: no kill before 20 s', read(svc_dir() + '/cgroup.kill') == '')
        sv.reconcile(set(), 51)
        check('stop timeout: cgroup.kill at 20 s', read(svc_dir() + '/cgroup.kill').strip() == '1')
    finally:
        cleanup(sv)


def test_backoff_and_give_up():
    sv = fresh(CRASHER)
    try:
        spawns, last, t = [], None, 0.0
        while t <= 60:
            sv.reconcile({UID}, t)
            pid = sv.pid(UID)
            if pid and pid != last:
                spawns.append(t)
                last = pid
                wait_for(lambda: dead(pid))
            t += 0.5
        gaps = [b - a for a, b in zip(spawns, spawns[1:])]
        check('backoff: 5 spawns then give up', len(spawns) == 5, repr(spawns))
        check('backoff: gaps 1/2/4/8 s (+0.5 detection tick)',
              gaps == [1.5, 2.5, 4.5, 8.5], repr(gaps))
        check('backoff: gave_up', sv.state.get(UID, {}).get('gave_up') is True)
        sv.reconcile(set(), 61)
        sv.reconcile({UID}, 62)
        check('backoff: next login starts fresh', sv.pid(UID) is not None)
    finally:
        cleanup(sv)


def test_adopt_after_reexec():
    sv = fresh()
    sv2 = L.UserSupervisors()
    try:
        sv.reconcile({UID}, 0)
        pid = sv.pid(UID)
        wait_for(lambda: execd(pid))
        with open(svc_dir() + '/cgroup.procs', 'w') as f:
            f.write('')
        with open(svc_dir() + '/init.scope/cgroup.procs', 'w') as f:
            f.write('%d\n' % pid)
        sv2.adopt()
        sv2.reconcile({UID}, 0)
        check('adopt: same pid', sv2.pid(UID) == pid, repr(sv2.pid(UID)))
        check('adopt: nothing spawned', read(svc_dir() + '/cgroup.procs') == '')
    finally:
        sv.state.clear()
        cleanup(sv2)


def test_adopt_wrong_uid():
    sv = fresh()
    other = UID + 1
    fake_user_tree(other)
    sv2 = L.UserSupervisors()
    try:
        sv.reconcile({UID}, 0)
        pid = sv.pid(UID)
        wait_for(lambda: execd(pid))
        with open(svc_dir() + '/cgroup.procs', 'w') as f:
            f.write('')
        with open(svc_dir(other) + '/init.scope/cgroup.procs', 'w') as f:
            f.write('%d\n' % pid)
        sv.state.clear()
        sv2.adopt()
        check('adopt: uid from the cgroup path', sv2.pid(other) == pid and sv2.pid(UID) is None,
              repr(sv2.state))
        sv2.reconcile({UID}, 0)
        check('adopt: own uid gets its own instance',
              sv2.pid(UID) is not None and sv2.pid(UID) != pid)
    finally:
        cleanup(sv2)


def test_linger_without_session():
    sv = fresh()
    try:
        open(os.path.join(LINGER, ME), 'w').close()
        sv.reconcile(set(), 0)
        pid = sv.pid(UID)
        check('linger: started with no session', pid is not None)
        sv.reconcile(set(), 100)
        check('linger: never stopped for lack of a session',
              sv.pid(UID) == pid and not dead(pid))
    finally:
        cleanup(sv)



def test_crash_kills_leftovers():
    sv = fresh(CRASHER)
    try:
        sv.reconcile({UID}, 0)
        pid = sv.pid(UID)
        wait_for(lambda: dead(pid))
        sv.reconcile({UID}, 0.5)
        check('crash: orphaned services killed via cgroup.kill',
              read(svc_dir() + '/cgroup.kill').strip() == '1')
    finally:
        cleanup(sv)


def test_start_clears_stale_tree():
    sv = fresh()
    try:
        os.makedirs(svc_dir() + '/probe', exist_ok=True)
        sv.reconcile({UID}, 0)
        check('start: stale per-service dir from a dead instance removed',
              not os.path.exists(svc_dir() + '/probe'))
        check('start: still started', sv.pid(UID) is not None)
    finally:
        cleanup(sv)



def test_start_waits_for_populated_tree():
    sv = fresh()
    try:
        with open(svc_dir() + '/cgroup.events', 'w') as f:
            f.write('populated 1\nfrozen 0\n')
        sv.reconcile({UID}, 0)
        check('populated leftover tree: killed, not started yet',
              sv.pid(UID) is None and read(svc_dir() + '/cgroup.kill').strip() == '1')
        with open(svc_dir() + '/cgroup.events', 'w') as f:
            f.write('populated 0\nfrozen 0\n')
        sv.reconcile({UID}, 0.25)
        check('populated leftover tree: started once empty', sv.pid(UID) is not None)
    finally:
        cleanup(sv)



def test_clean_exit_not_restarted():
    sv = fresh()
    try:
        sv.reconcile({UID}, 0)
        pid = sv.pid(UID)
        wait_for(lambda: execd(pid))
        os.kill(pid, signal.SIGTERM)
        wait_for(lambda: dead(pid))
        sv.reconcile({UID}, 1)
        sv.reconcile({UID}, 100)
        check('clean exit (PID 1 shutdown, a deliberate kill): not restarted',
              sv.pid(UID) is None, repr(sv.pid(UID)))
        check('clean exit: not counted as a crash',
              sv.state.get(UID, {}).get('crashes') == [])
        sv.reconcile(set(), 101)
        sv.reconcile({UID}, 102)
        check('clean exit: next login starts it', sv.pid(UID) is not None)
    finally:
        cleanup(sv)


# -- Task 3: linger -----------------------------------------------------------

def test_set_linger_roundtrip():
    try:
        L.set_linger(UID, True)
        check('linger: file created', os.path.exists(os.path.join(LINGER, ME)))
        check('linger: linger_uids follows', UID in L.linger_uids())
        L.set_linger(UID, False)
        check('linger: file removed', not os.path.exists(os.path.join(LINGER, ME)))
        check('linger: linger_uids follows off', UID not in L.linger_uids())
        L.set_linger(UID, False)
        check('linger: disabling twice is fine', True)
    finally:
        cleanup()


def test_set_linger_unknown_uid():
    try:
        L.set_linger(4000000000, True)
        check('linger: unknown uid raises ValueError', False)
    except ValueError:
        check('linger: unknown uid raises ValueError', True)


def test_linger_auth():
    U = 0xFFFFFFFF
    cases = [
        ((0, 1000, set()), 1000),
        ((1000, 1000, {1000}), 1000),
        ((1000, U, {1000}), 1000),
        ((0, 0, set()), None),
        ((1000, 1001, {1000, 1001}), None),
        ((1000, 1000, set()), None),
        ((None, 1000, {1000}), None),
    ]
    for args, want in cases:
        got = L.linger_allowed(*args)
        check('linger_allowed%r' % (args,), got == want, repr(got))



# -- review fixes -------------------------------------------------------------

# BIN detached, so not our child: what logind sees after a cold restart
# adopts an instance an earlier logind started.
def spawn_orphan():
    import subprocess
    out = subprocess.run(['sh', '-c', '"$0" >/dev/null 2>&1 & echo $!', BIN],
                         capture_output=True, text=True).stdout
    return int(out.strip())


def ctl_sock(uid=UID):
    return '%s/%d/schema-user/ctl.sock' % (RUNUSER, uid)


def adopt_orphan(sv):
    pid = spawn_orphan()
    wait_for(lambda: execd(pid))
    with open(svc_dir() + '/init.scope/cgroup.procs', 'w') as f:
        f.write('%d\n' % pid)
    sv.adopt()
    return pid


def test_adopted_clean_exit_not_restarted():
    sv = fresh()
    try:
        pid = adopt_orphan(sv)
        check('adopted orphan tracked', sv.pid(UID) == pid)
        os.kill(pid, signal.SIGTERM)    # its clean exit unlinks ctl.sock
        wait_for(lambda: not os.path.exists('/proc/%d' % pid))
        sv.reconcile({UID}, 0)
        sv.reconcile({UID}, 10)
        check('adopted clean exit (no ctl.sock): not restarted', sv.pid(UID) is None)
    finally:
        cleanup(sv)


def test_adopted_crash_restarted():
    sv = fresh()
    try:
        pid = adopt_orphan(sv)
        os.makedirs(os.path.dirname(ctl_sock()), exist_ok=True)
        open(ctl_sock(), 'w').close()   # a SIGKILLed instance leaves it behind
        os.kill(pid, signal.SIGKILL)
        wait_for(lambda: not os.path.exists('/proc/%d' % pid))
        sv.reconcile({UID}, 0)
        sv.reconcile({UID}, 2)
        check('adopted crash (ctl.sock left): restarted',
              sv.pid(UID) is not None and sv.pid(UID) != pid)
    finally:
        try:
            os.unlink(ctl_sock())
        except OSError:
            pass
        cleanup(sv)


def test_active_uid_prefers_session_uids():
    a, b = UID + 1000, UID + 2000      # lingering lower uid, session higher uid
    for u in (a, b):
        os.makedirs('%s/%d' % (RUNUSER, u), exist_ok=True)
    try:
        with open(os.path.join(RUNDIR, 'sessions', '9'), 'w') as f:
            f.write('UID=%d\nUSER=x\nSEAT=seat0\nACTIVE=1\n' % b)
        got = L.get_active_uid()
        check('active uid: the session owner, not the lowest /run/user dir',
              got == b, repr(got))
    finally:
        os.unlink(os.path.join(RUNDIR, 'sessions', '9'))
        for u in (a, b):
            os.rmdir('%s/%d' % (RUNUSER, u))


def test_active_uid_skips_linger_without_session():
    import shutil
    shutil.rmtree('%s/%d' % (RUNUSER, UID), ignore_errors=True)   # earlier tests' runtime dir
    a, b = UID + 1000, UID + 2000
    for u in (a, b):
        os.makedirs('%s/%d' % (RUNUSER, u), exist_ok=True)
    saved = L.linger_uids
    L.linger_uids = lambda: {a}
    try:
        got = L.get_active_uid()
        check('active uid: a lingering runtime dir is not a login', got == b, repr(got))
    finally:
        L.linger_uids = saved
        for u in (a, b):
            os.rmdir('%s/%d' % (RUNUSER, u))


def test_adopt_skips_dead_pid():
    sv = L.UserSupervisors()
    saved = L.find_user_supervisors
    L.find_user_supervisors = lambda: {UID: 4194303}
    try:
        sv.adopt()
        check('adopt: a pid gone before its start time was read is skipped',
              sv.pid(UID) is None, repr(sv.state))
    finally:
        L.find_user_supervisors = saved


def test_adopt_reaps_zombies():
    pid = os.posix_spawn('/bin/true', ['/bin/true'], {})
    wait_for(lambda: dead(pid))
    L.UserSupervisors().adopt()
    try:
        os.waitpid(pid, os.WNOHANG)
        check('adopt: zombies left from before a re-exec reaped', False)
    except ChildProcessError:
        check('adopt: zombies left from before a re-exec reaped', True)


def test_start_without_delegation():
    sv = fresh()
    saved = L.CGROUP_ROOT
    blocker = os.path.join(TMP, 'not-a-dir')
    open(blocker, 'w').close()
    L.CGROUP_ROOT = blocker
    try:
        sv.reconcile({UID}, 0)
        pid = sv.pid(UID)
        check('no delegation: still runs (no cgroup move)',
              bool(pid) and wait_for(lambda: execd(pid)))
    finally:
        L.CGROUP_ROOT = saved
        cleanup(sv)


def test_stop_escalates_after_relogin():
    sv = fresh(STUBBORN)
    try:
        sv.reconcile({UID}, 0)
        pid = sv.pid(UID)
        wait_for(lambda: execd(pid))
        sv.reconcile(set(), 1)
        sv.reconcile(set(), 31)          # SIGTERM, ignored
        sv.reconcile({UID}, 33)          # user back while it is stopping
        sv.reconcile({UID}, 51)
        check('stop hung, user back: cgroup.kill still sent',
              read(svc_dir() + '/cgroup.kill').strip() == '1')
    finally:
        cleanup(sv)


TESTS = [
    test_cgroup_path,
    test_delegate,
    test_delegate_unwritable,
    test_spawn_moves_then_execs,
    test_find,
    test_remove,
    test_session_uids_skips_synthesised,
    test_start_on_session,
    test_kill_switch_file,
    test_kill_switch_cmdline,
    test_grace,
    test_stop_timeout_kill,
    test_backoff_and_give_up,
    test_adopt_after_reexec,
    test_adopt_wrong_uid,
    test_linger_without_session,
    test_crash_kills_leftovers,
    test_start_clears_stale_tree,
    test_start_waits_for_populated_tree,
    test_clean_exit_not_restarted,
    test_set_linger_roundtrip,
    test_set_linger_unknown_uid,
    test_linger_auth,
    test_adopted_clean_exit_not_restarted,
    test_adopted_crash_restarted,
    test_active_uid_prefers_session_uids,
    test_active_uid_skips_linger_without_session,
    test_adopt_skips_dead_pid,
    test_adopt_reaps_zombies,
    test_start_without_delegation,
    test_stop_escalates_after_relogin,
]


def main():
    for t in TESTS:
        try:
            t()
        except Exception as e:
            check(t.__name__, False, 'raised %r' % e)
    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
