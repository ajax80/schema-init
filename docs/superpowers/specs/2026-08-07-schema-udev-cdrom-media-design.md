# schema-udev slice 3e — cdrom_id media status + optical filesystem

**Sub-project:** B (udevd-retirement endgame) · **Slice:** 3e
**Date:** 2026-08-07 · **Base:** master @ 743ab27 (#94 merged)
**Branch:** `feat/schema-udev-cdrom-media`

## Goal

Close the two deferrals slice 3d opened. Extend the existing `cdrom_id` builtin
from drive-capability detection (3d, capabilities-only) to also report **media
status** (`ID_CDROM_MEDIA*`) and the **optical filesystem** (`ID_FS_*` via
ISO9660, plus a UDF prober). Reliability against a not-ready/cold drive is the
central requirement — the #94 lesson: a warm parity tool can succeed where the
daemon's cold coldplug read fails, so the artifact of record is the **daemon's
shadow db**, not the parity tool.

## Context / what already exists (master @ 743ab27)

- `cdrom_id.h`: `cdrom_id_decode(buf,len,out)` walks GET CONFIGURATION feature
  descriptors → capability keys (Profile List, feature 0x0000); `cdrom_get_config`
  (SG_IO 0x46, RT=0, 2048B); `cdrom_id_build`; `CEMIT` dedup macro. Capabilities
  only — it ignores the current-profile field and reads no media/fs.
- `udev_builtins.h`: `UB_CDROM` fires on block `sr*`/`scd*`; UB_BLKID **excludes**
  `sr*` (correct — we do NOT run blkid on optical). `run_builtins` calls
  `cdrom_id_build` last. **No gate change needed in 3e.**
- `udev-parity.h`: `parity_cdrom_media(key)` defers `ID_CDROM_MEDIA*` → 3e;
  `parity_is_optical(devpath)` + the block-branch line defer optical `ID_FS_` → 3e.
  **These two deferrals are what 3e removes.**
- `blkid_fs.h` / `blkid_pt.h`: carry `bpt_read_at`, `bpt_emit`, `fs_emit_label`,
  `fs_emit_uuid`, `fs_safe_bytes`, `fs_encode_bytes` (all on master). The
  ISO9660-specific `fs_trim_bytes`/`fs_trim_encode_bytes`/`fs_probe_iso9660` were
  reverted in the #94 pare-back but are intact at `git show 0652c65:blkid_fs.h`
  (lines ~247–300) for salvage.

## Scope (decided in brainstorm)

- **Media-ready reliability:** TEST UNIT READY poll + bounded retry (mirrors real
  cdrom_id; bounded so it can never wedge the event loop).
- **Media keys — full set:** presence + state + type + counts.
- **Optical filesystem:** ISO9660 **and** UDF, both a full parse and both verified
  live this slice — ISO9660 against the burned Wardriver disc, UDF (full key set)
  against the `POWERT_TOUR_DVD` bridge disc. A UDF disc turned up, so UDF is no
  longer code-only.

Out of scope (unchanged deferrals): true non-USB `scsi_id` and `mtd_probe` (no
hardware on blakbox); blkid geometry keys `ID_FS_SIZE/BLOCKSIZE/LASTBLOCK`
(stay in `parity_deferred`).

## Architecture

All media work lands in `cdrom_id.h`; optical filesystem probing lands in a new
`optical_fs.h`. No new builtin, no `ub_select` change. `cdrom_id_build` becomes
the orchestrator for the whole optical device:

```
cdrom_id_build(devnode):
  1. GET CONFIGURATION (existing)   -> capabilities (3d, unchanged)
                                    -> + current-profile media type (3e)
  2. cdrom_test_unit_ready(devnode) -> bounded poll; if never ready, STOP here
                                       (emit only what step 1 gave = capabilities)
  3. READ DISC INFO (0x51)          -> ID_CDROM_MEDIA_STATE + session/track counts
  4. READ TOC (0x43, fmt 0)         -> data/audio track split
  5. optical_fs_probe(devnode)      -> ID_FS_* (ISO9660 / UDF)
```

Steps 3–5 run **only after** TEST UNIT READY reports ready. This is the single
choke point that fixes the #94 cold-read failure: no media/fs read is ever
attempted against a not-ready drive.

### Media-ready gate — `cdrom_test_unit_ready`

- SG_IO TEST UNIT READY: cdb `[0]=0x00`, all else 0; `SG_DXFER_NONE`.
- Interpret sense: ready (good status) → return READY. Sense key 0x02 (NOT READY)
  with ASC 0x3A (MEDIUM NOT PRESENT) → return NO_MEDIA (stop, no media keys).
  Sense key 0x02 with ASC 0x04 (LOGICAL UNIT NOT READY / becoming ready) →
  retry. Any other error → treat as not-ready, retry.
- Bounded retry: at most **N=5** attempts, `nanosleep` **200 ms** between
  (≤ ~1 s worst case). After N, give up → caller emits capabilities only.
  Rationale: spin-up of an already-inserted disc completes well within 1 s;
  we are not waiting for a human to insert a disc (that is the kernel MEDIA
  CHANGE uevent's job, a later slice). Bounded so the single-threaded event
  loop can never stall.

### Media presence + type — from GET CONFIGURATION current profile

- GET CONFIG response header carries the **current profile** at bytes `[6..7]`
  (big-endian); 3d ignored it. If current profile is `0x0000` (no current
  profile) or `0xFFFF`, treat as no media. Otherwise emit `ID_CDROM_MEDIA=1`
  and `ID_CDROM_MEDIA_<type>=1` using the **same profile→key table** as the
  capability decode but with the `ID_CDROM_MEDIA_` prefix (e.g. profile 0x11 →
  `ID_CDROM_MEDIA_DVD_R`).
- `ID_CDROM_MEDIA` is also emitted when READ DISC INFO succeeds (belt-and-braces:
  a disc that returns disc info is present even if the profile read is odd).

### Media state + counts — READ DISC INFO (0x51) + READ TOC (0x43)

- **READ DISC INFO** cdb: `[0]=0x51`, `[7..8]=alloc len` (BE), else 0. Standard
  disc info (data type 000b). Parse:
  - `byte2` bits 0–1 = disc status: `0`→`blank`, `1`→`appendable`, `2`→`complete`,
    `3`→other (emit no state). → `ID_CDROM_MEDIA_STATE`.
  - sessions = `(byte9<<8)|byte4` → `ID_CDROM_MEDIA_SESSION_COUNT`.
  - first track = `byte3`; last track = `(byte11<<8)|byte6`;
    track total = last − first + 1 → `ID_CDROM_MEDIA_TRACK_COUNT`.
- **READ TOC** cdb: `[0]=0x43`, `[1]=0x00` (MSF off), `[2]=0x00` (format 0 = TOC),
  `[6]=1` (start track), `[7..8]=alloc len` (BE). Response: 4-byte header
  (data len, first track, last track) + 8-byte track descriptors. Per descriptor:
  `byte1` = ADR/control; `control & 0x04` set → data track, else audio; `byte2` =
  track number; track `0xAA` = lead-out (skip). Count →
  `ID_CDROM_MEDIA_TRACK_COUNT_DATA`, `ID_CDROM_MEDIA_TRACK_COUNT_AUDIO`.
- If READ DISC INFO fails but a disc is present (per profile), emit
  `ID_CDROM_MEDIA=1` only; skip state/counts (do not fabricate).

### Optical filesystem — new `optical_fs.h`

- `#include "blkid_fs.h"` to reuse `bpt_read_at`, `bpt_emit`, `fs_emit_label`,
  `fs_emit_uuid`, `fs_safe_bytes`, `fs_encode_bytes` (no duplication).
- **ISO9660:** salvage `fs_probe_iso9660` + `fs_trim_bytes` +
  `fs_trim_encode_bytes` verbatim from `0652c65:blkid_fs.h` into `optical_fs.h`.
  Emits `ID_FS_TYPE=iso9660`, `ID_FS_USAGE=filesystem`, `ID_FS_SYSTEM_ID`,
  `ID_FS_LABEL(_ENC)`, `ID_FS_APPLICATION_ID`, `ID_FS_UUID`,
  `ID_FS_VERSION="Joliet Extension"` (when a Joliet SVD is present).
- **UDF (full parse):** new `fs_probe_udf`. Detect via the Volume Recognition
  Sequence (`NSR02`/`NSR03` at sectors 16–20) ⇒ `ID_FS_TYPE=udf`,
  `ID_FS_USAGE=filesystem`. Then AVDP @ LBA 256 → Main Volume Descriptor
  Sequence extent; walk it for the Primary Volume Descriptor (tag 1) and Logical
  Volume Descriptor (tag 6):
  - LVD LogicalVolumeIdentifier (dstring@84) ⇒ `ID_FS_LABEL(+_ENC)` +
    `ID_FS_LOGICAL_VOLUME_ID`.
  - PVD VolumeIdentifier (dstring@24) ⇒ `ID_FS_VOLUME_ID`.
  - PVD VolumeSetIdentifier (dstring@72) ⇒ `ID_FS_VOLUME_SET_ID`; its first 16
    chars lowercased (right-padded with `0`) ⇒ `ID_FS_UUID(+_ENC)`.
  - LVD DomainIdentifier suffix UDF revision (@240, BCD) ⇒ `ID_FS_VERSION`
    (e.g. `1.02`).
  - PVD ImplementationIdentifier (@389, leading `*` stripped, encoded) ⇒
    `ID_FS_APPLICATION_ID`.
  Every field is emitted only when present — never fabricated. Verified live
  against the `POWERT_TOUR_DVD` bridge disc (all offsets confirmed against real
  systemd-udevd's `/run/udev/data/b11:0`).
- **Ordering — UDF FIRST, then ISO9660.** `optical_fs_probe(devnode, out)` tries
  UDF, then ISO9660; first hit wins. Real udev reports `ID_FS_TYPE=udf` for a
  UDF+ISO9660 bridge disc even though the ISO9660 PVD is readable, so UDF must
  win. A pure-ISO9660 disc (no UDF VRS) falls through to the ISO9660 prober.
  Called from `cdrom_id_build` **only after media-ready**.

### Producer note (why cdrom_id, not blkid)

Real udev emits optical `ID_FS_*` from the blkid builtin, gated by a
media-present rule. We produce the identical shadow-db artifact from `cdrom_id`
because `cdrom_id` already holds the media-ready gate; routing optical FS through
blkid would re-introduce the cold-read path #94 proved unreliable. The record
that lands in `/run/schema-udev/data/b<maj>:<min>` is what parity is measured
against — the internal producer is an implementation detail.

## Parity classifier changes (`udev-parity.h`)

- Delete `parity_cdrom_media` and its call site in the block branch — bring
  `ID_CDROM_MEDIA*` in scope.
- Delete `parity_is_optical` and the `parity_is_optical(devpath) && ID_FS_`
  deferral line — bring optical `ID_FS_` in scope. (Geometry sub-keys remain
  deferred via the existing `parity_deferred`.)
- Net effect: after 3e, an in-scope optical `ID_CDROM_MEDIA*` or `ID_FS_*` key
  that we fail to reproduce is a genuine gap the parity gate must flag.

## Testing

### Unit / fixture tests (`tests/`)

- **Media decode** (`tests/test_cdrom_media.c`, new): feed captured GET CONFIG,
  READ DISC INFO, and READ TOC buffers for the burned Wardriver disc through the
  pure decoders; assert the exact key set (state=… , counts, media type). Add the
  blank-disc fixtures and assert `ID_CDROM_MEDIA_STATE=blank`. Do **not**
  hard-code `TRACK_COUNT=0` — a blank recordable disc commonly reports one
  invisible incomplete track; derive the expected count from the captured blank
  fixture, not from assumption.
- **ISO9660 decode:** reuse/port the existing iso9660 fixture assertion (the
  Wardriver PVD) against `optical_fs.h`. Ground truth: `ID_FS_TYPE=iso9660`,
  `ID_FS_LABEL=Wardriver.2026.1080p.WEBRip.x264`, `ID_FS_SYSTEM_ID=LINUX`,
  `ID_FS_UUID=2026-05-09-01-34-23-00`, `ID_FS_VERSION="Joliet Extension"`,
  `ID_FS_APPLICATION_ID` beginning `K3B`.
- **UDF decode:** assert `ID_FS_TYPE=udf` against a captured UDF VRS fixture
  (only if a UDF disc is available at capture time; otherwise ship a synthetic
  minimal VRS fixture and mark UDF unverified-on-hardware).

Fixtures live in `tests/fixtures/`. **Re-capture live from the attached drive**
(discs are on hand) rather than trusting banked hex — capture GET CONFIG, READ
DISC INFO, READ TOC for both burned and blank discs via a small `scratchpad`
SG_IO tool, then commit the byte buffers as `.h` fixtures.

### Parity (`sudo ./udev-parity`)

Must remain **0/0** in both directions with a disc inserted, now that
`ID_CDROM_MEDIA*` and optical `ID_FS_*` are in scope. 3a/3b/3c/3d parity intact.

### Live gate — `tests/verify_cdrom_media_live.sh` (THE #94 lesson)

The gate asserts the **daemon's shadow db record**, not the warm parity tool:

1. Ensure the daemon is running; trigger a coldplug/uevent for `sr0`
   (`udevadm trigger` equivalent or the daemon's coldplug path) with the burned
   disc inserted.
2. Read `/run/schema-udev/data/b<maj>:<min>` for sr0 and assert it **contains**:
   `ID_CDROM_MEDIA`, `ID_CDROM_MEDIA_STATE=complete`, `ID_CDROM_MEDIA_TRACK_COUNT`,
   `ID_CDROM_MEDIA_TRACK_COUNT_DATA`, `ID_FS_TYPE=iso9660`, and the Wardriver
   `ID_FS_LABEL`. Positive presence checks (anti-hollow) — never `grep -v … ==0`.
3. Regression guard: assert the daemon record emits **zero** `ID_FS_*` when no
   media is present (empty drive) — the exact #94 false-positive class, inverted.
4. Blank-disc pass (manual/prompted): with the blank disc inserted and re-triggered,
   assert `ID_CDROM_MEDIA_STATE=blank` and no `ID_FS_*`.
5. 3a/3b/3c/3d live gates remain green.

Positive assertions on the daemon artifact are mandatory: a warm parity 0/0 is
necessary but **not sufficient** — the daemon's cold path must be shown to
produce the keys.

### vmtest

`cd ~/schema-livetest && ./vmtest.sh` must PASS unchanged (schema-udev is not
PID 1; the boot rail is unaffected). Five markers as usual.

## Files

- **Modify** `cdrom_id.h`: add `cdrom_test_unit_ready`, current-profile media
  type in the GET CONFIG path, `cdrom_read_disc_info` (0x51),
  `cdrom_read_toc` (0x43), and orchestrate steps 1–5 in `cdrom_id_build`.
- **Create** `optical_fs.h`: salvaged ISO9660 prober + trim helpers + new UDF
  prober + `optical_fs_probe`.
- **Modify** `udev-parity.h`: remove the two 3e deferrals.
- **Create** `tests/test_cdrom_media.c` + fixtures; register in Makefile.
- **Create** `tests/verify_cdrom_media_live.sh`.
- **Modify** `Makefile`: build/test lines for the new unit test.
- No change to `udev_builtins.h` (`ub_select`) or `schema-udev.c`.

## Global constraints

- schema-udev netlink bind stays byte-identical: `sa.nl_groups = 1;` (kernel
  uevents only; NEVER group 2). No edit to `schema-udev.c` in this slice.
- Shadow db path stays `/run/schema-udev/data` (ours), never `/run/udev/data`.
- E: lines remain derived properties only; `V:1` trailing; semantic/set parity
  (order-independent); byte-parity is a non-goal.
- First-writer-wins on all `ub_add`/`CEMIT`/`bpt_emit` merges; never overwrite a
  kernel-provided property.
- Never fabricate a key: if a read fails or a field is absent, omit — do not
  guess. Bounded retries only; no unbounded loops in the event path.
- Mechanism-only, one-header-per-builtin pattern preserved.
