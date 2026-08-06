# udev parity/shadow harness — design

**Status:** design approved 2026-08-06
**Context:** Phase 3b de-risking for the schema-udev flagship. Phase 3a (merged, PR #77) gave us the `/run/udev/data` record encoder + `udev_db_filename` + the libudev frame encoder; Phase 2 gave `coldplug_walk_root`/`uevent_from_sysfs`. This harness uses those to *measure* the gap between schema-udev and real systemd-udevd before any cutover work begins.

## Goal

Produce a **data-driven cutover worklist**: for every device on the box, enumerate the enriched properties real udevd provides that schema-udev does not, ranked by how many devices depend on each, and mapped to the udev builtin that produces it. Turn "reproduce 169 rule files" into "build these N builtins/keys first — they cover X% of the gap."

## Key insight (shapes the whole report)

`/run/udev/data/<id>` stores **only the rule/builtin-added `E:` properties** (`ID_INPUT`, `ID_SERIAL`, `ID_FS_UUID`, …). The kernel keys schema-udev already synthesizes (`DEVNAME`, `MAJOR`, `MINOR`, `DEVPATH`, `SUBSYSTEM`, `MODALIAS`) are **not** stored there — libudev merges them from sysfs at read time. So:

- schema-udev's synthesized set ≈ the kernel keys.
- real udev's `E:` set ≈ the enrichments schema-udev lacks.
- **The gap = udev's `E:` set.** The harness therefore inventories and ranks udev's `E:` keys; overlap with schema-udev's kernel keys is reported separately (usually near-zero) so a rare "already covered" key is visible.

## Non-goals

- No monitor-frame (group-2) parity in this version — deferred; the property gap already dictates the build order.
- No writes anywhere. Pure read of `/sys` and `/run/udev/data`. No netlink, no daemon interaction, `schema-udev.c` untouched.
- Not part of `make test` runtime (needs root + a live `/run/udev/data`); it is an on-demand diagnostic. Its *pure* logic is unit-tested.

## Architecture

A standalone read-only tool `tools/udev-parity.c` that `#include`s `../schema-udev.h` and reuses `coldplug_walk_root`, `uevent_from_sysfs`, `udev_db_filename`, `uevent_get`. Pure, unit-testable helpers live in a new small header `udev-parity.h` (same test-seam pattern as `schema-udev.h`) so `tests/test_parity.c` can drive them against `/tmp` fixtures without root.

### Data flow
1. `coldplug_walk_root("/sys", collect)` — for each device, `uevent_from_sysfs` → schema-udev's property set (`struct uevent`).
2. `udev_db_filename(ev, key)` → read `/run/udev/data/<key>`; parse its `E:` lines → real udev's enriched props (`struct uevent`).
3. For that device, update global aggregates:
   - each udev `E:` key: increment its device-count, unless the key also exists in schema-udev's synth set (then count it as "already covered" + check value match).
   - per-subsystem counters: devices seen, devices with a db entry, distinct `E:` keys.
4. After the walk: sort the missing-key table by device-count desc; print the report.

### Report format
```
== schema-udev vs /run/udev/data parity ==
Scanned <N> devices, <K> with a udev db entry, across <M> subsystems.

Per subsystem (devices / with-db / distinct E: keys / reproduced by schema-udev):
  input   :  22 /  20 /  9 / 0
  tty     :   6 /   4 /  5 / 0
  block   :   8 /   8 / 12 / 1
  ...

TOP MISSING PROPERTIES (udev E: keys, by device count):
  ID_INPUT                22   [builtin: input_id]
  ID_SERIAL               14   [builtin: usb_id]
  ID_FS_UUID               5   [builtin: blkid]
  ID_NET_NAME_PATH         3   [builtin: net_id]
  ID_VENDOR_FROM_DATABASE  2   [hwdb]
  ...

VALUE MISMATCHES (keys in both, differing value): <count> — <list or "none">
```

## Components (units)

- **`tools/udev-parity.c`** — `main`: sets up aggregates, runs the walk via a file-scope collector callback (same nftw/global-callback pattern schema-udev already uses), reads each db file, prints the report. Owns no reusable logic beyond orchestration.
- **`udev-parity.h`** (pure, unit-tested):
  - `int udev_db_read_eprops(const char *path, struct uevent *out)` — open `/run/udev/data/<id>`, parse only `E:KEY=value` lines into `out` (reuse `struct uevent`); ignore `V:`/`I:`/`G:`/`Q:`/`S:`/`L:`/`W:`; return 0 on success, -1 if unreadable.
  - `const char *parity_builtin_hint(const char *key)` — map a property key to the udev builtin/source that produces it, by prefix: `ID_INPUT`→`input_id`, `ID_NET`→`net_id`, `ID_SERIAL`/`ID_USB`/`ID_MODEL`/`ID_VENDOR` (not `_FROM_DATABASE`)→`usb_id`, `ID_FS`/`ID_PART`→`blkid`, `ID_PATH`→`path_id`, `*_FROM_DATABASE`→`hwdb`, `ID_V4L`/`ID_VIDEO`→`v4l_id`, else `""`.
  - `struct keycount { char key[UE_KEY_MAX]; int count; }` + `void keycount_add(struct keycount *tab, int *n, int max, const char *key)` — increment or append; and `void keycount_sort_desc(struct keycount *tab, int n)`.

## Testing

`tests/test_parity.c` (unit, no root, added to `make test`):
- `udev_db_read_eprops` against a `/tmp` fixture file containing a mix of `V:`, `I:`, `E:KEY=v`, `G:tag`, `S:link` lines → asserts only the `E:` keys/values are captured, others ignored, count correct.
- `parity_builtin_hint` for representative keys (`ID_INPUT_KEYBOARD`→`input_id`, `ID_FS_UUID`→`blkid`, `ID_NET_NAME_PATH`→`net_id`, `ID_VENDOR_FROM_DATABASE`→`hwdb`, `DEVNAME`→`""`).
- `keycount_add`/`keycount_sort_desc`: adding the same key twice yields count 2; a new key appends; sort orders by count descending.

The full tool is validated by running it on the live box (`sudo ./udev-parity`) and sanity-checking the report; no automated gate for the live run. Build must be `-Wall -Wextra` clean. `make test` stays green including the new test. vmtest not required (PID-1/daemon untouched) but must still PASS since the tree builds.

## Files
- Create `udev-parity.h`, `tools/udev-parity.c`, `tests/test_parity.c`.
- Modify `Makefile`: add `test_parity` to the `test` target; add a `parity` target that builds `tools/udev-parity.c` → `./udev-parity`.
- `schema-udev.c`, `schema-udev.h`: **unchanged** (harness only consumes the header's existing inline functions).

## Success criteria
1. `sudo ./udev-parity` on blakbox prints a per-subsystem summary + a ranked TOP MISSING PROPERTIES list with builtin hints, over the real device tree.
2. The ranked list correctly reflects `/run/udev/data` contents (spot-checkable against a couple of `E:` entries by hand).
3. Unit tests for the db parser, builtin-hint map, and keycount aggregation pass; `make test` green; build `-Wall -Wextra` clean.
4. Nothing is written; `schema-udev.c` untouched.
