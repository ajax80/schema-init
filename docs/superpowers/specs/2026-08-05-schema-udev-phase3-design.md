# schema-udev Phase 3 (mechanism) — libudev monitor frame + /run/udev/data encoders

**Status:** design approved 2026-08-05
**Predecessors:** Phase 1 (#75, uevent→schema→action, group-1 only), Phase 2 (#76, coldplug + `/dev/schema/` symlinks) — both merged to master and deployed live.
**Roadmap:** Tier-2 reclamation flagship. This is the *mechanism* half of Phase 3 — the two hardest isolated primitives needed before real systemd-udevd can ever be retired. **It does NOT retire udevd and changes NOTHING on the live daemon path.**

## Goal

Build and byte-level-test two pure encoders that the eventual udevd cutover will need:

- **A. libudev monitor frame encoder** — turn a `struct uevent` into the exact wire format libudev clients (PipeWire, KDE, NetworkManager, udisks2, seatd) receive on netlink **group 2**.
- **B. `/run/udev/data/<id>` record encoder** — the on-disk device database format `libudev` reads via `udev_device_new_from_syspath`.

Both are pure functions in `schema-udev.h`, exercised only by unit tests. **Neither is called from `schema-udev.c`.** No group-2 socket is opened; nothing is written under real `/run/udev`.

## Why nothing is wired live (the hard safety boundary)

- **Group-2 emission while real udevd runs = doubled events.** Real udevd already broadcasts every processed event on group 2. If schema-udev also emitted, every libudev client would see each add/remove **twice** → PipeWire/KDE misbehavior. So the send path cannot be exercised on the live desktop; it is verified only by unit tests here and, at cutover, in the VM/an isolated netns.
- **Writing real `/run/udev/data` while udevd owns it = db corruption.** udevd is the sole writer of that tree today. schema-udev writing there would clobber udevd's entries. So the db encoder takes an explicit base directory; tests use `/tmp`; the real path is used only post-cutover.

"Off by default" here means literally: the daemon does not reference these functions yet. Wiring them in is the future cutover work, out of scope tonight.

## Explicit non-goals (deferred to the bench cutover session)

Measured on blakbox 2026-08-05: retiring udevd requires reproducing **169 udev rule files** (159 system + 10 local), **26 of which apply `uaccess`/ACL tags** (desktop-critical: user access to mic/cam/GPU/input), plus builtins schema-udev lacks (`input_id`, `net_id`, `ata_id`, `scsi_id`, `blkid`, `v4l_id`, `mtp-probe`, `fido_id`, hwdb). None of that is in scope. Also out of scope: opening a live group-2 socket, writing real `/run/udev/data`, and stopping `udevd.svc`. devtmpfs owns the primary `/dev` nodes, so the eventual cutover keeps device nodes but must first replace ACLs, persistent symlinks, and the property db — a separate deliberate session with SSH recovery + spare-boot fallback.

## Ground truth — captured real frames (committed fixtures)

Two real libudev monitor frames were captured read-only from the live box (a benign `change` on `/dev/null` and `/dev/tty0`) and committed:

- `tests/fixtures/libudev-frame-mem-change.bin` — 283 bytes, SUBSYSTEM=`mem`
- `tests/fixtures/libudev-frame-tty-change.bin` — 284 bytes, SUBSYSTEM=`tty`

Decoded header of the `mem` frame (the authoritative layout):

```
prefix       = "libudev\0"        (8 bytes)
magic        = 0xfeedcafe         BIG-ENDIAN on wire (htobe32/htonl)
header_size  = 40                 NATIVE byte order
props_off    = 40                 NATIVE byte order
props_len    = 243                NATIVE byte order
subsys_hash  = 0xc365cd83         BIG-ENDIAN of murmur2("mem")
devtype_hash = 0x00000000
bloom_hi     = 0x00000000
bloom_lo     = 0x00000000
```

**Critical, easy-to-get-wrong:** only `magic` and the four hash fields are big-endian (`htobe32`). `header_size`, `props_off`, `props_len` are **native** byte order. An "everything big-endian" encoder produces a frame libudev rejects.

**Non-circular murmur2 test vectors** (host-order values, i.e. `ntohl` of the on-wire field):

| input | murmur2 (host order) |
|-------|----------------------|
| `"mem"` | `0xc365cd83` |
| `"tty"` | `0x8afa90c8` |

Property payload observed (NUL-separated `KEY=VALUE`, each record incl. the last is NUL-terminated; blob begins at `props_off`): `UDEV_DATABASE_VERSION=1`, `ACTION=change`, `DEVPATH=/devices/virtual/mem/null`, `SUBSYSTEM=mem`, `DEVNAME=/dev/null`, `SEQNUM=…`, `MAJOR=1`, `MINOR=3`, `USEC_INITIALIZED=…`, plus rule-added keys. Note `DEVNAME` in a *processed* frame carries the `/dev/` prefix (the enriched form), unlike raw kernel group-1 events.

## Feature A — libudev monitor frame encoder

`ssize_t libudev_frame_build(const struct uevent *ev, char *buf, size_t bufsz)` in `schema-udev.h`.

**Header struct** (`schema-udev.h`), packed exactly 40 bytes:

```c
struct udev_monitor_header {
    char     prefix[8];              /* "libudev\0" */
    uint32_t magic;                  /* htobe32(0xfeedcafe) */
    uint32_t header_size;            /* 40, native */
    uint32_t properties_off;         /* 40, native */
    uint32_t properties_len;         /* native */
    uint32_t filter_subsystem_hash;  /* htobe32(murmur2(SUBSYSTEM)) */
    uint32_t filter_devtype_hash;    /* htobe32(murmur2(DEVTYPE)) or 0 */
    uint32_t filter_tag_bloom_hi;    /* 0 (no tags) */
    uint32_t filter_tag_bloom_lo;    /* 0 (no tags) */
};
```
(Assert `sizeof == 40` in the header via a compile-time check; do not rely on struct packing alone — serialize field-by-field into `buf` to be layout-safe.)

**Build steps:**
1. Serialize the payload first: for each `ev->key[i]=ev->val[i]`, append `KEY=VALUE\0` (each record NUL-terminated, including the last). Compute `properties_len` = total payload bytes. Require ACTION, DEVPATH, SUBSYSTEM to be present in `ev` (return -1 if not — a valid monitor frame needs them).
2. Write the 40-byte header: literal `"libudev\0"`; `magic=htobe32(0xfeedcafe)`; `header_size=properties_off=40` (native); `properties_len` (native); `filter_subsystem_hash=htobe32(murmur2(subsystem))`; `filter_devtype_hash=htobe32(murmur2(devtype))` if `DEVTYPE` present else 0; bloom hi/lo = 0.
3. Total = 40 + properties_len. Return it, or -1 if it exceeds `bufsz`.

**`murmur2`** — MurmurHash2/32 exactly as systemd `string_hash32`: seed 0, `m=0x5bd1e995`, `r=24`, standard body + tail + finalization. Signature `uint32_t murmur2(const char *str)` hashing `strlen(str)` bytes. Must produce the table vectors above.

## Feature B — /run/udev/data record encoder

`ssize_t udev_db_record_build(const struct uevent *ev, char *buf, size_t bufsz)` — the file *contents*:
```
V:1
E:KEY=value        (one per property we carry)
```
Version line `V:1` first, then one `E:` line per `ev` property (`KEY=value`). (Tags `G:`/`Q:`, devlinks `S:`, `I:<usec>`, `L:<priority>` are part of the format but schema-udev has none to emit yet — the encoder emits `V:` + `E:` lines only; document the others as recognized-but-unused.) Return length or -1 on overflow.

**`int udev_db_filename(const struct uevent *ev, char *out, size_t outsz)`** — derive the db key from the event:
- char device (`MAJOR`/`MINOR` present, `SUBSYSTEM`!=block): `c<major>:<minor>`
- block device (`SUBSYSTEM`==`block`): `b<major>:<minor>`
- net device (`SUBSYSTEM`==`net`, `IFINDEX` present): `n<ifindex>`
- otherwise: `+<SUBSYSTEM>:<sysname>` where sysname = basename of `DEVPATH`
Return 0 on success, -1 if the event lacks the keys for any form.

The write-to-disk wrapper `int udev_db_write(const char *base_dir, const struct uevent *ev)` composes `base_dir + "/" + udev_db_filename(...)` and writes the record. **`base_dir` is always explicit** — tests pass `/tmp`; the live `/run/udev/data` is never passed tonight.

## Files

- **Modify `schema-udev.h`** — add `<stdint.h>`, `<endian.h>`; `murmur2`; `struct udev_monitor_header` + `sizeof==40` static assert; `libudev_frame_build`; `udev_db_filename`, `udev_db_record_build`, `udev_db_write`.
- **Create `tests/test_libudev_frame.c`** — (1) `murmur2("mem")==0xc365cd83`, `murmur2("tty")==0x8afa90c8`; (2) build a frame for a synthetic `mem`/`change` event, assert prefix, `ntohl(magic)==0xfeedcafe`, native `header_size==40`, `properties_off==40`, `properties_len` matches payload, `ntohl(filter_subsystem_hash)==0xc365cd83`; (3) load `tests/fixtures/libudev-frame-mem-change.bin`, assert its header fields decode to the documented values (guards the layout against drift); (4) self-decode round-trip: parse our frame back the libudev way (check magic, walk `properties_off`, recover ACTION/DEVPATH/SUBSYSTEM); (5) `-1` when ACTION/DEVPATH/SUBSYSTEM missing or buffer too small.
- **Create `tests/test_udev_db.c`** — `udev_db_filename` for char (`c1:3`), block (`b8:0`), net (`n2`), other (`+mem:null`); `udev_db_record_build` exact string (`V:1\nE:...`); `udev_db_write` to a `/tmp` base creates the correctly-named file with correct contents; overflow → -1.
- **Modify `Makefile`** — add `test_libudev_frame` and `test_udev_db` to the test target.
- **`schema-udev.c` — UNCHANGED** (deliberate: nothing wired live; the daemon build and behavior are byte-identical to Phase 2).
- **Modify `README.md`** — a short "Phase 3 (interop mechanism — built, not yet active)" note describing the two encoders and that udevd retirement is a future, separate cutover.
- **Add** `tests/fixtures/libudev-frame-mem-change.bin`, `tests/fixtures/libudev-frame-tty-change.bin` (already committed with the spec).

## Testing strategy

- **Unit (primary gate):** the two new tests above + all existing tests green, `-Wall -Wextra` clean. The golden-fixture assertions + murmur vectors are ground-truth (captured from real udev), so a correct encoder is proven against the real client format, not against our own reading of it.
- **vmtest:** `~/schema-livetest/vmtest.sh` PASS — the daemon is unchanged so this must stay green; it guards against an accidental `schema-udev.c` regression or a header include that breaks the build.
- **No live-box step.** Nothing is deployed. The next time the live daemon changes is the cutover session, which is out of scope.

## Success criteria

1. `libudev_frame_build` produces a frame whose header matches the captured golden frame's layout and whose `filter_subsystem_hash` equals real udev's for the same subsystem.
2. `murmur2` matches both ground-truth vectors.
3. `udev_db_record_build`/`udev_db_filename` produce the documented on-disk format and key names.
4. All unit tests pass; vmtest PASS; `schema-udev.c` untouched; no live change.
5. The encoders are reachable only from tests — grep confirms `schema-udev.c` does not reference them.
