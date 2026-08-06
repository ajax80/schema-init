# schema-udev builtin #4: net_id — Design

**Status:** approved 2026-08-06
**Builtin:** #4 of the udevd-retirement cutover worklist (Phase 3b), after path_id (#1), usb_id (#2), input_id (#3).
**Boundary:** mechanism-only, off by default, `schema-udev.c` / `schema-udev.h` byte-identical. Wired to nothing live.

## Goal

Reproduce, byte-for-byte, the `ID_NET_NAME_*` / `ID_NET_LABEL_*` / `ID_NET_NAMING_SCHEME`
properties that systemd-udevd's `net_id` builtin synthesizes for network interfaces — by reading
the interface's sysfs and walking to its bus parent, running udev's fixed naming logic. Acceptance
is **0 mismatches vs real udev across the 9 net devices on blakbox** (both directions), with the
PCI + platform + devicetree + USB bus branches implemented so the builtin is correct on the rest of
the fleet (Pis name onboard NICs via platform/devicetree; dongles via USB), not just this box.

## Normative reference

Faithful port of systemd `src/udev/udev-builtin-net_id.c` (v259 — the version pinned in
`ID_NET_NAMING_SCHEME` on this box). Where this document and the systemd source disagree, **the
systemd source governs and the live parity gate is the final authority.** Greg should consult the
upstream file directly while implementing; this spec pins the emission model, gate order, prefix
logic, and the four in-scope bus-suffix formats so the port is unambiguous.

## What net_id emits — and what it does NOT

net_id emits ONLY these keys (all `=<value>`, never `=0`, absence == false):

| Key | When |
|---|---|
| `ID_NET_NAMING_SCHEME` | stamped `v259` on every non-stacked, supported-ARPHRD device |
| `ID_NET_NAME_MAC` | permanent hw addr, 6 bytes, non-infiniband |
| `ID_NET_NAME_ONBOARD` | ACPI `acpi_index` / DT / platform onboard index present |
| `ID_NET_LABEL_ONBOARD` | firmware `label` present |
| `ID_NET_NAME_PATH` | bus-parent path resolved (PCI/USB/platform/devicetree) |
| `ID_NET_NAME_SLOT` | PCI hotplug slot present and distinct from PATH |

**net_id does NOT emit `ID_NET_DRIVER`** — that is the `net_setup_link` builtin (out of scope,
deferred exactly like usb_id's `*_FROM_DATABASE`). It also does NOT emit the final `ID_NET_NAME`,
`ID_PATH`, `ID_BUS`, `ID_VENDOR_ID`, `ID_MODEL_ID` — those come from `net_setup_link`, our path_id
(#1), and the rules file, respectively. Pure sysfs: **no ethtool, no ioctl, no socket.**

## Ground truth (blakbox, decoded from `/run/udev/data/n*` + `/sys/class/net`)

Exact per-device expected net_id output — the live gate's target:

| device | iflink≠ifindex? | ARPHRD | expected net_id keys |
|---|---|---|---|
| enp6s0 | no | ether(1) | `ID_NET_NAMING_SCHEME=v259`, `ID_NET_NAME_PATH=enp6s0`, `ID_NET_NAME_MAC=enxa8a1590be8ef` |
| wlp5s0 | no | ether(1) | `ID_NET_NAMING_SCHEME=v259`, `ID_NET_NAME_PATH=wlp5s0` |
| docker0 | no | ether(1) | `ID_NET_NAMING_SCHEME=v259` |
| podman0 | no | ether(1) | `ID_NET_NAMING_SCHEME=v259` |
| lo | — | loopback(772) | *nothing* |
| wgnord | — | none(65534) | *nothing* |
| tailscale0 | — | none(65534) | *nothing* |
| veth0 | **yes** (stacked) | ether(1) | *nothing* |
| veth88d9c02 | **yes** (stacked) | ether(1) | *nothing* |

Three buckets: 2 named (bus parent resolved), 2 scheme-only (standalone virtual — ether, not
stacked, but no bus parent → only the scheme stamp), 5 empty (stacked or unsupported ARPHRD).
wlp5s0 has no `ID_NET_NAME_MAC` — its hw addr is not `NET_ADDR_PERM` (`addr_assign_type != 0`).

## Sysfs layout (exact — the gate cannot catch a wrong path)

On the interface node `<sysroot>/sys/class/net/<ifname>` (equivalently the `/sys/devices/...`
devpath net_id is invoked with):

- `ifindex`, `iflink` — decimal ints. `iflink != ifindex` ⇒ **stacked**, emit nothing.
- `type` — ARPHRD number (decimal). Only `1` (ETHER), `256` (SLIP), `32` (INFINIBAND) proceed.
- `addr_assign_type` — `0` = `NET_ADDR_PERM` (required for `ID_NET_NAME_MAC`).
- `address` — colon hex MAC (e.g. `a8:a1:59:0b:e8:ef`); strip colons → `a8a1590be8ef`.
- `addr_len` — hw addr byte length; must be `6` for MAC name.
- `uevent` — contains `DEVTYPE=` (`wlan` → `wl`, `wwan` → `ww`; absent for plain ether → `en`).

Bus parent: walk the devpath up the parent chain (reuse path_id.h `pi_parent`) reading each
ancestor's `subsystem` symlink basename (`pi_subsystem`). First ancestor whose subsystem is
`pci` / `usb` / `platform` / `of` (devicetree) selects the bus branch. If none, no PATH/SLOT.

## Algorithm (port) — gate order is load-bearing

1. Read `ifindex`, `iflink`. If they differ → **stacked device, emit nothing, return 0.**
2. Read `type`. If not in {1, 256, 32} → emit nothing, return 0.
3. Emit `ID_NET_NAMING_SCHEME=v259` (pinned constant — the one version-coupled value; matches this
   box's udev. Documented as such so a future scheme bump is a one-line change + gate re-baseline).
4. **Prefix** from `type` + `DEVTYPE`:
   - `type==1` (ether): `DEVTYPE=wlan` → `wl`; `DEVTYPE=wwan` → `ww`; else → `en`.
   - `type==256` (slip) → `sl`.
   - `type==32` (infiniband) → `ib` (only under the infiniband naming scheme; on this box no ib
     hardware, so this branch is unit-tested only).
5. **MAC name:** if `addr_assign_type==0` AND `addr_len==6` AND not infiniband → emit
   `ID_NET_NAME_MAC = <prefix> + "x" + <12 lowercase hex, colons stripped>`
   (e.g. `enxa8a1590be8ef`).
6. **Resolve bus parent** (walk up, match subsystem) and dispatch:

### Bus-suffix formats (in scope: PCI, USB, platform, devicetree)

**PCI** (`names_pci`) — the only branch the blakbox live gate exercises:
- Read domain/bus/slot/function from the PCI address in the parent's basename
  (`0000:06:00.0` → domain=0, bus=06, slot=00, func=0) and `<pciparent>/dev_port`.
- `ID_NET_NAME_PATH = <prefix> + [ "P"<domain> if domain!=0 ] + "p"<bus> + "s"<slot>`
  `+ [ "f"<func> if func>0 or multifunction ] + [ "d"<dev_port> if dev_port>0 ]`
  (all numbers decimal; domain 0 omitted). enp6s0 = `en` + `p6` + `s0` (bus 6, slot 0, func 0
  single-function, dev_port 0) — matches ground truth.
- `ID_NET_NAME_SLOT`: if the device sits in a PCI hotplug slot (`/sys/bus/pci/slots/<n>/address`
  matches the parent, per `names_pci_slot`), emit `<prefix>` + optional domain + `s`<slot#> +
  the same func/dev_port suffix — but **only if it differs from NAME_PATH** (udev drops it
  otherwise). No hotplug slots on blakbox → not emitted here; unit-tested with a fabricated
  `/sys/bus/pci/slots` tree.
- `ID_NET_NAME_ONBOARD` / `ID_NET_LABEL_ONBOARD`: read `<pciparent>/acpi_index` (→ ONBOARD index)
  and `<pciparent>/label` (→ LABEL string). Absent on blakbox → not emitted; unit-tested.

**USB** (`names_usb`): parse the USB device basename (`sysname`) into port/config/interface —
delimiters `-`, `:`, `.` (e.g. `1-1.2:1.0`). Build specifier `"u" + <ports> + [config] +
[interface]`. Then walk to the USB device's **PCI parent** and delegate to `names_pci` with the
USB specifier appended to the PCI path (→ `enP0p1u1...`). If no PCI parent (USB-host controller),
name is `<prefix>` + specifier (`enu1c2i0`). Ports = the dotted chain after the first `-`;
config appended as `c<n>` if config≠1; interface as `i<n>` if interface≠0.

**platform** (`names_platform`, ACPI): read the platform id from the `platform`-subsystem
ancestor's basename (the ACPI `_HID:_UID` form, 10 or 11 chars, colon at index 7 for a 3-char
vendor or 8 for 4-char). Bail if length ∉ {10,11}, colon misplaced, or vendor chars invalid
(`-EOPNOTSUPP` → emit nothing for this branch). Format:
`ID_NET_NAME_PATH = <prefix> + "a" + <lowercase vendor> + <hex model> + "i" + <instance>`.

**devicetree** (`names_devicetree`): only under the devicetree-aliases naming scheme. Resolve the
device's `of_node` (or parent's), match it against an alias under `/proc/device-tree/aliases`
(or `<sysroot>` equivalent) whose index gives N. Format: `ID_NET_NAME_PATH = <prefix> + "d" + <N>`.
Bail (emit nothing for this branch) if no `of_node` or no matching alias.

Ordering within a device: SCHEME → MAC → ONBOARD/LABEL → PATH → SLOT, matching udev's emission
order so the live full-line diff is order-exact.

## Components

- **`net_id.h`** (new): includes `path_id.h` (→ `schema-udev.h`) for `pi_sysattr` / `pi_parent` /
  `pi_base` / `pi_subsystem` / `safe_copy` / `struct uevent`. Public:
  `int net_id_build(const char *sysroot, const char *devpath, struct uevent *out)`.
  Internal statics (mirroring input_id.h's shape): `nid_emit` (UE_MAX_KEYS-guarded),
  `nid_prefix`, `nid_mac_name`, `nid_names_pci`, `nid_names_usb`, `nid_names_platform`,
  `nid_names_devicetree`, `nid_find_bus_parent`.
- **`tests/test_net_id.c`** (new): fabricated sysfs trees under a tmpdir, one per branch —
  stacked-skip (iflink≠ifindex), ARPHRD-skip (lo/none), prefix selection (en/wl/ww/sl/ib),
  MAC perm-vs-random and non-6-byte, PCI (domain 0 & nonzero, func 0 & >0, multifunction,
  dev_port>0), PCI hotplug slot (NAME_SLOT distinct from PATH), ACPI onboard/label, USB
  (with & without PCI parent), platform (valid + malformed id bail), devicetree (alias hit + miss).
  Assert the exact emitted key set per tree.
- **`tests/verify_net_id_live.sh`** (new): same shape as `verify_input_id_live.sh`. For every
  `/sys/class/net/*`, run `net_id_build`, exact full-line diff of the emitted
  `ID_NET_NAME_*`/`ID_NET_LABEL_*`/`ID_NET_NAMING_SCHEME` subset vs `/run/udev/data/n*` (filter
  udev's set to those keys — `ID_NET_DRIVER`/`ID_PATH`/`ID_BUS`/`ID_NET_NAME` are owned elsewhere
  and expected-absent from our output, so they are excluded from BOTH directions of the diff).
  Expect **9 devices, 0 mismatches**, both directions (wrong value AND under/over-emission).
- **`Makefile`**: one line to build+run `tests/test_net_id.c`.
- **`schema-udev.c` / `schema-udev.h`**: unchanged (byte-identical; fits existing `UE_MAX_KEYS`).

## Testing / acceptance gate

1. `make test` green incl. `test_net_id`, `-Wall -Wextra` clean.
2. Boundary: `git diff master -- schema-udev.c schema-udev.h` empty; `grep net_id schema-udev.c`
   empty.
3. Live: `tests/verify_net_id_live.sh` → **9 devices, 0 mismatches**, both directions.
4. `vmtest.sh` → RESULT: PASS (new header must not disturb the PID-1 boot rail).

## Out of scope

- `ID_NET_DRIVER` (net_setup_link's job), the final `ID_NET_NAME` (net_setup_link), `.link`-file
  policy.
- Bus branches **ccw / vio / xen / bcma / netdevsim** — IBM-mainframe / Xen / kernel-test buses
  that never touch this fleet. Deliberately omitted; add only if hardware ever needs them.
- hwdb-derived names (builtin #6, hwdb, is later in the worklist).
- Any live wiring — `schema-udev.c` stays byte-identical; the header is mechanism only.
