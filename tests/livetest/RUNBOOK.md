# schema-init live-boot test — run after Brad & Lisa leave

Goal: one reboot validates BOTH merged/pending features, then merge PR #8.
- PR #7 (timers) is already MERGED to master.
- PR #8 (start_timeout) is OPEN on `master`, MERGEABLE/CLEAN — merge only AFTER this passes.

## FAST PATH: VM harness (no hardware reboot) — `./vmtest.sh`
De-risk the hardware reboot first. `~/schema-livetest/vmtest.sh` builds the
current branch, packs a minimal initramfs (static busybox + the 3 test svcs +
a reporter), and boots it with the SAME host kernel under QEMU/KVM as
`rdinit=/sbin/schema-init` — a true PID-1 boot rail, ~2 min, no reboot.
- Run: `cd ~/schema-livetest && ./vmtest.sh`   (REBUILD=0 to skip rebuild, TIMEOUT=N)
- PASS prints when the boot rail shows: `test-timer timer-fire`+`timer-done`,
  `test-hang start-timeout`, and `test-dependent spawn/oneshot-done`.
- Serial always saved to `last-vmtest-serial.log`.
- Gotchas baked in: kernel needs `/init` + `rdinit=` (not `init=`); service
  stdout goes to per-service logfiles, so the reporter forces `>/dev/console`;
  QEMU stdin must be redirected from `/dev/null` to prevent hangs in background tasks.
Use this on every branch change; only do the hardware reboot below once it's green.

## Important: master ≠ running system
The live PID 1 on blakbox is still the OLD binary. Merging didn't deploy it.
Build `feat/start-timeout` (contains BOTH features) and install as boot init.

## 1. Build (branch feat/start-timeout = timers + start_timeout in one)
```
cd ~/projects/schema-init
git checkout feat/start-timeout && git pull
make clean && make
file schema-init    # confirm it built
```

## 2. Install the test services (live dir is /etc/schema-init/services/)
```
sudo cp ~/schema-livetest/test-timer.svc     /etc/schema-init/services/
sudo cp ~/schema-livetest/test-hang.svc      /etc/schema-init/services/
sudo cp ~/schema-livetest/test-dependent.svc /etc/schema-init/services/
```

## 3. Install the new init binary as boot PID 1
CHECK where the running init actually is before overwriting (see memory:
project_schema_init_blakbox — real /boot path, decoy in root-fs). Typically:
```
sudo cp schema-init /sbin/schema-init   # or wherever init= points; VERIFY FIRST
```
Keep a backup of the current one: `sudo cp /sbin/schema-init /sbin/schema-init.bak`

## 4. Reboot, then check the log
```
sudo reboot
# after login:
cat /var/log/schema-init/*.log  2>/dev/null
# or the run log:
cat /run/log/schema-init/* 2>/dev/null
journalctl -k 2>/dev/null | grep schema   # if anything
```

## 5. PASS criteria
- TIMER (#7): ~30s after boot, log shows `test-timer ... timer-fire` then
  `timer-done`; `/run/test-timer.fired` exists. Repeats every 60s.
- START-TIMEOUT (#8): `test-hang` logs `start-timeout` ~90s in; it is a
  non-critical oneshot so it EXCISES; `test-dependent` then runs and
  `/run/test-dependent.ran` appears = boot did NOT hang on the wedged service.

## 6. After PASS
```
gh pr merge 8 --merge          # merge start_timeout to master
# remove the test services so they don't keep firing:
sudo rm /etc/schema-init/services/test-timer.svc \
        /etc/schema-init/services/test-hang.svc \
        /etc/schema-init/services/test-dependent.svc
```

## If a test FAILS
Do NOT merge #8. Capture the log, tell Claire, fix on the branch first.
Rollback init if boot is unhappy: boot a known-good kernel/init, restore
`/sbin/schema-init.bak`.
