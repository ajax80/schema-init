#!/usr/bin/env python3
# The ISO's passwordless grant: exact subcommands only, and enough of them that
# the first-boot wizard still works.
import os, re, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INST = os.path.join(REPO, "distros/fedora-installer")
ks = open(os.path.join(INST, "schema.ks")).read()
wiz = open(os.path.join(INST, "firstboot-flip-wizard.sh")).read()
helper = open(os.path.join(INST, "schema-flip-apply.sh")).read()
results = []


def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  ok  " if cond else "  FAIL ") + name + ("" if cond else "  " + str(detail)))


m = re.search(r"^# BEGIN flip grant\n(.*?)^# END flip grant\n", ks, re.M | re.S)
check("kickstart marks its grant block", m)
rule = ""
if m:
    rule = subprocess.run(["sh", "-c", 'FBUSER=alice LIB=/L\n' + m.group(1) + 'flip_grant\n'],
                          capture_output=True, text=True).stdout
grants = [g.strip() for g in rule.split("NOPASSWD:", 1)[-1].split(",")] if rule else []
check("grant is for the login user only", rule.startswith("alice ALL=(root) NOPASSWD: "), rule)
check("every entry pins one subcommand",
      grants and all(re.fullmatch(r"/L/schema-flip-apply [a-z-]+", g) for g in grants), grants)
granted = {g.split()[1] for g in grants}

used = set(re.findall(r"(?:^|[\s($|{;])H ([a-z-]+)", wiz, re.M))
check("wizard's calls were found", {"arm", "check", "reboot", "retire", "retire-reboot"} <= used, used)
check("every wizard call is granted", used <= granted, used - granted)
check("nothing granted the wizard doesn't use", granted <= used, granted - used)

cases = set(re.findall(r"^\s+([a-z-]+)\)", helper, re.M))
for line in re.findall(r"^\s+(dbus-[a-z|-]+)\)", helper, re.M):
    cases |= set(line.split("|"))
check("helper handles every grant", granted <= cases, granted - cases)

check("wizard probes with an argument the grant allows",
      re.search(r'sudo -n -l "\$h" root-state', wiz), "bare sudo -l cannot match a pinned rule")

fn = re.search(r"^retire_grant\(\)\s*\{(.*?)\}", helper, re.M)
check("retire_grant removes grant and autostart", fn and "$SUDOERS" in fn.group(1) and "$SYS_AUTOSTART" in fn.group(1)
      and "SUDOERS=/etc/sudoers.d/schema-flip" in helper, helper[:0])
for c in ("retire", "retire-reboot"):
    body = helper.split("    %s)" % c, 1)[-1].split(";;", 1)[0]
    check(c + " calls retire_grant", "retire_grant" in body, body)
check("wizard never waits at a sudo prompt", re.search(r'^H\(\) \{ sudo -n "\$HELPER"', wiz, re.M))
resolve = helper.split("    resolve)", 1)[-1].split(";;", 1)[0]
check("resolve keeps the grant: the wizard stays re-runnable", "SUDOERS" not in resolve, resolve)

# a sudo call after retiring would prompt with no terminal and fail
fc = re.search(r"^finish_clean\(\)\s*\{(.*?)\}", wiz, re.M).group(1)
check("finish_clean retires", "H retire" in fc, fc)
lines = wiz.splitlines()
for i, l in enumerate(lines):
    if re.search(r"\bfinish_clean\b", l) and "()" not in l and "exit 0" not in l:
        after = []
        for n in lines[i + 1:]:
            if re.match(r"\s*(elif\b|else\b|fi\b|;;)", n):
                break
            after.append(n)
        after = "\n".join(after)
        check("no sudo after finish_clean (line %d)" % (i + 1), not re.search(r"\bH [a-z]", after), after)

print("test_ks_flip_sudoers: %d/%d" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
