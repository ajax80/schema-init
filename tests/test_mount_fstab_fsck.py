#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(REPO, 'scripts', 'schema-mount-fstab')

FAKES = {
    'blkid': '''#!/bin/sh
case "$*" in
  *LABEL=missing*) ;;
  *"-s TYPE"*) echo "${FAKE_TYPE:-ext4}" ;;
  *LABEL=*) for a; do l=$a; done; echo "/dev/fake-${l#LABEL=}" ;;
esac
''',
    'fsck': '''#!/bin/sh
echo "fsck $*" >> "$T/calls"
[ -z "$FAKE_FSCK_SLEEP" ] || { echo "start $(date +%s.%N)" >> "$T/times"; sleep "$FAKE_FSCK_SLEEP"; echo "end $(date +%s.%N)" >> "$T/times"; }
rc=$(head -1 "$T/rcs" 2>/dev/null); sed -i 1d "$T/rcs" 2>/dev/null
exit ${rc:-0}
''',
    'fsck.ext4': '#!/bin/sh\n',
    'fsck.vfat': '#!/bin/sh\n',
    'fsck.msdos': '#!/bin/sh\n',
    'mount': '#!/bin/sh\necho "mount $*" >> "$T/calls"\n',
    'findmnt': '#!/bin/sh\nexit 1\n',
    'sulogin': '#!/bin/sh\necho sulogin >> "$T/calls"\n',
    'setsid': '#!/bin/sh\nshift 2\nexec "$@"\n',
    'plymouth': '#!/bin/sh\n',
    'modprobe': '#!/bin/sh\necho "modprobe $*" >> "$T/calls"\n',
    'btrfs': '''#!/bin/sh
echo "btrfs $*" >> "$T/calls"
[ "$2" = ready ] || exit 0
n=$(($(cat "$T/ready" 2>/dev/null || echo 0) + 1)); echo $n > "$T/ready"
[ $n -ge ${FAKE_READY_AFTER:-1} ]
''',
}

results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''))


def run(fstab_line, rcs=(), fat=None, cmdline='', ftype='ext4', ready_after=1, fsck_sleep='', tmpdir=None):
    t = tempfile.mkdtemp(prefix='schema-fsck-')
    try:
        b = os.path.join(t, 'bin')
        os.mkdir(b)
        for n, body in FAKES.items():
            p = os.path.join(b, n)
            open(p, 'w').write(body)
            os.chmod(p, 0o755)
        if fat is not None:
            sz16, flag, sig, fat1 = fat
            bs = bytearray(1024)
            bs[11:13] = (512).to_bytes(2, 'little')
            bs[14:16] = (1).to_bytes(2, 'little')
            bs[22:24] = sz16.to_bytes(2, 'little')
            o = 37 if sz16 else 65
            bs[o], bs[o + 1] = flag, sig
            w = 2 if sz16 else 4
            bs[512 + w:512 + 2 * w] = fat1.to_bytes(w, 'little')
            open(os.path.join(t, 'fat'), 'wb').write(bs)
        open(os.path.join(t, 'cmdline'), 'w').write(cmdline + '\n')
        open(os.path.join(t, 'rcs'), 'w').write(''.join(f'{r}\n' for r in rcs))
        open(os.path.join(t, 'fstab'), 'w').write(fstab_line.replace('@', t) + '\n')
        env = dict(os.environ, T=t, PATH=b + ':' + os.environ['PATH'],
                   SCHEMA_FSTAB=os.path.join(t, 'fstab'), SCHEMA_CONSOLE=os.path.join(t, 'console'),
                   SCHEMA_CMDLINE=os.path.join(t, 'cmdline'), FAKE_TYPE=ftype,
                   FAKE_READY_AFTER=str(ready_after), FAKE_FSCK_SLEEP=fsck_sleep,
                   TMPDIR=tmpdir or t)
        t0 = time.monotonic()
        out = subprocess.run(['sh', SCRIPT], env=env, capture_output=True, text=True, timeout=30).stdout
        global elapsed, leftover, times
        elapsed = time.monotonic() - t0
        leftover = [n for n in os.listdir(t) if n.startswith('schema-fsck.')]
        tp = os.path.join(t, 'times')
        times = open(tp).read().splitlines() if os.path.exists(tp) else []
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

fsck_called = lambda calls: any(c.startswith('fsck') for c in calls)
F32, F16 = 0x0FFFFFFF, 0xFFFF
cases = [
    ('FAT32 clean: no fsck', (0, 0, 0x29, F32), False),
    ('FAT32 dirty bit: fsck', (0, 1, 0x29, F32), True),
    ('FAT32 surface-scan bit: fsck', (0, 2, 0x29, F32), True),
    ('FAT32 dirty among other flags: fsck', (0, 0x41, 0x29, F32), True),
    ('FAT32 no extended BPB signature: fsck', (0, 0, 0, F32), True),
    ('FAT32 FAT[1] clean-shutdown bit clear (Windows): fsck', (0, 0, 0x29, F32 & ~(1 << 27)), True),
    ('FAT32 FAT[1] hard-error bit clear: fsck', (0, 0, 0x29, F32 & ~(1 << 26)), True),
    ('FAT16 clean: no fsck', (200, 0, 0x29, F16), False),
    ('FAT16 dirty bit: fsck', (200, 1, 0x29, F16), True),
    ('FAT16 FAT[1] clean-shutdown bit clear: fsck', (200, 0, 0x29, F16 & ~(1 << 15)), True),
    ('FAT16 FAT[1] hard-error bit clear: fsck', (200, 0, 0x29, F16 & ~(1 << 14)), True),
]
for name, fat, want in cases:
    out, calls = run('@/fat @/m vfat defaults 0 2', fat=fat)
    check(name, fsck_called(calls) == want and mounted(calls), calls)

out, calls = run('@/fat @/m vfat defaults 0 2', fat=(0, 0, 0x29, F32), cmdline='ro forcefsck')
check('FAT32 clean, forcefsck: fsck -f', any(c.startswith('fsck -a -T -f') for c in calls), calls)

out, calls = run('@/fat @/m vfat defaults 0 2', fat=(0, 0, 0x29, F32), cmdline='fsck.mode=skip')
check('fsck.mode=skip: no fsck', not fsck_called(calls) and mounted(calls), calls)

out, calls = run('@/fat @/m auto defaults 0 2', fat=(0, 0, 0x29, F32), ftype='vfat')
check('type auto -> vfat, clean: no fsck', not fsck_called(calls) and 'unmounted cleanly' in out, out)

out, calls = run('@/fat @/m msdos defaults 0 2', fat=(0, 1, 0x29, F32))
check('msdos dirty: fsck', fsck_called(calls), calls)

out, calls = run('@/nofile @/m vfat defaults 0 2')
check('FAT unreadable: fsck', fsck_called(calls), calls)

btrfs_calls = lambda calls: [c for c in calls if c.startswith('btrfs')]

out, calls = run('LABEL=d @/m btrfs defaults 0 0')
check('btrfs: scan, ready, then mounted',
      btrfs_calls(calls) == ['btrfs device scan', 'btrfs device ready /dev/fake-d'] and mounted(calls)
      and calls.index('btrfs device ready /dev/fake-d') < [i for i, c in enumerate(calls) if c.startswith('mount')][0], calls)

out, calls = run('LABEL=d @/m btrfs defaults 0 0', ready_after=3)
check('btrfs member late: scanned until ready, then mounted',
      btrfs_calls(calls).count('btrfs device ready /dev/fake-d') == 3 and mounted(calls), calls)

out, calls = run('LABEL=d @/m btrfs defaults,x-systemd.device-timeout=1 0 0', ready_after=10**6)
check('btrfs member never appears: logged after the timeout, mount still tried',
      'not all devices' in out and mounted(calls) and 1 <= btrfs_calls(calls).count('btrfs device scan') <= 2, out)

out, calls = run('LABEL=d @/m btrfs defaults 0 0')
check('btrfs: module loaded before the scan', calls.index('modprobe -q btrfs') < calls.index('btrfs device scan'), calls)

out, calls = run('@/img @/m btrfs loop 0 0')
check('btrfs loop entry: no wait, mounted', not btrfs_calls(calls) and mounted(calls), calls)

out, calls = run('@/fat @/m btrfs defaults 0 0', fat=(0, 0, 0x29, 0x0FFFFFFF))
check('btrfs on a regular file: no wait, mounted', not btrfs_calls(calls) and mounted(calls), calls)

out, calls = run('LABEL=d @/m btrfs defaults,degraded 0 0', ready_after=10**6)
check('btrfs degraded: scanned once, mounted without waiting',
      btrfs_calls(calls) == ['btrfs device scan'] and mounted(calls), calls)

out, calls = run('LABEL=d @/m auto defaults 0 0', ftype='btrfs')
check('type auto -> btrfs: waits for ready', 'btrfs device ready /dev/fake-d' in calls and mounted(calls), calls)

out, calls = run('LABEL=d @/m ext4 defaults 0 0')
check('ext4: no btrfs calls', not btrfs_calls(calls) and mounted(calls), calls)

three = 'LABEL=a @/a ext4 defaults 0 2\nLABEL=b @/b ext4 defaults 0 2\nLABEL=c @/c ext4 defaults 0 2'
out, calls = run(three, fsck_sleep='1')
starts = sorted(float(x.split()[1]) for x in times if x.startswith('start'))
ends = sorted(float(x.split()[1]) for x in times if x.startswith('end'))
check('three devices: fscks overlap, all mounted',
      len(starts) == 3 and starts[-1] < ends[0] and sum(c.startswith('mount') for c in calls) == 3
      and elapsed < 2.5, f'{elapsed:.2f}s {times}')
check('pre-pass dir removed', leftover == [], leftover)
check('mounts keep fstab order',
      [c.split()[-1][-2:] for c in calls if c.startswith('mount')] == ['/a', '/b', '/c'], calls)

out, calls = run(three, fsck_sleep='1', tmpdir='/nonexistent')
check('no temp dir: sequential fallback, all checked and mounted',
      sum(c.startswith('fsck') for c in calls) == 3 and sum(c.startswith('mount') for c in calls) == 3
      and elapsed >= 3, f'{elapsed:.2f}s')

out, calls = run('LABEL=d @/m ext4 defaults 0 2\nLABEL=d @/m/sub ext4 defaults 0 2')
check('same spec twice: one fsck', sum(c.startswith('fsck') for c in calls) == 1
      and sum(c.startswith('mount') for c in calls) == 2, calls)

out, calls = run('LABEL=missing @/m ext4 defaults,x-systemd.device-timeout=2 0 2')
check('device never appears: one timeout, not two',
      'not present after 2s' in out and not mounted(calls) and elapsed < 3.5, f'{elapsed:.2f}s {out}')

out, calls = run('LABEL=missing @/m ext4 defaults,nofail,x-systemd.device-timeout=2 0 2\nLABEL=d @/n ext4 defaults 0 2',
                 fsck_sleep='2')
check('missing nofail device waits alongside the other fsck',
      'nofail, continuing' in out and mounted(calls) and elapsed < 3.5, f'{elapsed:.2f}s')

print(f"{sum(results)}/{len(results)} passed")
sys.exit(0 if all(results) else 1)
