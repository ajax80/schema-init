#!/usr/bin/env python3
# schema-top's live view in a pty: sort keys, scrolling, quit restores the terminal.
# Fixture: test_board_top.sh's fake table builder, 30 services, no cgroups.
import os, pty, re, select, subprocess, sys, tempfile, termios, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(REPO)
results = []
def check(name, ok, detail=""):
    results.append(bool(ok)); print(("  PASS  " if ok else "  FAIL  ") + name + (("  — " + detail) if not ok and detail else ""))

T = tempfile.mkdtemp()
mk = os.path.join(T, "mk")
src = open("tests/test_board_top.sh").read()
csrc = src[src.index("cat > \"$T/mk.c\" <<'CEOF'\n") + len("cat > \"$T/mk.c\" <<'CEOF'\n"):src.index("\nCEOF\n")]
open(mk + ".c", "w").write(csrc)
subprocess.run(["gcc", "-O2", "-std=c99", "-D_GNU_SOURCE", "-I.", "-o", mk, mk + ".c", "-lrt"], check=True)
SHM = "/schema-top-keys-%d" % os.getpid()
subprocess.run([mk, SHM, "30"], check=True)
top = os.path.join(T, "schema-top")
os.symlink(os.path.join(REPO, "schema-board"), top)

env = dict(os.environ, SCHEMA_BOARD_SHM=SHM, SCHEMA_BOARD_CGROOT=os.path.join(T, "cg"),
           SCHEMA_BOARD_SIZE="80x12", SCHEMA_RECORDER_SHM="/schema-nonexistent", TERM="xterm")
pid, fd = pty.fork()
if pid == 0:
    os.execve(top, [top], env)
before = termios.tcgetattr(fd)

buf = b""
def frame_after(keys=b"", want=None, timeout=3.0):
    global buf
    if keys:
        os.write(fd, keys)
    end = time.time() + timeout
    text = ""
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                buf += os.read(fd, 65536)
            except OSError:
                break
        text = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", buf).decode("utf-8", "replace")
        last = text.split("schema-top")[-1] if "schema-top" in text else ""
        if want is None or re.search(want, last):
            return last
    return text.split("schema-top")[-1] if "schema-top" in text else ""

f = frame_after(want=r"sort: CPU.*\[1-")
check("first frame: sort CPU, a window [1-…]", re.search(r"sort: CPU", f) and "[1-" in f, f[:200])
f = frame_after(b"m", r"sort: MEM")
check("m sorts by memory", "sort: MEM" in f, f[:120])
f = frame_after(b"n", r"sort: NAME")
check("n sorts by name", "sort: NAME" in f, f[:120])
f = frame_after(b"jjjjj", r"\[6-")
check("j x5 scrolls the window to start at 6", "[6-" in f, f[:200])
f = frame_after(b"\x1b[A", r"\[5-")
check("Up arrow scrolls back one", "[5-" in f, f[:200])
f = frame_after(b"r", r"\(rev\)")
check("r reverses", "(rev)" in f, f[:120])
os.write(fd, b"q")
deadline = time.time() + 2
status = None
while time.time() < deadline:
    p, st = os.waitpid(pid, os.WNOHANG)
    if p:
        status = st
        break
    try:
        select.select([fd], [], [], 0.1) and os.read(fd, 65536)
    except OSError:
        pass
check("q exits 0 within 2 s", status is not None and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, str(status))
try:
    check("terminal restored", termios.tcgetattr(fd) == before)
except termios.error:
    check("terminal restored (pty closed with it)", True)
# stdin not a terminal (ssh -n, a script): no busy loop on stdin, cursor shown again on exit (review I2)
import resource
before_ru = resource.getrusage(resource.RUSAGE_CHILDREN)
p = subprocess.Popen([top], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE)
time.sleep(2)
p.terminate()
out, _ = p.communicate(timeout=3)
ru = resource.getrusage(resource.RUSAGE_CHILDREN)
cpu = (ru.ru_utime - before_ru.ru_utime) + (ru.ru_stime - before_ru.ru_stime)
check("stdin /dev/null: under 0.3 s CPU in 2 s", cpu < 0.3, "%.2f s" % cpu)
check("stdin /dev/null: still drawing frames", out.count(b"schema-top") >= 2, str(out.count(b"schema-top")))
check("stdin /dev/null: cursor not left hidden", b"\x1b[?25l" not in out or out.rstrip().endswith(b"\x1b[?25h\x1b[0m"), repr(out[-40:]))
subprocess.run([mk, "rm", SHM])
print("test_board_top_keys: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
