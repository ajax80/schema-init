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
for _d in (RUNDIR + '/sessions', RUNDIR + '/seats', RUNDIR + '/users', CG, LINGER, LOGS):
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
        check('spawn: XDG_RUNTIME_DIR', env.get('XDG_RUNTIME_DIR') == '/run/user/%d' % UID,
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


TESTS = [
    test_cgroup_path,
    test_delegate,
    test_delegate_unwritable,
    test_spawn_moves_then_execs,
    test_find,
    test_remove,
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
