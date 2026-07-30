# Service Hardening Phase 2 (chrony) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Harden `chronyd` under schema-init to a right-sized 5-capability bounding set (`.svc`-only), and fix the livetest harness so it can no longer false-green on a self-privdropping daemon.

**Architecture:** chronyd keeps doing its own privdrop (root → create/chown `/run/chrony` → `setuid` to `chrony`, retaining `CAP_SYS_TIME`). We stop trimming the bounding set below what that privdrop needs. The harness gains a static `test_privdrop` helper that replays chronyd's exact privileged syscall sequence under the trimmed set, so a too-tight cap list makes the VM test go red instead of green. No init/parser code changes — Phase-1 machinery is sufficient.

**Tech Stack:** C (raw Linux syscalls, `-static`, no libcap), bash livetest harness under QEMU/KVM, schema-init `.svc` files.

## Global Constraints

- No changes to `service.c`, `caps.c`, `init.c`, `service.h`, or the `.svc` parser. `.svc` + harness only.
- The chrony bounding set is exactly: `CAP_SYS_TIME,CAP_NET_BIND_SERVICE,CAP_CHOWN,CAP_SETUID,CAP_SETGID`. Bit values: CHOWN=0, SETGID=6, SETUID=7, NET_BIND=10, SYS_TIME=25. Mask = `0x00000000020004C1`; `/proc/<pid>/status` prints `CapBnd: 00000000020004c1`.
- chrony runtime identity: uid/gid `996` (`getent passwd chrony` → `chrony:x:996:996`).
- The livetest initramfs has **no libc** — every added binary MUST be built `-static`.
- The helper MUST treat `EADDRINUSE` on `bind(:123)` as success (permission check already passed); only `EACCES`/`EPERM` is a capability failure. This lets the host-side test run while the real chronyd may hold `:123`.
- `schema-ctl reload` refuses modified `.svc` files; the chrony change takes effect only on a fresh boot. Hardware verification is a reboot with the existing rollback net, not a hot-reload.
- Spec: `docs/superpowers/specs/2026-07-30-service-hardening-phase2-chronyd-design.md`.

---

## File Structure

- **Create** `tests/livetest/test_privdrop.c` — static helper that replays chronyd's privdrop under the caps schema-init hands it. One responsibility: prove the 5-cap set permits the full self-privdrop, or exit nonzero.
- **Modify** `tests/livetest/vmtest.sh` — build+stage the helper; replace the false-greening `test-chrony.svc` (`/bin/sleep`) with `test-privdrop.svc`; swap the report probes and assertions.
- **Modify** `services/chronyd.svc` — add `no_new_privs=1` + the 5-cap `keep_caps` (the shipped deliverable).

All three land in one PR on branch `service-hardening-phase2-chrony`.

---

### Task 1: The `test_privdrop` helper

**Files:**
- Create: `tests/livetest/test_privdrop.c`

**Interfaces:**
- Consumes: nothing (freestanding binary). At runtime schema-init hands it `CapEff = CapBnd = CapPrm = 0x20004C1` and `NoNewPrivs=1`, running as uid 0.
- Produces: a binary `/bin/test_privdrop` (staged by Task 2) that, on full success, is running as uid 996 with `CapEff = 0x2000000`, has written `PRIVDROP_OK` into `/run/chrony-test/ok`, and is idling in `pause()`. On any cap-gated failure it writes `PRIVDROP_FAIL step=<x> errno=<n>` to fd 2 and `_exit(1)`.

- [ ] **Step 1: Create the feature branch**

```bash
cd ~/projects/schema-init
git checkout -b service-hardening-phase2-chrony
```

- [ ] **Step 2: Write the helper**

Create `tests/livetest/test_privdrop.c`:

```c
/* test_privdrop — livetest helper. Replays chronyd's real self-privdrop under
 * whatever capability set schema-init hands us, so the vmtest cannot false-green
 * the way the Phase-1 /bin/sleep test-chrony.svc did. Static, no libcap, no
 * /etc/passwd (numeric uid). Exits 0-path by idling in pause() so the harness
 * can inspect /proc/<pid>/status while it is still alive in its cgroup. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <grp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <netinet/in.h>
#include <linux/capability.h>

#define CHRONY_UID 996
#define CHRONY_GID 996
#define RUNDIR     "/run/chrony-test"

static void fail(const char *step) {
    dprintf(2, "PRIVDROP_FAIL step=%s errno=%d\n", step, errno);
    _exit(1);
}

/* Lower permitted to exactly CAP_SYS_TIME and raise it into effective. After a
 * setuid() to non-root with PR_SET_KEEPCAPS, permitted survives but effective is
 * cleared; this restores CAP_SYS_TIME the way chronyd's libcap call does. */
static int retain_sys_time(void) {
    struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct data[2];
    unsigned long long mask = 1ULL << CAP_SYS_TIME;
    memset(data, 0, sizeof(data));
    data[0].effective = data[0].permitted = data[0].inheritable =
        (unsigned)(mask & 0xffffffffu);
    data[1].effective = data[1].permitted = data[1].inheritable =
        (unsigned)(mask >> 32);
    return syscall(SYS_capset, &hdr, data);
}

int main(void) {
    if (geteuid() != 0) fail("not-root");

    /* 1. bind a privileged NTP port -> CAP_NET_BIND_SERVICE. EADDRINUSE means
     *    the permission check already passed (real chronyd may hold :123). */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) fail("socket");
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(123);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 &&
        (errno == EACCES || errno == EPERM))
        fail("bind123");

    /* 2. create + chown the runtime dir -> CAP_CHOWN. */
    mkdir(RUNDIR, 0750);                 /* EEXIST is fine */
    if (chown(RUNDIR, CHRONY_UID, CHRONY_GID) != 0) fail("chown");

    /* 3. drop to the chrony user, keeping caps across the transition. */
    if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) != 0) fail("keepcaps");
    if (setgroups(0, NULL) != 0) fail("setgroups");   /* CAP_SETGID */
    if (setgid(CHRONY_GID) != 0) fail("setgid");      /* CAP_SETGID */
    if (setuid(CHRONY_UID) != 0) fail("setuid");      /* CAP_SETUID */

    /* 4. restore CAP_SYS_TIME into effective, now that we are non-root. */
    if (retain_sys_time() != 0) fail("capset");

    /* 5. prove CAP_SYS_TIME is effective post-drop: a zero-offset clock set is
     *    cap-gated but harmless. adjtimex returns clock state (>=0) or -1. */
    struct timex tx;
    memset(&tx, 0, sizeof(tx));
    tx.modes = ADJ_OFFSET;
    tx.offset = 0;
    if (adjtimex(&tx) == -1 && errno == EPERM) fail("adjtimex");

    /* 6. sentinel written as uid 996 into the dir we now own -> proves we ran
     *    the whole chain to the end. The harness greps this out of serial. */
    int fd = open(RUNDIR "/ok", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) fail("sentinel-open");
    if (write(fd, "PRIVDROP_OK\n", 12) != 12) fail("sentinel-write");
    close(fd);

    for (;;) pause();   /* stay alive in the cgroup for /proc inspection */
    return 0;
}
```

- [ ] **Step 3: Compile it and run the positive control (full root)**

Run:
```bash
cd ~/projects/schema-init/tests/livetest
cc -static -O2 -std=c11 -D_GNU_SOURCE -Wall -Wextra -o /tmp/test_privdrop test_privdrop.c
sudo rm -rf /run/chrony-test
sudo timeout 3 /tmp/test_privdrop; echo "exit=$?"
cat /run/chrony-test/ok 2>&1
```
Expected: compiles clean; `timeout` kills the idling process (`exit=124`); `/run/chrony-test/ok` contains `PRIVDROP_OK`. This proves the syscall sequence itself is correct when all caps are present.

- [ ] **Step 4: Run the negative control (prove it can go red)**

Run (drop `CAP_SETUID` from the effective set via capsh, so the helper's `setuid` must EPERM):
```bash
sudo rm -rf /run/chrony-test
sudo capsh --caps="cap_chown,cap_net_bind_service,cap_setgid,cap_sys_time+eip" \
  --keep=1 -- -c '/tmp/test_privdrop' ; echo "exit=$?"
cat /run/chrony-test/ok 2>&1
```
Expected: prints `PRIVDROP_FAIL step=setuid errno=1`, `exit=1`, and `/run/chrony-test/ok` does **not** exist (`cat: ... No such file or directory`). This is the anti-false-green property: a too-tight cap set makes the helper fail loudly instead of idling green. (If `capsh` behaves differently on this build, the authoritative negative proof is Task 2's temporary-red step; note it and move on.)

- [ ] **Step 5: Clean up and commit the helper**

```bash
sudo rm -rf /run/chrony-test /tmp/test_privdrop
cd ~/projects/schema-init
git add tests/livetest/test_privdrop.c
git commit -m "test(livetest): static privdrop helper for self-privdropping daemons

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Wire the helper into the vmtest and replace the false-greening test

**Files:**
- Modify: `tests/livetest/vmtest.sh` (build/stage ~line 33; svc block lines 111-119; report lines 236-238; assertions lines 318-321)

**Interfaces:**
- Consumes: `tests/livetest/test_privdrop.c` from Task 1; the runtime facts from its Produces block (uid 996, `CapEff=0x2000000`, `CapBnd=0x20004C1`, sentinel `/run/chrony-test/ok`).
- Produces: a vmtest that fails unless a genuine PID-1 boot drives `test_privdrop` through its full privdrop under the 5-cap set.

- [ ] **Step 1: Build and stage the helper in the initramfs**

In `tests/livetest/vmtest.sh`, immediately after the schema-init binary check (the `[ -x "$BIN" ] || ...` line, ~line 33), add:

```bash
# Static privdrop helper (initramfs has no libc -> must be -static). Exercises
# chronyd's real self-privdrop so the cap set is validated, not just asserted.
PRIVDROP="$WORK/test_privdrop"
cc -static -O2 -std=c11 -D_GNU_SOURCE -o "$PRIVDROP" "$HERE/test_privdrop.c" \
  || { echo "PRIVDROP HELPER BUILD FAILED"; cc -static -std=c11 -D_GNU_SOURCE -o "$PRIVDROP" "$HERE/test_privdrop.c"; exit 1; }
```

Then in the initramfs assembly (just after the `ln -sf /bin/busybox "$ROOT/bin/sleep"` line, ~line 44), add:

```bash
cp "$PRIVDROP" "$ROOT/bin/test_privdrop"
```

- [ ] **Step 2: Replace the `test-chrony.svc` block with `test-privdrop.svc`**

Delete the existing block (lines 111-119, the `# chrony live-pilot flavor:` comment through its `EOF`) and replace with:

```bash
# Phase 2 chrony hardening: the REAL self-privdrop path. test_privdrop replays
# chronyd's exact privileged sequence (bind :123 -> chown /run -> setgid/setuid
# -> retain CAP_SYS_TIME) under the 5-cap keep set. A /bin/sleep here would
# false-green -- it exercises none of those -- which was exactly the Phase-1 gap.
cat > "$ROOT/etc/schema-init/services/test-privdrop.svc" <<'EOF'
name=test-privdrop
exec=/bin/test_privdrop
needs_root=1
no_new_privs=1
keep_caps=CAP_SYS_TIME,CAP_NET_BIND_SERVICE,CAP_CHOWN,CAP_SETUID,CAP_SETGID
EOF
```

- [ ] **Step 3: Replace the chrony report probes**

In `vmfinish.sh`'s CPUSET-REPORT section, delete the three `CHRONY_PID` / `chrony-nnp` / `chrony-capbnd` lines (236-238) and replace with:

```bash
PRIVDROP_PID=$(head -1 /sys/fs/cgroup/schema-init/test-privdrop/cgroup.procs 2>/dev/null)
echo "privdrop-result: $(cat /run/chrony-test/ok 2>&1)"
echo "privdrop-uid: $(grep -E '^Uid:' /proc/$PRIVDROP_PID/status 2>&1)"
echo "privdrop-nnp: $(grep NoNewPrivs /proc/$PRIVDROP_PID/status 2>&1)"
echo "privdrop-capbnd: $(grep CapBnd /proc/$PRIVDROP_PID/status 2>&1)"
echo "privdrop-capeff: $(grep CapEff /proc/$PRIVDROP_PID/status 2>&1)"
```

- [ ] **Step 4: Replace the chrony assertions**

Delete the chrony assertion block (lines 318-321, the `# chrony pilot:` comment and its two `grep -Eq "chrony-...` lines) and replace with:

```bash
# Phase 2 chrony: the self-privdrop actually SUCCEEDS under the 5-cap keep set.
# Anti-false-green: test_privdrop writes /run/chrony-test/ok and reaches uid 996
# only if every cap-gated step (bind/chown/setgid/setuid/adjtimex) is permitted.
grep -Eq "privdrop-result: PRIVDROP_OK"                 "$SERIAL" || { echo "  MISS: privdrop helper did not complete (cap set too tight?)"; pass=0; }
grep -Eq "privdrop-uid:.*Uid:[[:space:]]*996"           "$SERIAL" || { echo "  MISS: privdrop helper did not drop to uid 996"; pass=0; }
grep -Eq "privdrop-nnp:.*NoNewPrivs:[[:space:]]*1"      "$SERIAL" || { echo "  MISS: privdrop NoNewPrivs != 1"; pass=0; }
grep -Eq "privdrop-capbnd:.*CapBnd:[[:space:]]*00000000020004c1" "$SERIAL" || { echo "  MISS: privdrop CapBnd != 5-cap set"; pass=0; }
grep -Eq "privdrop-capeff:.*CapEff:[[:space:]]*0000000002000000" "$SERIAL" || { echo "  MISS: privdrop CapEff != CAP_SYS_TIME after drop"; pass=0; }
```

- [ ] **Step 5: Prove the assertions are live (temporary red)**

Before trusting the green, confirm the new assertions can actually fail. Make a targeted, reversible one-line edit that points the test svc at `/bin/sleep` (which drives no privdrop), run once, then reverse the exact same edit — leaving all your real Task-2 edits intact:
```bash
cd ~/projects/schema-init/tests/livetest
# forward: swap only the exec line of test-privdrop.svc to /bin/sleep
sed -i 's|^exec=/bin/test_privdrop$|exec=/bin/sleep|' vmtest.sh
./vmtest.sh 2>&1 | tail -30
# reverse: put it back exactly
sed -i 's|^exec=/bin/sleep$|exec=/bin/test_privdrop|' vmtest.sh
git diff --stat tests/livetest/vmtest.sh   # confirm ONLY your intended edits remain
```
Expected from the run: `RESULT: FAIL` with `MISS: privdrop helper did not complete (cap set too tight?)` and `MISS: privdrop helper did not drop to uid 996` — a `/bin/sleep` neither writes the sentinel nor drops to uid 996. The reverse `sed` restores the helper exec line; `git diff --stat` should show `vmtest.sh` changed only by your Task-2 edits (the two `sed`s cancel out). The point is only to witness red once.

Caveat: the forward `sed` targets `test-privdrop.svc`'s exec line only. `/bin/sleep` with no `args=` exits immediately, but its absence from the cgroup still trips the two MISS assertions — that is the red we want. Do not add `args=600`; the empty-cgroup path is a valid failure demonstration.

- [ ] **Step 6: Run the vmtest for real (green)**

Run:
```bash
cd ~/projects/schema-init/tests/livetest
./vmtest.sh 2>&1 | tail -30
```
Expected: `>> RESULT: PASS`. In the serial tail, `privdrop-result: PRIVDROP_OK`, `privdrop-uid: Uid: 996 996 996 996`, `privdrop-capbnd: CapBnd: 00000000020004c1`, `privdrop-capeff: CapEff: 0000000002000000`. This proves the 5-cap set permits a real self-privdrop under genuine PID-1 boot — the property the pilot's `/bin/sleep` test could not check.

- [ ] **Step 7: Commit the harness change**

```bash
cd ~/projects/schema-init
git add tests/livetest/vmtest.sh
git commit -m "test(livetest): drive real privdrop, replace false-greening chrony sleep test

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Harden the real `chronyd.svc` and verify on hardware

**Files:**
- Modify: `services/chronyd.svc`

**Interfaces:**
- Consumes: the 5-cap set proven sufficient by Task 2's green vmtest.
- Produces: the shipped hardened `chronyd.svc`. Verified live by a reboot + `chronyc tracking`.

- [ ] **Step 1: Edit `services/chronyd.svc`**

Add two lines after `critical=0` so the file reads exactly:

```
name=chronyd
exec=/usr/sbin/chronyd
args=-d
dep=network-manager
needs_root=1
critical=0
no_new_privs=1
keep_caps=CAP_SYS_TIME,CAP_NET_BIND_SERVICE,CAP_CHOWN,CAP_SETUID,CAP_SETGID
```

- [ ] **Step 2: Commit the svc change**

```bash
cd ~/projects/schema-init
git add services/chronyd.svc
git commit -m "service(chrony): harden to 5-cap bounding set + no_new_privs (phase 2)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 3: Stage on the live box (deployed .svc, backup first)**

The deployed copy lives at `/etc/schema-init/services/chronyd.svc` and takes effect at next boot only.
```bash
sudo cp /etc/schema-init/services/chronyd.svc /etc/schema-init/services/chronyd.svc.pre-phase2
sudo cp ~/projects/schema-init/services/chronyd.svc /etc/schema-init/services/chronyd.svc
sudo diff /etc/schema-init/services/chronyd.svc.pre-phase2 /etc/schema-init/services/chronyd.svc
```
Expected diff: only the two added lines.

- [ ] **Step 4: Confirm the rollback net before rebooting**

Verify the GRUB fallback and the preharden binary still exist (same net used last session):
```bash
ls -l /usr/bin/schema-init.bak-20260730-preharden
sudo sha256sum /proc/1/exe   # note the current running binary hash
```
Rollback if boot is unhappy: at GRUB, `e` → change `init=/sbin/schema-init` to `init=/usr/bin/schema-init.bak-20260730-preharden` → Ctrl-X. Recovery net: getty-tty2 autologin root. To revert the svc from a recovery shell: `cp /etc/schema-init/services/chronyd.svc.pre-phase2 /etc/schema-init/services/chronyd.svc` then reboot.

- [ ] **Step 5: Reboot and verify chrony synced under the hardened svc**

Reboot (Jonathan issues this). After boot:
```bash
pgrep -x chronyd && echo ALIVE
chronyc tracking | grep -E 'Reference ID|Leap status'      # expect Leap status: Normal
ps -o pid,user,args -p "$(pgrep -x chronyd)"               # USER must be chrony, not root
sudo grep -E 'NoNewPrivs|CapBnd|CapEff' /proc/"$(pgrep -x chronyd)"/status
```
Expected: chronyd alive as user `chrony`; `Leap status: Normal` (synced); `NoNewPrivs: 1`; `CapBnd: 00000000020004c1`; `CapEff` shows only `CAP_SYS_TIME` (`0000000002000000`) — chrony self-dropped everything else. If chronyd is DORMANT/crash-looping, the cap set is still wrong: capture the service log (`/var/log/schema-init/chronyd.log`), roll back via Step 4, and reopen the cap analysis.

- [ ] **Step 6: Finish the branch**

Once hardware-verified, integrate per `superpowers:finishing-a-development-branch` (merge to master / open PR). Do not merge before the reboot verification passes.

---

## Notes carried from the spec

- **Optional tightening (later, not this PR):** after green, one in-VM pass can test dropping `CAP_NET_BIND_SERVICE` (client-only chrony may use ephemeral ports). Keep whatever the VM proves.
- **Coupling:** if `/etc/chrony.conf` later gains `lock_all` (needs `CAP_IPC_LOCK`) or scheduling directives (needs `CAP_SYS_NICE`), widen `keep_caps` accordingly. Current conf has only `driftfile`, so the 5-cap set is complete.
- **Default-flip stays gated** on its own later PR + full-fleet vmtest sweep. This PR only proves the pattern for a self-privdropping daemon and closes the harness false-green.
