#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(REPO, 'scripts', 'schema-mount-fstab')

FAKES = {
    'blkid': '''#!/bin/sh
case "$*" in
  *"-s TYPE"*) echo ext4 ;;
  *LABEL=*) for a; do l=$a; done; echo "/dev/fake-${l#LABEL=}" ;;
esac
''',
    'fsck': '''#!/bin/sh
echo "fsck $*" >> "$T/calls"
rc=$(head -1 "$T/rcs" 2>/dev/null); sed -i 1d "$T/rcs" 2>/dev/null
exit ${rc:-0}
''',
    'fsck.ext4': '#!/bin/sh\n',
    'mount': '#!/bin/sh\necho "mount $*" >> "$T/calls"\n',
    'findmnt': '#!/bin/sh\nexit 1\n',
    'sulogin': '#!/bin/sh\necho sulogin >> "$T/calls"\n',
    'setsid': '#!/bin/sh\nshift 2\nexec "$@"\n',
    'plymouth': '#!/bin/sh\n',
}

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))


def run(fstab_line, rcs=()):
    t = tempfile.mkdtemp(prefix='schema-fsck-')
    try:
        b = os.path.join(t, 'bin')
        os.mkdir(b)
        for n, body in FAKES.items():
            p = os.path.join(b, n)
            open(p, 'w').write(body)
            os.chmod(p, 0o755)
        open(os.path.join(t, 'rcs'), 'w').write(''.join(f'{r}\n' for r in rcs))
        open(os.path.join(t, 'fstab'), 'w').write(fstab_line.replace('@', t) + '\n')
        env = dict(os.environ, T=t, PATH=b + ':' + os.environ['PATH'],
                   SCHEMA_FSTAB=os.path.join(t, 'fstab'), SCHEMA_CONSOLE=os.path.join(t, 'console'))
        out = subprocess.run(['sh', SCRIPT], env=env, capture_output=True, text=True, timeout=30).stdout
        cp = os.path.join(t, 'calls')
        calls = open(cp).read().splitlines() if os.path.exists(cp) else []
        return out, calls
    finally:
        shutil.rmtree(t, ignore_errors=True)


def mounted(calls):
    return any(c.startswith('mount ') for c in calls)


out, calls = run('LABEL=d @/m ext4 defaults 0 0')
check('pass 0: no fsck, mounted', not any(c.startswith('fsck') for c in calls) and mounted(calls), calls)

out, calls = run('LABEL=d @/m ext4 defaults 0 2', [0])
check('pass 2 clean: fsck -a on the device, then mounted',
      calls[:1] == ['fsck -a -T -t ext4 /dev/fake-d'] and mounted(calls), calls)

out, calls = run('LABEL=d @/m auto defaults 0 2', [0])
check('type auto: blkid type passed to fsck', calls[:1] == ['fsck -a -T -t ext4 /dev/fake-d'], calls)

out, calls = run('LABEL=d @/m ext4 defaults 0 2', [1])
check('fsck fixed (1): mounted, logged', mounted(calls) and 'fixed errors' in out, out)

out, calls = run('LABEL=d @/m ext4 defaults,nofail 0 2', [4])
check('nofail, unfixable (4): not mounted, no shell',
      not mounted(calls) and 'sulogin' not in calls and 'not mounted' in out, calls)

out, calls = run('LABEL=d @/m ext4 defaults 0 2', [4, 0])
check('unfixable, repaired in shell: sulogin, recheck, mounted',
      'sulogin' in calls and calls.count('fsck -a -T -t ext4 /dev/fake-d') == 2 and mounted(calls)
      and 'clean now' in out, calls)

out, calls = run('LABEL=d @/m ext4 defaults 0 2', [4, 4])
check('unfixable after shell: not mounted, boot goes on',
      'sulogin' in calls and not mounted(calls) and 'still fails' in out and 'done, with failures' in out, out)

out, calls = run('LABEL=d @/m ext4 defaults 0 2', [4, 8])
check('recheck failing (8) after shell: not mounted', not mounted(calls) and 'still fails (8)' in out, out)

out, calls = run('LABEL=d @/m nosuchfs defaults 0 2')
check('no fsck.nosuchfs: not checked, mounted', not any(c.startswith('fsck') for c in calls)
      and mounted(calls) and 'not checked' in out, out)

print(f"{sum(results)}/{len(results)} passed")
sys.exit(0 if all(results) else 1)
