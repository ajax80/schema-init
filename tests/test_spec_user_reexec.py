#!/usr/bin/env python3
"""%posttrans re-execs each schema-user still on the replaced binary."""
import os, re, subprocess, sys
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
read = lambda p: open(os.path.join(REPO, p)).read()
SPEC = read("schema-init.spec")

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

post = re.search(r"^%posttrans\n(.*?)(?=^%\w)", SPEC, re.S | re.M).group(1)
m = re.search(r"^for pid in \$\(pgrep -x schema-user[^\n]*\n.*?^done\n", post, re.S | re.M)
check("block present", m)
blk = m.group(0) if m else ""
check("only instances on a deleted binary", '*" (deleted)")' in blk)
check("skips one identical to the new file", "cmp -s /proc/$pid/exe %{_bindir}/schema-user && continue" in blk)
check("asks each user's instance", 'env XDG_RUNTIME_DIR="/run/user/$uid" %{_bindir}/schema-ctl --user reexec' in blk)
check("one line on refusal", "log out and back in to finish" in blk)
# the socket and the process are the user's: never ask it as root, never wait forever
check("client runs as that user", 'setpriv --reuid="$uid" --regid="$gid" --clear-groups' in blk)
check("client is time-limited", "timeout 30 setpriv" in blk)
check("no per-uid dedupe an impostor could win", "done_uids" not in blk)
pid1 = post.find("schema-ctl reexec ||")
check("after the PID 1 re-exec", pid1 >= 0 and m and post.find(blk) > pid1)
pend = re.search(r"^if \[ -e /run/schema-init.reexec-pending \]; then\n.*?^fi\n", post, re.S | re.M)
check("not inside the reexec-pending block", pend and "schema-user" not in pend.group(0))
r = subprocess.run(["sh", "-n"], input=blk.replace("%{_bindir}", "/usr/bin"), text=True)
check("block parses (sh -n)", m and r.returncode == 0)
check("roadmap ticked", "- [x] `schema-user` re-adopts" in read("docs/roadmap.md"))
check("operations.md documents it", "schema-ctl --user reexec" in read("docs/operations.md"))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
