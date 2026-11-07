#!/usr/bin/env python3
# Stable apt only moves when the GitHub release is published, which is after the
# manual release test (ISO install + both wizard flips). A tag only rehearses.
import os, re, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
text = open(os.path.join(REPO, ".github/workflows/deb.yml")).read()
results = []


def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  ok  " if cond else "  FAIL ") + name + ("" if cond else "  " + str(detail)))


check("runs when a release is published", re.search(r"^  release:\n    types: \[released\]$", text, re.M))
check("a tag still builds and tests", re.search(r"^    tags: \['v\*'\]$", text, re.M))

m = re.search(r"- name: dispatch to schema-init-apt\n\s+if: (.+)\n", text)
check("dispatch step has a condition", m)
expr = m.group(1) if m else "False"


class Github:
    def __init__(self, event_name, ref):
        self.event_name, self.ref = event_name, ref


def dispatches(event_name, ref):
    py = expr.replace("&&", " and ").replace("||", " or ")
    return bool(eval(py, {"github": Github(event_name, ref), "startsWith": lambda s, p: s.startswith(p)}))


for event, ref, want, why in (
    ("push", "refs/heads/master", True, "master push -> dev channel"),
    ("push", "refs/tags/v9.9.9", False, "tag push must wait for the release test"),
    ("release", "refs/tags/v9.9.9", True, "published release -> stable"),
    ("release", "refs/tags/other", False, "release on a non-version tag"),
    ("push", "refs/heads/pi-x", False, "pi-* branch builds only"),
):
    got = dispatches(event, ref)
    check(why, got is want, got)

chan = re.search(r"CHANNEL: \$\{\{ (.+) \}\}", text).group(1)
py = chan.replace("&&", " and ").replace("||", " or ").replace("'", '"')
for ref, want in (("refs/tags/v9.9.9", "stable"), ("refs/heads/master", "dev")):
    got = eval(py, {"github": Github("x", ref), "startsWith": lambda s, p: s.startswith(p)})
    check("%s builds for %s" % (ref, want), got == want, got)

print("test_deb_workflow: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
