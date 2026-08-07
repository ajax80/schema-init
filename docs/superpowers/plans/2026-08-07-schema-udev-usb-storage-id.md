# schema-udev usb-storage block identity (sub-project B slice 3b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring usb-storage block identity in-scope and parity-verified by firing the existing `usb_id` builtin on usb-storage block disks, emitting `ID_INSTANCE`/`ID_USB_INSTANCE`, and letting partitions inherit — resolving the sdd `-0:0` identity slice 1 deferred. No new builtin.

**Architecture:** Four small edits to existing files. `usb_id.h` already walks up to the usb_device and composes the scsi `-C:L` serial from sysfs; it just never runs on block nodes and doesn't emit the instance keys. We (1) gate `usb_id` onto usb-storage block disks, (2) add `ID_INSTANCE`/`ID_USB_INSTANCE`, (3) make usb-chain block identity in-scope in the parity classifier, (4) extend `rules_block_bypass` so usb-storage partitions inherit.

**Tech Stack:** C (C99, `-Wall -Wextra`), existing schema-init Makefile + synthetic-sysfs test harness in `tests/test_usb_id.c`, `~/schema-livetest/vmtest.sh`.

## Global Constraints

- **No new builtin, no SCSI INQUIRY.** Everything comes from `usb_id` reading sysfs (the kernel already published scsi `vendor`/`model`/`rev`). True `scsi_id` (SG_IO INQUIRY for non-usb SCSI) is out of scope — no target on blakbox.
- **`schema-udev.c`, `schema-udev.h`, and the group-1 netlink bind stay byte-identical.**
- **Ground truth (blakbox):** sdd `b8:48` → `ID_BUS=usb`, `ID_SERIAL=Seagate_Expansion_NZ0FC26W-0:0`, `ID_SERIAL_SHORT=NZ0FC26W`, `ID_INSTANCE=0:0`, `ID_USB_INSTANCE=0:0`; partition sdd1 `b8:49` inherits `ID_BUS=usb` + `ID_SERIAL` + `ID_INSTANCE=0:0`. ATA disks `b8:0/16/32` unchanged (`ID_ATA=1`, `ID_BUS=ata`).
- **`ID_INSTANCE`/`ID_USB_INSTANCE` fire only on the scsi/usb-storage path** (when `scsidir` is found); regular usb devices emit neither, matching udev.
- **Honesty:** the live gate asserts the parity tool's computed counters AND positive reproduction of the composed `-0:0` serial. Never loosen the classifier without the keys actually being produced.
- Terse code, style matches surrounding files.

---

### Task 1: `usb_id.h` — emit `ID_INSTANCE`/`ID_USB_INSTANCE` + unit test

**Files:**
- Modify: `usb_id.h`
- Test: `tests/test_usb_id.c` (append a usb-storage/scsi case)

**Interfaces:**
- Produces: `usb_id_build` additionally emits `ID_INSTANCE` and `ID_USB_INSTANCE` = `C:L` (scsi channel:lun) when the device chain includes a scsi_device.

- [ ] **Step 1: Append the failing usb-storage test to `tests/test_usb_id.c`**

Add this function (uses the existing `mkfile`/`mklink`/`mkdirp`/`mk_iface`/`ev_get` helpers) and call it from `main()` before the final print:

```c
static void test_usb_storage_scsi(void) {
    char tmpl[] = "/tmp/schema-usbid-scsi-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    char utgt[2100], stgt[2100], btgt[2100], p[3000];
    snprintf(utgt, sizeof utgt, "%s/bus/usb", root);   mkdirp(utgt);
    snprintf(stgt, sizeof stgt, "%s/bus/scsi", root);  mkdirp(stgt);
    snprintf(btgt, sizeof btgt, "%s/class/block", root); mkdirp(btgt);

    const char *dev = "/devices/pci0/usb1/1-1";
    const char *ifc = "/devices/pci0/usb1/1-1/1-1:1.0";
    const char *scsi = "/devices/pci0/usb1/1-1/1-1:1.0/host0/target0:0:0/0:0:0:0";
    const char *blk  = "/devices/pci0/usb1/1-1/1-1:1.0/host0/target0:0:0/0:0:0:0/block/sdd";

    /* usb_device */
    snprintf(p, sizeof p, "%s%s/subsystem", root, dev);   mklink(p, utgt);
    snprintf(p, sizeof p, "%s%s/idVendor", root, dev);    mkfile(p, "0bc2\n");
    snprintf(p, sizeof p, "%s%s/idProduct", root, dev);   mkfile(p, "2322\n");
    snprintf(p, sizeof p, "%s%s/bcdDevice", root, dev);   mkfile(p, "9300\n");
    snprintf(p, sizeof p, "%s%s/serial", root, dev);      mkfile(p, "NZ0FC26W\n");
    /* usb_interface: mass storage (class 08 sub 06), driver usb-storage */
    mk_iface(root, ifc, "00\n", "08\n", "06\n", "50\n", "usb-storage");
    /* scsi_device with vendor/model/rev sysattrs */
    snprintf(p, sizeof p, "%s%s/subsystem", root, scsi);  mklink(p, stgt);
    snprintf(p, sizeof p, "%s%s/vendor", root, scsi);     mkfile(p, "Seagate \n");
    snprintf(p, sizeof p, "%s%s/model", root, scsi);      mkfile(p, "Expansion\n");
    snprintf(p, sizeof p, "%s%s/rev", root, scsi);        mkfile(p, "9300\n");
    /* block device */
    snprintf(p, sizeof p, "%s%s/subsystem", root, blk);   mklink(p, btgt);

    struct uevent ev;
    assert(usb_id_build(root, blk, &ev) == 0);
    assert(strcmp(ev_get(&ev, "ID_BUS"), "usb") == 0);
    assert(strcmp(ev_get(&ev, "ID_VENDOR"), "Seagate") == 0);
    assert(strcmp(ev_get(&ev, "ID_MODEL"), "Expansion") == 0);
    assert(strcmp(ev_get(&ev, "ID_SERIAL"), "Seagate_Expansion_NZ0FC26W-0:0") == 0);
    assert(strcmp(ev_get(&ev, "ID_SERIAL_SHORT"), "NZ0FC26W") == 0);
    assert(strcmp(ev_get(&ev, "ID_INSTANCE"), "0:0") == 0);
    assert(strcmp(ev_get(&ev, "ID_USB_INSTANCE"), "0:0") == 0);
    printf("test_usb_storage_scsi OK\n");
}
```

Add `test_usb_storage_scsi();` in `main()` before `printf("ALL usb_id tests passed\n")`.

- [ ] **Step 2: Run to verify it fails**

Run: `cc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_usb_id.c -o /tmp/schema-test-usbid && /tmp/schema-test-usbid`
Expected: FAIL — `ID_INSTANCE`/`ID_USB_INSTANCE` are NULL (not yet emitted). (The `ID_SERIAL` `-0:0` composition already works from slice 1, so only the two instance asserts fail.)

- [ ] **Step 3: Emit the instance keys in `usb_id.h`**

In `usb_id_build`, change the serial/scsidir block to capture the instance string:

```c
    char instance[16] = "";
    if (scsidir[0]) {
        unsigned H, C, T, L;
        if (sscanf(pi_base(scsidir), "%u:%u:%u:%u", &H, &C, &T, &L) == 4) {
            snprintf(instance, sizeof instance, "%u:%u", C, L);
            char inst[32];
            snprintf(inst, sizeof inst, "-%s", instance);
            safe_copy(serial + strlen(serial), inst, sizeof serial - strlen(serial));
        }
    }
```

(Declare `char instance[16] = "";` immediately before the `if (scsidir[0])` block; the rest of that block is unchanged except computing `instance` and building `inst` from it.)

Then in the `UEMIT` section, add the two keys. After `if (type) UEMIT("ID_TYPE", type);`:

```c
    if (instance[0]) UEMIT("ID_INSTANCE", instance);
```

After `if (type) UEMIT("ID_USB_TYPE", type);`:

```c
    if (instance[0]) UEMIT("ID_USB_INSTANCE", instance);
```

- [ ] **Step 4: Run to verify it passes**

Run: `cc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_usb_id.c -o /tmp/schema-test-usbid && /tmp/schema-test-usbid`
Expected: PASS — `test_usb_storage_scsi OK` and `ALL usb_id tests passed` (existing tests still green — the instance keys are additive and gated on `scsidir`, so the camera/mouse/pico cases are unaffected).

- [ ] **Step 5: Commit**

```bash
git add usb_id.h tests/test_usb_id.c
git commit -m "feat(usb_id): emit ID_INSTANCE/ID_USB_INSTANCE for usb-storage (scsi C:L)"
```

---

### Task 2: Wire usb_id onto usb-storage block + open parity scope

**Files:**
- Modify: `udev_builtins.h`, `udev-parity.h`, `udev_rules.h`

**Interfaces:**
- Consumes: existing `ub_ancestor_in`, `usb_id_build`, `udev_identity_key`.

- [ ] **Step 1: Fire `UB_USB` on usb-storage block disks (`udev_builtins.h`)**

In `ub_select`, after the existing usb_device gate:

```c
    if (subsystem && strcmp(subsystem, "usb") == 0 &&
        devtype && strcmp(devtype, "usb_device") == 0) sel |= UB_USB;
```

add:

```c
    static const char *const usb_anc[] = { "usb", NULL };
    if (subsystem && strcmp(subsystem, "block") == 0 &&
        devtype && strcmp(devtype, "disk") == 0 &&
        ub_ancestor_in(sysroot, devpath, usb_anc))
        sel |= UB_USB;
```

`run_builtins` already dispatches `UB_USB` (`usb_id_build(sysroot, devpath, &tmp)`), which handles being invoked on a block devpath (it walks up to the usb_device). No change to `run_builtins`.

- [ ] **Step 2: Make usb-chain block identity in-scope (`udev-parity.h`)**

In `parity_in_scope_missing`, the `block` branch currently makes identity in-scope only on `/ata`. Extend to `/usb` and update the comment:

```c
        /* identity keys are ours on the ATA chain (ata_id) and the usb chain
         * (usb_id on usb-storage block, slice 3b); real non-usb SCSI stays
         * deferred (scsi_id, no target). */
        if (udev_identity_key(key) && devpath &&
            (strstr(devpath, "/ata") != NULL || strstr(devpath, "/usb") != NULL))
            return 1;
```

- [ ] **Step 3: Let usb-storage partitions inherit (`udev_rules.h`)**

In `rules_block_bypass`, extend the ancestor `ID_BUS` allowance from `ata` to `ata||usb`:

```c
    if ((strcmp(key, "ID_ATA") == 0 || udev_identity_key(key)) &&
        uevent_get(anc, "ID_BUS") &&
        (strcmp(uevent_get(anc, "ID_BUS"), "ata") == 0 ||
         strcmp(uevent_get(anc, "ID_BUS"), "usb") == 0))
        return 0;   /* ATA/usb-storage disk identity allowed onto its partitions */
```

- [ ] **Step 4: Build daemon + parity tool**

Run: `make schema-udev parity`
Expected: both compile clean, `-Wall -Wextra`.

- [ ] **Step 5: Commit**

```bash
git add udev_builtins.h udev-parity.h udev_rules.h
git commit -m "feat(usb-storage): fire usb_id on usb block + in-scope parity + partition inherit"
```

---

### Task 3: Live gate + full verification

**Files:**
- Create: `tests/verify_usb_storage_id_live.sh`
- Modify: `Makefile` (parity dep already lists the headers from slice 3a; no change needed unless a header is newly referenced — verify)

**Interfaces:**
- Consumes: `./udev-parity`, `./schema-udev`, real `/run/udev/data`.

- [ ] **Step 1: Write `tests/verify_usb_storage_id_live.sh`**

```sh
#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s schema-udev parity

# counters via the parity tool (sudo: blkid raw reads)
OUT=$(sudo ./udev-parity)
MM=$(echo "$OUT" | sed -n 's/^VALUE MISMATCHES (keys in both, differing value): //p')
MISS=$(echo "$OUT" | sed -n 's/^IN-SCOPE MISSING (device-class aware): //p')
echo "mismatches=$MM inscope-missing=$MISS"
[ "$MM" = "0" ] || { echo "FAIL: $MM value mismatches"; echo "$OUT" | grep '^VALMIS'; exit 1; }
[ "$MISS" = "0" ] || { echo "FAIL: $MISS in-scope missing"; echo "$OUT" | grep '^INSCOPE-MISS'; exit 1; }

# coldplug into the shadow db and check the usb disk + its partition
sudo rm -rf /run/schema-udev
sudo ./schema-udev & UDPID=$!
sleep 2; sudo kill "$UDPID" 2>/dev/null || true; wait "$UDPID" 2>/dev/null || true

d=/run/schema-udev/data/b8:48
grep -q '^E:ID_BUS=usb$' "$d" || { echo "FAIL: sdd missing ID_BUS=usb"; exit 1; }
grep -q '^E:ID_SERIAL=Seagate_Expansion_NZ0FC26W-0:0$' "$d" || { echo "FAIL: sdd wrong/missing composed ID_SERIAL"; cat "$d"; exit 1; }
grep -q '^E:ID_INSTANCE=0:0$' "$d" || { echo "FAIL: sdd missing ID_INSTANCE=0:0"; exit 1; }

part=/run/schema-udev/data/b8:49
if [ -e "$part" ]; then
    grep -q '^E:ID_BUS=usb$' "$part" || { echo "FAIL: sdd1 did not inherit ID_BUS=usb"; exit 1; }
    grep -q '^E:ID_SERIAL=Seagate_Expansion_NZ0FC26W-0:0$' "$part" || { echo "FAIL: sdd1 did not inherit ID_SERIAL"; exit 1; }
fi

# regression: ATA disks still native
for k in b8:0 b8:16 b8:32; do
    grep -q '^E:ID_ATA=1$' "/run/schema-udev/data/$k" || { echo "FAIL: $k lost ID_ATA"; exit 1; }
    grep -q '^E:ID_BUS=ata$' "/run/schema-udev/data/$k" || { echo "FAIL: $k lost ID_BUS=ata"; exit 1; }
done

echo ">> RESULT: PASS (usb-storage id live gate: 0/0, sdd+sdd1 composed serial, ATA intact)"
```

Make executable: `chmod +x tests/verify_usb_storage_id_live.sh`.

- [ ] **Step 2: Run the live gate**

Run: `./tests/verify_usb_storage_id_live.sh`
Expected: `>> RESULT: PASS (usb-storage id live gate: 0/0, sdd+sdd1 composed serial, ATA intact)`. On failure the printed `VALMIS`/`INSCOPE-MISS` lines or the missing-key message localize it.

- [ ] **Step 3: Full unit suite**

Run: `make test`
Expected: all green, including `test_usb_storage_scsi OK` / `ALL usb_id tests passed`.

- [ ] **Step 4: vmtest boot rail**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS`. Not PID 1; a regression here means something leaked into the boot rail.

- [ ] **Step 5: Commit**

```bash
git add tests/verify_usb_storage_id_live.sh Makefile
git commit -m "test(usb-storage): live gate (0/0 + sdd/sdd1 composed serial + ATA intact)"
```

---

## Self-Review

**Spec coverage:**
- Fire usb_id on usb-storage block → Task 2 Step 1. ✓
- `ID_INSTANCE`/`ID_USB_INSTANCE` emission → Task 1. ✓
- usb-chain block identity in-scope → Task 2 Step 2. ✓
- usb-storage partition inheritance → Task 2 Step 3. ✓
- Unit test (synthetic usb→scsi→block tree) → Task 1. ✓
- Live gate 0/0 + composed serial + partition + ATA regression guard → Task 3. ✓
- vmtest unchanged, boundary (schema-udev.c/.h untouched) → Task 3 Step 4 + Global Constraints. ✓
- True scsi_id deferred → Global Constraints. ✓

**Type consistency:** `usb_id_build(sysroot, devpath, out)` unchanged; the new gate calls it exactly as the existing usb_device path does. `instance[16]` local; `ID_INSTANCE`/`ID_USB_INSTANCE` emitted via the existing `UEMIT` macro. `ub_ancestor_in(sysroot, devpath, subs)` matches its definition. The `/usb` substring test (parity) and `ub_ancestor_in` (dispatch) both select the same usb-storage disks.

**Placeholder scan:** none — all code written in full. The only conditional is Task 3's `Makefile` step, which says to verify the parity dep line already covers the touched headers (it does after slice 3a: `udev_builtins.h udev_rules.h udev-parity.h`); `usb_id.h` is pulled transitively via `udev_builtins.h` and is already implicitly covered, so no Makefile change is expected.
