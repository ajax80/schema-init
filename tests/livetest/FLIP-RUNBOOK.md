# schema-udev E3 flip — runbook

The flip makes **schema-udev** the device manager on blakbox in place of
systemd-udevd. It is a **reboot flip** (not a live in-place swap — killing
udevd under a running GUI session is the risky path we deliberately avoid).
Everything below is reversible with one file restore + a second reboot.

Branch: `feat/schema-udev-cutover-e3`. Validated by `udev-boot-vmtest.sh`
(schema-init → schema-udev-as-udevd → ready handshake → dep=udevd fires).

---

## 0. Pre-flight — confirm all green (changes nothing)

```sh
cd ~/projects/schema-init
git checkout feat/schema-udev-cutover-e3 && git pull
make clean && make schema-init schema-udev
make test                                   # expect: all OK
make verify-rules-live verify-eprops-live
sudo ./verify-rules-live | tail -1          # expect: IN-SCOPE DIVERGENCE: 0 -> PASS
sh tests/livetest/udev-vmtest.sh      | grep RESULT   # expect: PASS  (device mgmt)
sh tests/livetest/udev-boot-vmtest.sh | grep RESULT   # expect: PASS  (boot handshake)
```

Do NOT proceed unless both vmtests say PASS and verify-rules-live is 0.

## 1. Back up everything (the whole rollback lives here)

```sh
sudo cp -a /usr/bin/schema-udev /usr/bin/schema-udev.bak-preflip
sudo cp -a /etc/schema-init/services /etc/schema-init/services.bak-preflip
```

## 2. Install the new binary

```sh
sudo cp ~/projects/schema-init/schema-udev /usr/bin/schema-udev
md5sum /usr/bin/schema-udev        # note it: this is the new authority
```

## 3. Rewire the services (3 edits, deps stay intact)

**a. Point the `udevd` service at schema-udev** (keep the name `udevd` so
`network-up.svc`/anything with `dep=udevd` keeps resolving; only exec +
ready_path change):

```sh
sudo tee /etc/schema-init/services/udevd.svc >/dev/null <<'EOF'
name=udevd
exec=/usr/bin/schema-udev
needs_root=1
critical=1
priority=critical
ready_path=/run/schema-udev/ready
EOF
```

**b. Retire the standalone schema-udev shadow** (else two schema-udev
instances fight over /dev + netlink):

```sh
sudo mv /etc/schema-init/services/schema-udev.svc \
        /etc/schema-init/services/schema-udev.svc.retired-preflip
```

**c. Retire udev-trigger** (it runs `udevadm trigger`, which needs systemd-udevd's
control socket; schema-udev does its own coldplug in main(), so it's redundant):

```sh
sudo mv /etc/schema-init/services/udev-trigger.svc \
        /etc/schema-init/services/udev-trigger.svc.retired-preflip
```

## 4. Arm the switch (the sentinel = LIVE mode)

```sh
sudo touch /etc/schema-init/schema-udev.live
```

## 5. Flip = reboot

```sh
sudo reboot
```

schema-init comes up, starts `udevd` (= schema-udev LIVE), it owns /dev, runs
coldplug, writes `/run/schema-udev/ready`; `network-up` (dep=udevd) then fires.

## 6. Verify on real hardware (this is the moment of truth)

```sh
ps -eo pid,comm | grep -E 'schema-udev|systemd-udevd'   # expect schema-udev, NO systemd-udevd
ls -l /run/schema-udev/ready                            # ready marker present
ls /dev/disk/by-uuid /dev/disk/by-partuuid /dev/disk/by-id 2>&1 | head
ls /dev/disk/by-designator 2>&1                         # esp/xbootldr on the real root disk
udevadm info /dev/nvme0n1p1 | head                      # libudev consumers see props
loginctl                                                # your seat exists
```
Then: **log into the GUI**, **plug in a USB stick** and confirm it appears in
the file manager. Check network came up (`ip a`, browser).

## Rollback (if anything is off — no data risk, one reboot back)

```sh
sudo rm -f /etc/schema-init/schema-udev.live            # disarm LIVE mode
sudo rm -rf /etc/schema-init/services
sudo mv /etc/schema-init/services.bak-preflip /etc/schema-init/services
sudo cp -a /usr/bin/schema-udev.bak-preflip /usr/bin/schema-udev
sudo reboot
```

You're back to systemd-udevd exactly as it is right now.

---

## Known-debt carried into the flip (none boot/mount/ACL-affecting)

`verify-eprops-live` = 161 (E-DIFF 0). All cosmetic/secondary: ~41 stale acpi
vendor labels (a gate-baseline artifact, not reproducible by current systemd),
~56 usb/sound vendor-DB labels (deferred: needs `hwdb --subsystem=usb` modalias
synthesis), 22 networkd link-files, misc. See NOW.claire.md for the full ledger.

## Other flip hazards — checked & cleared

- **4 services call `udevadm`** (network-up, mount-ocean-drives, sddm-logged,
  udev-trigger). Post-flip there is no `/run/udev/control` socket, so
  `udevadm trigger/settle/control` fail — BUT `udevadm settle` **fails fast**
  (exit 1, "not running"), it does not hang, and **none of these scripts use
  `set -e`**, so they continue. The real work is redundant: schema-udev does a
  full coldplug before any dep=udevd service runs. Specifically
  `mount-ocean-drives.sh` polls `/dev/disk/by-uuid/<uuid>` directly (which
  schema-udev creates), so the Ocean drives still mount. `udevadm info`
  (read-only, reads /run/udev/data) keeps working. **No action required**;
  optionally append `|| true` to the settle/trigger lines to silence the logs.
- **Fresh /run:** schema-init mounts a new tmpfs over /run at boot, so no stale
  initrd/prior udev DB carries over — schema-udev populates /run/udev/data clean.
- **No bootloader change:** schema-init is already PID 1; the flip only changes
  what it *starts*. Nothing in /boot or the kernel cmdline is touched.

## What the VM could NOT test (tomorrow's kink list, expected)

virtio-only: real **nvme**, **USB 2/3 hotplug**, and **DRM/display** paths.
These are the "work the kinks" items for the day after the flip — not blockers.
