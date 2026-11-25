#!/usr/bin/env python3
# Regression test for the broker's per-uid connection cap (SDBUS_MAX_CONN_PER_UID):
# one local user connecting in a loop must not exhaust the root broker's fds. The
# live broker, raw sockets: the cap is enforced at accept, before any auth.
import os, re, resource, shutil, socket, subprocess, sys, tempfile, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BROKER = os.path.join(REPO, "schema-dbus")
with open(os.path.join(REPO, "sdbus_conn.h")) as f:
    CAP = int(re.search(r"#define SDBUS_MAX_CONN_PER_UID (\d+)", f.read()).group(1))
results = []


def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  ok  " if cond else "  FAIL ") + name + ("" if cond else "  " + str(detail)))


if os.geteuid() == 0:
    print("test_dbus_conn_cap: skipped (root is exempt from the cap)")
    sys.exit(0)
soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
if hard != resource.RLIM_INFINITY and hard < CAP + 64:
    print("test_dbus_conn_cap: skipped (RLIMIT_NOFILE hard %d)" % hard)
    sys.exit(0)
resource.setrlimit(resource.RLIMIT_NOFILE, (max(soft, CAP + 64) if hard == resource.RLIM_INFINITY
                                            else min(hard, max(soft, CAP + 64)), hard))

d = tempfile.mkdtemp(prefix="sdbus-cap-")
bus = os.path.join(d, "bus")
policy = os.path.join(d, "dbus.policy")
with open(policy, "w") as f:
    f.write("allow = send_type:method_call\nallow = send_type:method_return\n")
env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": d,
       "SCHEMA_DBUS_SOCKET": bus, "SCHEMA_DBUS_POLICY": policy,
       "SCHEMA_DBUS_SVCDIRS": d + "/svc", "SCHEMA_DBUS_OVERRIDE_DIR": d + "/ovr",
       "SCHEMA_DBUS_MASKFILE": d + "/masked", "SCHEMA_DBUS_SYSTEM_CONF": d + "/system.conf"}
log = open(os.path.join(d, "broker.log"), "w")
proc = subprocess.Popen([BROKER], env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
for _ in range(100):
    if os.path.exists(bus):
        break
    time.sleep(0.05)


def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(bus)
    return s


def dropped(s, timeout=1.0):
    # the broker closes a refused connection at once: EOF. A kept one stays silent
    s.settimeout(timeout)
    try:
        return s.recv(1) == b""
    except socket.timeout:
        return False
    except ConnectionResetError:
        return True


try:
    held = [connect() for _ in range(CAP)]
    check("the first %d connections from one uid are kept" % CAP,
          not dropped(held[0], 0.3) and not dropped(held[-1], 0.3))
    extra = connect()
    check("connection %d from the same uid is closed" % (CAP + 1), dropped(extra))
    extra.close()
    held.pop().close()
    # the broker may take the new connection before the closed one's EOF: retry
    for _ in range(20):
        again = connect()
        if not dropped(again, 0.5):
            break
        again.close()
    else:
        again = None
    check("closing one frees a slot", again is not None)
    check("the broker is still running", proc.poll() is None)
    for s in held + ([again] if again else []):
        s.close()
finally:
    proc.terminate()
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    log.close()
    shutil.rmtree(d, ignore_errors=True)

print("test_dbus_conn_cap: %s" % ("ok" if all(results) else "FAILED"))
sys.exit(0 if all(results) else 1)
