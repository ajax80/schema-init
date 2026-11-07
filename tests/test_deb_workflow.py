#!/usr/bin/env python3
# Stable apt only moves when the GitHub release is published, which is after the
# manual release test, and it gets the exact debs the tag run built and tested.
import os, re, shutil, stat, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
text = open(os.path.join(REPO, ".github/workflows/deb.yml")).read()
SCRIPT = os.path.join(REPO, "tools/deb-promote.sh")
results = []


def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  ok  " if cond else "  FAIL ") + name + ("" if cond else "  " + str(detail)))


class Github:
    def __init__(self, event_name, ref):
        self.event_name, self.ref = event_name, ref


def ev(expr, event, ref):
    py = expr.replace("&&", " and ").replace("||", " or ")
    return eval(py, {"github": Github(event, ref), "startsWith": lambda s, p: s.startswith(p)})


def job(name):
    m = re.search(r"^  %s:\n(.*?)(?=^  [a-z]+:\n|\Z)" % name, text, re.M | re.S)
    return m.group(1) if m else ""


check("runs when a release is published", re.search(r"^  release:\n    types: \[released\]$", text, re.M))
check("a tag still builds and tests", re.search(r"^    tags: \['v\*'\]$", text, re.M))

for j in ("native", "armhf"):
    m = re.search(r"^    if: (.+)$", job(j), re.M)
    check(j + " has a condition", m)
    if m:
        check(j + ": builds on push", ev(m.group(1), "push", "refs/tags/v9.9.9"))
        check(j + ": a release does not rebuild", not ev(m.group(1), "release", "refs/tags/v9.9.9"))

m = re.search(r"- name: dispatch to schema-init-apt\n\s+if: (.+)\n", job("publish"))
check("publish's dispatch has a condition", m)
if m:
    for event, ref, want, why in (("push", "refs/heads/master", True, "master push -> dev"),
                                  ("push", "refs/tags/v9.9.9", False, "tag push only rehearses"),
                                  ("push", "refs/heads/pi-x", False, "pi-* builds only")):
        check(why, ev(m.group(1), event, ref) is want)
m = re.search(r"^    if: (.+)$", job("promote"), re.M)
check("promote has a condition", m)
if m:
    check("promote runs on a release", ev(m.group(1), "release", "refs/tags/v9.9.9"))
    check("promote does not run on a tag push", not ev(m.group(1), "push", "refs/tags/v9.9.9"))
check("promote reads run lists", "actions: read" in job("promote"))
check("promote calls the script", "tools/deb-promote.sh" in job("promote"))

chan = re.search(r"CHANNEL: \$\{\{ (.+) \}\}", text)
check("CHANNEL is an expression", chan)
if chan:
    check("tag builds for stable", ev(chan.group(1), "x", "refs/tags/v9.9.9") == "stable")
    check("master builds for dev", ev(chan.group(1), "x", "refs/heads/master") == "dev")

# the script, against a fake gh
VER = re.search(r"^Version:\s*(\S+)", open(os.path.join(REPO, "schema-init.spec")).read(), re.M).group(1)
FAKE = """#!/bin/sh
printf '%s token=%s\\n' "$*" "$GH_TOKEN" >> "$FAKE_LOG"
case "$1 $2" in
"run list") printf '%s' "$FAKE_RUN"; exit 0 ;;
esac
exit "${FAKE_API_RC:-0}"
"""


def promote(tag, run="12345", api_rc=0):
    d = tempfile.mkdtemp()
    with open(d + "/gh", "w") as f:
        f.write(FAKE)
    os.chmod(d + "/gh", 0o755)
    env = dict(os.environ, PATH=d + ":" + os.environ["PATH"], FAKE_LOG=d + "/log", FAKE_RUN=run,
               FAKE_API_RC=str(api_rc), GH_TOKEN="read-token", APT_DISPATCH_TOKEN="apt-token")
    r = subprocess.run(["sh", SCRIPT, tag, "abc123"], env=env, capture_output=True, text=True)
    log = open(d + "/log").read() if os.path.exists(d + "/log") else ""
    shutil.rmtree(d)
    return r.returncode, r.stdout + r.stderr, log


if os.path.exists(SCRIPT):
    rc, out, log = promote("v" + VER)
    check("green tag run: exit 0", rc == 0, out)
    check("looks up the tag's own green push run", "--event push" in log and "--branch v" + VER in log
          and "--commit abc123" in log and "--status success" in log, log)
    check("dispatches that run to stable", "client_payload[run_id]=12345" in log and "client_payload[channel]=stable" in log, log)
    check("reads with the job token, dispatches with the apt token",
          re.search(r"run list.*token=read-token", log) and re.search(r"dispatches.*token=apt-token", log), log)
    rc, out, log = promote("v" + VER, run="")
    check("no green tag run: fails", rc != 0)
    check("no green tag run: nothing dispatched", "dispatches" not in log, log)
    check("no green tag run: says what to do", "tag" in out and "green" in out, out)
    for bad in ("V" + VER, "vm-bench-notes", "v" + VER + "-rc1", "v0.0.1"):
        rc, out, log = promote(bad)
        check("tag %s isn't this version: refused" % bad, rc != 0 and "dispatches" not in log, (rc, log))
    rc, out, log = promote("v" + VER, api_rc=1)
    check("failed dispatch fails the job", rc != 0)
else:
    check("tools/deb-promote.sh exists", False)

print("test_deb_workflow: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
